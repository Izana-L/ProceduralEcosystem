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

    // --- Vigor (acoplamiento, Fase 1) ---
    /** Semisaturación de luz de una heliófila. Kl = KlMax*(1-ShadeTolerance).
        Más alto = la luz limita antes (heliófilas más exigentes). */
    UPROPERTY(EditAnywhere, config, Category = "Vigor", meta = (ClampMin = "0.0001"))
    float KlMax = 0.35f;

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

    // --- Especies ---
    UPROPERTY(EditAnywhere, config, Category = "Especies")
    TArray<TSoftObjectPtr<USpeciesData>> Species;

    // --- Debug ---
    /** Material de decal (dominio Deferred Decal) con un parámetro de textura "FieldTex". */
    UPROPERTY(EditAnywhere, config, Category = "Debug")
    TSoftObjectPtr<UMaterialInterface> HeatmapDecalMaterial;

    static const UEcosystemSettings* Get() { return GetDefault<UEcosystemSettings>(); }
};