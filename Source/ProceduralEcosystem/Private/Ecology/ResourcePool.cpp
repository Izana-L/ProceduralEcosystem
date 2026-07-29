#include "Ecology/ResourcePool.h"

void FResourcePool::RegenerateTowardBase(const FField2D& Base, float RechargeRate, float DiffusionRate, float DtYears)
{
    const int32 W = Next.Width;
    const int32 H = Next.Height;
    if (W <= 0 || H <= 0 || Next.Data.Num() != W * H || Base.Data.Num() != Next.Data.Num())
    {
        return;
    }

    // Snapshot es MIEMBRO, no local: la asignacion reutiliza la capacidad de los
    // ticks anteriores (ver nota de C5 en la cabecera). Con TArray, operator= a
    // un array del mismo tamano es un memcpy sin tocar el heap.
    Snapshot = Next.Data;

    const float* RESTRICT Src = Snapshot.GetData();
    const float* RESTRICT BaseData = Base.Data.GetData();
    float* RESTRICT Dst = Next.Data.GetData();

    for (int32 y = 0; y < H; ++y)
    {
        for (int32 x = 0; x < W; ++x)
        {
            const int32 c = y * W + x;

            // Recarga lenta hacia el mapa base (meteorizacion).
            const float Recharge = RechargeRate * (BaseData[c] - Src[c]) * DtYears;

            // Difusion: Laplaciano discreto sobre los vecinos validos (bordes clampados, no wrap-around).
            float Lap = 0.f;
            int32 NeighborCount = 0;
            if (x > 0) { Lap += Src[c - 1] - Src[c]; ++NeighborCount; }
            if (x < W - 1) { Lap += Src[c + 1] - Src[c]; ++NeighborCount; }
            if (y > 0) { Lap += Src[c - W] - Src[c]; ++NeighborCount; }
            if (y < H - 1) { Lap += Src[c + W] - Src[c]; ++NeighborCount; }
            const float Diffusion = (NeighborCount > 0) ? DiffusionRate * (Lap / NeighborCount) * DtYears : 0.f;

            Dst[c] = FMath::Max(0.f, Src[c] + Recharge + Diffusion);
        }
    }
}
