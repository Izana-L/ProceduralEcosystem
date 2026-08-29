/**
 * @file SpaceColonization.h
 * @author Juan Luque Roldán
 * @brief API del motor de colonización del espacio: del asset de especie al esqueleto de ramas.
 *
 * Declara el namespace SpaceColonization, que produce el esqueleto de un árbol haciendo
 * crecer nodos hacia una nube de atractores sembrada en la envolvente de copa, y le
 * asigna radios por pipe model y perfil de tronco. Es un namespace de funciones sobre
 * las estructuras que se le pasan por referencia: no guarda estado, con lo que las
 * piezas puras —mezcla de tropismos, jitter, ángulo de inserción, radios— se prueban
 * sueltas. Declara además FSpaceColonizationConfig, los ajustes de la generación que no
 * son parámetros naturales de la especie. Toda decisión estocástica sale del RngState
 * que recibe: misma especie, misma semilla y misma posición dan la misma geometría.
 *
 * @ingroup eco_geometry
 * @see @ref bib_runions2007
 * @see @ref bib_prusinkiewicz1990
 * @see @ref bib_shinozaki1964
 * @see @ref bib_metzger1893
 */

#pragma once

#include "CoreMinimal.h"

class USpeciesData;
struct FLightFieldCoarse;
struct FTreeSkeleton;
struct FTreeLightGridFine;
struct FAttractorCloud;

/**
 * Ajustes de la generación que no son parámetros naturales de la especie.
 *
 * Los rasgos botánicos (radios del algoritmo, tropismos, forma de copa) viven en
 * USpeciesData y son los mismos para toda la especie; aquí quedan los mandos de la
 * generación en sí, que el actor de un árbol concreto puede tocar sin modificar el
 * asset compartido. Los valores por defecto son los de producción.
 */
struct FSpaceColonizationConfig
{
    /** Luz disponible Q por debajo de la cual un atractor se considera en sombra y se poda. */
    float LightCullThreshold = 0.15f;

    /** Margen en cm de la rejilla de luz fina alrededor de la envolvente de copa. */
    float FineGridPaddingCm = 100.f;

    /** Radio de la sombra que deposita cada nodo, en múltiplos del paso @f$D@f$. */
    float LeafShadowRadiusScale = 1.0f;

    /** Alcance hacia abajo de esa sombra, en múltiplos del paso @f$D@f$. */
    float LeafShadowDepthScale = 2.0f;

    /** Sombra que aporta cada nodo con follaje; se acumula por vóxel. */
    float PerNodeShadowDensity = 0.05f;

    /** Usar el gradiente de la rejilla de luz fina como fototropismo (peso wPhot). */
    bool bEnablePhototropism = true;

    /** Activar la autopoda: refresco de luz y poda de atractores cada LightEvery iteraciones. */
    bool bEnableSelfPruning = true;

    /**
     * Máximo de hijos que un nodo puede acumular en toda su vida.
     *
     * El paso CRECER recorre todos los nodos en cada iteración, no solo las puntas: sin
     * este tope, un nodo que siga viendo atractores emite un hijo nuevo en cada una de
     * las MaxIter pasadas y acaba con decenas de hijos que parten del mismo anillo de
     * sección al mallar, el abanico de muñones del ápice abierto. Un nodo saturado deja
     * de competir por atractores, que pasan al siguiente nodo en rango en vez de
     * quedarse bloqueados.
     *
     * A 2 la ramificación es binaria, que es lo botánicamente típico: el primer hijo
     * continúa la rama y el segundo la bifurca. A 3 la copa ya se lee deshilachada.
     */
    int32 MaxChildrenPerNode = 2;

    /**
     * Igual, pero para los nodos del eje principal (BNF_Axis).
     *
     * El eje se pre-construye entero antes del bucle, así que cada uno de sus nodos
     * interiores ya gasta un hijo en su propia continuación. Con el presupuesto general
     * de 2 solo le queda una rama lateral y el fuste sale pelado; con 3 saca dos, que
     * es lo que da un verticilo sin volver al abanico.
     *
     * @note Se toma siempre el máximo con MaxChildrenPerNode: el eje nunca tiene menos
     *       presupuesto que una rama corriente.
     */
    int32 MaxAxisChildrenPerNode = 3;

