#include "Terrain/TerrainErosion.h"
#include "Terrain/Field2D.h"
#include "Core/EcoCore.h"
#include "Async/ParallelFor.h"

namespace
{
    /**
     * Recorre los 8 vecinos de Moore de (x, y) que caen dentro de la rejilla e
     * invoca Fn(k, nx, ny) con k = indice del offset.
     *
     * Las dos pasadas de ThermalErode -la que mide el exceso sobre el talud y la
     * que recoge el reparto- llevaban el MISMO bucle con el mismo test de
     * limites copiado. Y no es cosmetico: el reparto solo cuadra si emisor y
     * receptor recorren los vecinos en el mismo orden y con las mismas
     * distancias; tenerlo escrito una sola vez lo garantiza por construccion.
     */
    template <typename FNeighborFn>
    FORCEINLINE void ForEachMooreNeighbor(int32 x, int32 y, int32 W, int32 H, FNeighborFn&& Fn)
    {
        // Vecindad de Moore. Las distancias son simetricas (mismo |offset| visto
        // desde ambos lados), asi que emisor y receptor calculan el MISMO exceso.
        static constexpr int32 DX[8] = { 1, -1,  0,  0,  1,  1, -1, -1 };
        static constexpr int32 DY[8] = { 0,  0,  1, -1,  1, -1,  1, -1 };

        for (int32 k = 0; k < 8; ++k)
        {
            const int32 nx = x + DX[k];
            const int32 ny = y + DY[k];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H)
            {
                continue;
            }
            Fn(k, nx, ny);
        }
    }

    /** Bilinear sobre un buffer suelto (la gota trabaja en coordenadas de
        rejilla fraccionales; el llamador garantiza 0 <= x < W-1, idem y). */
    FORCEINLINE float SampleGrid(const TArray<float>& Hgt, int32 W, float X, float Y)
    {
        const int32 Ix = (int32)X;
        const int32 Iy = (int32)Y;
        const float U = X - Ix;
        const float V = Y - Iy;
        const int32 I = Iy * W + Ix;
        return Hgt[I] * (1.f - U) * (1.f - V)
            + Hgt[I + 1] * U * (1.f - V)
            + Hgt[I + W] * (1.f - U) * V
            + Hgt[I + W + 1] * U * V;
    }
}

void TerrainErosion::ThermalErode(FField2D& HeightCm, const FThermalParams& P)
{
    if (!HeightCm.IsValid() || P.Iterations <= 0 || P.Strength <= 0.f)
    {
        return;
    }

    const int32 W = HeightCm.Width;
    const int32 H = HeightCm.Height;
    const int32 N = W * H;
    const float Cell = static_cast<float>(HeightCm.CellSize);
    const float TalusTan = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(P.TalusAngleDeg, 1.f, 89.f)));
    // Tope 0.8: por encima, varias celdas vertiendo a la vez sobre un fondo de
    // valle pueden sobrepasarlo y hacer oscilar el maximo (medido en el arnes:
    // con 1.0 el pico crece; con <=0.8 converge siempre hacia el talud).
    const float Strength = FMath::Clamp(P.Strength, 0.f, 0.8f);

    // Los offsets de la vecindad viven en ForEachMooreNeighbor; aqui solo las
    // distancias, que dependen del tamano de celda de ESTE relieve.
    const float Diag = Cell * 1.41421356f;
    const float Dist[8] = { Cell, Cell, Cell, Cell, Diag, Diag, Diag, Diag };

    // Por celda: material total que vierte esta iteracion (cm) y suma de
    // excesos sobre el talud (para repartirlo proporcionalmente, Olsen 2004).
    // Verter TODO al vecino mas bajo concentra el material y oscila en los
    // fondos de valle; el reparto proporcional es estable.
    TArray<float> MoveM;   MoveM.SetNumUninitialized(N);
    TArray<float> TotalEx; TotalEx.SetNumUninitialized(N);
    TArray<float> Next;    Next.SetNumUninitialized(N);

    TArray<float>& Data = HeightCm.Data;

    for (int32 It = 0; It < P.Iterations; ++It)
    {
        // Paso 1 (paralelo): cada celda mide cuanto exceso sobre el talud
        // tiene con cada vecino mas bajo. Mueve 0.5*Strength del MAYOR exceso:
        // con Strength=1, el par mas empinado queda exactamente en el talud
        // (no se pasa de frenada aunque haya varios receptores).
        ParallelFor(H, [&](int32 y)
            {
                for (int32 x = 0; x < W; ++x)
                {
                    const int32 i = y * W + x;
                    const float h = Data[i];

                    float MaxExcess = 0.f;
                    float SumExcess = 0.f;
                    ForEachMooreNeighbor(x, y, W, H, [&](int32 k, int32 nx, int32 ny)
                        {
                            const float Excess = (h - Data[ny * W + nx]) - TalusTan * Dist[k];
                            if (Excess > 0.f)
                            {
                                SumExcess += Excess;
                                MaxExcess = FMath::Max(MaxExcess, Excess);
                            }
                        });
                    TotalEx[i] = SumExcess;
                    MoveM[i] = (SumExcess > 0.f) ? 0.5f * Strength * MaxExcess : 0.f;
                }
            });

        // Paso 2 (paralelo, gather): cada celda resta su vertido y recoge su
        // parte proporcional del de cada vecino. El receptor reconstruye el
        // exceso emisor->receptor con las MISMAS alturas y distancias que uso
        // el emisor, asi que el reparto cuadra sin atomics y es determinista
        // (nadie escribe fuera de su celda).
        ParallelFor(H, [&](int32 y)
            {
                for (int32 x = 0; x < W; ++x)
                {
                    const int32 i = y * W + x;
                    float Dh = -MoveM[i];
                    ForEachMooreNeighbor(x, y, W, H, [&](int32 k, int32 nx, int32 ny)
                        {
                            const int32 j = ny * W + nx;
                            if (MoveM[j] > 0.f)
                            {
                                const float ExcessFromJ = (Data[j] - Data[i]) - TalusTan * Dist[k];
                                if (ExcessFromJ > 0.f)
                                {
                                    Dh += MoveM[j] * ExcessFromJ / TotalEx[j];
                                }
                            }
                        });
                    Next[i] = Data[i] + Dh;
                }
            });

        Swap(Data, Next);
    }
}

