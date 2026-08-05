#include "Ecology/SpatialHash.h"

void FSpatialHash::Init(const FBox2D& WorldBounds, double InCellSize)
{
    CellSize = FMath::Max(InCellSize, 1.0); // evita division por cero si llega un 0 por error
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

    // Counting sort CSR compartido (EcoGrid::BuildCSR: contar, prefijo
    // acumulado, volcar con cursor). Cursor es un MIEMBRO persistente: la
    // asignacion interna reutiliza la capacidad de ticks anteriores en vez de
    // pedir ~168 KB al heap cada vez (optimizacion C5).
    EcoGrid::BuildCSR(GridW * GridH, Num,
        [this, &Pos](int32 i) { return CellOf(Pos[i]); },
        CellStart, SortedIdx, Cursor);
}
