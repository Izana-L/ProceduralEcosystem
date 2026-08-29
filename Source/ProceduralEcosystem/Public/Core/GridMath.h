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

    /**
     * Pone a cero un buffer de floats de rejilla con un memset.
     *
     * Los dos grids de luz tenian su propio ClearShadow y NO hacian lo mismo: el
     * grueso ya usaba Memzero, el fino recorria el array celda a celda. Y el fino
     * es el que se limpia MAS veces (una por refresco de luz del SCA, o sea
     * varias por arbol horneado), asi que la version lenta estaba justo donde
     * mas dolia. Una sola copia, la rapida, para los dos.
     */
    FORCEINLINE void ZeroFloats(TArray<float>& Buffer)
    {
        if (Buffer.Num() > 0)
        {
            FMemory::Memzero(Buffer.GetData(), Buffer.Num() * sizeof(float));
        }
    }

    /**
     * Luz disponible tras la sombra acumulada: Q = clamp(FullSun - Sombra, 0).
     *
     * Resta LINEAL con tope duro. La sigue usando el grid FINO (local a un hero
     * tree), donde "sombra" es una oclusion geometrica acotada por construccion y
     * lo que se busca es un gradiente barato para el fototropismo.
     *
     * NO la uses para el dosel a escala de paisaje: ver LightFromExtinction.
     */
    FORCEINLINE float LightFromShadow(float Shadow, float FullSun)
    {
        return FMath::Max(FullSun - Shadow, 0.f);
    }

    /**
     * Ley de Beer-Lambert: Q = DiffuseFloor + (FullSun - DiffuseFloor) * exp(-k * LAI).
     *
     * POR QUE NO VALE LA RESTA LINEAL PARA EL DOSEL. Con Q = max(FullSun - S, 0),
     * en cuanto la sombra acumulada supera FullSun la luz se clava en CERO exacto
     * y se queda ahi. Y con Q = 0 el factor de luz de todas las especies vale
     * tambien 0 (f_L = 0/(0+Kl)), o sea que la tolerante a la sombra pierde su
     * ventaja PRECISAMENTE en la sombra profunda, que es el unico sitio donde
     * deberia ganar. El unico eje de sucesion del modelo se apaga justo donde
     * tiene que actuar.
     *
     * La exponencial es asintotica: nunca llega a 0, asi que el ORDEN entre
     * especies se conserva a cualquier densidad de dosel. Ademas es la ley fisica
     * real de atenuacion a traves de un medio absorbente, con LAI (indice de area
     * foliar acumulado por encima del punto) como espesor optico y k como
     * coeficiente de extincion (~0.5 en hoja ancha).
     *
     * DiffuseFloor es la luz difusa del cielo que llega al sotobosque incluso bajo
     * dosel cerrado (medida real: 1-5% de la luz de fuera). Sin ese suelo, un
     * dosel muy denso volveria a producir el cero absoluto que la exponencial
     * venia a evitar.
     */
    FORCEINLINE float LightFromExtinction(float LeafAreaAbove, float FullSun, float ExtinctionK, float DiffuseFloor)
    {
        const float Floor = FMath::Clamp(DiffuseFloor, 0.f, FullSun);
        const float Transmitted = FMath::Exp(-FMath::Max(ExtinctionK, 0.f) * FMath::Max(LeafAreaAbove, 0.f));
        return Floor + (FullSun - Floor) * Transmitted;
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
     * Recorre las celdas de un bloque de (2R+1)^3 centrado en (Cx,Cy,Cz),
     * RECORTADO a la rejilla WxHxD, e invoca Fn(int32 CellIndex) en cada una.
     *
     * UNICA copia del triple bucle clampado que antes estaba escrito -con
     * nombres distintos y una dimension de diferencia- en FSpatialHash::
     * ForEachNeighbor (2D) y FAttractorCloud::ForEachInRange (3D). Sirve para
     * las dos: una rejilla 2D es este mismo recorrido con D = 1 y Cz = 0, y en
     * ese caso el bucle de Z da exactamente una vuelta.
     *
     * De paso es MAS RAPIDO que las dos versiones que sustituye: aquellas
     * iteraban -R..R en cada eje y descartaban dentro con un `if` por celda; aqui
     * los limites se recortan UNA vez por eje y el cuerpo del bucle no vuelve a
     * comprobar nada.
     *
     * ORDEN DE VISITA: z, luego y, luego x, todos ascendentes -el mismo de
     * antes-. Importa: las consultas de vecindad alimentan decisiones de la
     * simulacion y del SCA, y el orden fijo es parte del contrato de
     * determinismo.
     */
    template <typename FCellFn>
    void ForEachCellInBox(int32 Cx, int32 Cy, int32 Cz, int32 R,
        int32 Width, int32 Height, int32 Depth, FCellFn&& Fn)
    {
        const int32 X0 = FMath::Max(Cx - R, 0), X1 = FMath::Min(Cx + R, Width - 1);
        const int32 Y0 = FMath::Max(Cy - R, 0), Y1 = FMath::Min(Cy + R, Height - 1);
        const int32 Z0 = FMath::Max(Cz - R, 0), Z1 = FMath::Min(Cz + R, Depth - 1);

        for (int32 Iz = Z0; Iz <= Z1; ++Iz)
        {
            for (int32 Iy = Y0; Iy <= Y1; ++Iy)
            {
                const int32 RowBase = (Iz * Height + Iy) * Width;
                for (int32 Ix = X0; Ix <= X1; ++Ix)
                {
                    Fn(RowBase + Ix);
                }
            }
        }
    }

    /**
     * Igual, pero desenrollando ya el indice CSR: invoca Fn(int32 ItemIndex) por
     * cada item guardado en esas celdas. Es literalmente el cuerpo que tenian
     * FSpatialHash::ForEachNeighbor y FAttractorCloud::ForEachInRange, y ahora
     * las dos se reducen a calcular su celda central y llamar aqui.
     *
     * Incluye TODOS los items de las celdas tocadas (el bloque es un cubo, no
     * una esfera): el filtrado fino por distancia real lo hace el llamador, que
     * es lo barato.
     */
    template <typename FItemFn>
    void ForEachItemInBox(const TArray<int32>& CellStart, const TArray<int32>& SortedIdx,
        int32 Cx, int32 Cy, int32 Cz, int32 R,
        int32 Width, int32 Height, int32 Depth, FItemFn&& Fn)
    {
        ForEachCellInBox(Cx, Cy, Cz, R, Width, Height, Depth,
            [&CellStart, &SortedIdx, &Fn](int32 Cell)
            {
                for (int32 K = CellStart[Cell]; K < CellStart[Cell + 1]; ++K)
                {
                    Fn(SortedIdx[K]);
                }
            });
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
