#pragma once

#include "CoreMinimal.h"

class USpeciesData;          // Species/SpeciesData.h        (se incluye en el .cpp)
struct FTreeLightGridFine;   // Geometry/TreeLightGridFine.h (se incluye en el .cpp)

/**
 * Un atractor del SCA (doc. Fase 3, 3.1): un punto de "espacio/luz libre" en
 * la copa hacia el que el arbol quiere crecer. Se siembra una nube dentro de
 * la envolvente y el arbol crece nodo a nodo hacia ellos.
 *
 * BestNode/BestDist son estado de trabajo del paso ASOCIAR: se recalculan en
 * cada iteracion del SCA (no persisten). bAlive pasa a false cuando el atractor
 * se da por alcanzado (paso MATAR) o queda en sombra (autopoda).
 */
struct FAttractor
{
    FVector Pos = FVector::ZeroVector;
    bool    bAlive = true;
    int32   BestNode = INDEX_NONE; // nodo mas cercano dentro de d_i (se resetea cada iter)
    float   BestDist = 0.f;        // distancia a BestNode
};

/**
 * Nube de atractores + indice espacial para el SCA (doc. Fase 3, 3.1/3.2).
 *
 * Dos responsabilidades:
 *   1. SEMBRAR la envolvente de copa por especie (SampleCrownEnvelope), con la
 *      forma conica/esferica/columnar del Paso 1. Es determinista desde el
 *      RngState del arbol (doc. 3.8): mismo arbol -> misma nube.
 *   2. Servir de INDICE por rango para el paso "Asociar" del SCA: un indice CSR
 *      3D (counting sort) sobre las posiciones, para consultar los atractores
 *      cerca de un nodo en O(vecindad) en vez de O(N). Es el uso clasico de la
 *      rejilla como estructura de aceleracion del SCA.
 *
 * NOTA de decision: en el documento el primer CullByShade usa el grid grueso
 * global directamente; aqui se unifica en el grid FINO, que ya trae la sombra
 * de los vecinos via FTreeLightGridFine::SeedFromCoarse. Asi CullByShade tiene
 * una sola firma (contra el grid fino), tanto para el contexto de vecinos como
 * para la autopoda del follaje propio.
 *
 * El indice se construye UNA vez tras sembrar (los atractores no se mueven,
 * solo se marcan dead). Las consultas devuelven TODOS los indices de las
 * celdas: es el llamador (el SCA) quien filtra por bAlive, igual que hace
 * FSpatialHash en la Fase 2.
 */
struct PROCEDURALECOSYSTEM_API FAttractorCloud
{
    TArray<FAttractor> Attractors;

    // --- Indice CSR 3D (construido por BuildIndex tras SampleCrownEnvelope) ---
    float   CellSize = 0.f;                    // usar d_i (radio de influencia)
    FVector GridOrigin = FVector::ZeroVector;  // esquina min de la rejilla
    int32   GridW = 0, GridH = 0, GridD = 0;
    TArray<int32> CellStart;                   // offsets CSR, tamano GridW*GridH*GridD + 1
    TArray<int32> SortedIdx;                   // indices de atractor agrupados por celda

    int32 Num() const { return Attractors.Num(); }
    int32 CountAlive() const;
    void Reset();

    /**
     * Siembra NumAttractors puntos dentro de la envolvente de copa de la
     * especie (forma segun ECrownShape), en mundo, con la base del tronco en
     * TrunkBaseWorld. REEMPLAZA el contenido. Consume RngState -> reproducible.
     */
    void SampleCrownEnvelope(const USpeciesData& Species, const FVector& TrunkBaseWorld, uint32& RngState);

    /** Construye el indice CSR con InCellSize (usar d_i). Llamar tras sembrar. */
    void BuildIndex(float InCellSize);

    /** Marca dead los atractores cuya luz esta por debajo de LightThreshold en
        la rejilla fina (micro<-macro de vecinos y/o autopoda del follaje). */
    void CullByShade(const FTreeLightGridFine& Light, float LightThreshold);

    /**
     * Invoca Fn(int32 AttractorIndex) por cada atractor en las celdas dentro de
     * Radius de P (radio en unidades de mundo; se convierte a celdas). Incluye
     * atractores dead: filtra por Attractors[i].bAlive en Fn si hace falta.
     */
    template<typename FuncT>
    void ForEachInRange(const FVector& P, float Radius, FuncT&& Fn) const
    {
        if (CellSize <= 0.f || SortedIdx.Num() == 0)
        {
            return;
        }

        const int32 Cx = FMath::FloorToInt((P.X - GridOrigin.X) / CellSize);
        const int32 Cy = FMath::FloorToInt((P.Y - GridOrigin.Y) / CellSize);
        const int32 Cz = FMath::FloorToInt((P.Z - GridOrigin.Z) / CellSize);
        const int32 R = FMath::CeilToInt(Radius / CellSize);

        for (int32 Dz = -R; Dz <= R; ++Dz)
        {
            const int32 Nz = Cz + Dz;
            if (Nz < 0 || Nz >= GridD) continue;

            for (int32 Dy = -R; Dy <= R; ++Dy)
            {
                const int32 Ny = Cy + Dy;
                if (Ny < 0 || Ny >= GridH) continue;

                for (int32 Dx = -R; Dx <= R; ++Dx)
                {
                    const int32 Nx = Cx + Dx;
                    if (Nx < 0 || Nx >= GridW) continue;

                    const int32 C = (Nz * GridH + Ny) * GridW + Nx;
                    for (int32 K = CellStart[C]; K < CellStart[C + 1]; ++K)
                    {
                        Fn(SortedIdx[K]);
                    }
                }
            }
        }
    }

private:
    /** Mundo -> celda lineal con clamp (para construir el indice). */
    FORCEINLINE int32 CellOf(const FVector& P) const
    {
        const int32 Cx = FMath::Clamp(FMath::FloorToInt((P.X - GridOrigin.X) / CellSize), 0, GridW - 1);
        const int32 Cy = FMath::Clamp(FMath::FloorToInt((P.Y - GridOrigin.Y) / CellSize), 0, GridH - 1);
        const int32 Cz = FMath::Clamp(FMath::FloorToInt((P.Z - GridOrigin.Z) / CellSize), 0, GridD - 1);
        return (Cz * GridH + Cy) * GridW + Cx;
    }
};