void TerrainErosion::HydraulicErode(FField2D& HeightCm, uint32 Seed, const FHydraulicParams& P)
{
    if (!HeightCm.IsValid() || P.Droplets <= 0 || P.Strength <= 0.f)
    {
        return;
    }

    const int32 W = HeightCm.Width;
    const int32 H = HeightCm.Height;
    const int32 N = W * H;

    // El modelo de gotas esta calibrado para alturas en [0,1] y XY en celdas
    // (Beyer 2015): se erosiona una copia normalizada y se vuelve a cm con el
    // MISMO factor. No se re-normaliza al final: la erosion rebaja picos y
    // rellena valles, y ese encogimiento del rango ES el resultado fisico.
    float Mn, Mx;
    FField2D::MinMax(HeightCm.Data, Mn, Mx);
    const float Range = Mx - Mn;
    if (Range <= KINDA_SMALL_NUMBER)
    {
        return;
    }

    TArray<float> Hgt;
    Hgt.SetNumUninitialized(N);
    for (int32 i = 0; i < N; ++i)
    {
        Hgt[i] = (HeightCm.Data[i] - Mn) / Range;
    }

    // Pincel de arranque: disco de radio BrushRadius con peso (1 - d/r)
    // normalizado. Repartir el arranque evita los pozos de 1 celda que deja
    // erosionar solo el nodo mas cercano.
    struct FBrushCell { int32 Dx; int32 Dy; float Weight; };
    TArray<FBrushCell> Brush;
    {
        const int32 R = FMath::Max(1, P.BrushRadius);
        float Total = 0.f;
        for (int32 by = -R; by <= R; ++by)
        {
            for (int32 bx = -R; bx <= R; ++bx)
            {
                const float D = FMath::Sqrt(static_cast<float>(bx * bx + by * by));
                const float Wgt = 1.f - D / static_cast<float>(R);
                if (Wgt > 0.f)
                {
                    Brush.Add({ bx, by, Wgt });
                    Total += Wgt;
                }
            }
        }
        for (FBrushCell& B : Brush)
        {
            B.Weight /= Total;
        }
    }

    uint32 Rng = (Seed != 0u) ? Seed : 0x9E3779B9u; // el estado 0 es absorbente
    const float ErodeK = P.ErodeSpeed * FMath::Clamp(P.Strength, 0.f, 1.f);

    for (int32 d = 0; d < P.Droplets; ++d)
    {
        // NextUnit < 1 garantiza celda entera valida (indice <= W-2 / H-2).
        float Px = EcoRand::NextUnit(Rng) * (W - 1);
        float Py = EcoRand::NextUnit(Rng) * (H - 1);
        float DirX = 0.f, DirY = 0.f;
        float Speed = P.InitialSpeed;
        float Water = P.InitialWater;
        float Sediment = 0.f;

        for (int32 Life = 0; Life < P.MaxLifetime; ++Life)
        {
            const int32 Ix = (int32)Px;
            const int32 Iy = (int32)Py;
            const int32 I = Iy * W + Ix;
            const float U = Px - Ix;
            const float V = Py - Iy;

            // Gradiente y altura por interpolacion bilineal de los 4 nodos.
            const float H00 = Hgt[I];
            const float H10 = Hgt[I + 1];
            const float H01 = Hgt[I + W];
            const float H11 = Hgt[I + W + 1];
            const float GradX = (H10 - H00) * (1.f - V) + (H11 - H01) * V;
            const float GradY = (H01 - H00) * (1.f - U) + (H11 - H10) * U;
            const float HOld = H00 * (1.f - U) * (1.f - V) + H10 * U * (1.f - V)
                + H01 * (1.f - U) * V + H11 * U * V;

            // Direccion: mezcla de inercia y gradiente cuesta abajo.
            DirX = DirX * P.Inertia - GradX * (1.f - P.Inertia);
            DirY = DirY * P.Inertia - GradY * (1.f - P.Inertia);
            const float Len = FMath::Sqrt(DirX * DirX + DirY * DirY);
            if (Len <= KINDA_SMALL_NUMBER)
            {
                // Llano perfecto: rumbo aleatorio del MISMO stream (determinista).
                const float Ang = EcoRand::NextRange(Rng, 0.f, 2.f * PI);
                DirX = FMath::Cos(Ang);
                DirY = FMath::Sin(Ang);
            }
            else
            {
                DirX /= Len;
                DirY /= Len;
            }

            Px += DirX;
            Py += DirY;
            if (Px < 0.f || Px >= (float)(W - 1) || Py < 0.f || Py >= (float)(H - 1))
            {
                break; // la gota sale del mapa (y se lleva su sedimento)
            }

            const float HNew = SampleGrid(Hgt, W, Px, Py);
            const float DeltaH = HNew - HOld;

            // Capacidad de transporte ~ pendiente * velocidad * agua.
            const float Capacity = FMath::Max(-DeltaH, P.MinSedimentCapacity)
                * Speed * Water * P.SedimentCapacityFactor;

            if (Sediment > Capacity || DeltaH > 0.f)
            {
                // Deposita: cuesta arriba rellena el hoyo (como mucho DeltaH);
                // si va sobrecargada, suelta una fraccion del excedente.
                const float Amt = (DeltaH > 0.f)
                    ? FMath::Min(DeltaH, Sediment)
                    : (Sediment - Capacity) * P.DepositSpeed;
                Sediment -= Amt;

                // Deposito bilineal en los 4 nodos de la celda de PARTIDA.
                Hgt[I] += Amt * (1.f - U) * (1.f - V);
                Hgt[I + 1] += Amt * U * (1.f - V);
                Hgt[I + W] += Amt * (1.f - U) * V;
                Hgt[I + W + 1] += Amt * U * V;
            }
            else
            {
                // Erosiona: nunca mas que el propio desnivel (cavar por encima
                // de eso dejaria hoyos delante de la gota).
                const float Amt = FMath::Min((Capacity - Sediment) * ErodeK, -DeltaH);
                float Taken = 0.f;
                for (const FBrushCell& B : Brush)
                {
                    const int32 Cx = Ix + B.Dx;
                    const int32 Cy = Iy + B.Dy;
                    if (Cx < 0 || Cx >= W || Cy < 0 || Cy >= H)
                    {
                        continue;
                    }
                    const int32 C = Cy * W + Cx;
                    const float Delta = FMath::Min(Hgt[C], Amt * B.Weight); // sin celdas negativas
                    Hgt[C] -= Delta;
                    Taken += Delta;
                }
                Sediment += Taken;
            }

            // Energia: v'^2 = v^2 - dh*g (cuesta abajo dh<0 -> acelera).
            Speed = FMath::Sqrt(FMath::Max(0.f, Speed * Speed - DeltaH * P.Gravity));
            Water *= (1.f - P.EvaporateSpeed);
        }
    }

    for (int32 i = 0; i < N; ++i)
    {
        HeightCm.Data[i] = Mn + Hgt[i] * Range;
    }
}
