/**
 * @file TreeFoliage.h
 * @author Juan Luque Roldán
 * @brief API de colocación de las hojas sobre el esqueleto de ramas.
 *
 * Declara TreeFoliage::Build, el paso que el mallador ejecuta al terminar la madera y que
 * llena la sección de follaje con tarjetas de hoja. Las hojas se reparten a lo largo de las
 * ramillas, no en sus puntas: cada una ocupa una ranura a distancia fija medida sobre la
 * longitud acumulada del esqueleto, y de una ranura a la siguiente el punto de inserción
 * gira el ángulo de divergencia de la especie. Como la longitud acumulada crece de forma
 * monótona a lo largo de cualquier cadena de nodos, la espiral resultante queda continua
 * al cruzar una bifurcación sin llevar ningún contador, y no depende de la resolución con
 * que la colonización del espacio haya troceado la rama.
 *
 * @ingroup eco_geometry
 * @see @ref bib_vogel1979
 */

#pragma once

#include "CoreMinimal.h"

class  USpeciesData;
struct FTreeMeshBuffers;   // Geometry/TreeMeshBuilder.h
struct FTreeSkeleton;      // Geometry/TreeSkeleton.h
struct FTreeWindData;      // Geometry/TreeWindData.h
struct FTreeLightGridFine; // Geometry/TreeLightGridFine.h

/**
 * Colocación de las hojas sobre un esqueleto ya mallado.
 *
 * Solo llevan hoja los nodos cuyo radio queda por debajo de
 * `TipRadiusCm * LeafBearingRadiusScale`: la hoja sale de la madera del año, no del
 * tronco. Cada hoja es una tarjeta de hoja, un quad de cuatro vértices con su propia
 * orientación, tamaño y desfase de aleteo.
 *
 * No consume ningún flujo de RNG: toda la variación sale de hashes estables de la terna
 * (semilla, rama, ranura), de modo que aclarar o espesar el follaje nunca desplaza la
 * secuencia que la colonización del espacio ya gastó para generar la madera.
 */
namespace TreeFoliage
{
    /**
     * Genera las tarjetas de hoja del árbol y las escribe en @p OutLeaves.
     *
     * @param Wind       Datos de viento por nodo: aporta la longitud acumulada que define
     *                   las ranuras, la rama a la que pertenece cada nodo y su balanceo.
     * @param FrameN     Normal del marco de rotación mínima por nodo, la que calcula el
     *                   mallador; da el azimut de la espiral sin torsión parásita.
     * @param FrameB     Binormal del mismo marco.
     * @param FineLight  Rejilla de luz fina del árbol, de la que sale la orientación
     *                   heliotrópica de la lámina. Sin ella la hoja mira al cielo.
     * @param Seed       Semilla del árbol: solo se hashea, nunca se avanza.
     * @param OutLeaves  Destino; se vacía al entrar.
     * @pre  @p Wind, @p FrameN y @p FrameB deben corresponder a este mismo esqueleto; si
     *       no, la llamada no emite nada.
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
