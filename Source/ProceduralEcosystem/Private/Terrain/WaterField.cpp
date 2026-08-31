/**
 * @file WaterField.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del bake hidrológico: depresiones, D8, acumulación y TWI.
 *
 * Contiene el rellenado de depresiones Priority-Flood + épsilon, con heap binario
 * propio y desempate por índice de celda, y FWaterField::BakeFromHeightField, que
 * encadena sobre él las cuatro etapas restantes del pipeline: dirección de flujo
 * D8, ordenación descendente por cota, acumulación aguas abajo y evaluación del
 * índice con su normalización. Aloja también los offsets de la vecindad D8,
 * compartidos por el rellenado y por el enrutado para que no puedan divergir.
 *
 * @ingroup eco_terrain
 * @see @ref bib_barnes2014
 * @see @ref bib_ocallaghan1984
 * @see @ref bib_beven1979
 */

#include "Terrain/WaterField.h"
#include "Terrain/HeightField.h"
#include "Async/ParallelFor.h"

// =====================================================================
// Rellenado de depresiones (Priority-Flood + épsilon)
// =====================================================================
namespace
{
    /** Offsets de los 8 vecinos (D8), en el MISMO orden para el rellenado de
        depresiones y para la dirección de flujo: copia única, de modo que ambos
        recorridos no puedan divergir en silencio. */
    constexpr int32 GD8X[8] = { 1, 1, 0,-1,-1,-1, 0, 1 };
    constexpr int32 GD8Y[8] = { 0, 1, 1, 1, 0,-1,-1,-1 };

    /** Entrada de la cola de prioridad: cota de trabajo e índice lineal de celda. */
    struct FPFNode { double Z; int32 Idx; };

    /** Orden del min-heap. El desempate por índice lo hace reproducible bit a bit. */
    FORCEINLINE bool PFLess(const FPFNode& A, const FPFNode& B)
    {
        return A.Z != B.Z ? A.Z < B.Z : A.Idx < B.Idx; // min-heap; desempate determinista
    }

    /**
     * Rellena las depresiones del relieve para que toda celda interior drene de
     * forma MONÓTONA hasta el borde.
     *
     * Inunda desde el borde con una cola de prioridad: al extraer la celda más
     * baja se cierran sus vecinos aún abiertos y se eleva a @f$ z_{cur}+\varepsilon @f$
     * todo el que quede por debajo de la cota actual. El épsilon impone una
     * pendiente mínima en lo rellenado, así que el D8 posterior siempre encuentra
     * un vecino cuesta abajo y no quedan planos indefinidos.
     *
     * @param H      Relieve de partida, con alturas en cm.
     * @param Filled Cotas de trabajo resultantes; se dimensiona aquí.
     * @note Trabaja en double porque con float el épsilon de 0.01 cm se perdería
     *       por precisión en terrenos grandes.
     */
    void FillDepressionsPriorityFlood(const FField2D& H, TArray<double>& Filled)
    {
        const int32 W = H.Width;
        const int32 Ht = H.Height;
        const int32 N = W * Ht;

        Filled.SetNumUninitialized(N);

        TArray<uint8> Closed;
        Closed.Init(0, N);

        // Heap binario propio, preasignado a N (cada celda entra una sola vez):
        // así GetData() no se invalida y no hay reasignaciones ni shrink por
        // extracción.
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

        // Siembra: todas las celdas del borde conservan su cota y hacen de salida
        // del mapa; el agua abandona el terreno por ahí.
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

        const double Epsilon = 0.01; // cm: pendiente mínima impuesta al rellenar

        while (Count > 0)
        {
            const FPFNode cur = Pop();
            const int32 cx = cur.Idx % W;
            const int32 cy = cur.Idx / W;

            for (int32 k = 0; k < 8; ++k)
            {
                const int32 nx = cx + GD8X[k];
                const int32 ny = cy + GD8Y[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= Ht) continue;

                const int32 n = ny * W + nx;
                if (Closed[n]) continue;
                Closed[n] = 1;

                double z = static_cast<double>(H.Data[n]);
                if (z <= cur.Z) z = cur.Z + Epsilon; // rellena la depresión
                Filled[n] = z;
                Push(z, n);
            }
        }
    }
}

