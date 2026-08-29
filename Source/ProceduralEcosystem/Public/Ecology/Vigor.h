/**
 * @file Vigor.h
 * @author Juan Luque Roldán
 * @brief Función de vigor: el acoplamiento entre los campos de recursos y el crecimiento.
 *
 * Reúne las curvas de respuesta de una especie a la luz, al agua y a los nutrientes y
 * la regla que las combina en el escalar de [0,1] que modula el crecimiento, alimenta
 * el acumulador de estrés y decide la germinación. Cada curva tiene aquí su única
 * copia —saturante de Monod, con semisaturación dependiente de la tolerancia a la
 * sombra en el caso de la luz, o campana de nicho unimodal para agua y nutrientes— y
 * EcoVigor::EvaluateVigor es el único punto del proyecto donde se evalúa el vigor, de
 * modo que el tick, la germinación y el heatmap de idoneidad respondan con el mismo
 * número. Todo inline salvo los Make*Response y el bake, que viven en Vigor.cpp.
 *
 * @ingroup eco_ecology
 * @see @ref bib_liebig1840
 * @see @ref bib_monod1949
 * @see @ref bib_nichounimodal
 * @see @ref bib_toleranciasombra
 */

#pragma once

#include "CoreMinimal.h"
#include "Ecology/CarbonModel.h" // EcoCarbon::FCO2Params (multiplicador de CO2)
#include "Config/EcosystemSettings.h" // EEcoVigorCombine (UENUM: tiene que vivir en cabecera reflejada)

struct FField2D;
struct FHeightField;
struct FWaterField;
struct FNutrientField;
struct FLightFieldCoarse;
class  USpeciesData;

/** Recurso que está limitando el vigor en un punto: el argmin de los tres factores. */
enum class EEcoLimiter : uint8
{
    Light,
    Water,
    Nutrient
};

/**
 * Curvas de respuesta a los recursos y regla que las combina en el vigor.
 *
 * @code
 * fL = Amax * Q / (Q + KlMax*(1 - ShadeTolerance) + LightEps)   luz
 * fW = (W/WaterDemand)    / ((W/WaterDemand)    + 1)            agua       (Monod)
 * fN = (N/NutrientDemand) / ((N/NutrientDemand) + 1)            nutrientes (Monod)
 * vigor = min(fL, fW, fN)                                       ley del mínimo
 * @endcode
 *
 * Con la respuesta de nicho activa, fW y fN pasan a ser campanas unimodales
 * (@ref NicheFactor) y el modo de combinación lo elige @ref EvaluateVigor. Un árbol en
 * suelo rico pero a la sombra sigue limitado por la luz, y distintas zonas del paisaje
 * quedan limitadas por distinto recurso (@ref EEcoLimiter). Factores y vigor viven en
 * [0,1]; el multiplicador de CO2 (@ref EcoCarbon::CO2Factor) se aplica fuera, sobre el
 * resultado, porque depende del individuo y no del punto.
 */
namespace EcoVigor
{
    /** Épsilon del denominador de la curva de luz: evita 0/0 en oscuridad total. */
    static constexpr float LightEps = 1e-4f;

    /**
     * Factor de luz con tolerancia a la sombra, @f$ f_L = Q/(Q + K_l + \varepsilon) @f$
     * con @f$ K_l = K_{lMax}(1 - s) @f$: una especie tolerante (s→1) satura con poca
     * luz y una heliófila (s→0) necesita Q alto.
     *
     * @param Q     Luz disponible en el punto, ya atenuada por el dosel.
     * @param KlMax Semisaturación de la especie menos tolerante a la sombra.
     * @return Factor en [0,1), sin el techo de asimilación de la especie.
     */
    FORCEINLINE float LightFactor(float Q, float ShadeTolerance, float KlMax)
    {
        const float Kl = KlMax * (1.f - ShadeTolerance);
        return Q / (Q + Kl + LightEps);
    }

    /**
     * Respuesta de UNA especie a la luz, ya resuelta contra los settings. Mismo patrón
     * que FResourceResponse: se construye una vez con @ref MakeLightResponse y se
     * evalúa con @ref LightFactor.
     */
    struct FLightResponse
    {
        /** Semisaturación de la especie menos tolerante, en unidades de luz. */
        float KlMax = 5.f;

        /** Tolerancia a la sombra de la especie, en [0,1]. */
        float ShadeTolerance = 0.f;

