#pragma once

#include "CoreMinimal.h"

struct FField2D;
struct FHeightField;
struct FWaterField;
struct FNutrientField;
struct FLightFieldCoarse;
class  USpeciesData;

/**
 * Funcion de VIGOR: el acoplamiento entre los campos de recursos (agua,
 * nutrientes, luz) y el crecimiento. Es "el pegamento" de la Fase 1 y la
 * funcion central que consumira toda la simulacion de la Fase 2.
 *
 * Ley del minimo de Liebig (documento de diseno, 2.6):
 *
 *     fL = Q / (Q + KlMax*(1 - ShadeTolerance) + eps)     luz (tol. a la sombra)
 *     fW = (W/WaterDemand)   / ((W/WaterDemand)   + 1)     agua       (Monod)
 *     fN = (N/NutrientDemand)/ ((N/NutrientDemand)+ 1)     nutrientes (Monod)
 *     vigor = min(fL, fW, fN)          <- el recurso mas ESCASO limita
 *
 * Un arbol en suelo rico pero a la sombra sigue limitado por la luz -> realista,
 * y ademas genera variedad: distintas zonas del paisaje quedan limitadas por
 * distinto recurso (ver EEcoLimiter).
 *
 * Todo son floats: los factores viven en [0,1] y el vigor tambien. Las tres
 * funciones de factor son inline (header) porque la Fase 2 las llamara por
 * agente en el bucle caliente; mantenerlas identicas aqui y alli garantiza que
 * el heatmap de idoneidad y el crecimiento real usen EXACTAMENTE la misma
 * formula.
 */

/** Que recurso esta limitando el vigor en un punto (el argmin de Liebig). */
enum class EEcoLimiter : uint8
{
    Light,
    Water,
    Nutrient
};

namespace EcoVigor
{
    /** Epsilon del denominador de luz (evita 0/0 en oscuridad total). Igual al del documento. */
    static constexpr float LightEps = 1e-4f;

    /**
     * Factor de luz con tolerancia a la sombra. Kl = KlMax*(1-ShadeTolerance):
     * una especie tolerante (ShadeTolerance->1) satura con poca luz; una
     * heliofila (->0) necesita Q alto. Q es la luz disponible tras la sombra.
     */
    FORCEINLINE float LightFactor(float Q, float ShadeTolerance, float KlMax)
    {
        const float Kl = KlMax * (1.f - ShadeTolerance);
        return Q / (Q + Kl + LightEps);
    }

    /** Factor de recurso saturante (Monod). Vale 0.5 cuando Resource == Demand. */
    FORCEINLINE float MonodFactor(float Resource, float Demand)
    {
        // Demand llega ya con ClampMin>0 desde USpeciesData; el max es un cinturon
        // de seguridad por si alguien construye un species en codigo sin validar.
        const float R = Resource / FMath::Max(Demand, KINDA_SMALL_NUMBER);
        return R / (R + 1.f);
    }

    FORCEINLINE float WaterFactor(float W, float WaterDemand) { return MonodFactor(W, WaterDemand); }
    FORCEINLINE float NutrientFactor(float N, float NutrientDemand) { return MonodFactor(N, NutrientDemand); }

    /** Ley del minimo: el recurso mas escaso manda. */
    FORCEINLINE float Combine(float fL, float fW, float fN)
    {
        return FMath::Min3(fL, fW, fN);
    }

    /** Igual que Combine pero ademas dice CUAL de los tres es el limitante. */
    FORCEINLINE float CombineWithLimiter(float fL, float fW, float fN, EEcoLimiter& OutLimiter)
    {
        float V = fL; OutLimiter = EEcoLimiter::Light;
        if (fW < V) { V = fW; OutLimiter = EEcoLimiter::Water; }
        if (fN < V) { V = fN; OutLimiter = EEcoLimiter::Nutrient; }
        return V;
    }

    /**
     * Vigor en una posicion de mundo. Muestrea los tres campos y aplica Liebig.
     * Esta es la forma de Fase 1 del VigorAt(pos, especie, W, N, L) del diseno.
     *
     * @param WorldPos  Punto en mundo (cm). La Z importa: la luz se muestrea en 3D,
     *                  asi que pasa la Z del suelo (o de la copa) segun el caso.
     * @param KlMax     Semisaturacion de luz global (heliofila); ver EcosystemSettings.
     */
    PROCEDURALECOSYSTEM_API float VigorAtWorld(
        const FVector& WorldPos,
        const USpeciesData& Species,
        const FWaterField& Water,
        const FNutrientField& Nutrient,
        const FLightFieldCoarse& Light,
        float KlMax,
        EEcoLimiter* OutLimiter = nullptr);

    /**
     * Rellena un campo de IDONEIDAD (vigor en [0,1]) para una especie sobre toda
     * la rejilla del relieve: en cada nodo muestrea agua/nutrientes/luz al nivel
     * del suelo y evalua Liebig. Es lo que pinta el heatmap de la Fase 1.
     *
     * OutSuitability se reinicializa con la MISMA geometria que Height.Field, de
     * modo que el TArray<float> resultante encaja directamente en el
     * UFieldVisualizer (mismo tamano que el resto de campos).
     *
     * @param OutLimiter  Opcional: si no es null, se rellena con el limitante por
     *                     nodo (0=luz,1=agua,2=nutrientes) para un mapa cualitativo.
     */
    PROCEDURALECOSYSTEM_API void BakeSuitabilityField(
        const FHeightField& Height,
        const FWaterField& Water,
        const FNutrientField& Nutrient,
        const FLightFieldCoarse& Light,
        const USpeciesData& Species,
        float KlMax,
        FField2D& OutSuitability,
        TArray<uint8>* OutLimiter = nullptr);
}
