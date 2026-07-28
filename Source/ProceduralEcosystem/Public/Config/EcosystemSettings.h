#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EcosystemSettings.generated.h"

class USpeciesData;
class UMaterialInterface;

/**
 * Configuración central del proyecto. Aparece en
 * Project Settings -> Game -> "Procedural Ecosystem".
 *
 * Punto ÚNICO para semilla maestra, tiempo de simulación, relieve, especies
 * y material del heatmap. Se irá ampliando en cada fase.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Procedural Ecosystem"))
class PROCEDURALECOSYSTEM_API UEcosystemSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    virtual FName GetCategoryName() const override { return TEXT("Game"); }

    // --- Reproducibilidad ---
    UPROPERTY(EditAnywhere, config, Category = "Reproducibilidad")
    int32 MasterSeed = 12345;

    // --- Tiempo de simulación (tick desacoplado del frame) ---
    /** Segundos reales por tick en modo vivo. */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "0.01"))
    float SecondsPerSimTick = 0.5f;

    /** Años simulados que avanza cada tick. */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "0.01"))
    float YearsPerTick = 1.0f;

    /** Tope de ticks por frame (evita el "spiral of death" si baja el framerate). */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "1"))
    int32 MaxStepsPerFrame = 4;

    UPROPERTY(EditAnywhere, config, Category = "Tiempo")
    bool bStartPaused = true;

    // --- Relieve (Fase 0/1) ---
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "2"))
    int32 HeightfieldResolution = 512; // muestras por lado

    /** cm por muestra. 512 * 200 cm = ~1 km de lado. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1"))
    float HeightfieldCellSizeCm = 200.f;

    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0"))
    float HeightScaleCm = 30000.f; // amplitud vertical

    // --- Recursos: agua (Fase 1) ---
    /** Rango de salida del TWI. Debe casar con NutrientOutputMax para que el vigor
        (Monod) reciba agua y nutrientes en escalas comparables. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Agua", meta = (ClampMin = "0.001"))
    float WaterOutputMax = 10.f;

    /** Rellena depresiones (priority-flood) antes del D8. Off = ablación. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Agua")
    bool bFillWaterSinks = true;

    // --- Recursos: nutrientes (Fase 1) ---
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "0.001"))
    float NutrientOutputMax = 10.f;

    /** Frecuencia base del Perlin parcheado: más baja = parches más grandes. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "0.0"))
    double NutrientPatchFrequency = 0.00015;

    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "1"))
    int32 NutrientOctaves = 3;

    // --- Recursos: luz gruesa (Fase 1) ---
    /** cm por voxel horizontal de la rejilla de luz (varios metros). */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Luz", meta = (ClampMin = "1"))
    float LightCellSizeXYCm = 400.f;

    /** cm por voxel vertical de la rejilla de luz. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Luz", meta = (ClampMin = "1"))
    float LightCellSizeZCm = 400.f;

    /** Margen de altura (cm) por encima del relieve para alojar las copas. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Luz", meta = (ClampMin = "0"))
    float LightCanopyHeadroomCm = 8000.f;


    /** Especie por defecto para el heatmap de idoneidad (índice en Species). */
    UPROPERTY(EditAnywhere, config, Category = "Vigor", meta = (ClampMin = "0"))
    int32 HeatmapSpeciesIndex = 0;

    // --- Árboles de prueba (Eco.AddTree): copa que deposita sombra ---
    UPROPERTY(EditAnywhere, config, Category = "Vigor|Árbol de prueba", meta = (ClampMin = "1"))
    float TestTreeCanopyRadiusCm = 1200.f;

    UPROPERTY(EditAnywhere, config, Category = "Vigor|Árbol de prueba", meta = (ClampMin = "1"))
    float TestTreeCanopyDepthCm = 6000.f;

    UPROPERTY(EditAnywhere, config, Category = "Vigor|Árbol de prueba", meta = (ClampMin = "0", ClampMax = "1"))
    float TestTreeCanopyDensity = 0.9f;
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0.01"))
    float LightHalfSaturationMax = 5.f;

    /** S_THRESH: vigor por debajo del cual se acumula estrés. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float StressVigorThreshold = 0.3f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressAccumulationRate = 1.f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressRecoveryRate = 0.5f;

    /** Peso del estrés acumulado en la probabilidad de morir por tick. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressMortalityWeight = 0.2f;

    /** Semillas por unidad de biomasa y año simulado (media de la Poisson). */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float SeedRatePerBiomass = 0.1f;

    /** Multiplicador de germinación: prob = VigorEnDestino * GerminationRate. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float GerminationRate = 0.5f;

    /** Luz mínima en el punto de caída para considerarlo "sitio seguro". */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float MinLightForGermination = 0.5f;

    /** Fracción de la biomasa que vuelve como pulso de nutrientes al morir. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientDecompositionFactor = 0.3f;

    // --- Ecología (Fase 2): regeneración de campos ---
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float WaterRechargeRate = 0.3f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float WaterDiffusionRate = 0.1f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientRechargeRate = 0.15f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientDiffusionRate = 0.2f;

    // --- Ecología (Fase 2): grid de luz grueso (FLightFieldCoarse) ---
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "50"))
    float LightCoarseCellSizeCm = 400.f;

    /** Nº de voxels verticales. Layers * LightCoarseCellSizeCm debe cubrir HeightScaleCm + el arbol mas alto posible. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1"))
    int32 LightCoarseLayers = 32;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float MinGerminationSpacingCm = 100.f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "50"))
    float SpatialHashCellSizeCm = 500.f;

    /**
     * Nº de arboles por tarea del ParallelFor del tick. El nº de chunks se
     * deriva de este valor (ceil(Poblacion / Grain)), NO del nº de hilos de la
     * maquina: eso es lo que garantiza que la reduccion de deltas sea bit a bit
     * identica en cualquier CPU (ver nota de determinismo en SimulateTick).
     * Mas pequeno = mas paralelismo pero mas coste de reduccion; 512 es un
     * punto medio razonable para poblaciones de miles-decenas de miles.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1"))
    int32 TickChunkGrainSize = 512;

    // ================================================================
    // --- Render y LOD (Fase 4): el puente de escala ---
    // ================================================================

    /** Interruptor maestro de la capa de render instanciada. Apagado = solo
        simulación + esferas de debug (útil para la ablación de la Fase 7). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bEnableTreeRendering = true;

    /** Buckets de tamaño por especie (doc. Apéndice A: p.ej. 5). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1", ClampMax = "32"))
    int32 NumAgeBuckets = 5;

    /** Histéresis del cambio de bucket, en fracción de bucket. Evita que un
        árbol parado en el borde haga add/remove de instancia cada tick. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "0.49"))
    float BucketHysteresis = 0.15f;

    /** R_hero: radio (cm) dentro del cual un árbol puede ser hero (SCA en vivo). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float HeroRadiusCm = 6000.f;   // 60 m

    /** Nº máximo de hero trees simultáneos (working set pequeño: decenas). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    int32 HeroBudget = 24;

    /** Hero trees generados por frame: amortiza los ms de SCA para no dar hitches. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 MaxHeroPerFrame = 1;

    /** R_impostor: a partir de aquí (cm) se dibuja el impostor en vez de la malla. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float ImpostorRadiusCm = 25000.f;   // 250 m

    /** Más allá de esto (cm) el árbol no se dibuja (lo cubriría el HLOD de World Partition). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float CullRadiusCm = 120000.f;      // 1.2 km

    /** Cadencia del re-nivelado completo. Los árboles se mueven despacio
        respecto a la cámara: no hace falta cada frame (doc. §4.3). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 RelevelEveryNFrames = 5;

    /** Jitter de tamaño por instancia (doc. Apéndice A: 0.9-1.1 -> 0.1). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "0.5"))
    float InstanceScaleJitter = 0.1f;

    /** Cambio mínimo de escala para molestarse en actualizar la instancia. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float ScaleUpdateThreshold = 0.02f;

    /** Arquetipos horneados por frame cuando se piden bajo demanda. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 MaxBakesPerFrame = 2;

    /** Hornear TODA la librería al arrancar (hitch inicial de ~1 s, cero después).
        Recomendado para demos y para medir framerate sin ruido. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bPrebakeLibraryOnStart = false;

    /** Las instancias cercanas proyectan sombra (VSM). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bInstancesCastShadow = true;

    /** Los impostors NO deberían proyectar sombra: que lo haga el proxy HLOD (doc. §4.6). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bImpostorsCastShadow = false;

    /** Floats de PerInstanceCustomData (1 = fase estacional por árbol, gancho de la Fase 5). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "4"))
    int32 NumInstanceCustomDataFloats = 1;

    // --- Especies ---
    UPROPERTY(EditAnywhere, config, Category = "Especies")
    TArray<TSoftObjectPtr<USpeciesData>> Species;
    // ================================================================
    // --- Fase 5: ciclo de vida y dinamica ---
    // ================================================================

    /** Modo "bosque vivo": interpola la escala de los hero trees entre ticks
        para que el crecimiento se vea CONTINUO y no a saltos (doc. Fase 5). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|BosqueVivo")
    bool bSmoothHeroGrowth = true;

    /** Constante de tiempo (s) del suavizado de escala del hero. Mas alto = mas
        lento y suave. Suavizado exponencial: estable a cualquier framerate. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|BosqueVivo", meta = (ClampMin = "0.01"))
    float HeroGrowthSmoothingSeconds = 0.6f;

    /** Nº de muertes recientes que la simulacion conserva (anillo) para que la
        capa de render genere caidas/tocones/hojarasca (lo consume el Paso 4). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|BosqueVivo", meta = (ClampMin = "0"))
    int32 DeathEventBufferSize = 256;
    // --- Debug ---
    /** Material de decal (dominio Deferred Decal) con un parámetro de textura "FieldTex". */
    UPROPERTY(EditAnywhere, config, Category = "Debug")
    TSoftObjectPtr<UMaterialInterface> HeatmapDecalMaterial;

    static const UEcosystemSettings* Get() { return GetDefault<UEcosystemSettings>(); }
};