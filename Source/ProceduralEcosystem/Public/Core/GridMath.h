/**
 * @file GridMath.h
 * @author Juan Luque Roldán
 * @brief Aritmética de rejilla compartida por las estructuras espaciales del proyecto:
 *        indexado, dimensionado, índice CSR, vecindad y leyes de luz.
 *
 * Copia única de la aritmética que comparten `FField2D`, `FSpatialHash`,
 * `FLightFieldCoarse`, `FTreeLightGridFine` y `FAttractorCloud`: conversión de mundo a
 * celda con y sin clamp, índice lineal de vóxel, dimensionado defensivo desde una caja
 * envolvente, construcción del índice espacial CSR por counting sort y recorrido del
 * bloque de vecindad. Las estructuras solo aportan su geometría (origen, tamaño de
 * celda y dimensiones), con lo que no pueden divergir ni en el layout de memoria ni en
 * el orden de visita, ambos parte del contrato de determinismo. Aquí viven también las
 * dos leyes de luz del proyecto, para que compartan el mismo techo de luz plena.
 *
 * @ingroup eco_core
 * @see @ref bib_teschner2003
 */

#pragma once

#include "CoreMinimal.h"

/**
 * @brief Aritmética de rejilla uniforme: conversiones mundo-celda, indexado lineal,
 *        índice espacial CSR, recorrido de vecindad y atenuación de la luz.
 *
 * Todas las funciones son puras y sin estado. El índice lineal canónico es
 * @f$(I_z \cdot Height + I_y) \cdot Width + I_x@f$, con X como eje de variación más
 * rápida; ese layout es el que asumen el recorrido por filas y el borrado por memset.
 */
namespace EcoGrid
{
    /** Luz plena normalizada (cielo despejado). Techo común de las dos rejillas de luz,
        la gruesa y la fina, para que no puedan divergir. No es una irradiancia
        física. */
    constexpr float FullSunlight = 1.f;

    /** Mundo -> coordenada de rejilla fraccional, en unidades de celda; base de las
        interpolaciones bilineal y trilineal. */
    FORCEINLINE double ToGridCoord(double Coord, double Origin, double CellSize)
    {
        return (Coord - Origin) / CellSize;
    }

    /** Mundo -> índice de celda por floor, SIN clamp: el llamador decide qué hacer con
        los puntos que caen fuera de la rejilla. */
    FORCEINLINE int32 WorldToCell(double Coord, double Origin, double CellSize)
    {
        return FMath::FloorToInt32((Coord - Origin) / CellSize);
    }

    /** Mundo -> índice de celda con clamp a [0, NumCells-1]: los puntos de fuera se
        pegan al borde en lugar de indexar fuera de rango. */
    FORCEINLINE int32 WorldToCellClamped(double Coord, double Origin, double CellSize, int32 NumCells)
    {
        return FMath::Clamp(FMath::FloorToInt32((Coord - Origin) / CellSize), 0, NumCells - 1);
    }

    /** Índice lineal del vóxel (Ix, Iy, Iz) en una rejilla de Width x Height celdas por
        capa. X es el eje de variación más rápida. */
    FORCEINLINE int32 VoxelIndex(int32 Ix, int32 Iy, int32 Iz, int32 Width, int32 Height)
    {
        return (Iz * Height + Iy) * Width + Ix;
    }

    /**
     * Pone a cero un buffer de floats de rejilla con un único memset.
     *
     * Es el borrado que usan las dos rejillas de luz. Importa que sea el rápido porque la
     * rejilla fina se limpia varias veces por árbol horneado, una por cada refresco de luz
     * del SCA.
     */
    FORCEINLINE void ZeroFloats(TArray<float>& Buffer)
    {
        if (Buffer.Num() > 0)
        {
            FMemory::Memzero(Buffer.GetData(), Buffer.Num() * sizeof(float));
        }
    }

    /**
     * Luz disponible tras la sombra acumulada, por resta lineal con tope duro:
     * @f$Q = \max(FullSun - Shadow, 0)@f$.
     *
     * Es la ley de la rejilla fina, local a un hero tree, donde la sombra es una oclusión
     * geométrica acotada por construcción y lo que se busca es un gradiente barato para
     * el fototropismo del SCA, no una ley física.
     *
     * @warning No sirve para el dosel a escala de paisaje: satura en cero exacto.
     *          Ahí se usa @ref EcoGrid::LightFromExtinction.
     */
    FORCEINLINE float LightFromShadow(float Shadow, float FullSun)
    {
        return FMath::Max(FullSun - Shadow, 0.f);
    }