        /**
         * Capacidad fotosintética máxima de la especie, @f$ A_{max}(s) = 1 - c\,s @f$.
         *
         * Es el coste de la tolerancia a la sombra. Sin él, ShadeTolerance sería una
         * ventaja estrictamente monótona y gratuita: sube fL a cualquier nivel de luz
         * —también a pleno sol— sin costar nada en ninguna otra ecuación. Con el coste,
         * las curvas de la pionera y de la tolerante se cruzan, que es la única forma
         * de que ninguna gane en todas partes.
         */
        float MaxAssimilation = 1.f;
    };

    /** Evalúa la curva de luz con el coste de la tolerancia aplicado. */
    FORCEINLINE float LightFactor(float Q, const FLightResponse& R)
    {
        return R.MaxAssimilation * LightFactor(Q, R.ShadeTolerance, R.KlMax);
    }

    /** Factor de recurso saturante (Monod). Vale 0.5 cuando Resource == Demand. */
    FORCEINLINE float MonodFactor(float Resource, float Demand)
    {
        // Demand llega con ClampMin>0 desde USpeciesData; el máximo es el cinturón de
        // seguridad para una especie construida en código sin pasar por la validación.
        const float R = Resource / FMath::Max(Demand, KINDA_SMALL_NUMBER);
        return R / (R + 1.f);
    }

    /** Factor de agua: Monod sobre la demanda hídrica de la especie. */
    FORCEINLINE float WaterFactor(float W, float WaterDemand) { return MonodFactor(W, WaterDemand); }

    /** Factor de nutrientes: Monod sobre la demanda de nutrientes de la especie. */
    FORCEINLINE float NutrientFactor(float N, float NutrientDemand) { return MonodFactor(N, NutrientDemand); }

    // =====================================================================
    //  Respuesta de nicho: campana unimodal frente a saturante
    // =====================================================================
    // MonodFactor es monótona creciente —«más agua es mejor» vale para todas las
    // especies a la vez—, así que la mejor en el eje gana en todo el mapa y la
    // exclusión competitiva queda decidida por la forma de la curva antes de mirar
    // los números. La campana unimodal da a cada especie un óptimo y una anchura: la
    // de vaguada gana en la vaguada y la de ladera seca en la ladera. Es lo que
    // convierte la heterogeneidad del terreno (TWI del agua, fBm de los nutrientes)
    // en un tablero con varias casillas ganadoras en vez de en un ranking global.

    /**
     * Campana gaussiana centrada en Optimum: vale 1 en el óptimo y cae a los dos lados
     * con escala Width (a una anchura del óptimo queda en @f$ e^{-1} = 0{,}37 @f$).
     *
     * Las dos ramas tienen anchura propia. Que la de exceso sea mayor expresa que un
     * suelo más rico —o más húmedo— de lo que la especie necesita la perjudica menos
     * que quedarse corta, sin renunciar a la unimodalidad, que es lo que reparte
     * territorio entre especies.
     *
     * @param Resource     Valor del campo en el punto, en unidades del campo.
     * @param Optimum      Óptimo de la especie, en las mismas unidades.
     * @param DeficitWidth Anchura de la rama por debajo del óptimo.
     * @param ExcessWidth  Anchura de la rama por encima; <= 0 satura la respuesta en 1.
     * @return Factor en (0,1].
     * @note Nada de esto va normalizado: la conversión desde la fracción que guarda el
     *       asset a unidades del campo la hacen los Make*Response.
     */
    FORCEINLINE float NicheFactor(float Resource, float Optimum, float DeficitWidth, float ExcessWidth)
    {
        const float Delta = Resource - Optimum;

        // Rama de exceso con anchura propia: la campana asimétrica sigue siendo
        // unimodal, y por tanto sigue repartiendo territorio.
        //
        // ExcessWidth <= 0 recorta la respuesta a 1 por encima del óptimo y no es
        // inocuo: la curva se vuelve monótona no decreciente y un óptimo más bajo pasa
        // a ser mejor-o-igual en el 100% de las celdas. Solo sirve para reproducir
        // corridas hechas con esa configuración.
        if (Delta > 0.f)
        {
            if (ExcessWidth <= 0.f) { return 1.f; }
            const float z = Delta / ExcessWidth;
            return FMath::Exp(-z * z);
        }

        const float z = Delta / FMath::Max(DeficitWidth, KINDA_SMALL_NUMBER);
        return FMath::Exp(-z * z);
    }

    /**
     * Respuesta de UNA especie a UN recurso, ya resuelta a las unidades del campo. Se
     * construye con Make*Response y se evalúa con @ref ResourceFactor.
     *
     * Lleva los dos modelos a la vez a propósito: bUseNiche sale de
     * UEcosystemSettings::bUseNicheResponse, así que comparar campana y saturante es
     * cambiar una clave del .ini, sin tocar assets ni recompilar.
     */
    struct FResourceResponse
    {
        /** Demanda de la especie; solo interviene con bUseNiche=false (curva de Monod). */
        float Demand = 1.f;

