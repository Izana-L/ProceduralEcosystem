/**
 * @file EcologyRules.h
 * @author Juan Luque Roldán
 * @brief Catálogo de fórmulas puras del ciclo de vida de un árbol.
 *
 * Reúne, cada uno en su propia función, los pasos que el bucle de tick encadena:
 * crecimiento logístico, alometría altura-biomasa, acumulación de estrés, senescencia por
 * edad, supresión con histéresis, los dos canales de mortalidad y su composición,
 * fecundidad, dispersión, germinación e inhibición conespecífica, más el radio radicular
 * efectivo, el tope de extracción y el pulso de nutrientes de la descomposición. Declara
 * además el kernel radial de depósito y la reducción determinista del scratch, que se
 * implementan en EcologyRules.cpp.
 *
 * Ninguna función toca FTreePopulation, FSpatialHash ni los campos de recursos: solo
 * recibe y devuelve escalares, de modo que cada fórmula se puede comprobar aislada sin
 * montar una simulación entera.
 *
 * @ingroup eco_ecology
 * @see @ref bib_gapmodels
 */

#pragma once

#include "CoreMinimal.h"
#include "Core/EcoCore.h"
#include "Terrain/Field2D.h"
#include "Ecology/TickScratch.h"
#include "Ecology/Vigor.h"

/** Fórmulas del ciclo de vida: sin estado, sin dependencias del resto del módulo. */
namespace EcologyRules
{
    /** Factor de luz de la especie; reenvío a la única copia de la curva.
        @see EcoVigor::LightFactor */
    FORCEINLINE float LightFactor(float Q, float ShadeTolerance, float LightHalfSaturationMax)
    {
        return EcoVigor::LightFactor(Q, ShadeTolerance, LightHalfSaturationMax);
    }

    /**
     * Factor de agua o de nutrientes: los dos comparten la misma saturante de Monod.
     *
     * Available/Demand normaliza el recurso a «veces la necesidad de la especie» y la
     * curva x/(x+1) satura suavemente hacia 1 sin necesitar un tope duro. Reenvío a la
     * única copia de la fórmula, para que el heatmap de idoneidad y el tick respondan con
     * exactamente el mismo número.
     *
     * @see EcoVigor::MonodFactor
     */
    FORCEINLINE float DemandFactor(float Available, float Demand)
    {
        return EcoVigor::MonodFactor(Available, Demand);
    }

    /** Ley del mínimo de Liebig: el recurso más escaso limita el crecimiento; reenvío a
        la única copia de la regla de combinación. @see EcoVigor::Combine */
    FORCEINLINE float Vigor(float LightFactorValue, float WaterFactorValue, float NutrientFactorValue)
    {
        return EcoVigor::Combine(LightFactorValue, WaterFactorValue, NutrientFactorValue);
    }

