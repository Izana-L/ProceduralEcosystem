#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"

/**
 * Spatial hash grid 2D sobre las posiciones de los agentes-arbol (doc. Fase 2, 2.3).
 *
 * POR QUE UN HASH GRID Y NO UN OCTREE: los arboles son puntos de densidad
 * ~uniforme en el plano XY (la altura no importa para saber quien esta al
 * lado de quien). Un grid plano es mas simple, mas barato de reconstruir
 * cada tick y encaja mejor que un octree, que esta pensado para densidad
 * MUY desigual (ahi si se usa: en el grid de luz global grueso y en el fino
 * por hero-tree de la Fase 3).
 *
 * IMPLEMENTACION CSR (Compressed Sparse Row / "counting sort"): en vez de un
 * TArray<TArray<int32>> por celda (una allocation por celda, fragmentado en
 * memoria), se guardan dos arrays planos:
 *   - CellStart[c]  : indice de arranque de la celda c dentro de SortedIdx
 *   - SortedIdx[k]  : indices de agente, agrupados por celda
 * Consultar la celda c es iterar SortedIdx desde CellStart[c] hasta
 * CellStart[c+1]. Cero allocations por consulta, una sola pasada de
 * construccion por tick.
 *
 * DETERMINISMO: Build() es una pasada SERIAL (se llama una vez, antes del
 * ParallelFor del tick — ver doc. 2.5, "PRE"). Recorre los agentes en orden
 * de indice creciente, asi que dentro de cada celda SortedIdx queda siempre
 * en el mismo orden para una misma poblacion de entrada. ForEachNeighbor()
 * hereda ese orden fijo: dos corridas con la misma poblacion visitan los
 * vecinos en el mismo orden, sin importar cuantos hilos procesen despues
 * cada agente.
 *
 * Se reconstruye ENTERO cada tick (O(N)): nacimientos y muertes cambian el
 * conjunto de agentes, así que no compensa mantenerlo incrementalmente.
 */
struct PROCEDURALECOSYSTEM_API FSpatialHash
{
    double    CellSize = 100.0; // cm; ver nota de dimensionado mas abajo
    int32     GridW = 0;
    int32     GridH = 0;
    FVector2D Origin = FVector2D::ZeroVector;

    TArray<int32> CellStart; // tamaño GridW*GridH + 1
    TArray<int32> SortedIdx; // tamaño Num (tras el ultimo Build)

    bool IsValid() const { return GridW > 0 && GridH > 0; }

    /**
     * Fija la geometria del grid a partir de los limites del mundo simulado
     * y el tamaño de celda. Llamar solo cuando cambie el mundo (no cada
     * tick); Build() se encarga de repoblarlo con las posiciones actuales.
     *
     * @param WorldBounds  Extension del terreno simulable (cm), p.ej. la de
     *                     FHeightField::GetWorldBounds().
     * @param InCellSize   ~ radio de interaccion maximo entre arboles (doc.
     *                     Apendice A). Con vecindad de 3x3 celdas cubres un
     *                     radio de busqueda de hasta CellSize sin falsos
     *                     negativos cerca de los bordes de celda.
     */
    void Init(const FBox2D& WorldBounds, double InCellSize);

    /** Celda (cx,cy) -> indice lineal, con clamp a los bordes del grid. */
    FORCEINLINE int32 CellOf(const FVector& P) const
    {
        const int32 Cx = FMath::Clamp(FMath::FloorToInt((P.X - Origin.X) / CellSize), 0, GridW - 1);
        const int32 Cy = FMath::Clamp(FMath::FloorToInt((P.Y - Origin.Y) / CellSize), 0, GridH - 1);
        return Cy * GridW + Cx;
    }

    /**
     * Reconstruye el grid a partir de las posiciones actuales (counting sort
     * en dos pasadas: contar por celda, prefijo acumulado, volcar). O(Num).
     * Num puede ser menor que Pos.Num() (p.ej. si en el futuro se separan
     * vivos/muertos); nunca al reves.
     */
    void Build(const TArray<FVector>& Pos, int32 Num);

    /**
     * Invoca Fn(AgentIndex) por cada agente en las celdas dentro de Radius
     * de P (radio en CELDAS, redondeado hacia arriba: revisa un cuadrado de
     * (2r+1)x(2r+1) celdas, no un circulo exacto — falso-positivos baratos
     * de descartar en Fn si hace falta distancia exacta).
     * Fn debe ser invocable como Fn(int32 AgentIndex).
     */
    template<typename FuncT>
    void ForEachNeighbor(const FVector& P, float Radius, FuncT&& Fn) const
    {
        if (!IsValid() || SortedIdx.Num() == 0)
        {
            return;
        }

        const int32 Cx = FMath::FloorToInt((P.X - Origin.X) / CellSize);
        const int32 Cy = FMath::FloorToInt((P.Y - Origin.Y) / CellSize);
        const int32 R = FMath::CeilToInt(Radius / CellSize);

        for (int32 Dy = -R; Dy <= R; ++Dy)
        {
            const int32 Ny = Cy + Dy;
            if (Ny < 0 || Ny >= GridH) continue;

            for (int32 Dx = -R; Dx <= R; ++Dx)
            {
                const int32 Nx = Cx + Dx;
                if (Nx < 0 || Nx >= GridW) continue;

                const int32 C = Ny * GridW + Nx;
                for (int32 K = CellStart[C]; K < CellStart[C + 1]; ++K)
                {
                    Fn(SortedIdx[K]);
                }
            }
        }
    }
};