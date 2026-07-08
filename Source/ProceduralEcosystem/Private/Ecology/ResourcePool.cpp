#include "Ecology/ResourcePool.h"

void FResourcePool::RegenerateTowardBase(const FField2D& Base, float RechargeRate, float DiffusionRate, float DtYears)
{
    const TArray<float> Snapshot = Next.Data;
    const int32 W = Next.Width;
    const int32 H = Next.Height;

    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < W; ++x)
        {
            const int32 c = y * W + x;

            // Recarga lenta hacia el mapa base (meteorizacion).
            const float Recharge = RechargeRate * (Base.Data[c] - Snapshot[c]) * DtYears;

            // Difusion: Laplaciano discreto sobre los vecinos validos (bordes clampados, no wrap-around).
            float Lap = 0.f;
            int32 NeighborCount = 0;
            if (x > 0) { Lap += Snapshot[c - 1] - Snapshot[c]; ++NeighborCount; }
            if (x < W - 1) { Lap += Snapshot[c + 1] - Snapshot[c]; ++NeighborCount; }
            if (y > 0) { Lap += Snapshot[c - W] - Snapshot[c]; ++NeighborCount; }
            if (y < H - 1) { Lap += Snapshot[c + W] - Snapshot[c]; ++NeighborCount; }
            const float Diffusion = (NeighborCount > 0) ? DiffusionRate * (Lap / NeighborCount) * DtYears : 0.f;

            Next.Data[c] = FMath::Max(0.f, Snapshot[c] + Recharge + Diffusion);
        }
    }
}