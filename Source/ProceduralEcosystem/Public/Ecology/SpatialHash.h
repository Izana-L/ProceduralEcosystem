/**
 * @file SpatialHash.h
 * @author Juan Luque Roldán
 * @brief Índice espacial 2D de los agentes-árbol en formato CSR.
 *
 * Declara FSpatialHash, la rejilla uniforme que responde a «qué árboles hay alrededor de
 * este punto»: de ella dependen el espaciado mínimo de la germinación y el recuento de
 * conespecíficos. El índice son dos arrays planos —CellStart con el prefijo de cada celda
 * y SortedIdx con los agentes agrupados por celda— que se construyen por counting sort en
 * una sola pasada serial de coste @f$O(N)@f$, repetida entera cada tick porque
 * nacimientos y muertes cambian el conjunto de agentes. Recorrer los agentes en índice
 * creciente al construirlo fija el orden de visita de todas las consultas, y ese orden
 * forma parte del contrato de determinismo del tick.
 *
 * @ingroup eco_ecology
 * @see @ref bib_countingsortcsr
 * @see @ref bib_teschner2003
 */

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "Core/GridMath.h"

/**
 * Rejilla uniforme densa sobre la caja del mundo que indexa las posiciones de los agentes
 * en formato CSR (Compressed Sparse Row).
 *
 * Los agentes de la celda `c` son los `SortedIdx[k]` con `k` en
 * @f$[\mathrm{CellStart}[c], \mathrm{CellStart}[c+1])@f$. Dos arrays planos en lugar de un
 * `TArray<TArray<int32>>` por celda: consultar no reserva memoria y construir es una única
 * pasada. Se prefiere a un octree porque los árboles son puntos de densidad casi uniforme
 * en el plano XY —la altura no interviene en quién está al lado de quién—; el octree queda
 * para las rejillas de luz, donde la densidad sí es muy desigual.
 *
 * @note Pese al nombre no hay función hash ni tabla hash: la clave es directamente el
 *       índice de celda, con clamp en los bordes del mundo.
 * @pre Init antes del primer Build, y un Build de este tick antes de cualquier consulta.
 */
struct PROCEDURALECOSYSTEM_API FSpatialHash
{
    double    CellSize = 100.0;               ///< Lado de celda en cm.
    int32     GridW = 0;                      ///< Celdas en X.
    int32     GridH = 0;                      ///< Celdas en Y.
    FVector2D Origin = FVector2D::ZeroVector; ///< Esquina mínima del mundo indexado, en cm.

    TArray<int32> CellStart;                  ///< Prefijos por celda; tamaño GridW*GridH + 1.
    TArray<int32> SortedIdx;                  ///< Índices de agente agrupados por celda.

    /** true si la geometría está dimensionada, es decir, si Init ya se ha llamado. */
    bool IsValid() const { return GridW > 0 && GridH > 0; }

    /**
     * Dimensiona la rejilla a partir de los límites del mundo simulado y el lado de celda,
     * y vacía el índice.
     *
     * Se llama solo cuando cambia el mundo, nunca cada tick: repoblar el índice con las
     * posiciones actuales es cosa de Build.
     *
     * @param WorldBounds Extensión del terreno simulable en cm, típicamente la de
     *                    FHeightField::GetWorldBounds().
     * @param InCellSize  Lado de celda en cm, del orden del radio de interacción máximo
     *                    entre árboles: con una vecindad de 3x3 celdas se cubre un radio
     *                    de búsqueda de hasta CellSize sin falsos negativos junto a los
     *                    bordes de celda. Se acota a un mínimo de 1 cm.
     */
    void Init(const FBox2D& WorldBounds, double InCellSize);

    /** Índice lineal de la celda que contiene a P, con clamp a los bordes de la rejilla. */
    FORCEINLINE int32 CellOf(const FVector& P) const
    {
        const int32 Cx = EcoGrid::WorldToCellClamped(P.X, Origin.X, CellSize, GridW);
        const int32 Cy = EcoGrid::WorldToCellClamped(P.Y, Origin.Y, CellSize, GridH);
        return Cy * GridW + Cx;
    }

    /**
     * Reconstruye el índice completo a partir de las posiciones actuales, por counting
     * sort: contar por celda, prefijo acumulado y volcado con cursor. Coste @f$O(Num)@f$.
     *
     * @param Pos Posiciones de mundo de los agentes, en cm.
     * @param Num Cuántas entradas de Pos se indexan, desde el principio.
     * @pre Num <= Pos.Num().
     * @note Pasada serial, previa al paso paralelo del tick. El recorrido en índice
     *       creciente deja SortedIdx siempre en el mismo orden dentro de cada celda para
     *       una misma población de entrada, y de ahí hereda su determinismo
     *       @ref ForEachNeighbor.
     */
    void Build(const TArray<FVector>& Pos, int32 Num);

    /**
     * Invoca `Fn(int32 AgentIndex)` por cada agente indexado en las celdas que rodean a P.
     *
     * @param P      Centro de la consulta, en cm de mundo.
     * @param Radius Radio de búsqueda en cm; se convierte a celdas redondeando hacia
     *               arriba y se recorre un bloque cuadrado de (2R+1)x(2R+1) celdas.
     * @param Fn     Invocable como `Fn(int32 AgentIndex)`.
     * @note El bloque es cuadrado, no circular: filtrar por distancia exacta corresponde
     *       al llamante, que es donde resulta barato.
     */
    template<typename FuncT>
    void ForEachNeighbor(const FVector& P, float Radius, FuncT&& Fn) const
    {
        if (!IsValid() || SortedIdx.Num() == 0)
        {
            return;
        }

        const int32 Cx = EcoGrid::WorldToCellClamped(P.X, Origin.X, CellSize, GridW);
        const int32 Cy = EcoGrid::WorldToCellClamped(P.Y, Origin.Y, CellSize, GridH);
        const int32 R = FMath::CeilToInt32(Radius / CellSize);

        // Recorrido compartido con FAttractorCloud (EcoGrid::ForEachItemInBox): una
        // rejilla 2D es la 3D con una sola capa, así que basta con Depth = 1.
        EcoGrid::ForEachItemInBox(CellStart, SortedIdx, Cx, Cy, /*Cz*/ 0, R,
            GridW, GridH, /*Depth*/ 1, Forward<FuncT>(Fn));
    }

    /** Bytes reservados por los buffers persistentes, para el informe de perfilado. */
    int32 ScratchBytes() const
    {
        return (CellStart.Max() + SortedIdx.Max() + Cursor.Max()) * sizeof(int32);
    }

private:
    /**
     * Cursor de escritura del counting sort, miembro persistente en lugar de local.
     *
     * Como el índice se reconstruye entero cada tick, un cursor local costaría una reserva
     * y una copia del orden de 168 KB a 205x205 celdas cada vez; conservado entre llamadas
     * se reutiliza su capacidad y la reconstrucción no toca el heap.
     */
    TArray<int32> Cursor;
};
