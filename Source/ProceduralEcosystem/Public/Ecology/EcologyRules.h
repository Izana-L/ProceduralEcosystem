#pragma once

#include "CoreMinimal.h"
#include "Core/EcoCore.h"
#include "Terrain/Field2D.h"
#include "Ecology/TickScratch.h"
#include "Ecology/Vigor.h"
/**
 * Formulas puras del nucleo biologico (doc. Fase 2, seccion 2.6). Cada
 * funcion es un paso nombrado del pseudocodigo del documento: se pueden
 * testear por separado y se combinan en el bucle de tick.
 *
 * Deliberadamente NO tocan FTreePopulation, FSpatialHash ni los campos de
 * recursos: solo reciben floats y devuelven floats. Eso hace trivial
 * comprobar cada formula de forma aislada (ver Private/Test/EcoTests.cpp)
 * sin montar una simulacion entera.
 */
namespace EcologyRules
{
    /** fL = Q / (Q + Kl), Kl = LightHalfSaturationMax*(1-ShadeTolerance). Curva de saturacion (Monod). */
    FORCEINLINE float LightFactor(float Q, float ShadeTolerance, float LightHalfSaturationMax)
    {
        return EcoVigor::LightFactor(Q, ShadeTolerance, LightHalfSaturationMax);
    }

    /**
     * fW y fN comparten la misma forma matematica (Michaelis-Menten /
     * Monod): Available/Demand normaliza el recurso a "veces la necesidad
     * de la especie", y la curva x/(x+1) satura suavemente hacia 1 sin
     * necesitar un tope duro. Delega en EcoVigor (igual que LightFactor):
     * una sola copia de la formula garantiza que el heatmap de idoneidad y
     * el tick usen EXACTAMENTE el mismo numero.
     */
    FORCEINLINE float DemandFactor(float Available, float Demand)
    {
        return EcoVigor::MonodFactor(Available, Demand);
    }

    /** Ley del minimo de Liebig: el recurso mas escaso limita el crecimiento.
        Delega en EcoVigor::Combine (unica copia, mismo motivo que arriba). */
    FORCEINLINE float Vigor(float LightFactorValue, float WaterFactorValue, float NutrientFactorValue)
    {
        return EcoVigor::Combine(LightFactorValue, WaterFactorValue, NutrientFactorValue);
    }

