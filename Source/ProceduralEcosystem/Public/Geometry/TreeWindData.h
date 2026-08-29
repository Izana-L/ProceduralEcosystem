/**
 * @file TreeWindData.h
 * @author Juan Luque Roldán
 * @brief Atributos por nodo que animan el árbol con el viento: pivote, nivel, balanceo y oclusión.
 *
 * Declara FTreeWindNode y FTreeWindData, los datos que el material necesita para mover el
 * árbol en el vertex shader. Con una malla de árbol comprada o esculpida no se sabe dónde
 * empieza cada rama ni de quién cuelga, y esa jerarquía hay que reconstruirla con una
 * herramienta offline y hornearla a texturas de pivotes que el material lee por índice;
 * aquí el esqueleto lo genera el propio motor y cada nodo conoce a su padre, así que la
 * información sale gratis y se escribe directamente en los canales UV y en el color de
 * vértice al mallar, sin paso offline ni límite de resolución de textura. El mismo
 * recorrido aprovecha la rejilla de luz fina para dejar un término de oclusión ambiental
 * por vértice, con lo que el material queda atado al campo de la simulación.
 *
 * @ingroup eco_geometry
 * @see @ref bib_pivotpainter
 * @see @ref bib_vientovegetacion
 * @see @ref bib_zhukov1998
 */

#pragma once

#include "CoreMinimal.h"

struct FTreeSkeleton;
struct FTreeLightGridFine;
class  USpeciesData;

/**
 * Atributos de viento y oclusión de un nodo del esqueleto.
 *
 * La colonización del espacio produce nodos, no ramas. Una RAMA es la cadena de nodos
 * entre dos bifurcaciones, su PIVOTE es el punto donde nace —que es el punto sobre el que
 * debe rotar cuando la empuja el viento— y su NIVEL es cuántas bifurcaciones la separan
 * del tronco. Con esos tres datos el material compone un balanceo jerárquico: el tronco
 * flexa poco, la rama principal hereda ese movimiento y además flexa lo suyo, y la ramilla
 * vibra.
 */
struct FTreeWindNode
{
    /** Pivote de la rama a la que pertenece el nodo, en cm y relativo a la base del
        tronco (Skeleton.Nodes[0].Pos). El mallador lo pasa a metros. */
    FVector PivotLocalCm = FVector::ZeroVector;

    /** Nivel de la rama normalizado: 0 = tronco, 1 = la rama de mayor orden del árbol. */
    float BranchLevel01 = 0.f;

    /** Cuánto se mueve este punto con el viento: 0 en la base del tronco, que está
        empotrada, y ~1 en la punta de una ramilla fina. */
    float SwayWeight = 0.f;

    /** Desfase en [0,1) estable por rama. Sin él todas las ramas del árbol —y, al
        compartir malla, todos los árboles— oscilan al unísono. */
    float Phase01 = 0.f;

    /** Oclusión ambiental de copa: 1 = expuesto al cielo, 0 = interior en sombra. */
    float CanopyAO = 1.f;

    /** Variación de tinte en [0,1) estable por rama; rompe la uniformidad del material. */
    float TintVariation = 0.f;
};

/**
 * Atributos de viento y oclusión de todos los nodos de un esqueleto, más las pasadas
 * auxiliares que el mallador y el follaje reutilizan.
 *
 * Se calcula una vez, con el esqueleto ya terminado y justo antes de mallar. El mallador
 * copia cada campo a los vértices del anillo de su nodo, sobre los canales que soportan
 * tanto el UProceduralMeshComponent del hero tree como el UStaticMesh horneado de la
 * librería:
 *
 * @code
 * UV0   = (u, v)                   textura de corteza u hoja
 * UV1   = (PivotX, PivotY)         pivote de la rama, en metros y relativo
 * UV2   = (PivotZ, BranchLevel01)  a la base del tronco
 * UV3   = (SwayWeight, Phase01)
 * Color = (CanopyAO, TintVariation, BranchLevel01, 1)
 * @endcode
 *
 * Struct plano y no USTRUCT: es dato caliente y transitorio, igual que FTreeSkeleton.
 *
 * @note El pivote viaja en METROS porque Unreal guarda los UV de un UStaticMesh en media
 *       precisión salvo que se active «Use Full Precision UVs»: a 2.000 cm el paso de un
 *       float16 es de ~1 cm y el pivote saldría escalonado, mientras que en metros el peor
 *       paso baja a ~1,5 cm. El material solo tiene que multiplicar por 100.
 * @note Y va relativo a la base del tronco porque es el origen local al que el mallador
 *       refiere los vértices: así el material puede restarlos directamente, sin saber
 *       dónde está el árbol ni cuánto mide.
 */
struct PROCEDURALECOSYSTEM_API FTreeWindData
{
    /** Un elemento por nodo del esqueleto, en el mismo orden. */
    TArray<FTreeWindNode> Nodes;

    /** Índice del nodo que INICIA la rama a la que pertenece cada nodo. Se expone porque
        el follaje lo usa como sal estable por rama al derivar el desfase y el jitter de
        cada hoja. */
    TArray<int32> BranchRoot;

    /** Número de hijos de cada nodo. Build() ya paga la pasada y el mallador la necesita
        para localizar las puntas, que son los nodos sin hijos: una sola copia. */
    TArray<int32> ChildCount;

    /** Longitud de arco acumulada desde la raíz hasta cada nodo, en cm. Igual que
        ChildCount, se calcula una vez aquí y la comparten mallador y follaje. */
    TArray<float> AlongLen;

    /** Vacía los cuatro arrays. */
    void Reset();

    /** true si hay un atributo por cada nodo del esqueleto dado y el esqueleto no es vacío. */
    bool IsValidFor(const FTreeSkeleton& Skeleton) const;

    /**
     * Calcula los atributos de todos los nodos.
     *
     * @param Skeleton  Esqueleto terminado. Debe traer los radios del pipe model, que son
     *                  los que deciden qué ramas son flexibles.
     * @param Species   Aporta la rigidez de la especie frente al viento.
     * @param FineLight Rejilla de luz fina de la que sale la oclusión de copa. Puede ser
     *                  nullptr, y entonces CanopyAO vale 1 en todos los nodos.
     * @param Seed      Semilla del árbol: hace los desfases reproducibles y distintos
     *                  entre árboles.
     * @pre El esqueleto ya tiene sus radios calculados (@ref SpaceColonization::ComputeRadii).
     */
    void Build(const FTreeSkeleton& Skeleton, const USpeciesData& Species,
        const FTreeLightGridFine* FineLight, uint32 Seed);
};