    /**
     * Identidad de deformación: fija qué curvatura de tronco le toca a este árbol, con
     * independencia de la semilla con la que crece. -1 la deriva de la propia semilla,
     * que es el caso normal.
     *
     * La curvatura tiene que sobrevivir a dos cambios de semilla que ocurren sobre un
     * árbol que ya está en pantalla. Al envejecer: la semilla de un arquetipo de
     * librería incluye el bucket de edad, así que sin el override un árbol pasa de
     * recto a arqueado al cruzar de bucket. Y al promocionar a hero tree: el árbol
     * cercano regenera con semilla propia, derivada de su identificador estable, y sin
     * igualar la deformación una instancia arqueada se convierte en un hero tree recto
     * delante del jugador.
     *
     * @note Es int64 y no uint32 para que el centinela -1 no colisione con una semilla
     *       válida: 0xFFFFFFFF es un uint32 perfectamente legítimo.
     */
    int64 DeformSeedOverride = -1;
};

/**
 * Motor de colonización del espacio: construye el esqueleto de un árbol y sus radios.
 *
 * Funciones sobre estructuras que se le pasan por referencia, sin estado propio, en la
 * misma línea que @ref EcologyRules: todo entra y sale por parámetro, de modo que las
 * piezas puras —mezcla de direcciones, jitter, ángulo de inserción, pipe model, perfil
 * de tronco— se pueden ejercitar y verificar sueltas, y solo @ref GrowTree las orquesta.
 *
 * Toda decisión estocástica —siembra de atractores y jitter de dirección— sale del
 * RngState que recibe @ref GrowTree, así que una especie, una semilla y una posición
 * determinan la geometría por completo.
 */
namespace SpaceColonization
{
    /**
     * Mezcla ponderada de los tropismos que fijan hacia dónde crece un nodo.
     *
     * @code
     * dir = wSCA*DirSCA + wGrav*Up + wPhot*LightGradient + wPrev*DirPrev
     * @endcode
     *
     * @param DirSCA        Media de las direcciones a los atractores asignados al nodo.
     * @param DirPrev       Dirección del nodo padre; aporta la inercia del eje.
     * @param LightGradient Gradiente de la luz disponible; ZeroVector desactiva el
     *                      fototropismo.
     * @return Dirección unitaria. El gravitropismo del tallo apunta hacia arriba; si la
     *         suma se cancela se devuelve DirSCA, o la vertical si también es nula.
     */
    PROCEDURALECOSYSTEM_API FVector BlendGrowthDirection(
        const FVector& DirSCA, const FVector& DirPrev, const FVector& LightGradient,
        float wSCA, float wGrav, float wPhot, float wPrev);

    /**
     * Perturba una dirección unitaria para que dos ramas en la misma situación no
     * crezcan idénticas.
     *
     * @param NoiseAmount Intensidad en [0,1]; 0 devuelve la dirección sin cambio.
     * @param RngState    Estado del generador; se consume y avanza, de ahí que el
     *                    resultado sea reproducible.
     */
    PROCEDURALECOSYSTEM_API FVector JitterDirection(const FVector& Dir, float NoiseAmount, uint32& RngState);

    /**
     * Fuerza el ángulo de inserción: una rama lateral tiene que separarse de la
     * dirección de su padre al menos MinAngleDeg.
     *
     * Si Dir ya se separa lo suficiente se devuelve tal cual. Si no, se gira dentro del
     * plano que forman Dir y ParentDir, que es el giro mínimo capaz de alcanzar el
     * ángulo pedido y por tanto el que menos estropea la dirección elegida por los
     * atractores.
     *
     * @warning Solo debe aplicarse al primer nodo de una rama lateral. Aplicado a toda
     *          la cadena impediría que la rama se dirigiese hacia sus atractores.
     */
    PROCEDURALECOSYSTEM_API FVector ApplyBranchAngle(const FVector& Dir, const FVector& ParentDir, float MinAngleDeg);

