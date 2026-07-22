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
     * Radios de rama por pipe model (doc. 3.6): r_padre^n = sum r_hijo^n. Una
     * pasada de las puntas a la base aprovechando la invariante de FTreeSkeleton
     * (Parent < indice): recorrer en indice decreciente visita hijos antes que
     * padres, sin ordenar. Las puntas arrancan con TipRadiusCm.
     */
    PROCEDURALECOSYSTEM_API void ComputeRadii(FTreeSkeleton& Skeleton, const USpeciesData& Species);

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