#include "Ecology/ResourcePool.h"
#include "Async/ParallelFor.h"

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

    // RECORTAR A CERO **ANTES** DE DIFUNDIR. Next puede traer celdas negativas si
    // el consumo de un arbol supero lo que habia; el recorte final de mas abajo las
    // arregla, pero para entonces esa deuda ya se ha propagado por el Laplaciano a
    // las celdas vecinas, bajandoles recurso REAL. O sea: una demanda excesiva se
    // convertia en una externalidad que perjudicaba a los vecinos sin coste para
    // quien la ejercia, y ademas la masa del campo dejaba de conservarse.
    // (El tope de extraccion del tick evita que aparezcan; esto es el cinturon.)
    for (float& V : Snapshot) { V = FMath::Max(V, 0.f); }

    const float* RESTRICT Src = Snapshot.GetData();
    const float* RESTRICT BaseData = Base.Data.GetData();
    float* RESTRICT Dst = Next.Data.GetData();

    // Paralelo por filas: cada Dst[c] se calcula SOLO a partir de Snapshot y
    // Base (sin dependencia entre celdas de salida), asi que el resultado es
    // bit-identico al serial con cualquier numero de hilos. Es el mismo patron
    // determinista que ya usan los bakes de HeightField/WaterField/NutrientField,
    // y esta era la unica pasada 2D grande por-tick que seguia en serie
    // (512x512 celdas x2 pools cada tick: Profile.RegenMs).
    ParallelFor(H, [&](int32 y)
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
        });
}
