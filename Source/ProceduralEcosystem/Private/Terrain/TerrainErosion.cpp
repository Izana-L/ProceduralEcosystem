/**
 * @file TerrainErosion.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de las dos pasadas de erosión del relieve.
 *
 * Define TerrainErosion::ThermalErode, escrita como gather de dos pasadas sobre
 * doble buffer para poder paralelizarla sin atómicas, y
 * TerrainErosion::HydraulicErode, el modelo lagrangiano de gotas que simula sobre
 * una copia normalizada del campo. Aloja también los dos auxiliares comunes: el
 * recorrido de la vecindad de Moore que comparten las dos pasadas térmicas y el
 * muestreo bilineal sobre un buffer suelto con el que avanza la gota. La ley de
 * capacidad de transporte procede de Mei et al., con la caída en un paso de una
 * celda como proxy barato de la pendiente.
 *
 * @ingroup eco_terrain
 * @see @ref bib_olsen2004
 * @see @ref bib_beyer2015
 * @see @ref bib_mei2007
 */

#include "Terrain/TerrainErosion.h"
#include "Terrain/Field2D.h"
#include "Core/EcoCore.h"
#include "Async/ParallelFor.h"

namespace
{
    /**
     * Recorre los 8 vecinos de Moore de (x, y) que caen dentro de la rejilla e
     * invoca Fn(k, nx, ny), con k el índice del offset.
     *
     * Copia única del recorrido de vecindad de la erosión térmica: el reparto de
     * material solo cuadra si emisor y receptor visitan los vecinos en el mismo
     * orden y con las mismas distancias, y tenerlo escrito una sola vez lo
     * garantiza por construcción.
     */
    template <typename FNeighborFn>
    FORCEINLINE void ForEachMooreNeighbor(int32 x, int32 y, int32 W, int32 H, FNeighborFn&& Fn)
    {
        // Vecindad de Moore. Los offsets son simétricos (mismo |offset| visto
        // desde ambos lados), así que emisor y receptor calculan el MISMO exceso.
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

    /**
     * Interpolación bilineal sobre un buffer suelto de alturas, en coordenadas de
     * rejilla fraccionales (las que maneja la gota).
     *
     * @pre 0 <= X < W-1 y 0 <= Y < H-1: no comprueba límites, lo garantiza el
     *      llamador antes de mover la gota.
     */
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
    // Tope 0.8: por encima, varias celdas vertiendo a la vez sobre un mismo fondo
    // de valle pueden sobrepasarlo y hacer oscilar el máximo en vez de converger
    // hacia el talud.
    const float Strength = FMath::Clamp(P.Strength, 0.f, 0.8f);

    // Los offsets de la vecindad viven en ForEachMooreNeighbor; aquí solo las
    // distancias, que dependen del tamaño de celda de ESTE relieve.
    const float Diag = Cell * 1.41421356f;
    const float Dist[8] = { Cell, Cell, Cell, Cell, Diag, Diag, Diag, Diag };

    // Por celda: material que vierte en esta iteración (cm) y suma de excesos
    // sobre el talud, denominador del reparto proporcional. Verter todo el
    // material al vecino más bajo lo concentra y hace oscilar los fondos de
    // valle; repartirlo en proporción al exceso de cada vecino es estable.
    TArray<float> MoveM;   MoveM.SetNumUninitialized(N);
    TArray<float> TotalEx; TotalEx.SetNumUninitialized(N);
    TArray<float> Next;    Next.SetNumUninitialized(N);

    TArray<float>& Data = HeightCm.Data;

    for (int32 It = 0; It < P.Iterations; ++It)
    {
        // Pasada 1 (paralela): cada celda mide cuánto exceso sobre el talud tiene
        // con cada vecino más bajo y mueve 0.5*Strength del MAYOR de ellos. Ese
        // factor 0.5 hace que con Strength=1 el par más empinado quede exactamente
        // en el talud, sin pasarse aunque haya varios receptores.
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

        // Pasada 2 (paralela, gather): cada celda resta su vertido y recoge su
        // parte proporcional del de cada vecino. El receptor reconstruye el exceso
        // emisor->receptor con las MISMAS alturas y distancias que usó el emisor,
        // así que el reparto cuadra sin atómicas y es determinista: nadie escribe
        // fuera de su celda.
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

    // El modelo de gotas está calibrado para alturas en [0,1] y XY en celdas: se
    // erosiona una copia normalizada y se vuelve a cm con el MISMO factor. No se
    // re-normaliza al final; la erosión rebaja picos y rellena valles, y ese
    // encogimiento del rango ES el resultado físico.
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
    // normalizado a suma 1. Repartir el arranque sobre el disco evita los pozos de
    // una celda que deja erosionar solo el nodo más cercano.
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
        // NextUnit < 1 garantiza celda entera válida (índice <= W-2 / H-2).
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

            // Gradiente y altura por interpolación bilineal de los 4 nodos.
            const float H00 = Hgt[I];
            const float H10 = Hgt[I + 1];
            const float H01 = Hgt[I + W];
            const float H11 = Hgt[I + W + 1];
            const float GradX = (H10 - H00) * (1.f - V) + (H11 - H01) * V;
            const float GradY = (H01 - H00) * (1.f - U) + (H11 - H10) * U;
            const float HOld = H00 * (1.f - U) * (1.f - V) + H10 * U * (1.f - V)
                + H01 * (1.f - U) * V + H11 * U * V;

            // Dirección: mezcla de inercia y gradiente cuesta abajo.
            DirX = DirX * P.Inertia - GradX * (1.f - P.Inertia);
            DirY = DirY * P.Inertia - GradY * (1.f - P.Inertia);
            const float Len = FMath::Sqrt(DirX * DirX + DirY * DirY);
            if (Len <= KINDA_SMALL_NUMBER)
            {
                // Llano perfecto: rumbo aleatorio del MISMO stream, para no meter
                // una fuente de aleatoriedad fuera del contrato de determinismo.
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

            // Capacidad de transporte ~ pendiente * velocidad * agua, con la caída
            // en un paso (-DeltaH) como proxy de la pendiente y un mínimo para que
            // el llano no quede a capacidad cero.
            const float Capacity = FMath::Max(-DeltaH, P.MinSedimentCapacity)
                * Speed * Water * P.SedimentCapacityFactor;

            if (Sediment > Capacity || DeltaH > 0.f)
            {
                // Deposita: cuesta arriba rellena el hoyo (como mucho DeltaH);
                // si va sobrecargada, suelta una fracción del excedente.
                const float Amt = (DeltaH > 0.f)
                    ? FMath::Min(DeltaH, Sediment)
                    : (Sediment - Capacity) * P.DepositSpeed;
                Sediment -= Amt;

                // Depósito bilineal en los 4 nodos de la celda de PARTIDA.
                Hgt[I] += Amt * (1.f - U) * (1.f - V);
                Hgt[I + 1] += Amt * U * (1.f - V);
                Hgt[I + W] += Amt * (1.f - U) * V;
                Hgt[I + W + 1] += Amt * U * V;
            }
            else
            {
                // Erosiona: nunca más que el propio desnivel, porque cavar por
                // encima de eso dejaría hoyos por delante de la gota.
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

            // Energía: v'^2 = v^2 - dh*g (cuesta abajo dh<0 -> acelera).
            Speed = FMath::Sqrt(FMath::Max(0.f, Speed * Speed - DeltaH * P.Gravity));
            Water *= (1.f - P.EvaporateSpeed);
        }
    }

    for (int32 i = 0; i < N; ++i)
    {
        HeightCm.Data[i] = Mn + Hgt[i] * Range;
    }
}
