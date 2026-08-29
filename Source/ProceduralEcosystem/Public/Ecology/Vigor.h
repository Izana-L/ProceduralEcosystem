#pragma once

#include "CoreMinimal.h"
#include "Ecology/CarbonModel.h" // Fase 6: multiplicador de CO2

struct FField2D;
struct FHeightField;
struct FWaterField;
struct FNutrientField;
struct FLightFieldCoarse;
class  USpeciesData;
class  UEcosystemSettings;

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
 * FASE 6 (doc. 6.3): sobre ese minimo se aplica ademas un multiplicador
 * analitico de CO2 (Ecology/CarbonModel.h), que baja levemente el vigor dentro
 * de un dosel denso y a poca altura. Se pasa como puntero OPCIONAL: si es
 * nullptr no se aplica, de modo que todo el codigo anterior a la Fase 6 sigue
 * dando exactamente los mismos numeros.
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

    // =====================================================================
    //  NICHO DE RECURSO: respuesta UNIMODAL
    // =====================================================================
    // MonodFactor es MONOTONA CRECIENTE: "mas agua es mejor" es cierto para
    // todas las especies a la vez. Con esa forma, dos especies NUNCA pueden
    // repartirse un gradiente de humedad -solo cambia CUANTO les gusta el sitio
    // humedo, no CUAL es su sitio-, y el resultado es que la mejor en el eje
    // gana en todo el mapa: exclusion competitiva garantizada por la forma de la
    // curva, antes incluso de mirar los numeros.
    //
    // La respuesta unimodal le da a cada especie un OPTIMO y una ANCHURA. La de
    // vaguada gana en la vaguada y la de ladera seca en la ladera, y la
    // coexistencia se sostiene sola porque cada una es la mejor EN ALGUN SITIO.
    // Eso es lo que convierte la heterogeneidad que ya calcula la Fase 1 (TWI
    // del agua, fBm de los nutrientes) en un tablero con varias casillas
    // ganadoras en vez de en un ranking global.

    /**
     * Campana gaussiana centrada en Optimum: vale 1 en el optimo y cae a los dos
     * lados con escala Width (a una anchura del optimo queda en e^-1 = 0.37).
     *
     * bPenalizeExcess=false deja la respuesta SATURADA en 1 por encima del
     * optimo. Es la forma correcta para los nutrientes: un suelo mas rico de lo
     * que la especie necesita no la perjudica. Para el agua si conviene
     * penalizar (encharcamiento y anoxia radicular son reales, y ademas son lo
     * que impide que la especie de vaguada colonice tambien la cresta).
     *
     * Resource, Optimum y Width van todos en las UNIDADES DEL CAMPO (los mismos
     * valores que devuelve SampleWater/SampleNutrient), no normalizados: la
     * conversion desde la fraccion que guarda el asset la hacen Make*Response.
     */
    FORCEINLINE float NicheFactor(float Resource, float Optimum, float Width, bool bPenalizeExcess)
    {
        const float SafeWidth = FMath::Max(Width, KINDA_SMALL_NUMBER);
        float z = (Resource - Optimum) / SafeWidth;
        if (!bPenalizeExcess && z > 0.f) { z = 0.f; }
        return FMath::Exp(-z * z);
    }

    /**
     * Respuesta de UNA especie a UN recurso, ya resuelta a las unidades del
     * campo. Se construye con Make*Response y se evalua con ResourceFactor.
     *
     * Lleva los dos modelos a la vez a proposito: bUseNiche sale de
     * UEcosystemSettings::bUseNicheResponse, asi que se puede comparar A/B el
     * comportamiento nuevo con el anterior cambiando UNA clave del .ini, sin
     * tocar assets ni recompilar.
     */
    struct FResourceResponse
    {
        /** Solo se usa con bUseNiche=false (modelo Monod anterior). */
        float Demand = 1.f;

        /** Optimo y anchura en unidades del campo (ya multiplicados por su maximo). */
        float OptimumAbs = 5.f;
        float WidthAbs = 3.5f;

        bool  bPenalizeExcess = true;
        bool  bUseNiche = false;
    };

    /** Evalua la respuesta elegida. Unico punto donde se decide que curva se usa. */
    FORCEINLINE float ResourceFactor(float Available, const FResourceResponse& R)
    {
        return R.bUseNiche
            ? NicheFactor(Available, R.OptimumAbs, R.WidthAbs, R.bPenalizeExcess)
            : MonodFactor(Available, R.Demand);
    }

    /**
     * Construyen la respuesta de una especie resolviendo las fracciones del asset
     * contra WaterOutputMax / NutrientOutputMax de los settings.
     *
     * El asset guarda FRACCIONES [0..1] y no valores absolutos justamente para
     * que cambiar el rango de salida de un campo no invalide en silencio la
     * calibracion de las especies.
     */
    PROCEDURALECOSYSTEM_API FResourceResponse MakeWaterResponse(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

    PROCEDURALECOSYSTEM_API FResourceResponse MakeNutrientResponse(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

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
     * Rellena un campo de IDONEIDAD (vigor en [0,1]) para una especie sobre toda
     * la rejilla del relieve: en cada nodo muestrea agua/nutrientes/luz al nivel
     * del suelo y evalua Liebig. Es lo que pinta el heatmap de la Fase 1.
     *
     * OutSuitability se reinicializa con la MISMA geometria que Height.Field, de
     * modo que el TArray<float> resultante encaja directamente en el
     * UFieldVisualizer (mismo tamano que el resto de campos).
     *
     * @param WaterResponse     Respuesta de la especie al agua (Make*Response).
     * @param NutrientResponse  Idem para nutrientes. Se pasan YA CONSTRUIDAS para
     *                     que el heatmap evalue exactamente la misma curva que el
     *                     tick: si el mapa de idoneidad y el crecimiento real
     *                     usaran modelos distintos, el heatmap dejaria de servir
     *                     para explicar por que el bosque crece donde crece.
     * @param OutLimiter  Opcional: si no es null, se rellena con el limitante por
     *                     nodo (0=luz,1=agua,2=nutrientes) para un mapa cualitativo.
     * @param CO2         Fase 6: si no es nullptr se aplica el multiplicador de
     *                     CO2 (doc. 6.3) a ras de suelo, igual que hace el tick.
     *                     Pasarlo es importante para que el heatmap siga
     *                     representando el MISMO numero que usa el tick; si no,
     *                     el mapa de idoneidad y el crecimiento real dejarian
     *                     de cuadrar.
     */
    PROCEDURALECOSYSTEM_API void BakeSuitabilityField(
        const FHeightField& Height,
        const FWaterField& Water,
        const FNutrientField& Nutrient,
        const FLightFieldCoarse& Light,
        const USpeciesData& Species,
        float KlMax,
        const FResourceResponse& WaterResponse,
        const FResourceResponse& NutrientResponse,
        FField2D& OutSuitability,
        TArray<uint8>* OutLimiter = nullptr,
        const EcoCarbon::FCO2Params* CO2 = nullptr);
}
