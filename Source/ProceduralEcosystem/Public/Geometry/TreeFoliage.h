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
 * que la colonización del espacio haya troceado la rama. Declara también la rampa con la
 * que el follaje se espesa con la edad del arquetipo.
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
 * Cuántas hojas salen lo deciden tres palancas de la especie. LeafSpacingCm y LeafDensity
 * fijan el reparto base, el de la plántula. AdultLeafMultiplier lo espesa con la edad: el
 * paso efectivo entre hojas se acorta con la fracción de talla adulta del arquetipo
 * (USpeciesData::ArchetypeSizeRatio), de modo que la plántula conserva su reparto y el
 * adulto, cuya copa crece mucho más deprisa que la longitud de sus ramillas, no sale ralo.
 * MaxLeavesPerTree pone un tope absoluto por árbol que se cumple aclarando uniformemente
 * toda la copa, nunca cortando la emisión por el final.
 *
 * No consume ningún flujo de RNG: toda la variación sale de hashes estables de la terna
 * (semilla, rama, ranura), de modo que aclarar o espesar el follaje nunca desplaza la
 * secuencia que la colonización del espacio ya gastó para generar la madera.
 */
namespace TreeFoliage
{
    /**
     * Multiplicador de hojas por edad, en [1, AdultMultiplier]: vale exactamente 1 con
     * talla 0, exactamente @p AdultMultiplier con talla 1 y entre medias sube con el
     * cuadrado de la talla.
     *
     * La rampa es cuadrática y no lineal a propósito: así el bucket más joven queda
     * prácticamente intacto —a 0.2 de talla adulta recibe solo un 4 % del espesado— y casi
     * todo el efecto cae en los buckets altos, que son los que tienen copa que llenar. Con
     * cinco buckets y multiplicador 2, los factores por bucket son 1.04, 1.16, 1.36, 1.64
     * y 2.0.
     *
     * @param SizeRatio       Fracción de talla adulta del arquetipo; se recorta a [0,1].
     * @param AdultMultiplier Hojas del adulto por cada hoja de la plántula; 1 o menos
     *                        desactiva el efecto.
     * @return Factor por el que se divide LeafSpacingCm, nunca menor que 1.
     */
    FORCEINLINE float AgeLeafMultiplier(float SizeRatio, float AdultMultiplier)
    {
        const float A = FMath::Clamp(SizeRatio, 0.f, 1.f);
        return FMath::Lerp(1.f, FMath::Max(AdultMultiplier, 1.f), A * A);
    }

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
     * @note Con MaxLeavesPerTree > 0 el número de hojas nunca lo supera y, si el reparto
     *       lo desborda, lo alcanza exactamente. La selección es determinista y estable:
     *       subir el tope solo añade hojas y no mueve las que ya estaban.
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