        /** Óptimo de la especie en unidades del campo (la fracción del asset por su máximo). */
        float OptimumAbs = 5.f;

        /** Anchura de la rama por DEBAJO del óptimo, en las mismas unidades. */
        float WidthAbs = 3.5f;

        /** Anchura de la rama por ENCIMA del óptimo; <= 0 satura en 1 (@ref NicheFactor). */
        float ExcessWidthAbs = 3.5f;

        /** Curva elegida: campana de nicho si es true, saturante de Monod si no. */
        bool  bUseNiche = false;
    };

    /** Solo la campana de tolerancia, sin decidir entre nicho y Monod. */
    FORCEINLINE float NicheOnly(float Available, const FResourceResponse& R)
    {
        return NicheFactor(Available, R.OptimumAbs, R.WidthAbs, R.ExcessWidthAbs);
    }

    /** Evalúa la respuesta elegida: único punto donde se decide qué curva se usa. */
    FORCEINLINE float ResourceFactor(float Available, const FResourceResponse& R)
    {
        return R.bUseNiche ? NicheOnly(Available, R) : MonodFactor(Available, R.Demand);
    }

    /**
     * Construyen la respuesta de una especie resolviendo las fracciones del asset
     * contra WaterOutputMax / NutrientOutputMax de los settings.
     *
     * El asset guarda fracciones de [0,1] y no valores absolutos precisamente para que
     * cambiar el rango de salida de un campo no invalide en silencio la calibración de
     * las especies.
     */
    PROCEDURALECOSYSTEM_API FResourceResponse MakeWaterResponse(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

    PROCEDURALECOSYSTEM_API FResourceResponse MakeNutrientResponse(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

    PROCEDURALECOSYSTEM_API FLightResponse MakeLightResponse(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

    /**
     * Las tres respuestas de una especie, resueltas de una vez.
     *
     * Se construyen una vez por tick —no por árbol: son idénticas para todos los
     * individuos de la especie— y se pasan por valor al bucle paralelo, de modo que
     * dentro del ParallelFor no se toca ni un UObject. Garantizan además que el tick,
     * la germinación, el heatmap y la auditoría evalúen las mismas curvas.
     */
    struct FSpeciesResponses
    {
        FLightResponse    Light;
        FResourceResponse Water;
        FResourceResponse Nutrient;
    };

    PROCEDURALECOSYSTEM_API FSpeciesResponses MakeSpeciesResponses(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

    /** Ley del mínimo: el recurso más escaso manda. */
    FORCEINLINE float Combine(float fL, float fW, float fN)
    {
        return FMath::Min3(fL, fW, fN);
    }

    /**
     * Media geométrica de los tres factores: todos pesan siempre y, con el exponente
     * 1/3, se conserva la escala del vigor (tres factores de 0.6 dan 0.6), de modo que
     * los umbrales de estrés y los GrowthRate de los assets siguen calibrados.
     */
    FORCEINLINE float CombineGeometric(float fL, float fW, float fN)
    {
        const float Product = FMath::Max(fL, 0.f) * FMath::Max(fW, 0.f) * FMath::Max(fN, 0.f);
        return (Product > 0.f) ? FMath::Pow(Product, 1.f / 3.f) : 0.f;
    }

    /**
     * Igual que @ref Combine, pero devuelve además cuál de los tres es el limitante.
     * @param OutLimiter Recibe el argmin; los empates se resuelven en el orden luz,
     *                   agua, nutrientes.
     */
    FORCEINLINE float CombineWithLimiter(float fL, float fW, float fN, EEcoLimiter& OutLimiter)
    {
        float V = fL; OutLimiter = EEcoLimiter::Light;
        if (fW < V) { V = fW; OutLimiter = EEcoLimiter::Water; }
        if (fN < V) { V = fN; OutLimiter = EEcoLimiter::Nutrient; }
        return V;
    }

    /**
     * Único punto del proyecto donde se evalúa el vigor: devuelve la calidad del sitio
     * para una especie y, de paso, qué recurso la limita. Lo llaman el tick, la
     * germinación y el horneado del heatmap; si cada uno montara la fórmula por su
     * cuenta, el mapa de idoneidad dejaría de explicar por qué el bosque crece donde
     * crece en cuanto uno de los tres se quedara atrás.
     *
     * El limitante es siempre el argmin de los tres factores, sea cual sea el modo: la
     * pregunta «qué recurso frena a este árbol» tiene una única respuesta.
     *
     * @param Mode       Modo de combinación. El híbrido MinSupplyTimesNiche exige
     *                   respuesta de nicho activa y, sin ella, degenera en Minimum.
     * @param OutLimiter Recibe el recurso limitante.
     * @return Vigor en [0,1].
     * @note No aplica el multiplicador de CO2: depende de la altura de copa del
     *       individuo, que no es una propiedad del punto. Lo pone el llamante.
     */
    FORCEINLINE float EvaluateVigor(
        float LightQ, float WaterAvailable, float NutrientAvailable,
        const FLightResponse& Light, const FResourceResponse& Water, const FResourceResponse& Nutrient,
        EEcoVigorCombine Mode, EEcoLimiter& OutLimiter)
    {
        const float fL = LightFactor(LightQ, Light);

        // Híbrido: el mínimo decide entre SUMINISTROS (curvas de Monod, recursos que se
        // consumen y se agotan) y las TOLERANCIAS multiplican aparte, porque son aptitud
        // y no disponibilidad. Sin respuesta de nicho no hay tolerancias que separar.
        if (Mode == EEcoVigorCombine::MinSupplyTimesNiche && Water.bUseNiche)
        {
            const float fWs = MonodFactor(WaterAvailable, Water.Demand);
            const float fNs = MonodFactor(NutrientAvailable, Nutrient.Demand);
            const float Supply = CombineWithLimiter(fL, fWs, fNs, OutLimiter);
            return Supply * NicheOnly(WaterAvailable, Water) * NicheOnly(NutrientAvailable, Nutrient);
        }

        const float fW = ResourceFactor(WaterAvailable, Water);
        const float fN = ResourceFactor(NutrientAvailable, Nutrient);
        const float MinValue = CombineWithLimiter(fL, fW, fN, OutLimiter);

        return (Mode == EEcoVigorCombine::GeometricMean)
            ? CombineGeometric(fL, fW, fN)
            : MinValue;
    }

    /** Atajo sobre el paquete de respuestas de una especie (@ref FSpeciesResponses). */
    FORCEINLINE float EvaluateVigor(float LightQ, float WaterAvailable, float NutrientAvailable,
        const FSpeciesResponses& R, EEcoVigorCombine Mode, EEcoLimiter& OutLimiter)
    {
        return EvaluateVigor(LightQ, WaterAvailable, NutrientAvailable, R.Light, R.Water, R.Nutrient, Mode, OutLimiter);
    }

    /**
     * Hornea el campo de idoneidad de una especie sobre la rejilla del relieve: en cada
     * nodo muestrea agua, nutrientes y luz a ras de suelo y evalúa el vigor. Es el campo
     * que pinta el heatmap de idoneidad.
     *
     * OutSuitability se reinicializa con la misma geometría que Height.Field, así que el
     * TArray resultante encaja en el UFieldVisualizer junto al resto de campos.
     *
     * @param Water       Campo de agua a muestrear (ver la advertencia).
     * @param Nutrient    Campo de nutrientes, con el mismo criterio.
     * @param Responses   Las tres respuestas de la especie (@ref MakeSpeciesResponses).
     * @param CombineMode El mismo modo que usa el tick; con otro, el mapa deja de
     *                    explicar el crecimiento real.
     * @param OutLimiter  Opcional: limitante por nodo (0=luz, 1=agua, 2=nutrientes).
     * @param CO2         Opcional: aplica el multiplicador de CO2 a ras de suelo, igual
     *                    que hace el tick. Pasarlo mantiene el heatmap y el crecimiento
     *                    real sobre el mismo número.
     * @warning Water y Nutrient son FField2D crudos para que el llamante elija: el campo
     *          base es el potencial del terreno, congelado y optimista, y el pool es lo
     *          que los árboles leen de verdad, ya deprimido por el consumo. Con la
     *          simulación corriendo hay que pasar el pool; un bosque denso puede haber
     *          bajado la mediana de nutrientes un 20%, y sobre el base el mapa declara
     *          un reparto de nicho que ya no existe.
     */
    PROCEDURALECOSYSTEM_API void BakeSuitabilityField(
        const FHeightField& Height,
        const FField2D& Water,
        const FField2D& Nutrient,
        const FLightFieldCoarse& Light,
        const FSpeciesResponses& Responses,
        EEcoVigorCombine CombineMode,
        FField2D& OutSuitability,
        TArray<uint8>* OutLimiter = nullptr,
        const EcoCarbon::FCO2Params* CO2 = nullptr);
}
