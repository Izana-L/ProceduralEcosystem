#include "Ecology/EcologyRules.h"

namespace
{
    /**
     * Nucleo del kernel radial, compartido por la version densa y la dispersa
     * (asi no hay dos copias de la misma matematica que puedan divergir, y sobre
     * todo: ambas recorren las celdas en EXACTAMENTE el mismo orden, que es lo
     * que sostiene el determinismo de la reduccion).
     *
     * Dos pasadas:
     *   1. Suma los pesos crudos de las celdas dentro del radio (kernel lineal:
     *      1 en el centro, 0 en el borde) y los CACHEA en un buffer local.
     *   2. Reparte TotalAmount proporcional al peso NORMALIZADO por esa suma, de
     *      modo que lo depositado sume exactamente TotalAmount pase lo que pase
     *      con el redondeo a celdas. Reutiliza los pesos de la pasada 1: el peso
     *      lineal necesita la distancia real (un Sqrt por celda) y esto corre por
     *      cada arbol, cada campo y cada tick dentro del ParallelFor caliente;
     *      recomputarlo duplicaba todos esos Sqrt.
     *
     * Sink se invoca como Sink(int32 CellIndex, float Amount).
     */
    template <typename FSink>
    void ForEachKernelCell(const FField2D& Geometry, const FVector& WorldPos,
        float RadiusCm, float TotalAmount, FSink&& Sink)
    {
        if (RadiusCm <= 0.f || TotalAmount == 0.f || !Geometry.IsValid())
        {
            return;
        }

        double Gx, Gy;
        Geometry.WorldToGrid(WorldPos.X, WorldPos.Y, Gx, Gy);
        const int32 Cx = FMath::FloorToInt(Gx);
        const int32 Cy = FMath::FloorToInt(Gy);
        const int32 CellRadius = FMath::CeilToInt(RadiusCm / Geometry.CellSize);

        // Bloque de celdas afectado, RECORTADO una sola vez a la rejilla. Antes
        // el doble bucle -con su test de limites por celda- estaba escrito dos
        // veces, una por pasada, y el comentario de la pasada 2 tenia que pedir
        // "mismo recorrido" a mano: si alguien tocaba uno y no el otro, el orden
        // de emision dejaba de coincidir y con el se iba el determinismo de la
        // reduccion. Ahora hay UNA sola definicion del recorrido y las dos
        // pasadas la comparten por construccion.
        const int32 X0 = FMath::Max(Cx - CellRadius, 0), X1 = FMath::Min(Cx + CellRadius, Geometry.Width - 1);
        const int32 Y0 = FMath::Max(Cy - CellRadius, 0), Y1 = FMath::Min(Cy + CellRadius, Geometry.Height - 1);

        auto ForEachCellInDisc = [&](auto&& CellFn)
            {
                for (int32 Iy = Y0; Iy <= Y1; ++Iy)
                {
                    const int32 dy = Iy - Cy;
                    const int32 RowBase = Iy * Geometry.Width;
                    for (int32 Ix = X0; Ix <= X1; ++Ix)
                    {
                        CellFn(Ix - Cx, dy, RowBase + Ix);
                    }
                }
            };

        // Pesos de las celdas visitadas, en orden de visita. Inline: el radio
        // efectivo por defecto es ~1 celda (bloque 3x3); 64 cubre hasta 7x7 sin
        // tocar el heap, y con radios mayores simplemente reserva.
        TArray<float, TInlineAllocator<64>> Weights;

        // --- Pasada 1: peso crudo de cada celda dentro del radio (cacheado) ---
        float WeightSum = 0.f;
        ForEachCellInDisc([&](int32 dx, int32 dy, int32 /*Cell*/)
            {
                const double CellDistCm = FVector2D(dx, dy).Size() * Geometry.CellSize;
                const float W = FMath::Max(0.f, 1.f - static_cast<float>(CellDistCm / RadiusCm));
                Weights.Add(W);
                WeightSum += W;
            });

        if (WeightSum <= KINDA_SMALL_NUMBER)
        {
            // El radio es menor que una celda o el punto cae justo en el borde
            // del mundo: en vez de perder masa, deposita todo en la celda mas
            // cercana (clamp a los bordes de la rejilla).
            const int32 Ix = FMath::Clamp(Cx, 0, Geometry.Width - 1);
            const int32 Iy = FMath::Clamp(Cy, 0, Geometry.Height - 1);
            Sink(Iy * Geometry.Width + Ix, TotalAmount);
            return;
        }

        // --- Pasada 2: repartir TotalAmount con los pesos ya calculados ---
        // Mismo recorrido (y por tanto mismo orden de emision, que sostiene el
        // determinismo de la reduccion), sin recomputar ninguna distancia.
        const float InvWeightSum = 1.f / WeightSum;
        int32 WeightIdx = 0;
        ForEachCellInDisc([&](int32 /*dx*/, int32 /*dy*/, int32 Cell)
            {
                const float W = Weights[WeightIdx++];
                if (W <= 0.f) { return; }

                Sink(Cell, TotalAmount * (W * InvWeightSum));
            });
    }
}

void EcologyRules::DepositKernel(const FField2D& Geometry, TArray<float>& Deltas,
    const FVector& WorldPos, float RadiusCm, float TotalAmount)
{
    ForEachKernelCell(Geometry, WorldPos, RadiusCm, TotalAmount,
        [&Deltas](int32 Cell, float Amount)
        {
            if (Deltas.IsValidIndex(Cell)) { Deltas[Cell] += Amount; }
        });
}

void EcologyRules::DepositKernelSparse(const FField2D& Geometry, TArray<FCellDelta>& OutDeltas,
    const FVector& WorldPos, float RadiusCm, float TotalAmount)
{
    ForEachKernelCell(Geometry, WorldPos, RadiusCm, TotalAmount,
        [&OutDeltas](int32 Cell, float Amount)
        {
            OutDeltas.Add(FCellDelta{ Cell, Amount });
        });
}

void EcologyRules::ReduceScratchInto(const TArray<FTickScratch>& Contexts,
    TArray<float>& DestWater, TArray<float>& DestNutrient,
    TArray<FPendingSeed>& OutSeeds, TArray<FPendingDeathPulse>& OutDeathPulses)
{
    OutSeeds.Reset();
    OutDeathPulses.Reset();

    // Orden de indice creciente: FIJO, no depende de que hilo fisico ejecuto
    // cada tarea ni de cuantos nucleos tiene la maquina. Y dentro de cada tarea,
    // orden de insercion (arbol por indice creciente, celda por el orden del
    // kernel). Suma en float no asociativa + orden fijo = bit a bit reproducible.
    for (const FTickScratch& Ctx : Contexts)
    {
        for (const FCellDelta& D : Ctx.WaterDeltas)
        {
            checkSlow(DestWater.IsValidIndex(D.Cell));
            DestWater[D.Cell] += D.Amount;
        }
        for (const FCellDelta& D : Ctx.NutrientDeltas)
        {
            checkSlow(DestNutrient.IsValidIndex(D.Cell));
            DestNutrient[D.Cell] += D.Amount;
        }

        OutSeeds.Append(Ctx.Seeds);
        OutDeathPulses.Append(Ctx.DeathPulses);
    }
}
