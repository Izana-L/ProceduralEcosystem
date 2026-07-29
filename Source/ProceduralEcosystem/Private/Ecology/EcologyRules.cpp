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
     *      1 en el centro, 0 en el borde).
     *   2. Reparte TotalAmount proporcional al peso NORMALIZADO por esa suma, de
     *      modo que lo depositado sume exactamente TotalAmount pase lo que pase
     *      con el redondeo a celdas.
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

        // --- Pasada 1: peso crudo de cada celda dentro del radio ---
        float WeightSum = 0.f;
        for (int32 dy = -CellRadius; dy <= CellRadius; ++dy)
        {
            const int32 Iy = Cy + dy;
            if (Iy < 0 || Iy >= Geometry.Height) continue;

            for (int32 dx = -CellRadius; dx <= CellRadius; ++dx)
            {
                const int32 Ix = Cx + dx;
                if (Ix < 0 || Ix >= Geometry.Width) continue;

                const double CellDistCm = FVector2D(dx, dy).Size() * Geometry.CellSize;
                WeightSum += FMath::Max(0.f, 1.f - static_cast<float>(CellDistCm / RadiusCm));
            }
        }

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

        // --- Pasada 2: repartir TotalAmount proporcional al peso normalizado ---
        const float InvWeightSum = 1.f / WeightSum;
        for (int32 dy = -CellRadius; dy <= CellRadius; ++dy)
        {
            const int32 Iy = Cy + dy;
            if (Iy < 0 || Iy >= Geometry.Height) continue;

            for (int32 dx = -CellRadius; dx <= CellRadius; ++dx)
            {
                const int32 Ix = Cx + dx;
                if (Ix < 0 || Ix >= Geometry.Width) continue;

                const double CellDistCm = FVector2D(dx, dy).Size() * Geometry.CellSize;
                const float W = FMath::Max(0.f, 1.f - static_cast<float>(CellDistCm / RadiusCm));
                if (W <= 0.f) continue;

                Sink(Iy * Geometry.Width + Ix, TotalAmount * (W * InvWeightSum));
            }
        }
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