    /**
     * Radios de rama por pipe model: @f$r_{padre}^{\,n} = \sum r_{hijo}^{\,n}@f$, con
     * @f$n@f$ = Species.PipeExp.
     *
     * Una sola pasada de las puntas a la base, sin ordenar ni recursión: por la
     * invariante de FTreeSkeleton (Parent < índice del hijo), recorrer en índice
     * decreciente visita cada hijo antes que su padre. Las puntas arrancan con
     * TipRadiusCm.
     *
     * @post Escribe a la vez FBranchNode::Radius y FBranchNode::PipeRadius con el mismo
     *       valor; @ref ApplyTrunkProfile pisa después solo el primero.
     */
    PROCEDURALECOSYSTEM_API void ComputeRadii(FTreeSkeleton& Skeleton, const USpeciesData& Species);

    /**
     * Perfil externo del fuste sobre los radios del pipe model: ensanche de pie y
     * afilado con la altura.
     *
     * El pipe model da @f$r_{padre} = r_{hijo}@f$ exactamente en una cadena sin
     * bifurcaciones, con lo que el tronco desnudo queda como un cilindro perfecto. No
     * es un error: describe la madera funcional, no la geometría externa de un fuste,
     * que acumula corteza y albura y ensancha en el pie para repartir el momento de
     * vuelco. Este paso añade esa capa geométrica encima.
     *
     * Termina con una pasada de monotonía —ningún nodo más fino que su hijo más
     * grueso—, sin la cual el afilado puede dejar el eje más fino que el primer nodo de
     * copa y aparece un estrangulamiento en cono invertido justo bajo la copa.
     *
     * @pre Llamar después de @ref ComputeRadii: parte de PipeRadius.
     * @post Escribe FBranchNode::Radius y deja PipeRadius intacto, que es la referencia
     *       estructural que consultan los datos de viento.
     */
    PROCEDURALECOSYSTEM_API void ApplyTrunkProfile(FTreeSkeleton& Skeleton, const USpeciesData& Species);

    /**
     * Genera el esqueleto completo de un árbol por colonización del espacio.
     *
     * @par Secuencia
     * @li Sembrar la nube de atractores en la envolvente de copa.
     * @li Dimensionar la rejilla de luz fina sobre esa envolvente, sembrarla con la
     *     sombra de los vecinos y podar los atractores que ya nacen en sombra: es la
     *     vía por la que el árbol «ve» a sus vecinos.
     * @li Pre-construir el eje principal (tronco desnudo y líder).
     * @li Iterar asociar, crecer y matar, con tropismos y jitter, refrescando la luz
     *     cada LightEvery iteraciones para la autopoda de la copa interior.
     * @li Doblar el esqueleto terminado (deformación de tronco), aplicar el pipe model
     *     y rematar con el perfil de tronco.
     *
     * @param RngState      Estado del generador; se consume y avanza. El mallado
     *                      posterior parte del estado en que lo deja esta llamada.
     * @param TrunkBaseWorld Posición en mundo del pie del tronco.
     * @param CoarseLight   Rejilla de luz gruesa global, de la que sale la sombra de
     *                      los vecinos. Puede ser nullptr o no estar inicializada: el
     *                      árbol crece entonces sin contexto, que es como se hornean
     *                      los arquetipos de librería.
     * @param OutSkeleton   Salida principal: la consume el mallador.
     * @param OutFineLight  Rejilla de luz fina en su estado final.
     * @param OutAttractors Nube de atractores en su estado final. Junto con la anterior
     *                      se conserva para poder dibujarlas como depuración.
     */
    PROCEDURALECOSYSTEM_API void GrowTree(
        const USpeciesData& Species,
        uint32& RngState,
        const FVector& TrunkBaseWorld,
        const FLightFieldCoarse* CoarseLight,
        const FSpaceColonizationConfig& Config,
        FTreeSkeleton& OutSkeleton,
        FTreeLightGridFine& OutFineLight,
        FAttractorCloud& OutAttractors);
}