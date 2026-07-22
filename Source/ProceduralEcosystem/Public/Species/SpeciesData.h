#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeciesData.generated.h"



/**
 * Forma de la envolvente de copa donde se siembran los atractores del SCA
 * (Fase 3, doc. §3.4 "envolvente: forma de la copa"). La misma nube de
 * atractores con distinta forma ya diferencia una conifera de un roble.
 */
UENUM(BlueprintType)
enum class ECrownShape : uint8
{
    Conical,    // conica, estrecha (conifera excurrente)
    Spherical,  // esferica, ancha (roble decurrente)
    Columnar    // alta y estrecha (cipres)
};

/**

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

    /** Altura (cm) de un arbol adulto (Biomass == MaxBiomass). Escala de HeightFromBiomass. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "1"))
    float MaxHeightCm = 2000.f; // 20 m por defecto

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
    // ================================================================
    // --- Morfología SCA (Fase 3): geometría por-árbol ---
    // ================================================================
    // Estos parámetros NO intervienen en la Fase 2 (población). Los consume
    // la generación geométrica de los hero trees. La misma dirección de
    // crecimiento ponderada produce siluetas distintas cambiando los pesos
    // de tropismo: son la principal palanca de variedad entre especies
    // (doc. §3.4). Todas las longitudes en cm, coherente con el resto.

    // --- Envolvente de copa (siembra de atractores, doc. §3.1) ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa")
    ECrownShape CrownShape = ECrownShape::Spherical;

    /** Radio horizontal de la copa (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    float CrownRadiusCm = 400.f; // 4 m

    /** Altura vertical de la copa, de su base al ápice (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    float CrownHeightCm = 800.f; // 8 m

    /** Fracción de la altura total ocupada por tronco desnudo bajo la copa [0..1). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "0.95"))
    float TrunkFraction = 0.3f;

    /** Nº de atractores sembrados en la copa. Más = copa más tupida y más coste. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    int32 NumAttractors = 400;

    // --- Tropismos: dirección de crecimiento por iteración (doc. §3.4) ---
    /** Peso del llenado de espacio (dirección promedio hacia los atractores). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wSCA = 1.0f;

    /** Gravitropismo: sesgo hacia arriba (alto en conífera = líder recto dominante). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wGrav = 0.3f;

    /** Fototropismo: inclinación al gradiente de luz de la rejilla fina (opcional). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wPhot = 0.1f;

    /** Inercia / rigidez: mantiene la dirección previa y quita el zigzag. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wPrev = 0.4f;

    /** Jitter de dirección por-árbol (0..1): variación reproducible desde el RngState. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0", ClampMax = "1"))
    float DirNoise = 0.1f;

    // --- Radios del SCA: debe cumplirse d_k < D < d_i (doc. §3.1, Apéndice A) ---
    /** D: longitud del internodo (paso por iteración), cm. Fija la resolución del esqueleto. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float StepLengthD = 40.f;

    /** d_i: radio de influencia. Un nodo "ve" atractores hasta esta distancia. Debe ser > D. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float InfluenceRadiusDi = 200.f;

    /** d_k: radio de muerte. Un atractor a esta distancia de un nodo nuevo se da por alcanzado. Debe ser < D. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "0.1"))
    float KillRadiusDk = 30.f;

    /** Iteraciones máximas de crecimiento (doc: 30-100). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    int32 MaxIter = 60;

    /** Cada cuántas iteraciones se refresca la luz interna (autopoda emergente). 0 = nunca. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "0"))
    int32 LightEvery = 8;

    /** Tamaño del vóxel de la rejilla de luz fina local (cm; doc: 25-50 cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float FineVoxelSizeCm = 35.f;

    // --- Pipe model: radios de rama (doc. §3.6) ---
    /** Exponente del pipe model: r_padre^n = Σ r_hijo^n. n≈2 (da Vinci) a ~2.5 (empírico). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "1.0", ClampMax = "4.0"))
    float PipeExp = 2.2f;

    /** Radio de las ramillas terminales (cm); el pipe model engrosa hacia la base. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "0.05"))
    float TipRadiusCm = 1.5f;

    // --- Mallado: de esqueleto a malla (doc. §3.7) ---
    /** K: nº de vértices del anillo de sección de cada rama (tubos). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "3", ClampMax = "16"))
    int32 RingSegments = 6;

    /** Lado de una leaf card (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "1"))
    float LeafSizeCm = 20.f;

    /** Densidad de hojas: fracción de nodos terminales que reciben leaf card [0..1]. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "0", ClampMax = "1"))
    float LeafDensity = 0.8f;

    // --- Placeholders para fases posteriores ---
  
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