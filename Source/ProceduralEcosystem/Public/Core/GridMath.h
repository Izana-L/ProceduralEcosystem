#pragma once

#include "CoreMinimal.h"

/**
 * Helpers de rejilla compartidos por TODAS las estructuras espaciales del
 * proyecto (FField2D, FSpatialHash, FLightFieldCoarse, FTreeLightGridFine,
 * FAttractorCloud).
 *
 * Antes cada una reimplementaba la misma aritmetica -mundo -> celda con floor y
 * clamp, indexado lineal 3D, dimensionado desde una caja envolvente, counting
 * sort CSR- y cualquier cambio habia que replicarlo en cinco sitios. Aqui vive
 * la UNICA copia; las estructuras solo aportan su geometria (Origin, CellSize,
 * dimensiones).
 */
namespace EcoGrid
{
    /** Luz plena normalizada (cielo despejado). La comparten los dos grids de
        sombra por voxel (coarse y fine): un solo valor, imposible que diverjan. */
    constexpr float FullSunlight = 1.f;

    /** Mundo -> coordenada de rejilla FRACCIONAL (en muestras). */
    FORCEINLINE double ToGridCoord(double Coord, double Origin, double CellSize)
    {
        return (Coord - Origin) / CellSize;
    }

    /** Mundo -> indice de celda (floor), SIN clamp. */
    FORCEINLINE int32 WorldToCell(double Coord, double Origin, double CellSize)
    {
        return FMath::FloorToInt32((Coord - Origin) / CellSize);
    }

    /** Mundo -> indice de celda con clamp a [0, NumCells-1]. */
    FORCEINLINE int32 WorldToCellClamped(double Coord, double Origin, double CellSize, int32 NumCells)
    {
        return FMath::Clamp(FMath::FloorToInt32((Coord - Origin) / CellSize), 0, NumCells - 1);
    }

    /** Indice lineal de un voxel (Ix,Iy,Iz) en una rejilla Width x Height x Layers. */
    FORCEINLINE int32 VoxelIndex(int32 Ix, int32 Iy, int32 Iz, int32 Width, int32 Height)
    {
        return (Iz * Height + Iy) * Width + Ix;
    }

    /** Luz disponible tras la sombra acumulada: Q = clamp(FullSun - Sombra, 0). */
    FORCEINLINE float LightFromShadow(float Shadow, float FullSun)
    {
        return FMath::Max(FullSun - Shadow, 0.f);
    }

    /**
     * Dimensiona una rejilla 3D que cubra Bounds con celdas de CellSize, con al
     * menos 1 celda por eje y un tope defensivo MaxPerAxis (evita reservar gigas
     * si llega una caja degenerada o enorme).
     */
    FORCEINLINE void DimensionsFromBounds(const FBox& Bounds, double CellSize, int32 MaxPerAxis,
        int32& OutW, int32& OutH, int32& OutD)
    {
        const FVector Size = (Bounds.Max - Bounds.Min).ComponentMax(FVector(CellSize));
        OutW = FMath::Clamp(FMath::CeilToInt32(Size.X / CellSize), 1, MaxPerAxis);
        OutH = FMath::Clamp(FMath::CeilToInt32(Size.Y / CellSize), 1, MaxPerAxis);
        OutD = FMath::Clamp(FMath::CeilToInt32(Size.Z / CellSize), 1, MaxPerAxis);
    }

    /**
     * Indice espacial CSR por counting sort (contar por celda, prefijo
     * acumulado, volcar con cursor). O(NumItems), orden fijo: recorre los items
     * en indice creciente, asi que dentro de cada celda SortedIdx queda siempre
     * en el mismo orden -> determinista.
     *
     * CellOfItem se invoca como CellOfItem(int32 ItemIndex) -> int32 celda.
     * Cursor es un buffer de trabajo del llamador (persistente si quiere evitar
     * la allocation por tick, como hace FSpatialHash).
     */
    template <typename FCellOf>
    void BuildCSR(int32 NumCells, int32 NumItems, FCellOf&& CellOfItem,
        TArray<int32>& CellStart, TArray<int32>& SortedIdx, TArray<int32>& Cursor)
    {
        // Reset + SetNumZeroed: SetNumZeroed solo cera los elementos NUEVOS, asi
        // que sin el Reset conservaria los prefijos de la pasada anterior.
        CellStart.Reset();
        CellStart.SetNumZeroed(NumCells + 1);

        for (int32 i = 0; i < NumItems; ++i)
        {
            ++CellStart[CellOfItem(i) + 1];
        }
        for (int32 c = 0; c < NumCells; ++c)
        {
            CellStart[c + 1] += CellStart[c];
        }

        Cursor = CellStart;
        SortedIdx.SetNumUninitialized(NumItems, EAllowShrinking::No);
        for (int32 i = 0; i < NumItems; ++i)
        {
            SortedIdx[Cursor[CellOfItem(i)]++] = i;
        }
    }
}
