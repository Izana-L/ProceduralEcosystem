#pragma once

#include "CoreMinimal.h"
#include "Core/EcoCore.h"
#include "Terrain/Field2D.h"
#include "Ecology/TickScratch.h"
/**
 * Formulas puras del nucleo biologico (doc. Fase 2, seccion 2.6). Cada
 * funcion es un paso nombrado del pseudocodigo del documento: se pueden
 * testear por separado y se combinan en el bucle de tick (clase 5).
 *
 * Deliberadamente NO tocan FTreePopulation, FSpatialHash ni los campos de
 * recursos: solo reciben floats y devuelven floats. Eso hace trivial
 * comprobar cada formula de forma aislada (p.ej. en un test o en el
 * inmediato Blueprint de debug) sin montar una simulacion entera.
 */
namespace EcologyRules
{
    /** fL = Q / (Q + Kl), Kl = KlMax*(1-ShadeTolerance). Curva de saturacion (Monod). */
    FORCEINLINE float LightFactor(float Q, float ShadeTolerance, float LightHalfSaturationMax)
    {
        const float Kl = LightHalfSaturationMax * (1.f - ShadeTolerance);
        return Q / (Q + Kl + 1e-4f); // +epsilon: evita 0/0 si Q=Kl=0
    }

    /**
     * fW y fN comparten la misma forma matematica (Michaelis-Menten /
     * Monod): Available/Demand normaliza el recurso a "veces la necesidad
     * de la especie", y la curva x/(x+1) satura suavemente hacia 1 sin
     * necesitar un tope duro.
     */
    FORCEINLINE float DemandFactor(float Available, float Demand)
    {
        const float Ratio = Available / FMath::Max(Demand, KINDA_SMALL_NUMBER);
        return Ratio / (Ratio + 1.f);
    }

    /** Ley del minimo de Liebig: el recurso mas escaso limita el crecimiento. */
    FORCEINLINE float Vigor(float LightFactorValue, float WaterFactorValue, float NutrientFactorValue)
    {
        return FMath::Min3(LightFactorValue, WaterFactorValue, NutrientFactorValue);
    }

    /**
     * Crecimiento logistico: lento en plantula (B pequeño), rapido a media
     * vida, saturante cerca de MaxBiomass (el termino 1-B/MaxBiomass tiende
     * a 0). Devuelve la NUEVA biomasa, ya sumado el incremento.
     */
    FORCEINLINE float GrowBiomassLogistic(float Biomass, float VigorValue, float GrowthRate,
        float MaxBiomass, float DtYears)
    {
        const float dB = GrowthRate * VigorValue * Biomass * (1.f - Biomass / MaxBiomass) * DtYears;
        return FMath::Max(0.f, Biomass + dB);
    }

    /**
     * Altura visual a partir de la biomasa. Escalado alometrico simple:
     * H = MaxHeight * (B/MaxBiomass)^(1/3). La raiz cubica viene de asumir
     * que la biomasa escala aprox. con el volumen del arbol (~ largo^3): es
     * una aproximacion deliberadamente burda para la Fase 2 (solo alimenta
     * el grid de luz y el tamaño de las esferas de debug); el Pipe Model de
     * la Fase 3 la sustituira por geometria real.
     */
    FORCEINLINE float HeightFromBiomass(float Biomass, float MaxBiomass, float MaxHeightCm)
    {
        const float Ratio = FMath::Clamp(Biomass / MaxBiomass, 0.f, 1.f);
        return MaxHeightCm * FMath::Pow(Ratio, 1.f / 3.f);
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
        const float pAge = DtYears * FMath::Pow(AgeRatio, 4.f);
        const float pStress = DtYears * Stress * StressMortalityWeight;
        return 1.f - (1.f - pAge) * (1.f - pStress);
    }

    /** Nº de semillas emitidas este tick (Poisson de media SeedRate*Biomass*dt). */
    FORCEINLINE int32 ComputeSeedCount(float SeedRatePerBiomass, float Biomass, float DtYears,
        uint32& RngState)
    {
        const float Lambda = SeedRatePerBiomass * Biomass * DtYears;
        return EcoRand::PoissonInt(RngState, Lambda);
    }

    /** Desplazamiento XY (cm) de una semilla respecto al arbol madre. */
    FORCEINLINE FVector2D SampleSeedOffsetCm(float DispersalRadiusCm, uint32& RngState)
    {
        const float Angle = EcoRand::NextRange(RngState, 0.f, 2.f * PI);
        const float Dist = EcoRand::SampleDispersalDistance(RngState, DispersalRadiusCm);
        return FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Dist;
    }

    /** Probabilidad de germinar en el punto de caida, dado el vigor alli. */
    FORCEINLINE float GerminationProbability(float VigorAtSite, float GerminationRate)
    {
        return FMath::Clamp(VigorAtSite * GerminationRate, 0.f, 1.f);
    }

    /** "Sitio seguro": la luz en el punto de caida debe superar el minimo global. */
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
    /**
     * Reparte TotalAmount (puede ser negativo: consumo) entre las celdas de
     * Deltas dentro de RadiusCm de WorldPos, con un kernel lineal
     * NORMALIZADO (ver .cpp) para que la suma de lo depositado sea
     * EXACTAMENTE TotalAmount pase lo que pase con el redondeo a celdas.
     * Geometry describe la rejilla de Deltas (debe tener el mismo tamaño
     * que el campo que se parcheará luego en la reducción).
     * Solo escribe en Deltas: es la mitad "local al hilo" del patrón de
     * scratch, así que es segura de llamar desde dentro de un ParallelFor.
     */
    void DepositKernel(const FField2D& Geometry, TArray<float>& Deltas,
        const FVector& WorldPos, float RadiusCm, float TotalAmount);

    /**
     * Reducción serial (doc. 2.4): suma los deltas de agua/nutrientes de
     * TODOS los contextos sobre los arrays de destino, recorriendo Contexts
     * en orden de índice creciente (fijo — no depende de qué hilo físico
     * ejecutó cada tarea). Concatena semillas y pulsos de muerte en ese
     * mismo orden. DEBE llamarse siempre de forma serial, nunca dentro de
     * un ParallelFor: es precisamente el punto donde el scratch privado de
     * cada tarea vuelve a converger en un único estado compartido.
     */
    void ReduceScratchInto(const TArray<FTickScratch>& Contexts,
        TArray<float>& DestWater, TArray<float>& DestNutrient,
        TArray<FPendingSeed>& OutSeeds, TArray<FPendingDeathPulse>& OutDeathPulses);
}