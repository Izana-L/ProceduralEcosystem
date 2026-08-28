#pragma once

#include "CoreMinimal.h"

class USpeciesData;
struct FLightFieldCoarse;
struct FTreeSkeleton;
struct FTreeLightGridFine;
struct FAttractorCloud;

/**
 * Ajustes del SCA que NO son parametros naturales de la especie (esos viven en
 * USpeciesData). Son knobs de la generacion en si; el actor hero puede tunearlos
 * sin tocar el asset de especie. Valores por defecto razonables.
 */
struct FSpaceColonizationConfig
{
    /** Q por debajo del cual un atractor se considera "en sombra" y se poda. */
    float LightCullThreshold = 0.15f;

    /** Margen (cm) de la rejilla fina alrededor de la envolvente de la copa. */
    float FineGridPaddingCm = 100.f;

    /** Radio de la sombra que deposita cada nodo = escala * D. */
    float LeafShadowRadiusScale = 1.0f;

    /** Profundidad de esa sombra hacia abajo = escala * D. */
    float LeafShadowDepthScale = 2.0f;

    /** Sombra que aporta cada nodo al follaje (se acumula). */
    float PerNodeShadowDensity = 0.05f;

    /** Usar el gradiente de luz de la rejilla fina como fototropismo (wPhot). */
    bool bEnablePhototropism = true;

    /** Activar la autopoda interna (refresco de luz cada LightEvery iters). */
    bool bEnableSelfPruning = true;

    /**
     * Maximo de hijos que puede acumular UN nodo en toda su vida.
     *
     * El paso CRECER recorre TODOS los nodos en cada iteracion, no solo las
     * puntas, y nada impedia que un nodo que sigue viendo atractores sacara un
     * hijo nuevo en cada una de las MaxIter pasadas: aparecian nodos con
     * decenas de hijos, todos partiendo del MISMO anillo de seccion al mallar.
     * Ese abanico de munones es el artefacto de "apice abierto". Un nodo
     * saturado deja de reclamar atractores, que pasan al siguiente nodo en
     * rango, en vez de quedarse bloqueados.
     *
     * A 2 la ramificacion es BINARIA, que es lo botanicamente tipico: el primer
     * hijo continua la rama y el segundo la bifurca. A 3 ya se lee como
     * deshilachado.
     */
    int32 MaxChildrenPerNode = 2;

    /**
     * Igual, pero para los nodos del EJE PRINCIPAL (BNF_Axis).
     *
     * El eje se pre-construye entero antes del bucle del SCA, asi que cada uno
     * de sus nodos interiores YA gasta un hijo en su propia continuacion. Con
     * el presupuesto general de 2 solo podria sacar una rama lateral y el
     * fuste saldria pelado; con 3 saca dos, que es lo que da un verticilo
     * decente sin volver al abanico.
     */
    int32 MaxAxisChildrenPerNode = 3;

    /**
     * IDENTIDAD DE DEFORMACION: fija que curvatura de tronco le toca a este
     * arbol, con independencia de la semilla con la que crece. -1 = derivarla de
     * la propia semilla del arbol (lo normal).
     *
     * Existe porque la curvatura tiene que sobrevivir a dos cosas que la semilla
     * de crecimiento no sobrevive:
     *   1. Envejecer. En la libreria de arquetipos la semilla incluye el bucket
     *      de edad, asi que sin este override un arbol cambiaria de recto a
     *      arqueado al cruzar de bucket -exactamente el fallo que el jitter de
     *      variante evita a mano en UTreeLibrary::GetArchetypeSpecies-.
     *   2. Promocionar a hero. Un arbol cercano regenera con semilla propia
     *      (Hash32(StableId)); sin igualar la deformacion, la instancia arqueada
     *      se convertiria en un hero recto delante del jugador.
     *
     * int64 y no uint32 para que el centinela -1 no colisione con una semilla
     * valida (0xFFFFFFFF es un uint32 perfectamente legitimo).
     */
    int64 DeformSeedOverride = -1;
};

/**
 * Motor del Space Colonization Algorithm (doc. Fase 3, 3.1/3.3/3.6).
 *
 * Namespace de funciones sobre estructuras que se le pasan (en la linea de
 * EcologyRules en la Fase 2): no guarda estado propio, todo entra y sale por
 * referencia, y las piezas puras (mezcla de direccion, jitter, pipe model) se
 * pueden testear sueltas.
 *
 * DETERMINISMO: toda decision estocastica (siembra de atractores, jitter de
 * direccion) sale del RngState que se pasa -> mismo arbol, misma semilla,
 * misma geometria (doc. 3.8).
 */
