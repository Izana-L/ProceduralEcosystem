/**
 * @file AttractorCloud.h
 * @author Juan Luque Roldán
 * @brief Nube de atractores de la colonización del espacio y su índice de consulta por rango.
 *
 * Declara FAttractorCloud, que responde a las dos preguntas que el algoritmo de
 * colonización del espacio hace sobre el hueco disponible en la copa: dónde hay algo
 * hacia lo que crecer y qué atractores ve un nodo concreto. La siembra reparte los
 * puntos dentro de la envolvente de copa de la especie —cónica, columnar o elipsoidal,
 * más una falda bajo la base de copa— de forma determinista desde la semilla del árbol.
 * La consulta se apoya en un índice CSR 3D de celda @f$d_i@f$ construido por counting
 * sort, que baja el paso ASOCIAR de @f$O(N)@f$ por nodo a @f$O(\text{vecindad})@f$. Los
 * atractores no se mueven nunca —solo se marcan muertos—, así que el índice se
 * construye una sola vez por árbol.
 *
 * @ingroup eco_geometry
 * @see @ref bib_runions2007
 * @see @ref bib_weberpenn1995
 * @see @ref bib_countingsortcsr
 * @see @ref bib_teschner2003
 */

#pragma once

#include "CoreMinimal.h"
#include "Core/GridMath.h"

class USpeciesData;          // Species/SpeciesData.h        (se incluye en el .cpp)
struct FTreeLightGridFine;   // Geometry/TreeLightGridFine.h (se incluye en el .cpp)

/**
 * Punto de espacio libre dentro de la copa hacia el que el árbol quiere crecer.
 *
 * BestNode y BestDist son estado de trabajo del paso ASOCIAR: se reinician y se
 * recalculan enteros en cada iteración del algoritmo, no persisten. `bAlive` pasa a
 * false por dos vías y no vuelve a true: alcanzado (paso MATAR, una rama nueva cae a
 * menos de @f$d_k@f$) o en sombra (@ref FAttractorCloud::CullByShade).
 */
struct FAttractor
{
    FVector Pos = FVector::ZeroVector; ///< Posición en mundo, en cm.
    bool    bAlive = true;             ///< false = alcanzado o podado por falta de luz.
    int32   BestNode = INDEX_NONE;     ///< Nodo más cercano dentro de @f$d_i@f$ en esta iteración.
    float   BestDist = 0.f;            ///< Distancia a BestNode; arranca en @f$d_i@f$.
};

/**
 * Nube de atractores de un árbol: siembra de la envolvente de copa e índice espacial.
 *
 * Dos responsabilidades. Sembrar la envolvente de copa de la especie
 * (@ref SampleCrownEnvelope) según su ECrownShape, de forma determinista desde el
 * RngState del árbol: misma semilla, misma nube. Y servir de índice por rango al paso
 * ASOCIAR (@ref ForEachInRange) sobre un CSR 3D de celda @f$d_i@f$.
 *
 * @ref CullByShade tiene una sola firma, siempre contra la rejilla de luz FINA: ésta ya
 * trae la sombra de los vecinos vía FTreeLightGridFine::SeedFromCoarse, de modo que el
 * mismo código sirve para el contexto de vecinos y para la autopoda del follaje propio.
 *
 * El índice se construye una vez tras sembrar, porque los atractores no se mueven, solo
 * se marcan muertos. Las consultas devuelven todos los índices de las celdas del bloque
 * y filtrar por `bAlive` corresponde al llamador, igual que en @ref FSpatialHash.
 */
struct PROCEDURALECOSYSTEM_API FAttractorCloud
{
    TArray<FAttractor> Attractors;

    // ==== Índice CSR 3D (lo construye BuildIndex tras sembrar) ====
    float   CellSize = 0.f;                    ///< Arista de celda; el crecimiento usa @f$d_i@f$.
    FVector GridOrigin = FVector::ZeroVector;  ///< Esquina mínima de la rejilla, en mundo.
    int32   GridW = 0, GridH = 0, GridD = 0;   ///< Dimensiones de la rejilla en celdas.
    TArray<int32> CellStart;                   ///< Prefijo por celda: W*H*D + 1 entradas.
    TArray<int32> SortedIdx;                   ///< Índices de atractor agrupados por celda.

    int32 Num() const { return Attractors.Num(); }

    /** Atractores todavía vivos; el bucle de crecimiento lo usa como condición de parada. */
    int32 CountAlive() const;

    /** Vacía la nube y el índice. */
    void Reset();

    /**
     * Siembra Species.NumAttractors puntos en la envolvente de copa, en coordenadas de
     * mundo y con la base del tronco en TrunkBaseWorld.
     *
     * @param RngState Estado del generador; se consume y avanza, de ahí la
     *                 reproducibilidad de la nube.
     * @note Reemplaza el contenido previo. El índice queda obsoleto: hay que volver a
     *       llamar a @ref BuildIndex después.
     */
    void SampleCrownEnvelope(const USpeciesData& Species, const FVector& TrunkBaseWorld, uint32& RngState);

    /**
     * Construye el índice CSR con celda InCellSize.
     * @pre La nube ya está sembrada. El crecimiento pasa @f$d_i@f$, con lo que la
     *      vecindad de una consulta de ese radio cabe en el bloque de 3x3x3 celdas.
     */
    void BuildIndex(float InCellSize);

    /**
     * Marca muertos los atractores cuya luz disponible en la rejilla fina queda por
     * debajo de LightThreshold: sombra de los vecinos y autopoda del follaje propio.
     */
    void CullByShade(const FTreeLightGridFine& Light, float LightThreshold);

    /**
     * Invoca `Fn(int32 AttractorIndex)` para cada atractor de las celdas que cubren la
     * esfera de radio Radius (en unidades de mundo) centrada en P.
     *
     * @note Es una consulta por CELDAS: devuelve también atractores fuera del radio
     *       exacto y atractores muertos. Filtrar por distancia y por `bAlive`
     *       corresponde a Fn.
     */
    template<typename FuncT>
    void ForEachInRange(const FVector& P, float Radius, FuncT&& Fn) const
    {
        if (CellSize <= 0.f || SortedIdx.Num() == 0)
        {
            return;
        }

        const int32 Cx = EcoGrid::WorldToCell(P.X, GridOrigin.X, CellSize);
        const int32 Cy = EcoGrid::WorldToCell(P.Y, GridOrigin.Y, CellSize);
        const int32 Cz = EcoGrid::WorldToCell(P.Z, GridOrigin.Z, CellSize);
        const int32 R = FMath::CeilToInt(Radius / CellSize);

        // Recorrido del bloque de celdas compartido con FSpatialHash. Es el bucle más
        // caliente de la generación —una consulta por nodo y por iteración— y su orden
        // de visita forma parte del contrato de determinismo.
        EcoGrid::ForEachItemInBox(CellStart, SortedIdx, Cx, Cy, Cz, R,
            GridW, GridH, GridD, Forward<FuncT>(Fn));
    }

private:
    /** Posición en mundo a índice lineal de celda, con clamp a la rejilla. */
    FORCEINLINE int32 CellOf(const FVector& P) const
    {
        const int32 Cx = EcoGrid::WorldToCellClamped(P.X, GridOrigin.X, CellSize, GridW);
        const int32 Cy = EcoGrid::WorldToCellClamped(P.Y, GridOrigin.Y, CellSize, GridH);
        const int32 Cz = EcoGrid::WorldToCellClamped(P.Z, GridOrigin.Z, CellSize, GridD);
        return EcoGrid::VoxelIndex(Cx, Cy, Cz, GridW, GridH);
    }
};