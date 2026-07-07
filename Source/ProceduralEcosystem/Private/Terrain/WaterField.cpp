#include "Terrain/WaterField.h"
#include "Terrain/HeightField.h"
#include "Async/ParallelFor.h"

// =====================================================================
// Rellenado de sumideros: Priority-Flood + epsilon (Barnes et al. 2014).
//
// El D8 simple sobre ruido fractal deja muchos minimos locales que cortan
// la acumulacion de flujo en microcuencas inconexas. Rellenar las
// depresiones hace que toda celda interior drene de forma MONOTONA hasta el
// borde, conectando la red y dando valles humedos realistas.
//
// El termino epsilon impone una pendiente minima en lo rellenado, asi que
// el D8 siempre halla un vecino cuesta abajo y no quedan planos indefinidos.
//
// Se trabaja en double para que el epsilon no se pierda por precision de
// float en terrenos grandes. Cola de prioridad con desempate por indice ->
// resultado determinista (reproducible bit a bit). Heap binario propio para
// no depender de la version de la API de TArray::Heap* ni sufrir el realloc
// por shrink en cada pop.
// =====================================================================
namespace
{
    struct FPFNode { double Z; int32 Idx; };

    FORCEINLINE bool PFLess(const FPFNode& A, const FPFNode& B)
    {
        return A.Z != B.Z ? A.Z < B.Z : A.Idx < B.Idx; // min-heap; desempate determinista
    }

    void FillDepressionsPriorityFlood(const FField2D& H, TArray<double>& Filled)
    {
        const int32 W = H.Width;
        const int32 Ht = H.Height;
        const int32 N = W * Ht;

        Filled.SetNumUninitialized(N);

        TArray<uint8> Closed;
        Closed.Init(0, N);

        // Almacenamiento del heap preasignado a N (cada celda entra 1 vez):
        // asi GetData() no se invalida y no hay reasignaciones ni shrink.
        TArray<FPFNode> HeapBuf;
        HeapBuf.SetNumUninitialized(N);
        FPFNode* Heap = HeapBuf.GetData();
        int32 Count = 0;

        auto Push = [&](double Z, int32 Idx)
            {
                int32 i = Count++;
                Heap[i] = { Z, Idx };
                while (i > 0) // sift-up
                {
                    const int32 p = (i - 1) >> 1;
                    if (PFLess(Heap[i], Heap[p]))
                    {
                        const FPFNode t = Heap[i]; Heap[i] = Heap[p]; Heap[p] = t;
                        i = p;
                    }
                    else break;
                }
            };

        auto Pop = [&]() -> FPFNode
            {
                const FPFNode top = Heap[0];
                Heap[0] = Heap[--Count];
                int32 i = 0;
                for (;;) // sift-down
                {
                    const int32 l = 2 * i + 1;
                    const int32 r = 2 * i + 2;
                    int32 m = i;
                    if (l < Count && PFLess(Heap[l], Heap[m])) m = l;
                    if (r < Count && PFLess(Heap[r], Heap[m])) m = r;
                    if (m == i) break;
                    const FPFNode t = Heap[i]; Heap[i] = Heap[m]; Heap[m] = t;
                    i = m;
                }
                return top;
            };

        // Semilla: todas las celdas del borde conservan su cota y hacen de
        // salida del mapa (el agua abandona el terreno por el borde).
        for (int32 y = 0; y < Ht; ++y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                if (x == 0 || y == 0 || x == W - 1 || y == Ht - 1)
                {
                    const int32 c = y * W + x;
                    Filled[c] = H.Data[c];
                    Closed[c] = 1;
                    Push(Filled[c], c);
                }
            }
        }