    /**
     * Integra un paso de crecimiento logístico y devuelve la biomasa nueva:
     * @f$ B' = B + r\,V\,B\,(1 - B/B_{max})\,\Delta t @f$.
     *
     * Lento en plántula, rápido a media vida y saturante cerca de MaxBiomass. El vigor
     * modula la tasa intrínseca; el tick pasa GrowthRate ya multiplicado por los factores
     * de declive de la senescencia y de la supresión.
     *
     * @param VigorValue Vigor del árbol este tick, en [0,1].
     * @param DtYears    Paso de integración, en años.
     * @return Biomasa nueva, acotada a [0, MaxBiomass].
     * @note Euler explícito sin control de paso: la estabilidad la garantiza el recorte
     *       final.
     * @see DeclineGrowthFactor
     * @see @ref bib_verhulst1838
     */
    FORCEINLINE float GrowBiomassLogistic(float Biomass, float VigorValue, float GrowthRate,
        float MaxBiomass, float DtYears)
    {
        const float SafeMax = FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER);
        const float dB = GrowthRate * VigorValue * Biomass * (1.f - Biomass / SafeMax) * DtYears;
        return FMath::Clamp(Biomass + dB, 0.f, SafeMax);
    }

    /**
     * Fracción de altura adulta en [0,1]: raíz cúbica de la biomasa normalizada, porque
     * la biomasa escala con el volumen (~largo^3).
     *
     * Única copia de la alometría del proyecto: la usan HeightFromBiomass —que alimenta
     * la rejilla de luz y el espaciado de la germinación— y TreeArchetype::HeightRatio, que
     * reparte los buckets de LOD. Si divergieran, un árbol se vería de un tamaño y
     * sombrearía como otro.
     *
     * @see @ref bib_alometria
     */
    FORCEINLINE float HeightRatioFromBiomass(float Biomass, float MaxBiomass)
    {
        const float Ratio = FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);
        return FMath::Pow(Ratio, 1.f / 3.f);
    }

    /**
     * Altura del árbol en cm: la altura adulta de la especie por su fracción alométrica.
     *
     * Es la altura que el tick almacena en la población y con la que el árbol deposita su
     * sombra en la rejilla de luz; la geometría real del tronco y la copa la construye
     * aparte la capa de render.
     */
    FORCEINLINE float HeightFromBiomass(float Biomass, float MaxBiomass, float MaxHeightCm)
    {
        return MaxHeightCm * HeightRatioFromBiomass(Biomass, MaxBiomass);
    }

    /**
     * Avanza un paso el acumulador de estrés del árbol y lo devuelve acotado a [0,1].
     *
     * Sube proporcionalmente al déficit de vigor por debajo del umbral, se recupera a
     * tasa fija cuando hay vigor de sobra y en todo caso decae proporcionalmente a lo ya
     * acumulado.
     *
     * Ese decaimiento -k·S es lo que da a la ecuación un punto fijo intermedio,
     * @f$ S^* = (umbral - vigor)\,\alpha/k @f$: una rampa continua entre 0 y 1 que
     * traduce la calidad del sitio en riesgo de forma suave. Con k = 0 la función es un
     * integrador puro con tope y su punto fijo es un escalón —0 exacto con vigor por
     * encima del umbral, 1 por debajo—, de modo que dos sitios de calidad muy distinta
     * producen la misma demografía y dos especies separadas por centésimas de vigor
     * quedan separadas por una diferencia de mortalidad infinita: exclusión competitiva
     * fabricada por la forma de la función.
     *
     * @param StressThreshold Vigor por debajo del cual el estrés se acumula.
     * @param StressDecayRate Coeficiente k; con k > 0 hay punto fijo continuo.
     * @param DtYears         Paso de integración, en años.
     * @return Estrés nuevo, en [0,1].
     */
    FORCEINLINE float UpdateStress(float Stress, float VigorValue, float StressThreshold,
        float StressAccumulationRate, float StressRecoveryRate, float StressDecayRate, float DtYears)
    {
        if (VigorValue < StressThreshold)
        {
            Stress += (StressThreshold - VigorValue) * StressAccumulationRate * DtYears;
        }
        else
        {
            Stress -= StressRecoveryRate * DtYears;
        }

        Stress -= StressDecayRate * Stress * DtYears;
        return FMath::Clamp(Stress, 0.f, 1.f);
    }

    /**
     * Peso del estrés en la mortalidad, atenuado por la longevidad de la especie:
     * @f$ w = w_0 \left(L_{ref}/L\right)^{e} @f$.
     *
     * Los parámetros de estrés son globales, mientras que el tiempo que una especie
     * necesita para crecer es un rasgo por especie: un impuesto de mortalidad idéntico en
     * %/año penaliza linealmente a la especie lenta. La única compensación que le queda
     * —la longevidad— actúa sobre el canal de edad, que con vidas realizadas muy por
     * debajo de la nominal aporta una fracción despreciable del riesgo. Con esta
     * atenuación la longevidad compra además resistencia al estrés crónico, que es lo que
     * mantiene la estrategia K dentro del espacio de estrategias viables.
     *
     * @param RefLongevity Longevidad de referencia a la que el peso vale BaseWeight.
     * @param Exponent     Intensidad de la atenuación; <= 0 devuelve BaseWeight sin tocar.
     * @see @ref bib_estrategiark
     */
    FORCEINLINE float EffectiveStressMortalityWeight(float BaseWeight, float Longevity,
        float RefLongevity, float Exponent)
    {
        if (Exponent <= 0.f || Longevity <= 0.f) { return BaseWeight; }
        return BaseWeight * FMath::Pow(FMath::Max(RefLongevity, 1.f) / Longevity, Exponent);
    }

    /**
     * Riesgo de morir por edad en este tick: @f$ p = \Delta t\,(edad/L)^4 @f$, casi nulo
     * durante la mayor parte de la vida y disparado cerca de la longevidad de la especie.
     *
     * @return Probabilidad en [0,1].
     * @see @ref bib_weibull1951
     */
    FORCEINLINE float AgeMortalityProbability(float Age, float Longevity, float DtYears)
    {
        const float AgeRatio = Age / FMath::Max(Longevity, KINDA_SMALL_NUMBER);
        // Potencia entera por multiplicaciones: FMath::Pow(x, 4.f) pasa por exp/log y
        // esto se evalúa por cada árbol vivo y cada tick.
        const float AgeRatio2 = AgeRatio * AgeRatio;
        return FMath::Clamp(DtYears * AgeRatio2 * AgeRatio2, 0.f, 1.f);
    }

    /** Riesgo de morir por estrés crónico en este tick, proporcional al estrés acumulado
        y a su peso efectivo. @see EffectiveStressMortalityWeight */
    FORCEINLINE float StressMortalityProbability(float Stress, float StressMortalityWeight, float DtYears)
    {
        return FMath::Clamp(DtYears * Stress * StressMortalityWeight, 0.f, 1.f);
    }

    /** Compone dos riesgos que actúan a la vez y son independientes:
        @f$ P = 1 - (1-a)(1-b) @f$. @see @ref bib_kalbfleisch2002 */
    FORCEINLINE float CombineIndependentRisks(float A, float B)
    {
        return FMath::Clamp(1.f - (1.f - A) * (1.f - B), 0.f, 1.f);
    }

    /**
     * Probabilidad de morir este tick, combinando edad y estrés como causas
     * independientes.
     *
     * Las piezas quedan además disponibles por separado, para poder atribuir la muerte al
     * canal cuyo riesgo dominaba —que es lo que dice si la longevidad está comprando
     * algo— y para sustituir el canal de estrés por el hazard propio de un árbol
     * suprimido.
     *
     * @return Probabilidad en [0,1].
     * @see SuppressedMortalityProbability
     */
    FORCEINLINE float MortalityProbability(float Age, float Longevity, float Stress,
        float StressMortalityWeight, float DtYears)
    {
        return CombineIndependentRisks(
            AgeMortalityProbability(Age, Longevity, DtYears),
            StressMortalityProbability(Stress, StressMortalityWeight, DtYears));
    }

    /**
     * Senescencia por edad: el árbol rebasa SenescenceAgeFraction de su longevidad y
     * entra en declive.
     *
     * Cubre solo el envejecimiento, que es irreversible; el tick hace el estado pegajoso
     * con un OR contra el snapshot de lectura. El declive por estrés queda deliberadamente
     * fuera y vive en UpdateSuppression, que sí es reversible: si compartieran estado, una
     * plántula que pasara unos años suprimida quedaría marcada de por vida aunque después
     * se abriera un claro sobre ella, lo que prohíbe el banco de plántulas con el que una
     * especie tolerante hereda los huecos sin competir por número de semillas.
     *
     * @see UpdateSuppression
     */
    FORCEINLINE bool IsSenescentByAge(float Age, float Longevity, float SenescenceAgeFraction)
    {
        return Age >= SenescenceAgeFraction * FMath::Max(Longevity, KINDA_SMALL_NUMBER);
    }

    /**
     * Supresión por estrés con histéresis, a la manera de un disparador Schmitt: un árbol
     * sano entra al alcanzar el umbral de estrés de su especie y no sale hasta bajar de
     * ExitFraction veces ese umbral.
     *
     * @param bWasSuppressed Estado en el snapshot de lectura, inmutable durante todo el
     *                       tick: la decisión no depende del orden en que los hilos
     *                       procesen los árboles.
     * @param ExitFraction   Fracción del umbral de entrada a la que se sale, en [0,1].
     * @warning Los dos umbrales tienen que ser distintos (ExitFraction < 1) o el estado
     *          parpadea tick a tick.
     * @see @ref bib_schmitt1938
     */
    FORCEINLINE bool UpdateSuppression(bool bWasSuppressed, float Stress,
        float SpeciesStressThreshold, float ExitFraction)
    {
        return bWasSuppressed
            ? (Stress > SpeciesStressThreshold * FMath::Clamp(ExitFraction, 0.f, 1.f))
            : (Stress >= SpeciesStressThreshold);
    }

    /** Factor multiplicativo del crecimiento (1 = normal, ~0 = detenido). Lo comparten la
        senescencia por edad y la supresión por estrés, cada una con su propia escala. */
    FORCEINLINE float DeclineGrowthFactor(bool bDeclining, float GrowthScale)
    {
        return bDeclining ? FMath::Clamp(GrowthScale, 0.f, 1.f) : 1.f;
    }

    /**
     * Aplica a una probabilidad de muerte ya combinada el multiplicador de la senescencia
     * por edad. La supresión por estrés no pasa por aquí: tiene su propio hazard por
     * especie.
     *
     * @note El multiplicador se acota por abajo a 1: la senescencia nunca reduce el riesgo.
     * @see SuppressedMortalityProbability
     */
    FORCEINLINE float ApplySenescentMortality(float pDeath, bool bSenescent, float Multiplier)
    {
        return bSenescent ? FMath::Clamp(pDeath * FMath::Max(1.f, Multiplier), 0.f, 1.f) : pDeath;
    }

    /**
     * Riesgo de morir de un árbol suprimido: hazard anual propio de la especie
     * (USpeciesData::SuppressedMortalityPerYear) en lugar del canal de estrés general.
     *
     * Es el único rasgo de especie capaz de reducir el riesgo en vez de amplificarlo, y
     * completa el compromiso entre crecimiento y supervivencia: la tolerante puede esperar
     * décadas bajo el dosel y la pionera no.
     *
     * @see @ref bib_toleranciasombra
     */
    FORCEINLINE float SuppressedMortalityProbability(float PerYear, float DtYears)
    {
        return FMath::Clamp(PerYear * DtYears, 0.f, 1.f);
    }

    /**
     * Semillas emitidas este tick: Poisson de media
     * SeedsPerAdultPerYear * escala(biomasa relativa) * dt.
     *
     * La biomasa entra relativa a MaxBiomass y no en absoluto. En absoluto, MaxBiomass se
     * convierte en un multiplicador accidental de fecundidad —una especie de MaxBiomass
     * 150 produciría 3,75 veces más semillas que una de 40 con el mismo parámetro de
     * siembra, solo por ser más grande— y, como el tamaño ya da altura y por tanto sombra
     * sobre los vecinos, acabaría siendo un eje monótono con tres ventajas encadenadas
     * que anula cualquier compromiso entre rasgos. En relativo, el parámetro significa
     * «semillas al año de un adulto ya crecido», igual para todas las especies, y la
     * fecundidad vuelve a ser un rasgo explícito (USpeciesData::SeedRateScale).
     *
     * @param BiomassHalfSaturation Biomasa relativa a la que la escala vale 1/2;
     *                              <= 0 deja la escala lineal.
     * @param RngState Stream RNG del árbol, que el muestreo de Poisson avanza.
     * @return Número entero de semillas emitidas.
     * @see @ref bib_dispersionsemillas
     */
    FORCEINLINE int32 ComputeSeedCount(float SeedsPerAdultPerYear, float Biomass, float MaxBiomass,
        float BiomassHalfSaturation, float DtYears, uint32& RngState)
    {
        const float RelativeBiomass =
            FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);

        // Saturar corta la realimentación crecimiento -> semillas -> crecimiento, que en
        // proporcionalidad lineal convierte una ventaja de crecimiento moderada en dos
        // órdenes de magnitud de lluvia de semillas y esteriliza al árbol suprimido. Con
        // semisaturación, lo que enciende la reproducción es la madurez y no el tamaño.
        const float Scale = (BiomassHalfSaturation > 0.f)
            ? RelativeBiomass / (RelativeBiomass + BiomassHalfSaturation)
            : RelativeBiomass;

        return EcoRand::PoissonInt(RngState, SeedsPerAdultPerYear * Scale * DtYears);
    }

    /** Desplazamiento XY en cm de una semilla respecto a su árbol madre: disco uniforme
        por área, la misma copia que usa el scatter de hojarasca de la capa de suelo.
        @see @ref bib_discouniforme */
    FORCEINLINE FVector2D SampleSeedOffsetCm(float DispersalRadiusCm, uint32& RngState)
    {
        return EcoRand::SampleDiscOffsetCm(RngState, DispersalRadiusCm);
    }

    /** Probabilidad de germinar en el punto de caída, proporcional al vigor allí. */
    FORCEINLINE float GerminationProbability(float VigorAtSite, float GerminationRate)
    {
        return FMath::Clamp(VigorAtSite * GerminationRate, 0.f, 1.f);
    }

    /**
     * Inhibición de Janzen-Connell: @f$ f = 0.5^{\,n/n_{1/2}} @f$, la probabilidad de
     * arraigar se divide por dos cada HalfCount adultos conespecíficos alrededor.
     *
     * Decae exponencialmente y no satura, a diferencia de 1/(1+kn): con la forma
     * hiperbólica la corrección máxima queda acotada por la razón de densidades locales
     * por mucho que se suba k, y eso se queda corto cuando una especie es mil veces más
     * abundante que otra.
     *
     * @param Conspecifics Adultos reproductivos de la misma especie dentro del radio de
     *                     inhibición, excluida la madre de la semilla.
     * @param HalfCount    Conespecíficos que reducen el arraigo a la mitad; <= 0 desactiva
     *                     la inhibición y devuelve 1.
     * @see @ref bib_janzenconnell
     */
    FORCEINLINE float ConspecificInhibitionFactor(int32 Conspecifics, float HalfCount)
    {
        if (HalfCount <= 0.f || Conspecifics <= 0) { return 1.f; }
        return FMath::Pow(0.5f, static_cast<float>(Conspecifics) / HalfCount);
    }

    /** «Sitio seguro»: la luz en el punto de caída supera el mínimo de germinación de la
        especie. Es lo que hace del sotobosque un territorio donde la pionera no puede
        germinar por muchas semillas que mande. */
    FORCEINLINE bool IsSafeGerminationSite(float LightAtSite, float MinLightForGermination)
    {
        return LightAtSite >= MinLightForGermination;
    }

    /** Nutrientes que la descomposición devuelve al suelo cuando un árbol muere,
        proporcionales a su biomasa. */
    FORCEINLINE float DeathNutrientPulse(float Biomass, float NutrientDecompositionFactor)
    {
        return Biomass * NutrientDecompositionFactor;
    }

    /**
     * Zona de influencia radicular: radio de consumo en cm, el del árbol adulto escalado
     * por la biomasa relativa.
     *
     * USpeciesData declara RootRadius en metros y referido a un adulto, así que con
     * Biomass == MaxBiomass el radio efectivo son exactamente esos metros y un árbol joven
     * consume, proporcionalmente, en menos espacio.
     *
     * @param MinAdultRadiusCm Mínimo aplicado al radio del ADULTO, no al efectivo.
     * @return Radio de consumo en cm.
     * @see @ref bib_zonadeinfluencia
     */
    FORCEINLINE float EffectiveRootRadiusCm(float RootRadiusM, float Biomass, float MaxBiomass,
        float MinAdultRadiusCm)
    {
        const float Ratio = FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);

        // El mínimo va sobre el radio del adulto para que la plántula siga compitiendo en
        // un radio minúsculo y el árbol crecido alcance siempre a sus vecinos: si el radio
        // no supera el tamaño de celda, el peso lineal 1-d/R vale 0 exacto en los cuatro
        // vecinos ortogonales, el kernel escribe una sola celda y el recurso del suelo
        // deja de ser un bien común -pozos privados, sin competencia subterránea-.
        const float AdultRadiusCm = FMath::Max(RootRadiusM * 100.f, MinAdultRadiusCm);
        return AdultRadiusCm * Ratio;
    }

    /**
     * Recorta la demanda de un árbol a lo realmente extraíble de su zona radicular:
     * MaxFraction por el recurso disponible por celda y por las celdas al alcance.
     *
     * Sin tope, una demanda mayor que el recurso disponible se deposita como cantidad
     * negativa, esa deuda se difunde a las celdas vecinas bajándoles recurso real y
     * después se destruye al recortar el campo a cero: competencia por interferencia no
     * intencionada, cuya intensidad crece con la demanda sin coste para quien la ejerce, y
     * masa del campo que deja de conservarse.
     *
     * @param MaxFraction Fracción máxima del recurso presente que se puede extraer en un
     *                    tick; <= 0 desactiva el tope.
     * @return Cantidad POSITIVA; el signo lo pone el llamante.
     * @see @ref bib_tilman1982
     */
    FORCEINLINE float ClampUptakeToAvailable(float Demand, float AvailablePerCell, int32 CellsInRange,
        float MaxFraction)
    {
        if (MaxFraction <= 0.f) { return FMath::Max(Demand, 0.f); }
        const float Extractable = MaxFraction * FMath::Max(AvailablePerCell, 0.f) * FMath::Max(CellsInRange, 1);
        return FMath::Clamp(Demand, 0.f, Extractable);
    }

    /** Cota superior de celdas que toca el kernel para un radio dado, @f$ (2R+1)^2 @f$:
        con ella se dimensiona el scratch antes del paso paralelo.
        @see FTickScratch::ReserveForTrees */
    FORCEINLINE int32 KernelCellCount(const FField2D& Geometry, float RadiusCm)
    {
        if (RadiusCm <= 0.f || Geometry.CellSize <= 0.0) { return 1; }
        const int32 R = FMath::CeilToInt(RadiusCm / Geometry.CellSize);
        return (2 * R + 1) * (2 * R + 1);
    }

    /**
     * Reparte TotalAmount entre las celdas de Deltas que caen dentro de RadiusCm de
     * WorldPos, con un kernel radial lineal normalizado, de modo que lo depositado suma
     * exactamente TotalAmount pese al redondeo a celdas.
     *
     * Versión densa: escribe directamente sobre un campo completo.
     *
     * @param Geometry    Rejilla que describe Deltas; ha de tener el mismo tamaño que el
     *                    campo que se parcheará después.
     * @param TotalAmount Cantidad total a repartir; negativa si es consumo.
     * @warning Solo desde código serial: pulsos de muerte y campo de descomposición. El
     *          paso paralelo del tick usa DepositKernelSparse.
     */
    void DepositKernel(const FField2D& Geometry, TArray<float>& Deltas,
        const FVector& WorldPos, float RadiusCm, float TotalAmount);

    /**
     * Versión dispersa del kernel de depósito: en lugar de escribir sobre un campo del
     * tamaño del mundo, añade pares (celda, cantidad) a la lista de la tarea.
     *
     * Recorre las celdas exactamente en el mismo orden que la versión densa, así que la
     * reducción posterior es reproducible.
     *
     * @note Solo escribe en OutDeltas: es segura de llamar dentro de un ParallelFor.
     * @see FCellDelta
     */
    void DepositKernelSparse(const FField2D& Geometry, TArray<FCellDelta>& OutDeltas,
        const FVector& WorldPos, float RadiusCm, float TotalAmount);

    /**
     * Funde el scratch privado de todas las tareas en el estado compartido: aplica sobre
     * los arrays de destino los deltas de agua y de nutrientes y concatena semillas,
     * pulsos de muerte y embudos por especie.
     *
     * Recorre Contexts en orden de índice creciente —fijo, independiente del hilo físico
     * que ejecutó cada tarea y del número de núcleos— y, dentro de cada contexto, las
     * entradas en su orden de inserción. Como la suma en coma flotante no es asociativa,
     * ese orden fijo es lo que hace el tick reproducible bit a bit.
     *
     * @warning Debe llamarse siempre de forma serial, nunca dentro de un ParallelFor.
     */
    void ReduceScratchInto(const TArray<FTickScratch>& Contexts,
        TArray<float>& DestWater, TArray<float>& DestNutrient,
        TArray<FPendingSeed>& OutSeeds, TArray<FPendingDeathPulse>& OutDeathPulses,
        TArray<FEcoSpeciesFlow>& OutFlow);
}
