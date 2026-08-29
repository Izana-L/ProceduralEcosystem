#pragma once

#include "CoreMinimal.h"
#include "Ecology/CarbonModel.h" // Fase 6: multiplicador de CO2
#include "Config/EcosystemSettings.h" // EEcoVigorCombine (UENUM: tiene que vivir en cabecera reflejada)

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

    /**
     * Respuesta de UNA especie a la luz, ya resuelta contra los settings.
     * Mismo patron que FResourceResponse: se construye una vez con
     * MakeLightResponse y se evalua con LightFactor.
     */
    struct FLightResponse
    {
        float KlMax = 5.f;
        float ShadeTolerance = 0.f;

        /**
         * Capacidad fotosintetica MAXIMA de la especie, Amax(s) = 1 - c*s.
         *
         * Es el coste de la tolerancia a la sombra, y sin el ShadeTolerance es una
         * ventaja estrictamente monotona y gratuita: sube fL a cualquier nivel de
         * luz -tambien a pleno sol- sin costar nada en ninguna otra ecuacion. Con
         * el coste, la curva de la pionera y la de la tolerante SE CRUZAN, y esa es
         * la unica forma de que ninguna gane en todas partes.
         */
        float MaxAssimilation = 1.f;
    };

    /** Evalua la curva de luz con el coste de tolerancia aplicado. */
    FORCEINLINE float LightFactor(float Q, const FLightResponse& R)
    {
        return R.MaxAssimilation * LightFactor(Q, R.ShadeTolerance, R.KlMax);
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
     * DeficitWidth y ExcessWidth son las anchuras de las dos ramas. Que la de
     * exceso sea MAYOR es como se expresa "un suelo mas rico (o mas humedo) de lo
     * que la especie necesita la perjudica menos que quedarse corta" sin renunciar
     * a la unimodalidad, que es lo unico que reparte territorio entre especies.
     *
     * Resource, Optimum y Width van todos en las UNIDADES DEL CAMPO (los mismos
     * valores que devuelve SampleWater/SampleNutrient), no normalizados: la
     * conversion desde la fraccion que guarda el asset la hacen Make*Response.
     */
    FORCEINLINE float NicheFactor(float Resource, float Optimum, float DeficitWidth, float ExcessWidth)
    {
        const float Delta = Resource - Optimum;

        // Rama derecha (exceso) con su propia anchura: una campana ASIMETRICA sigue
        // siendo unimodal -y por tanto sigue repartiendo territorio- mientras deja
        // que pasarse de humedo o de fertil cueste menos que quedarse corto, que es
        // lo que se queria decir con "un suelo mas rico no hace dano".
        //
        // ExcessWidth <= 0 recorta la respuesta a 1 por encima del optimo. Es el
        // comportamiento anterior y NO es inocuo: la curva deja de ser unimodal y se
        // vuelve monotona no decreciente, con lo que un optimo mas bajo pasa a ser
        // mejor-o-igual en el 100% de las celdas. Se conserva solo para poder
        // reproducir corridas antiguas.
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

        /** Anchura de la rama por ENCIMA del optimo. <= 0 = saturar en 1 (ver NicheFactor). */
        float ExcessWidthAbs = 3.5f;

        bool  bUseNiche = false;
    };

    /** Solo la campana de tolerancia (sin decidir entre nicho y Monod). */
    FORCEINLINE float NicheOnly(float Available, const FResourceResponse& R)
    {
        return NicheFactor(Available, R.OptimumAbs, R.WidthAbs, R.ExcessWidthAbs);
    }

    /** Evalua la respuesta elegida. Unico punto donde se decide que curva se usa. */
    FORCEINLINE float ResourceFactor(float Available, const FResourceResponse& R)
    {
        return R.bUseNiche ? NicheOnly(Available, R) : MonodFactor(Available, R.Demand);
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

    PROCEDURALECOSYSTEM_API FLightResponse MakeLightResponse(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

    /**
     * Las TRES respuestas de una especie, resueltas de una vez.
     *
     * Se construyen una sola vez por tick -no por arbol: son identicas para todos
     * los individuos de la especie- y se pasan por valor al bucle paralelo, asi que
     * dentro del ParallelFor no se toca ni un UObject. Y garantizan que el tick, la
     * germinacion, el heatmap y la auditoria evaluen literalmente las mismas curvas.
     */
    struct FSpeciesResponses
    {
        FLightResponse    Light;
        FResourceResponse Water;
        FResourceResponse Nutrient;
    };

    PROCEDURALECOSYSTEM_API FSpeciesResponses MakeSpeciesResponses(
        const USpeciesData& Species, const UEcosystemSettings& Settings);

    /** Ley del minimo: el recurso mas escaso manda. */
    FORCEINLINE float Combine(float fL, float fW, float fN)
    {
        return FMath::Min3(fL, fW, fN);
    }

    /** Media geometrica de los tres factores: todos pesan, siempre. */
    FORCEINLINE float CombineGeometric(float fL, float fW, float fN)
    {
        const float Product = FMath::Max(fL, 0.f) * FMath::Max(fW, 0.f) * FMath::Max(fN, 0.f);
        return (Product > 0.f) ? FMath::Pow(Product, 1.f / 3.f) : 0.f;
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
     * =====================================================================
     *  EL UNICO SITIO DONDE SE EVALUA EL VIGOR
     * =====================================================================
     * Devuelve el vigor de una especie en un punto y, de paso, que recurso lo
     * limita. Lo llaman el tick, la germinacion y el horneado del heatmap: si
     * cada uno montara la formula por su cuenta, el mapa de idoneidad dejaria de
     * explicar por que el bosque crece donde crece en cuanto uno de los tres se
     * quedara atras. (Es el mismo motivo por el que LightFactor y MonodFactor
     * tienen una sola copia; aqui se extiende a la combinacion entera.)
     *
     * El LIMITANTE que devuelve es siempre el argmin de los tres factores, en
     * cualquier modo: la pregunta "que recurso esta frenando a este arbol" tiene
     * la misma respuesta se combinen luego como se combinen.
     *
     * NO aplica el multiplicador de CO2: ese depende de la altura de copa del
     * individuo, que no es una propiedad del punto. Lo pone el llamante.
     */
    FORCEINLINE float EvaluateVigor(
        float LightQ, float WaterAvailable, float NutrientAvailable,
        const FLightResponse& Light, const FResourceResponse& Water, const FResourceResponse& Nutrient,
        EEcoVigorCombine Mode, EEcoLimiter& OutLimiter)
    {
        const float fL = LightFactor(LightQ, Light);

        // Hibrido: el minimo decide entre SUMINISTROS (curvas Monod, recursos que
        // se consumen y se agotan) y las tolerancias multiplican aparte. Sin
        // respuesta de nicho activa no hay tolerancias que separar y degenera en
        // la ley del minimo, que es justo lo que debe hacer.
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

    /** Atajo sobre el paquete de respuestas de una especie. */
    FORCEINLINE float EvaluateVigor(float LightQ, float WaterAvailable, float NutrientAvailable,
        const FSpeciesResponses& R, EEcoVigorCombine Mode, EEcoLimiter& OutLimiter)
    {
        return EvaluateVigor(LightQ, WaterAvailable, NutrientAvailable, R.Light, R.Water, R.Nutrient, Mode, OutLimiter);
    }

    /**
     * Rellena un campo de IDONEIDAD (vigor en [0,1]) para una especie sobre toda
     * la rejilla del relieve: en cada nodo muestrea agua/nutrientes/luz al nivel
     * del suelo y evalua Liebig. Es lo que pinta el heatmap de la Fase 1.
     *
     * OJO A QUE CAMPOS SE LE PASAN. Water y Nutrient son FField2D crudos
     * precisamente para que el llamante elija: el campo BASE es el potencial del
     * terreno -congelado, optimista- mientras que el POOL es lo que los arboles
     * leen de verdad, ya deprimido por el consumo. La diferencia no es cosmetica:
     * un bosque denso puede bajar la mediana de nutrientes un 20%, y un mapa de
     * idoneidad calculado sobre el base declara "cada especie tiene su zona" sobre
     * un reparto que en el pool real ya no existe. Pasa SIEMPRE el pool cuando la
     * simulacion este corriendo.
     *
     * OutSuitability se reinicializa con la MISMA geometria que Height.Field, de
     * modo que el TArray<float> resultante encaja directamente en el
     * UFieldVisualizer (mismo tamano que el resto de campos).
     *
     * @param Responses   Las tres respuestas de la especie (MakeSpeciesResponses).
     * @param CombineMode El MISMO que usa el tick (settings). Ambos se pasan ya
     *                     construidos para que el heatmap evalue exactamente la
     *                     misma curva que el tick: si el mapa de idoneidad y el
     *                     crecimiento real usaran modelos distintos, el heatmap
     *                     dejaria de servir para explicar por que el bosque crece
     *                     donde crece.
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
        const FField2D& Water,
        const FField2D& Nutrient,
        const FLightFieldCoarse& Light,
        const FSpeciesResponses& Responses,
        EEcoVigorCombine CombineMode,
        FField2D& OutSuitability,
        TArray<uint8>* OutLimiter = nullptr,
        const EcoCarbon::FCO2Params* CO2 = nullptr);
}
