#include "Ecology/EcologyRules.h"

void EcologyRules::DepositKernel(const FField2D& Geometry, TArray<float>& Deltas,
    const FVector& WorldPos, float RadiusCm, float TotalAmount)
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

    // --- Pasada 1: peso crudo de cada celda dentro del radio (kernel lineal: 1 en el centro, 0 en el borde) ---
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
        Deltas[Iy * Geometry.Width + Ix] += TotalAmount;
        return;
    }

    // --- Pasada 2: repartir TotalAmount proporcional al peso, normalizado por WeightSum ---
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

            Deltas[Iy * Geometry.Width + Ix] += TotalAmount * (W / WeightSum);
        }
    }
}

void EcologyRules::ReduceScratchInto(const TArray<FTickScratch>& Contexts,
    TArray<float>& DestWater, TArray<float>& DestNutrient,
    TArray<FPendingSeed>& OutSeeds, TArray<FPendingDeathPulse>& OutDeathPulses)
{
    OutSeeds.Reset();
    OutDeathPulses.Reset();

    // Orden de indice creciente: FIJO, no depende de que hilo fisico ejecuto
    // cada tarea ni de cuantos nucleos tiene la maquina.
    for (const FTickScratch& Ctx : Contexts)
    {
        check(Ctx.WaterDeltas.Num() == DestWater.Num());
        check(Ctx.NutrientDeltas.Num() == DestNutrient.Num());

        for (int32 c = 0; c < DestWater.Num(); ++c)
        {
            DestWater[c] += Ctx.WaterDeltas[c];
        }
        for (int32 c = 0; c < DestNutrient.Num(); ++c)
        {
            DestNutrient[c] += Ctx.NutrientDeltas[c];
        }

        OutSeeds.Append(Ctx.Seeds);
        OutDeathPulses.Append(Ctx.DeathPulses);
    }
}