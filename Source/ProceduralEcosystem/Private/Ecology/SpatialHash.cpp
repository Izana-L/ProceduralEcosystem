#include "Ecology/SpatialHash.h"

void FSpatialHash::Init(const FBox2D& WorldBounds, double InCellSize)
{
    CellSize = FMath::Max(InCellSize, 1.0); // evita división por cero si llega un 0 por error
    Origin = WorldBounds.Min;

    const FVector2D Size = WorldBounds.Max - WorldBounds.Min;
    GridW = FMath::Max(1, FMath::CeilToInt(Size.X / CellSize));
    GridH = FMath::Max(1, FMath::CeilToInt(Size.Y / CellSize));

    CellStart.Reset();
    SortedIdx.Reset();
}

void FSpatialHash::Build(const TArray<FVector>& Pos, int32 Num)
{
    check(Num <= Pos.Num());

    const int32 NumCells = GridW * GridH;
    CellStart.SetNumZeroed(NumCells + 1);

    // Pasada 1: contar cuantos agentes caen en cada celda (histograma).
    for (int32 i = 0; i < Num; ++i)
    {
        ++CellStart[CellOf(Pos[i]) + 1];
    }

    // Pasada 2: prefijo acumulado -> CellStart[c] queda como el offset de
    // arranque de la celda c en SortedIdx (patron CSR estandar).
    for (int32 c = 0; c < NumCells; ++c)
    {
        CellStart[c + 1] += CellStart[c];
    }

    // Pasada 3: volcar. Cursor es una copia editable de los offsets; se usa
    // como "siguiente hueco libre" de cada celda y se va incrementando.
    TArray<int32> Cursor = CellStart;
    SortedIdx.SetNumUninitialized(Num);
    for (int32 i = 0; i < Num; ++i)
    {
        const int32 c = CellOf(Pos[i]);
        SortedIdx[Cursor[c]++] = i;
    }
}