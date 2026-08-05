#pragma once

#include "CoreMinimal.h"

struct FTreeSkeleton;
struct FTreeLightGridFine;
class  USpeciesData;

/**
 * =============================================================================
 *  DATOS DE VIENTO POR NODO (Fase 6, doc. 6.1) + AO DE COPA (doc. 6.2)
 * =============================================================================
 *
 * EL PUNTO CLAVE DE LA FASE, y esta escrito literalmente en el documento:
 *
 *   "Sinergia con la Fase 3: como generamos la malla desde el esqueleto de
 *    ramas, ya tenemos el pivote y el padre de cada rama; podemos escribir
 *    directamente los datos de Pivot Painter en canales UV al mallar, en vez de
 *    reconstruirlos."
 *
 * Pivot Painter 2.0 existe porque, con una malla de arbol comprada o esculpida,
 * NO se sabe donde empieza cada rama ni de quien cuelga: hay que reconstruir esa
 * jerarquia con una herramienta offline y hornearla a texturas que el material
 * lee por indice. Nosotros GENERAMOS el esqueleto (FTreeSkeleton: cada nodo
 * conoce a su padre y su profundidad), asi que esa informacion ya la tenemos
 * gratis y podemos escribirla directamente en los canales UV de la malla. Nos
 * ahorramos el paso offline, las texturas de pivotes y el limite de resolucion
 * que imponen. Es la ventaja concreta de haber construido la geometria nosotros.
 *
 * -----------------------------------------------------------------------------
 *  QUE ES UNA "RAMA" AQUI
 * -----------------------------------------------------------------------------
 * El SCA produce nodos, no ramas. Una RAMA es la cadena de nodos entre dos
 * bifurcaciones: un nodo empieza rama nueva si su padre tuvo mas de un hijo (o si
 * es la raiz). El PIVOTE de esa rama es el punto donde nace, que es exactamente
 * el punto sobre el que debe rotar cuando la empuja el viento; y su NIVEL es
 * cuantas bifurcaciones hay entre ella y el tronco. Con eso el material hace el
 * balanceo jerarquico creible del que habla el doc: el tronco flexa poco, las
 * ramas principales heredan ese movimiento y ademas flexan lo suyo, las ramillas
 * vibran.
 *
 * -----------------------------------------------------------------------------
 *  DONDE VIAJA CADA COSA (contrato con el material -- ver Docs/Fase6_Guia.md)
 * -----------------------------------------------------------------------------
 * El mallador (TreeMeshBuilder) vuelca estos datos en los canales que TANTO el
 * UProceduralMeshComponent del hero tree COMO el UStaticMesh horneado de la
 * libreria soportan (UV0..UV3 + color de vertice):
 *
 *   UV0 = (u, v)                  UVs de textura de siempre (corteza / hoja)
 *   UV1 = (PivotX, PivotY)        pivote de la rama, en METROS, relativo a la
 *   UV2 = (PivotZ, BranchLevel01)   base del tronco (= origen local de la malla)
 *   UV3 = (SwayWeight, Phase01)
 *
 *   Color de vertice:
 *     R = CanopyAO      1 = expuesto al cielo, 0 = interior de copa sombreado
 *     G = TintVariation [0,1) estable por rama: rompe la uniformidad del material
 *     B = BranchLevel01 duplicado del nivel (util como mascara barata)
 *     A = 1
 *
 * POR QUE EL PIVOTE EN METROS Y NO EN CENTIMETROS: UE almacena los canales UV de
 * un UStaticMesh en media precision (float16) salvo que se active
 * "Use Full Precision UVs". A 2.000 cm (20 m) el paso de un float16 es ~1 cm y
 * el pivote saldria escalonado. En metros el valor maximo es ~20 y el paso baja
 * a ~0.015 m (1,5 cm) en el peor caso, mas que suficiente. El material solo tiene
 * que multiplicar por 100 para volver a centimetros.
 *
 * POR QUE RELATIVO A LA BASE DEL TRONCO: es justo el origen al que el mallador
 * refiere la malla (el hero resta TrunkBaseWorld y el horneado resta OriginWorld,
 * que es la posicion del nodo raiz). Asi el pivote esta en el MISMO espacio local
 * que la posicion del vertice y el material puede restarlos directamente, sin
 * saber donde esta el arbol en el mundo ni cuanto mide.
 *
 * -----------------------------------------------------------------------------
 *  AO DE COPA (doc. 6.2)
 * -----------------------------------------------------------------------------
 * "AO por densidad de copa: oscurecer el interior/parte baja de la copa [...]
 *  alimentando el termino de AO con el campo de luz". La rejilla de luz FINA del
 * hero tree (FTreeLightGridFine) ya contiene exactamente esa informacion al
 * terminar el SCA: es la que produjo la autopoda. Reutilizarla para el AO cuesta
 * un muestreo trilineal por nodo, una sola vez al mallar, y ata el material al
 * campo de la simulacion (que es la coherencia que vende el documento).
 */
struct FTreeWindNode
{
    /** Pivote de la rama a la que pertenece el nodo, en CM y relativo a la base
        del tronco (Skeleton.Nodes[0].Pos). El mallador lo pasa a metros. */
    FVector PivotLocalCm = FVector::ZeroVector;

    /** 0 = tronco, 1 = la rama de mayor orden del arbol. */
    float BranchLevel01 = 0.f;

    /** Cuanto se mueve este punto con el viento: 0 en la base del tronco
        (empotrada), ~1 en la punta de una ramilla fina. */
    float SwayWeight = 0.f;

    /** Desfase [0,1) ESTABLE por rama: sin el, todas las ramas del arbol -y con
        la misma malla, todos los arboles- oscilarian al unisono. */
    float Phase01 = 0.f;

    /** Oclusion ambiental de copa: 1 = expuesto al cielo, 0 = interior en sombra. */
    float CanopyAO = 1.f;

    /** Variacion de tinte [0,1) estable por rama (color de vertice, canal G). */
    float TintVariation = 0.f;
};

/**
 * Atributos de viento/AO de TODOS los nodos de un esqueleto. Se calcula una vez,
 * justo antes de mallar, y el mallador lo copia a los vertices.
 *
 * Struct PLANO (no USTRUCT): es dato caliente y transitorio, igual que
 * FTreeSkeleton.
 */
struct PROCEDURALECOSYSTEM_API FTreeWindData
{
    /** Un elemento por nodo del esqueleto, en el mismo orden. */
    TArray<FTreeWindNode> Nodes;

    /** Indice de la rama a la que pertenece cada nodo (= indice del nodo que la
        inicia). Se expone porque el mallador lo usa para dar a cada leaf card el
        desfase de SU rama. */
    TArray<int32> BranchRoot;

    void Reset();

    bool IsValidFor(const FTreeSkeleton& Skeleton) const;

    /**
     * Calcula los atributos de todos los nodos.
     *
     * @param Skeleton   Esqueleto YA terminado (con ComputeRadii aplicado: los
     *                   radios se usan para saber que ramas son flexibles).
     * @param Species    Aporta WindStiffness (rigidez de la especie).
     * @param FineLight  Rejilla fina del SCA para el AO de copa. Puede ser
     *                   nullptr: entonces CanopyAO = 1 en todos los nodos.
     * @param Seed       Semilla del arbol -> los desfases son reproducibles y
     *                   distintos entre arboles (doc. 0, determinismo por-arbol).
     */
    void Build(const FTreeSkeleton& Skeleton, const USpeciesData& Species,
        const FTreeLightGridFine* FineLight, uint32 Seed);
};