    /**
     * Crecimiento logistico: lento en plantula (B pequeno), rapido a media
     * vida, saturante cerca de MaxBiomass (el termino 1-B/MaxBiomass tiende
     * a 0). Devuelve la NUEVA biomasa, ya sumado el incremento.
     */
    FORCEINLINE float GrowBiomassLogistic(float Biomass, float VigorValue, float GrowthRate,
        float MaxBiomass, float DtYears)
    {
        const float SafeMax = FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER);
        const float dB = GrowthRate * VigorValue * Biomass * (1.f - Biomass / SafeMax) * DtYears;
        return FMath::Clamp(Biomass + dB, 0.f, SafeMax);
    }

    /**
     * Fraccion de altura adulta en [0,1]: raiz cubica de la biomasa normalizada
     * (la biomasa escala aprox. con el volumen, ~largo^3). UNICA copia de la
     * alometria: la usan HeightFromBiomass (tick / grid de luz) y
     * TreeArchetype::HeightRatio (buckets de LOD). Si divergieran, un arbol se
     * "veria" de un tamano y sombrearia como otro.
     */
    FORCEINLINE float HeightRatioFromBiomass(float Biomass, float MaxBiomass)
    {
        const float Ratio = FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);
        return FMath::Pow(Ratio, 1.f / 3.f);
    }

    /**
     * Altura visual a partir de la biomasa: H = MaxHeight * ratio^(1/3). Es una
     * aproximacion deliberadamente burda para la Fase 2 (solo alimenta el grid
     * de luz y el tamano de las esferas de debug); el Pipe Model de la Fase 3
     * la sustituye por geometria real.
     */
    FORCEINLINE float HeightFromBiomass(float Biomass, float MaxBiomass, float MaxHeightCm)
    {
        return MaxHeightCm * HeightRatioFromBiomass(Biomass, MaxBiomass);
    }

    /**
     * Estres: sube proporcional al deficit de vigor bajo el umbral, se recupera si
     * hay vigor de sobra, y DECAE proporcionalmente a lo acumulado. Acotado a [0,1].
     *
     * EL TERMINO DE DECAIMIENTO (-k*S) NO ES COSMETICO. Sin el la funcion es un
     * integrador puro con tope, y su punto fijo es un ESCALON: con vigor por encima
     * del umbral el estres cae a 0 exacto (mortalidad por estres nula, arbol
     * inmortal por ese canal), y con vigor por debajo sube sin freno hasta 1
     * (mortalidad plena). No existe ningun estres de equilibrio intermedio, asi que
     * dos sitios de calidad muy distinta -vigor 0.44 y 0.90- producen exactamente la
     * misma demografia, y dos especies separadas por unas centesimas de vigor en el
     * mismo punto quedan separadas por una diferencia de mortalidad infinita. Eso es
     * exclusion competitiva fabricada por la FORMA de la funcion.
     *
     * Con k > 0 el punto fijo pasa a ser S* = (Umbral - vigor)*Acumulacion/k: una
     * rampa continua entre 0 y 1 que hace que la calidad del sitio se traduzca en
     * riesgo de forma suave y monotona, que es lo que se queria decir con "estres".
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
     * Peso efectivo del estres en la mortalidad, atenuado por la longevidad:
     *
     *     peso = BaseWeight * (RefLongevity / Longevity)^Exponent
     *
     * Los parametros de estres son GLOBALES mientras el tiempo que una especie
     * necesita para crecer es un rasgo POR ESPECIE, asi que un impuesto de
     * mortalidad en %/ano identico para todas penaliza linealmente a la lenta. Y la
     * unica compensacion que el modelo le ofrecia -la longevidad- actua sobre la
     * mortalidad por EDAD, que con una vida realizada muy por debajo de la nominal
     * aporta una fraccion despreciable del riesgo: en la practica la longevidad no
     * compraba nada y la estrategia K quedaba fuera del espacio de estrategias
     * viables. Aqui la longevidad compra por fin resistencia al estres cronico, que
     * es como se paga en un bosque real.
     */
    FORCEINLINE float EffectiveStressMortalityWeight(float BaseWeight, float Longevity,
        float RefLongevity, float Exponent)
    {
        if (Exponent <= 0.f || Longevity <= 0.f) { return BaseWeight; }
        return BaseWeight * FMath::Pow(FMath::Max(RefLongevity, 1.f) / Longevity, Exponent);
    }

    /**
     * Riesgo de morir por EDAD en este tick. Crece con la 4a potencia de
     * Age/Longevity: casi nulo la mayor parte de la vida, se dispara cerca de la
     * longevidad de la especie.
     */
    FORCEINLINE float AgeMortalityProbability(float Age, float Longevity, float DtYears)
    {
        const float AgeRatio = Age / FMath::Max(Longevity, KINDA_SMALL_NUMBER);
        // Potencia entera con multiplicaciones: FMath::Pow(x, 4.f) usa exp/log
        // y esto se ejecuta por CADA arbol vivo en CADA tick.
        const float AgeRatio2 = AgeRatio * AgeRatio;
        return FMath::Clamp(DtYears * AgeRatio2 * AgeRatio2, 0.f, 1.f);
    }

    /** Riesgo de morir por ESTRES cronico en este tick. */
    FORCEINLINE float StressMortalityProbability(float Stress, float StressMortalityWeight, float DtYears)
    {
        return FMath::Clamp(DtYears * Stress * StressMortalityWeight, 0.f, 1.f);
    }

    /** Dos causas INDEPENDIENTES: P(morir) = 1 - (1-a)(1-b). */
    FORCEINLINE float CombineIndependentRisks(float A, float B)
    {
        return FMath::Clamp(1.f - (1.f - A) * (1.f - B), 0.f, 1.f);
    }

    /**
     * Probabilidad de morir este tick, combinando edad y estres como causas
     * INDEPENDIENTES. Composicion de los tres helpers de arriba: una sola copia de
     * cada formula, y las piezas quedan disponibles por separado para poder
     * ATRIBUIR la muerte a un canal u otro (que es lo que dice si la longevidad
     * esta haciendo algo) y para sustituir el canal de estres por el hazard propio
     * de un arbol suprimido.
     */
    FORCEINLINE float MortalityProbability(float Age, float Longevity, float Stress,
        float StressMortalityWeight, float DtYears)
    {
        return CombineIndependentRisks(
            AgeMortalityProbability(Age, Longevity, DtYears),
            StressMortalityProbability(Stress, StressMortalityWeight, DtYears));
    }

    /**
     * Senescencia por EDAD: el arbol rebasa una fraccion de su longevidad y entra en
     * declive. Es irreversible, y con razon: el envejecimiento no se revierte.
     *
     * ANTES ESTA FUNCION MEZCLABA DOS COSAS. Tambien devolvia true por estres alto, y
     * el llamante hacia el estado pegajoso con un OR sobre el tick anterior, con lo
     * que el declive por estres tambien resultaba irreversible: una plantula que
     * pasara unos anos suprimida quedaba marcada de por vida -crecimiento reducido y
     * mortalidad multiplicada- aunque despues se abriera un claro sobre ella. Eso
     * prohibe explicitamente el banco de plantulas, que es el mecanismo por el que
     * una especie tolerante hereda los huecos sin competir por numero de semillas.
     * El declive por estres vive ahora en UpdateSuppression, y es reversible.
     */
    FORCEINLINE bool IsSenescentByAge(float Age, float Longevity, float SenescenceAgeFraction)
    {
        return Age >= SenescenceAgeFraction * FMath::Max(Longevity, KINDA_SMALL_NUMBER);
    }

    /**
     * Supresion por ESTRES, con histeresis: un arbol sano entra al superar el umbral
     * de su especie y no sale hasta bajar de ExitFraction veces ese umbral. Los dos
     * umbrales tienen que ser distintos o el estado parpadea tick a tick.
     *
     * bWasSuppressed sale del snapshot de LECTURA, que es inmutable durante todo el
     * tick: asi la decision no depende del orden en que los hilos procesen a los
     * arboles y sigue siendo determinista bajo paralelismo.
     */
    FORCEINLINE bool UpdateSuppression(bool bWasSuppressed, float Stress,
        float SpeciesStressThreshold, float ExitFraction)
    {
        return bWasSuppressed
            ? (Stress > SpeciesStressThreshold * FMath::Clamp(ExitFraction, 0.f, 1.f))
            : (Stress >= SpeciesStressThreshold);
    }

    /** Factor multiplicativo del crecimiento (1 = normal, ~0 = detenido). Lo comparten
        la senescencia por edad y la supresion por estres, cada una con su escala. */
    FORCEINLINE float DeclineGrowthFactor(bool bDeclining, float GrowthScale)
    {
        return bDeclining ? FMath::Clamp(GrowthScale, 0.f, 1.f) : 1.f;
    }

    /** Aplica el multiplicador de mortalidad de la senescencia POR EDAD a una pDeath
        ya calculada. La supresion por estres NO usa esto: tiene su propio hazard por
        especie (ver USpeciesData::SuppressedMortalityPerYear), que puede ser MENOR
        que el normal -es lo que hace viable esperar decadas bajo el dosel-. */
    FORCEINLINE float ApplySenescentMortality(float pDeath, bool bSenescent, float Multiplier)
    {
        return bSenescent ? FMath::Clamp(pDeath * FMath::Max(1.f, Multiplier), 0.f, 1.f) : pDeath;
    }

    /** Probabilidad de morir de un arbol SUPRIMIDO: hazard propio de la especie, en
        vez del canal de estres general. Es la mitad que faltaba del compromiso r/K
        -la climacica aguanta la penumbra, la pionera no-, y hasta ahora ningun rasgo
        de especie podia REDUCIR el riesgo, solo amplificarlo. */
    FORCEINLINE float SuppressedMortalityProbability(float PerYear, float DtYears)
    {
        return FMath::Clamp(PerYear * DtYears, 0.f, 1.f);
    }

    /**
     * Numero de semillas emitidas este tick: Poisson de media
     * SeedsPerAdultPerYear * (Biomass/MaxBiomass) * dt.
     *
     * OJO A LA BIOMASA RELATIVA, que es un cambio de fondo y no una
     * normalizacion cosmetica. Con la biomasa ABSOLUTA, MaxBiomass se
     * convertia en un multiplicador de fecundidad accidental: una especie de
     * MaxBiomass 150 producia 3.75 veces mas semillas que una de 40 con el
     * mismo parametro de siembra, solo por ser mas grande. Como MaxBiomass ya
     * daba ademas mas altura (o sea mas sombra sobre los vecinos), acababa
     * siendo un eje monotono "mas es mejor" con tres ventajas encadenadas, y
     * eso anula cualquier compromiso que metas entre rasgos.
     *
     * En relativo, el parametro significa "semillas al año de un adulto ya
     * crecido", igual para todas las especies, y la fecundidad vuelve a ser un
     * RASGO explicito (USpeciesData::SeedRateScale) que puedes compensar en
     * otro eje.
     */
    FORCEINLINE int32 ComputeSeedCount(float SeedsPerAdultPerYear, float Biomass, float MaxBiomass,
        float BiomassHalfSaturation, float DtYears, uint32& RngState)
    {
        const float RelativeBiomass =
            FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);

        // SATURACION SOBRE LA BIOMASA RELATIVA. Con proporcionalidad lineal, la
        // fecundidad realimenta el crecimiento: quien crece mas rapido no solo tiene
        // mas individuos, sino que cada uno produce ademas mas semillas y cada
        // semilla otro arbol que crece. Una ventaja de crecimiento moderada se
        // convierte en una diferencia de lluvia de semillas de dos ordenes de
        // magnitud. Y la misma linealidad ESTERILIZA a la especie suprimida, que
        // asi no puede recuperarse aunque mejoren sus condiciones.
        //
        // Saturando, lo que enciende la reproduccion es la MADUREZ y no el tamano:
        // un adulto al 40% y otro al 90% de su MaxBiomass se diferencian en un
        // factor pequeno, no en uno de dos. HalfSaturation <= 0 = lineal (anterior).
        const float Scale = (BiomassHalfSaturation > 0.f)
            ? RelativeBiomass / (RelativeBiomass + BiomassHalfSaturation)
            : RelativeBiomass;

        return EcoRand::PoissonInt(RngState, SeedsPerAdultPerYear * Scale * DtYears);
    }

    /** Desplazamiento XY (cm) de una semilla respecto al arbol madre: disco
        uniforme por area (copia unica en EcoRand::SampleDiscOffsetCm, la misma
        que usa el scatter de hojarasca de la capa de suelo). */
    FORCEINLINE FVector2D SampleSeedOffsetCm(float DispersalRadiusCm, uint32& RngState)
    {
        return EcoRand::SampleDiscOffsetCm(RngState, DispersalRadiusCm);
    }

    /** Probabilidad de germinar en el punto de caida, dado el vigor alli. */
    FORCEINLINE float GerminationProbability(float VigorAtSite, float GerminationRate)
    {
        return FMath::Clamp(VigorAtSite * GerminationRate, 0.f, 1.f);
    }

    /**
     * Inhibicion de Janzen-Connell: la probabilidad de arraigar se divide por dos
     * cada HalfCount adultos conespecificos que haya alrededor.
     *
     *     factor = 0.5 ^ (Conspecifics / HalfCount)
     *
     * Decae exponencialmente y no satura, a diferencia de 1/(1+k*n): con la forma
     * hiperbolica la correccion maxima esta acotada por la razon de densidades
     * locales por mucho que se suba k, y eso deja corto el arreglo cuando una
     * especie es mil veces mas abundante que otra.
     *
     * HalfCount <= 0 desactiva la inhibicion (devuelve 1).
     */
    FORCEINLINE float ConspecificInhibitionFactor(int32 Conspecifics, float HalfCount)
    {
        if (HalfCount <= 0.f || Conspecifics <= 0) { return 1.f; }
        return FMath::Pow(0.5f, static_cast<float>(Conspecifics) / HalfCount);
    }

    /** "Sitio seguro": la luz en el punto de caida debe superar el minimo DE LA ESPECIE. */
    FORCEINLINE bool IsSafeGerminationSite(float LightAtSite, float MinLightForGermination)
    {
        return LightAtSite >= MinLightForGermination;
    }

    /** Pulso de nutrientes devuelto al suelo cuando un arbol muere (descomposicion). */
    FORCEINLINE float DeathNutrientPulse(float Biomass, float NutrientDecompositionFactor)
    {
        return Biomass * NutrientDecompositionFactor;
    }

    /**
     * Normaliza RootRadius (definido en SpeciesData en METROS, pensado para
     * un arbol adulto) a un radio de consumo en CM segun la biomasa actual.
     * El documento usa literalmente RootRadius*Biomass como radio; con
     * Biomass hasta MaxBiomass (~100 por defecto) eso dispara el radio muy
     * por encima de la distancia real entre arboles vecinos. Aqui se
     * normaliza: Biomass==MaxBiomass -> radio = RootRadius metros; un
     * arbol joven consume, proporcionalmente, en menos espacio.
     * (Apendice A marca RootRadius como "tunear": ajustad el asset sabiendo
     * que ahora se interpreta asi.)
     */
    FORCEINLINE float EffectiveRootRadiusCm(float RootRadiusM, float Biomass, float MaxBiomass,
        float MinAdultRadiusCm)
    {
        const float Ratio = FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);

        // El minimo se aplica al radio del ADULTO, no al efectivo: una plantula debe
        // seguir compitiendo en un radio minusculo. Lo que hay que impedir es que un
        // arbol CRECIDO no llegue a sus vecinos, que es lo que pasa cuando el radio
        // no supera el tamano de celda: el peso lineal 1-d/R vale 0 exacto en los
        // cuatro vecinos ortogonales y el kernel escribe UNA sola celda. Con eso, el
        // recurso del suelo deja de ser un bien comun -son pozos privados, uno por
        // arbol- y la competencia subterranea desaparece como interaccion.
        const float AdultRadiusCm = FMath::Max(RootRadiusM * 100.f, MinAdultRadiusCm);
        return AdultRadiusCm * Ratio;
    }

    /**
     * Consumo realmente extraible, topado contra lo que hay en la zona radicular.
     *
     * Sin tope, la demanda podia superar al recurso disponible: el kernel depositaba
     * la diferencia como cantidad NEGATIVA, esa deuda se difundia a las celdas
     * vecinas -bajandoles recurso de verdad- y luego se destruia al recortar a cero.
     * O sea, competencia por interferencia no intencionada cuya intensidad crecia
     * con la demanda sin coste para quien la ejercia, y de paso la masa del campo
     * dejaba de conservarse (ninguna cuenta de balance cuadraba).
     *
     * Devuelve una cantidad POSITIVA; el llamante le pone el signo.
     */
    FORCEINLINE float ClampUptakeToAvailable(float Demand, float AvailablePerCell, int32 CellsInRange,
        float MaxFraction)
    {
        if (MaxFraction <= 0.f) { return FMath::Max(Demand, 0.f); }
        const float Extractable = MaxFraction * FMath::Max(AvailablePerCell, 0.f) * FMath::Max(CellsInRange, 1);
        return FMath::Clamp(Demand, 0.f, Extractable);
    }

    /** Cota superior de celdas que toca el kernel para un radio dado (reserva de scratch). */
    FORCEINLINE int32 KernelCellCount(const FField2D& Geometry, float RadiusCm)
    {
        if (RadiusCm <= 0.f || Geometry.CellSize <= 0.0) { return 1; }
        const int32 R = FMath::CeilToInt(RadiusCm / Geometry.CellSize);
        return (2 * R + 1) * (2 * R + 1);
    }

    /**
     * Reparte TotalAmount (puede ser negativo: consumo) entre las celdas de
     * Deltas dentro de RadiusCm de WorldPos, con un kernel lineal NORMALIZADO
     * (ver .cpp) para que la suma de lo depositado sea EXACTAMENTE TotalAmount
     * pase lo que pase con el redondeo a celdas. Geometry describe la rejilla de
     * Deltas (debe tener el mismo tamano que el campo que se parcheara despues).
     *
     * Version DENSA: escribe directamente sobre un campo completo. Usala solo
     * desde codigo SERIAL (pulsos de muerte, campo de descomposicion).
     */
    void DepositKernel(const FField2D& Geometry, TArray<float>& Deltas,
        const FVector& WorldPos, float RadiusCm, float TotalAmount);

    /**
     * Version DISPERSA del anterior: en vez de escribir en un campo del tamano
     * del mundo, APENDA pares (celda, cantidad) a la lista de la tarea. Es la
     * que usa el paso paralelo del tick (ver FCellDelta para el porque).
     *
     * Recorre las celdas exactamente en el mismo orden que la version densa, asi
     * que la reduccion posterior es reproducible. Solo escribe en OutDeltas: es
     * segura de llamar desde dentro de un ParallelFor.
     */
    void DepositKernelSparse(const FField2D& Geometry, TArray<FCellDelta>& OutDeltas,
        const FVector& WorldPos, float RadiusCm, float TotalAmount);

    /**
     * Reduccion serial (doc. 2.4): aplica los deltas de agua/nutrientes de TODOS
     * los contextos sobre los arrays de destino, recorriendo Contexts en orden de
     * indice creciente (fijo: no depende de que hilo fisico ejecuto cada tarea) y,
     * dentro de cada contexto, las entradas en su orden de insercion. Concatena
     * semillas y pulsos de muerte en ese mismo orden. DEBE llamarse siempre de
     * forma serial, nunca dentro de un ParallelFor: es precisamente el punto donde
     * el scratch privado de cada tarea vuelve a converger en un unico estado
     * compartido.
     */
    void ReduceScratchInto(const TArray<FTickScratch>& Contexts,
        TArray<float>& DestWater, TArray<float>& DestNutrient,
        TArray<FPendingSeed>& OutSeeds, TArray<FPendingDeathPulse>& OutDeathPulses,
        TArray<FEcoSpeciesFlow>& OutFlow);
}