namespace SpaceColonization
{
    /**
     * Mezcla ponderada de la direccion de crecimiento de un nodo (doc. 3.4):
     *   dir = wSCA*DirSCA + wGrav*Up + wPhot*LightGradient + wPrev*DirPrev
     * Devuelta NORMALIZADA (fallback a DirSCA si el resultado es degenerado).
     * El gravitropismo es hacia arriba (tallo); pasa LightGradient = ZeroVector
     * para desactivar el fototropismo.
     */
    PROCEDURALECOSYSTEM_API FVector BlendGrowthDirection(
        const FVector& DirSCA, const FVector& DirPrev, const FVector& LightGradient,
        float wSCA, float wGrav, float wPhot, float wPrev);

    /**
     * Perturba una direccion unitaria por un angulo pequeno gobernado por
     * NoiseAmount [0..1] (0 = sin cambio). Reproducible desde RngState. Da la
     * variacion por-arbol y evita esqueletos identicos.
     */
    PROCEDURALECOSYSTEM_API FVector JitterDirection(const FVector& Dir, float NoiseAmount, uint32& RngState);

    /**
     * Fuerza que una rama lateral se SEPARE de la direccion de su padre al
     * menos MinAngleDeg (doc: angulo de insercion). Si Dir ya se separa lo
     * suficiente se devuelve tal cual; si no, se gira dentro del plano que
     * forman Dir y ParentDir, que es el giro minimo posible y por tanto el que
     * menos estropea la direccion que eligio el SCA.
     *
     * Solo debe aplicarse al PRIMER nodo de la rama: aplicado a toda la cadena
     * impediria que el SCA la dirigiese hacia sus atractores.
     */
    PROCEDURALECOSYSTEM_API FVector ApplyBranchAngle(const FVector& Dir, const FVector& ParentDir, float MinAngleDeg);

    /**
     * Radios de rama por pipe model (doc. 3.6): r_padre^n = sum r_hijo^n. Una
     * pasada de las puntas a la base aprovechando la invariante de FTreeSkeleton
     * (Parent < indice): recorrer en indice decreciente visita hijos antes que
     * padres, sin ordenar. Las puntas arrancan con TipRadiusCm.
     */
    PROCEDURALECOSYSTEM_API void ComputeRadii(FTreeSkeleton& Skeleton, const USpeciesData& Species);

    /**
     * Perfil de tronco sobre los radios del pipe model (ensanche de base +
     * afilado del eje). Escribe FBranchNode::Radius y NO toca PipeRadius, que
     * es la referencia estructural que consumen los datos de viento.
     *
     * POR QUE HACE FALTA: el pipe model da r_padre = r_hijo EXACTAMENTE en una
     * cadena sin bifurcaciones, asi que el tronco desnudo salia como un
     * cilindro perfecto. El pipe model no esta mal -describe la madera
     * funcional-, simplemente no describe la geometria externa de un tronco,
     * que acumula corteza y albura y ensancha en el pie para repartir el
     * momento de vuelco.
     *
     * Termina con una pasada de MONOTONIA (ningun nodo mas fino que su hijo
     * mas grueso). Sin ella, el afilado puede dejar el eje mas fino que el
     * primer nodo de copa y aparece un estrangulamiento en cono invertido
     * justo bajo la copa.
     */
    PROCEDURALECOSYSTEM_API void ApplyTrunkProfile(FTreeSkeleton& Skeleton, const USpeciesData& Species);

    /**
     * Genera el esqueleto de UN arbol por colonizacion del espacio.
     *
     * Flujo (doc. 3.3):
     *   1. Sembrar atractores en la copa (FAttractorCloud::SampleCrownEnvelope).
     *   2. Rejilla fina sobre la envolvente + sombra de vecinos (SeedFromCoarse)
     *      -> cull inicial (micro<-macro): el arbol "ve" a sus vecinos.
     *   3. Bucle asociar -> crecer -> matar, con tropismos + jitter, y refresco
     *      periodico de luz para la autopoda interna.
     *   4. ComputeRadii (pipe model) sobre el esqueleto terminado.
     *
     * OutSkeleton es la salida principal (la consume el mallador). OutFineLight
     * y OutAttractors se dejan en su estado final para que el actor hero pueda
     * dibujarlos como debug (ver la copa evitar la sombra).
     *
     * @param CoarseLight  Grid grueso global de la Fase 2 (sombra de vecinos).
     *                     Puede ser nullptr / invalido: entonces el arbol crece
     *                     sin contexto de vecinos (demo aislada).
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