        static const int32 NX[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
        static const int32 NY[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
        const double Epsilon = 0.01; // cm: pendiente minima impuesta al rellenar

        while (Count > 0)
        {
            const FPFNode cur = Pop();
            const int32 cx = cur.Idx % W;
            const int32 cy = cur.Idx / W;

            for (int32 k = 0; k < 8; ++k)
            {
                const int32 nx = cx + NX[k];
                const int32 ny = cy + NY[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= Ht) continue;

                const int32 n = ny * W + nx;
                if (Closed[n]) continue;
                Closed[n] = 1;

                double z = static_cast<double>(H.Data[n]);
                if (z <= cur.Z) z = cur.Z + Epsilon; // rellena la depresion
                Filled[n] = z;
                Push(z, n);
            }
        }
    }
}

void FWaterField::BakeFromHeightField(const FHeightField& Height, float OutputMax, bool bFillSinks)
{
    if (!Height.IsValid()) return;

    const FField2D& H = Height.Field;
    const int32 W = H.Width;
    const int32 Ht = H.Height;
    const int32 N = W * Ht;

    Field.Init(W, Ht, H.CellSize, H.Origin, 0.f);

    // -----------------------------------------------------------------
    // 0) Cotas de trabajo (double): relieve con sumideros rellenados o, si
    //    se desactiva (ablacion), una copia del relieve crudo. Todo lo que
    //    viene despues usa Elev en vez de H.Data.
    // -----------------------------------------------------------------
    TArray<double> Elev;
    if (bFillSinks)
    {
        FillDepressionsPriorityFlood(H, Elev);
    }
    else
    {
        Elev.SetNumUninitialized(N);
        for (int32 i = 0; i < N; ++i) Elev[i] = static_cast<double>(H.Data[i]);
    }

    // -----------------------------------------------------------------
    // 1) Direccion de flujo D8: para cada celda, el vecino con mayor
    //    caida por unidad de distancia. Tras el rellenado, toda celda
    //    interior tiene vecino cuesta abajo; solo el borde queda sin salida.
    //    Cada celda escribe su propio FlowTo[c] -> paralelizable y determinista.
    // -----------------------------------------------------------------
    TArray<int32> FlowTo;
    FlowTo.Init(-1, N); // -1 = sin salida (borde / salida del mapa)

    static const int32 NX[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
    static const int32 NY[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };
    static const double NDist[8] = { 1.0, 1.41421356, 1.0, 1.41421356,
                                     1.0, 1.41421356, 1.0, 1.41421356 };

    ParallelFor(Ht, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                const int32 c = y * W + x;
                const double hc = Elev[c];

                double bestDropPerDist = 0.0; // solo cuenta si hay descenso real
                int32 bestNeighbor = -1;

                for (int32 k = 0; k < 8; ++k)
                {
                    const int32 nx = x + NX[k];
                    const int32 ny = y + NY[k];
                    if (nx < 0 || ny < 0 || nx >= W || ny >= Ht) continue;

                    const int32 nIdx = ny * W + nx;
                    const double drop = hc - Elev[nIdx]; // positivo = cuesta abajo
                    const double dropPerDist = drop / NDist[k];

                    if (dropPerDist > bestDropPerDist)
                    {
                        bestDropPerDist = dropPerDist;
                        bestNeighbor = nIdx;
                    }
                }

                FlowTo[c] = bestNeighbor;
            }
        });

    // -----------------------------------------------------------------
    // 2) Orden descendente por cota (rellenada). Procesar de mas alto a mas
    //    bajo garantiza que, al tocarle el turno a una celda, ya recibio
    //    todo lo que drena hacia ella desde arriba. Desempate por indice ->
    //    bake reproducible bit a bit.
    // -----------------------------------------------------------------
    TArray<int32> Order;
    Order.SetNumUninitialized(N);
    for (int32 i = 0; i < N; ++i) Order[i] = i;

    Order.Sort([&Elev](int32 A, int32 B)
        {
            const double ha = Elev[A];
            const double hb = Elev[B];
            if (ha != hb) return ha > hb;
            return A < B; // desempate determinista
        });

    // -----------------------------------------------------------------
    // 3) Acumulacion de flujo: cada celda empieza en 1 (ella misma) y
    //    entrega su acumulado a la celda hacia la que drena. SERIAL: hay
    //    dependencia aguas arriba -> abajo, no se paraleliza.
    // -----------------------------------------------------------------
    TArray<float> FlowAcc;
    FlowAcc.Init(1.f, N);

    for (int32 c : Order)
    {
        const int32 to = FlowTo[c];
        if (to >= 0)
        {
            FlowAcc[to] += FlowAcc[c];
        }
    }

    // -----------------------------------------------------------------
    // 4) TWI crudo = ln(acumulacion / tan(pendiente)). Pendiente = la del
    //    propio drenaje D8 sobre la cota rellenada (coherente con la
    //    acumulacion). Un minimo evita dividir por cero en zonas planas, que
    //    deben salir humedas (mucho TWI). Paralelizable; min/max en serial.
    // -----------------------------------------------------------------
    constexpr double MinSlopeRad = 0.001; // ~0.06 grados
    TArray<float> TwiRaw;
    TwiRaw.SetNumUninitialized(N);

    ParallelFor(Ht, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                const int32 c = y * W + x;
                double slopeRad = MinSlopeRad;

                const int32 to = FlowTo[c];
                if (to >= 0)
                {
                    const double drop = Elev[c] - Elev[to]; // > 0
                    const int32 tx = to % W, ty = to / W;
                    const double distCm = FVector2D(x - tx, y - ty).Size() * H.CellSize;
                    slopeRad = FMath::Max(MinSlopeRad, FMath::Atan(drop / FMath::Max(distCm, 1.0)));
                }

                TwiRaw[c] = static_cast<float>(FMath::Loge(FlowAcc[c] / FMath::Tan(slopeRad)));
            }
        });

    float MinTwi = TNumericLimits<float>::Max();
    float MaxTwi = -TNumericLimits<float>::Max();
    for (const float V : TwiRaw)
    {
        MinTwi = FMath::Min(MinTwi, V);
        MaxTwi = FMath::Max(MaxTwi, V);
    }

    // -----------------------------------------------------------------
    // 5) Normalizacion lineal a [0, OutputMax]: deja el campo listo para la
    //    formula de vigor (Monod, Fase 2) sin reescalados ahi.
    // -----------------------------------------------------------------
    const float Range = FMath::Max(MaxTwi - MinTwi, KINDA_SMALL_NUMBER);
    ParallelFor(Ht, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                const int32 i = y * W + x;
                const float t = (TwiRaw[i] - MinTwi) / Range; // [0, 1]
                Field.Data[i] = t * OutputMax;
            }
        });
}