void FWaterField::BakeFromHeightField(const FHeightField& Height, float OutputMax, bool bFillSinks,
    bool bRankNormalize)
{
    if (!Height.IsValid()) return;

    const FField2D& H = Height.Field;
    const int32 W = H.Width;
    const int32 Ht = H.Height;
    const int32 N = W * Ht;

    Field.Init(W, Ht, H.CellSize, H.Origin, 0.f);

    // -----------------------------------------------------------------
    // 0) Cotas de trabajo (double): relieve con las depresiones rellenadas o,
    //    si el rellenado se desactiva, una copia del relieve crudo. Todo lo
    //    que viene después usa Elev y no H.Data.
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
    // 1) Dirección de flujo D8: toda el agua de una celda va al único vecino
    //    de los ocho con mayor caída por unidad de distancia. Tras el
    //    rellenado, toda celda interior tiene vecino cuesta abajo; solo el
    //    borde queda sin salida. Cada celda escribe su propio FlowTo[c], así
    //    que la pasada es paralelizable por filas y determinista.
    // -----------------------------------------------------------------
    TArray<int32> FlowTo;
    FlowTo.Init(-1, N); // -1 = sin salida (borde: el agua deja el mapa)

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
                    const int32 nx = x + GD8X[k];
                    const int32 ny = y + GD8Y[k];
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
    // 2) Orden descendente por cota de trabajo. Procesar de más alto a más
    //    bajo garantiza que, cuando le toca el turno a una celda, ya ha
    //    recibido todo lo que drena hacia ella desde arriba. El desempate por
    //    índice deja el bake reproducible bit a bit.
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
    // 3) Acumulación de flujo: cada celda empieza en 1 (ella misma) y entrega
    //    su acumulado a la celda hacia la que drena. SERIAL: la dependencia
    //    aguas arriba -> aguas abajo impide paralelizarlo.
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
    // 4) TWI crudo = ln(acumulación / tan(pendiente)). La pendiente es la del
    //    propio enlace D8 sobre la cota de trabajo, coherente con la
    //    acumulación. El mínimo evita dividir por cero en zonas planas, que
    //    deben salir húmedas (TWI alto).
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

    // -----------------------------------------------------------------
    // 5) Normalización a [0, OutputMax]: deja el campo listo para la función de
    //    vigor sin reescalados ahí. Destruye la escala física del índice, lo que
    //    es deliberado: agua y nutrientes deben entrar en el vigor con rangos
    //    comparables.
    //
    //    Por defecto se normaliza POR RANGO y no linealmente. La distribución del
    //    TWI es patológica para una normalización lineal: las celdas llanas
    //    cobran ln(1/tan(MinSlopeRad)) ≈ 6,9 unidades solo por ser llanas —tanto
    //    como multiplicar por mil su área drenante— y la salida de la cuenca
    //    mayor, con la pendiente clavada al mínimo y toda la acumulación del
    //    mapa, fija un máximo que ningún otro punto roza. El resultado lineal
    //    deja el 96% del mapa por debajo de 0,3·OutputMax y a las llanuras
    //    aisladas en 0,5-0,7: más lejos de cualquier ladera, en unidades de
    //    anchura de nicho, de lo que ninguna campana de especie puede cubrir, con
    //    lo que ningún árbol germina allí y las llanuras quedan sistemáticamente
    //    vacías. El rango disuelve esa isla conservando la ordenación espacial
    //    exacta, que es lo único que el índice reescalado significa.
    // -----------------------------------------------------------------
    if (bRankNormalize)
    {
        Field.FillRankNormalizedFrom(TwiRaw, OutputMax);
    }
    else
    {
        Field.FillNormalizedFrom(TwiRaw, OutputMax);
    }
}
