/**
 * @file ResourcePool.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de la regeneración de un pool de recurso: recarga y difusión.
 *
 * Contiene la única operación no inline de FResourcePool. Sobre el buffer de escritura,
 * que ya trae el consumo del tick descontado, aplica dos términos por celda: una
 * relajación hacia el campo base, que modela la meteorización del terreno, y un Laplaciano
 * discreto a cuatro vecinos que redistribuye el recurso entre celdas contiguas. Ambos se
 * calculan a partir de una copia inmutable del buffer, lo que permite repartir las filas
 * en paralelo obteniendo un resultado idéntico al serial.
 *
 * @ingroup eco_ecology
 * @see @ref bib_leveque2007
 */

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

    // Snapshot es miembro y no local: sobre un array del mismo tamaño, operator= de TArray
    // es un memcpy que reutiliza la capacidad y no toca el heap.
    Snapshot = Next.Data;

    // El recorte a cero va ANTES de difundir. Next puede traer celdas negativas si el
    // consumo de un árbol superó lo que había; el recorte final de más abajo las arregla,
    // pero para entonces esa deuda ya se habría propagado por el Laplaciano a las celdas
    // vecinas, bajándoles recurso real: una demanda excesiva se convertiría en una
    // externalidad sin coste para quien la ejerce y la masa del campo dejaría de
    // conservarse. El tope de extracción del tick evita que aparezcan; esto es el cinturón.
    for (float& V : Snapshot) { V = FMath::Max(V, 0.f); }

    const float* RESTRICT Src = Snapshot.GetData();
    const float* RESTRICT BaseData = Base.Data.GetData();
    float* RESTRICT Dst = Next.Data.GetData();

    // Paralelo por filas: cada Dst[c] sale solo de Snapshot y Base, sin dependencia entre
    // celdas de salida, así que el resultado es bit-idéntico al serial con cualquier
    // número de hilos. Es el mismo patrón determinista que usan los bakes de los campos
    // del terreno.
    ParallelFor(H, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                const int32 c = y * W + x;

                // Relajación hacia el potencial del terreno: la meteorización repone.
                const float Recharge = RechargeRate * (BaseData[c] - Src[c]) * DtYears;

                // Laplaciano discreto promediado entre los vecinos válidos: en bordes y
                // esquinas son 3 o 2, no hay wrap-around al lado opuesto del campo.
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
