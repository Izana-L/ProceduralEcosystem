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
     * Estres: sube proporcional al deficit de vigor bajo el umbral, se
     * recupera linealmente si hay vigor de sobra. Acotado a [0,1].
     */
    FORCEINLINE float UpdateStress(float Stress, float VigorValue, float StressThreshold,
        float StressAccumulationRate, float StressRecoveryRate, float DtYears)
    {
        if (VigorValue < StressThreshold)
        {
            Stress += (StressThreshold - VigorValue) * StressAccumulationRate * DtYears;
        }
        else
        {
            Stress -= StressRecoveryRate * DtYears;
        }
        return FMath::Clamp(Stress, 0.f, 1.f);
    }

    /**
     * Probabilidad de morir este tick, combinando edad y estres como causas
     * INDEPENDIENTES: P(sobrevivir a ambas) = (1-pAge)(1-pStress).
     * pAge crece con la 4a potencia de Age/Longevity: casi nula la mayor
     * parte de la vida, se dispara cerca de la longevidad de la especie.
     */
    FORCEINLINE float MortalityProbability(float Age, float Longevity, float Stress,
        float StressMortalityWeight, float DtYears)
    {
        const float AgeRatio = Age / FMath::Max(Longevity, KINDA_SMALL_NUMBER);
        // Potencia entera con multiplicaciones: FMath::Pow(x, 4.f) usa exp/log
        // y esto se ejecuta por CADA arbol vivo en CADA tick.
        const float AgeRatio2 = AgeRatio * AgeRatio;
        const float pAge = DtYears * AgeRatio2 * AgeRatio2;
        const float pStress = DtYears * Stress * StressMortalityWeight;
        return FMath::Clamp(1.f - (1.f - pAge) * (1.f - pStress), 0.f, 1.f);
    }

    /**
     * Senescencia (Fase 5): el arbol ENTRA en declive si es VIEJO (rebasa una
     * fraccion de su longevidad) o si acumula ESTRES por encima de un umbral.
     * Devuelve true si cualquiera de las dos condiciones aplica.
     *
     * OJO: esta funcion decide la ENTRADA, no la permanencia. Es pura sobre el
     * estres actual, asi que devuelve false en cuanto el arbol se recupera. La
     * senescencia es irreversible, y esa persistencia la impone el llamante
     * (SimulateTick) haciendo OR con el estado del tick anterior. No la uses
     * sola para decidir si un arbol ES senescente.
     */
    FORCEINLINE bool IsSenescent(float Age, float Longevity, float SenescenceAgeFraction,
        float Stress, float SenescenceStressThreshold)
    {
        const bool bOld = Age >= SenescenceAgeFraction * FMath::Max(Longevity, KINDA_SMALL_NUMBER);
        const bool bStressed = Stress >= SenescenceStressThreshold;
        return bOld || bStressed;
    }

    /** Factor multiplicativo del crecimiento en senescencia (1 = normal, ~0 = detenido). */
    FORCEINLINE float SenescentGrowthFactor(bool bSenescent, float SenescentGrowthScale)
    {
        return bSenescent ? FMath::Clamp(SenescentGrowthScale, 0.f, 1.f) : 1.f;
    }

    /** Aplica el multiplicador de mortalidad de la senescencia a una pDeath ya calculada. */
    FORCEINLINE float ApplySenescentMortality(float pDeath, bool bSenescent, float Multiplier)
    {
        return bSenescent ? FMath::Clamp(pDeath * FMath::Max(1.f, Multiplier), 0.f, 1.f) : pDeath;
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
        float DtYears, uint32& RngState)
    {
        const float RelativeBiomass =
            FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);
        const float Lambda = SeedsPerAdultPerYear * RelativeBiomass * DtYears;
        return EcoRand::PoissonInt(RngState, Lambda);
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
    FORCEINLINE float EffectiveRootRadiusCm(float RootRadiusM, float Biomass, float MaxBiomass)
    {
        const float Ratio = FMath::Clamp(Biomass / FMath::Max(MaxBiomass, KINDA_SMALL_NUMBER), 0.f, 1.f);
        return RootRadiusM * 100.f * Ratio;
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
        TArray<FPendingSeed>& OutSeeds, TArray<FPendingDeathPulse>& OutDeathPulses);
}
