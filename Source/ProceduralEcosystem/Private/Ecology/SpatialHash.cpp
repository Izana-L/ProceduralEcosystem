/**
 * @file SpatialHash.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de FSpatialHash: dimensionado de la rejilla y construcción CSR.
 *
 * Contiene las dos operaciones no inline del índice espacial: Init, que deriva el número
 * de celdas de los límites del mundo y el lado de celda, y Build, que delega el counting
 * sort en la rutina compartida EcoGrid::BuildCSR pasándole el cursor persistente como
 * buffer de trabajo. Las consultas de vecindad viven en la cabecera, por ser plantilla.
 *
 * @ingroup eco_ecology
 */

#include "Ecology/SpatialHash.h"

void FSpatialHash::Init(const FBox2D& WorldBounds, double InCellSize)
{
    CellSize = FMath::Max(InCellSize, 1.0); // divisor de todo el mapeo mundo-celda: nunca 0
    Origin = WorldBounds.Min;

    const FVector2D Size = WorldBounds.Max - WorldBounds.Min;
    GridW = FMath::Max(1, FMath::CeilToInt32(Size.X / CellSize));
    GridH = FMath::Max(1, FMath::CeilToInt32(Size.Y / CellSize));

    CellStart.Reset();
    SortedIdx.Reset();
    Cursor.Reset();
}

void FSpatialHash::Build(const TArray<FVector>& Pos, int32 Num)
{
    check(Num <= Pos.Num());

    // Counting sort CSR compartido: contar por celda, prefijo acumulado y volcado con
    // cursor. Cursor se pasa desde el miembro persistente para que la reconstrucción por
    // tick reutilice su capacidad en vez de volver a pedirla al heap.
    EcoGrid::BuildCSR(GridW * GridH, Num,
        [this, &Pos](int32 i) { return CellOf(Pos[i]); },
        CellStart, SortedIdx, Cursor);
}
