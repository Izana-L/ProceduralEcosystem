#pragma once

#include "CoreMinimal.h"

class  USpeciesData;
struct FTreeMeshBuffers;   // Geometry/TreeMeshBuilder.h
struct FTreeSkeleton;      // Geometry/TreeSkeleton.h
struct FTreeWindData;      // Geometry/TreeWindData.h
struct FTreeLightGridFine; // Geometry/TreeLightGridFine.h

/**
 * Colocacion de las hojas sobre el esqueleto (doc. Fase 3, 3.7).
 *
 * Las hojas se reparten a lo largo de las RAMILLAS, no en sus puntas: cada
 * hoja ocupa una ranura a distancia fija (LeafSpacingCm) medida sobre la
 * longitud acumulada del esqueleto, y cada ranura consecutiva gira el angulo
 * de divergencia de la especie (PhyllotaxisAngleDeg). Como la longitud
 * acumulada es monotona a lo largo de cualquier cadena de nodos, la espiral
 * queda continua al cruzar una bifurcacion sin llevar ningun contador, y es
 * independiente de la resolucion del SCA (StepLengthD).
 *
 * Solo llevan hoja los nodos cuyo radio del pipe model esta por debajo de
 * TipRadiusCm * LeafBearingRadiusScale: la hoja sale de la madera del año.
 *
 * DETERMINISMO: no consume ningun stream de RNG. Toda la variacion sale de
 * hashes de (Seed, rama, ranura), igual que los desfases de FTreeWindData, de
 * modo que colocar mas o menos hojas nunca desplaza la secuencia aleatoria que
 * el SCA ya consumio para generar la madera.
 */
namespace TreeFoliage
{
    /**
     * @param FrameN/FrameB  Marcos de rotacion minima por nodo que calcula el
     *                       mallador. Dan el azimut de la espiral sin torsion.
     * @param FineLight      Rejilla fina del SCA para el heliotropismo de la
     *                       hoja. nullptr = la hoja se orienta al cielo.
     */
    PROCEDURALECOSYSTEM_API void Build(
        const FTreeSkeleton& Skeleton,
        const FTreeWindData& Wind,
        const USpeciesData& Species,
        const TArray<FVector>& FrameN,
        const TArray<FVector>& FrameB,
        const FTreeLightGridFine* FineLight,
        uint32 Seed,
        FTreeMeshBuffers& OutLeaves);
}
