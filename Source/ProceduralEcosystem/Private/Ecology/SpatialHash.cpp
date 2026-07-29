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

    const int32 NumCells = GridW * GridH;
    // SetNumZeroed solo cera los elementos NUEVOS: si el array ya tiene
    // NumCells+1 elementos (todo tick despues del primero), conservaria los
    // prefijos del tick anterior. Reset() mantiene la capacidad, asi que el
    // SetNumZeroed siguiente re-cera todo el rango sin realojar.
    CellStart.Reset();
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
    // Es un MIEMBRO persistente: la asignacion reutiliza la capacidad de ticks
    // anteriores en vez de pedir ~168 KB al heap cada vez (optimizacion C5).
    Cursor = CellStart;
    SortedIdx.SetNumUninitialized(Num, EAllowShrinking::No);
    for (int32 i = 0; i < Num; ++i)
    {
        const int32 c = CellOf(Pos[i]);
        SortedIdx[Cursor[c]++] = i;
    }
}
