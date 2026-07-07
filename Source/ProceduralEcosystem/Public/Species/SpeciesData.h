#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeciesData.generated.h"

/**
 * Parámetros de especie. Son compartidos y de solo lectura en runtime: la
 * población (Fase 2) los referencia por índice/id, no los copia por agente.
 * Se editan en el editor como assets. Nombres alineados con FSpeciesParams
 * del documento de diseño técnico.
 *
 * Nota sobre los ClampMin: varios de estos parámetros se usan como DIVISORES
 * en las fórmulas de la Fase 2 (MaxBiomass, Longevity, WaterDemand,
 * NutrientDemand). Por eso su mínimo es un positivo pequeño, no 0: un asset
 * con 0 produciría NaN/Inf en cuanto arranque la simulación.
 */
UCLASS(BlueprintType)
class PROCEDURALECOSYSTEM_API USpeciesData : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    // --- Identidad / debug ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identidad")
    FName SpeciesName = TEXT("Especie");

    /** Color en el heatmap y en las esferas de debug. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identidad")
    FColor DebugColor = FColor::Green;

    // --- Ciclo vital (Fase 2) ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0"))
    float GrowthRate = 0.25f;

    /** Divisor en el crecimiento logístico (B/MaxBiomass): debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0.01"))
    float MaxBiomass = 100.f;

    /** Longevidad en años simulados. Divisor en la mortalidad por edad: debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0.01"))
    float Longevity = 200.f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0"))
    float MaturityAge = 20.f;

    // --- Recursos (Fase 1/2) ---
    /** 0 = heliófila (necesita mucha luz), 1 = tolerante a la sombra. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0", ClampMax = "1"))
    float ShadeTolerance = 0.5f;

    /** Divisor en el factor de agua (W/WaterDemand): debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0.001"))
    float WaterDemand = 1.f;

    /** Divisor en el factor de nutrientes (N/NutrientDemand): debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0.001"))
    float NutrientDemand = 1.f;

    /** Radio de raíz en metros; escala con la biomasa en el consumo (Fase 2). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0"))
    float RootRadius = 2.f;

    // --- Dispersión (Fase 2) ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dispersión", meta = (ClampMin = "0"))
    float SeedDispersalRadius = 15.f; // m

    // --- Placeholders para fases posteriores ---
    // Fase 3 (morfología SCA): wSCA, wGrav, wPhot, wPrev, D, d_i, d_k, PipeExp, TipRadius...
    // Fase 4 (LOD): NVariants, número de buckets de edad...

    virtual FPrimaryAssetId GetPrimaryAssetId() const override
    {
        return FPrimaryAssetId(TEXT("Species"), GetFName());
    }

#if WITH_EDITOR
    /** Validación de datos: avisa de configuraciones incoherentes al editar el asset. */
    virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};