    /**
     * Luz bajo el dosel por la ley de Beer-Lambert con suelo difuso:
     * @f$Q = Floor + (FullSun - Floor)\,e^{-k\,LAI}@f$. Es la ley de la rejilla gruesa, a
     * escala de paisaje.
     *
     * La resta lineal no sirve aquí: en cuanto la sombra acumulada supera la luz plena,
     * @f$Q@f$ se clava en cero exacto, y con @f$Q = 0@f$ el factor de luz se anula para
     * TODAS las especies (@ref EcoVigor::LightFactor), así que la tolerante a la sombra
     * perdería su ventaja precisamente en la sombra profunda, el único sitio donde debe
     * ganar, y el eje de sucesión del modelo se apagaría donde tiene que actuar. La
     * exponencial es asintótica, nunca alcanza el cero, y conserva el orden entre
     * especies a cualquier densidad de dosel.
     *
     * @param LeafAreaAbove Área foliar acumulada por encima del punto, que actúa como
     *                      espesor óptico. Los valores negativos se tratan como 0.
     * @param ExtinctionK   Coeficiente de extinción del follaje; ~0.5 en hoja ancha.
     * @param DiffuseFloor  Luz difusa que llega al sotobosque bajo dosel cerrado, del 1
     *                      al 5% de la de fuera; se acota a [0, FullSun]. Sin ese suelo
     *                      un dosel muy denso reproduce el cero que la exponencial
     *                      viene a evitar.
     * @see @ref bib_monsisaeki1953
     * @see @ref bib_gapmodels
     */
    FORCEINLINE float LightFromExtinction(float LeafAreaAbove, float FullSun, float ExtinctionK, float DiffuseFloor)
    {
        const float Floor = FMath::Clamp(DiffuseFloor, 0.f, FullSun);
        const float Transmitted = FMath::Exp(-FMath::Max(ExtinctionK, 0.f) * FMath::Max(LeafAreaAbove, 0.f));
        return Floor + (FullSun - Floor) * Transmitted;
    }

    /**
     * Dimensiona una rejilla 3D que cubra Bounds con celdas de lado CellSize.
     *
     * @param MaxPerAxis Tope de celdas por eje. Es una salvaguarda de memoria: sin él,
     *                   una caja enorme o corrupta reservaría gigas.
     * @param OutW,OutH,OutD Dimensiones resultantes, siempre >= 1, de modo que una caja
     *                   plana o degenerada sigue produciendo una rejilla válida.
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
     * Recorre las celdas del bloque de @f$(2R+1)^3@f$ centrado en (Cx, Cy, Cz),
     * recortado a la rejilla Width x Height x Depth, e invoca `Fn(int32 CellIndex)` en
     * cada una.
     *
     * Sirve igual para 2D y para 3D: una rejilla 2D es este mismo recorrido con
     * Depth = 1 y Cz = 0, y entonces el bucle de Z da una sola vuelta. Los límites se
     * recortan una vez por eje, así que el cuerpo del bucle no comprueba nada y el
     * índice de fila se calcula una vez por fila.
     *
     * @note El orden de visita es z, luego y, luego x, todos ascendentes. Es parte del
     *       contrato de determinismo: estas consultas alimentan decisiones de la
     *       simulación y del SCA.
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
     * Igual que @ref EcoGrid::ForEachCellInBox, pero desenrollando el índice CSR:
     * invoca `Fn(int32 ItemIndex)` por cada item guardado en esas celdas. Es el cuerpo
     * de las consultas de proximidad de `FSpatialHash` y de `FAttractorCloud`, que se
     * reducen a calcular su celda central y llamar aquí.
     *
     * @note Recorre TODOS los items de las celdas tocadas, porque el bloque es un cubo
     *       y no una esfera. El filtrado fino por distancia real corresponde al
     *       llamador, que es donde resulta barato.
     * @pre CellStart y SortedIdx provienen de @ref EcoGrid::BuildCSR sobre la misma
     *      rejilla.
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
     * Construye el índice espacial CSR por counting sort en tres pasadas: contar items
     * por celda, prefijo acumulado y volcado con cursor. Coste @f$O(NumItems)@f$ al ser
     * la clave un entero acotado, el índice de celda.
     *
     * @param CellOfItem Se invoca como `CellOfItem(int32 ItemIndex) -> int32 celda`.
     * @param CellStart  Salida: NumCells+1 prefijos; los items de la celda c ocupan
     *                   SortedIdx en [CellStart[c], CellStart[c+1]).
     * @param SortedIdx  Salida: índices de item agrupados por celda.
     * @param Cursor     Buffer de trabajo del llamador; conservarlo entre llamadas
     *                   evita una reserva por tick, como hace `FSpatialHash`.
     * @note El recorrido en índice creciente hace el volcado estable: dentro de cada
     *       celda SortedIdx queda siempre en el mismo orden, requisito del contrato de
     *       determinismo y no un simple detalle deseable.
     * @see @ref bib_countingsortcsr
     */
    template <typename FCellOf>
    void BuildCSR(int32 NumCells, int32 NumItems, FCellOf&& CellOfItem,
        TArray<int32>& CellStart, TArray<int32>& SortedIdx, TArray<int32>& Cursor)
    {
        // El Reset es obligatorio: SetNumZeroed solo pone a cero los elementos NUEVOS,
        // así que sin él se conservarían los prefijos de la pasada anterior.
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
