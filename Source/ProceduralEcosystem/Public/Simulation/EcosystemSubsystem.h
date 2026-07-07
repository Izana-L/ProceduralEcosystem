#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/EcoCore.h"
#include "Terrain/HeightField.h"
#include "Terrain/WaterField.h"
#include "Terrain/NutrientField.h"
#include "Terrain/LightFieldCoarse.h"
#include "Terrain/Field2D.h"
#include "EcosystemSubsystem.generated.h"

class UFieldVisualizer;
class ADecalActor;
class UMaterialInstanceDynamic;

/**
 * Motor de la simulación de ecosistema (Fase 0: andamiaje; Fase 1: campos + vigor).
 *
 *  - Tick de simulación desacoplado del frame de render (avance por "años").
 *  - RNG determinista por subsistema.
 *  - Relieve muestreable (FHeightField) como fuente de verdad.
 *  - Campos de recursos de la Fase 1: agua (TWI), nutrientes (Perlin), luz gruesa.
 *  - Función de vigor (Liebig) pintada como heatmap de idoneidad por especie.
 *  - Debug: agentes como esferas + árboles de prueba que proyectan sombra.
 *
 * En Fase 0/1, SimulateTick() sigue vacío: solo avanza el contador de años.
 * En Fase 2 se llenará con el bucle de ecología que consume estos campos.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UEcosystemSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // --- UWorldSubsystem ---
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

    // --- FTickableGameObject ---
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    // --- Control (consola) ---
    void SetPaused(bool bInPaused) { bPaused = bInPaused; }
    bool IsPaused() const { return bPaused; }
    void StepN(int32 N) { PendingSteps += FMath::Max(1, N); }
    int64 GetTickCount() const { return TickCount; }

    /** Alpha de interpolación de render [0..1] dentro del tick actual (para Fase 5). */
    float GetInterpolationAlpha() const;

    // --- Terreno y campos de recursos ---
    const FHeightField& GetHeightField() const { return HeightField; }
    const FWaterField& GetWaterField() const { return WaterField; }
    const FNutrientField& GetNutrientField() const { return NutrientField; }
    const FLightFieldCoarse& GetLightField() const { return LightField; }

    // --- Debug agents ---
    void AddDebugAgent(const FVector& WorldPos, const FColor& Color, float Radius);
    void AddRandomDebugAgent();
    void ClearDebugAgents();

    // --- Heatmaps ---
    /** Pinta un campo de recurso crudo (0=agua, 1=nutrientes, 2=luz a ras de suelo). */
    void PaintResourceField(int32 Which);

    /** Pinta la IDONEIDAD (vigor Liebig) de una especie. -1 = usar la de settings. */
    void PaintVigor(int32 SpeciesIndex = -1);

    /** Campo de prueba (la altura): se mantiene de la Fase 0 por compatibilidad. */
    void PaintTestField();

    // --- Árboles de prueba (proyectan sombra -> cambian el vigor) ---
    void AddRandomTestTree();
    void ClearTestTrees();

private:
    void SimulateTick(float DtYears);  // un paso de ecología (VACÍO en Fase 0/1)
    void DrawDebug();
    void EnsureHeatmapDecal();

    // Fase 1: generación de campos y luz
    void BakeResourceFields();         // agua + nutrientes (una vez)
    void EnsureLightGrid();            // dimensiona la rejilla de luz al terreno
    void RebuildLightShadows();        // limpia y re-deposita todas las copas de prueba
    void RepaintActiveHeatmap();       // re-pinta lo último mostrado (tras cambiar sombras)

    // Estado de tiempo
    double Accumulator = 0.0;
    int64  TickCount = 0;
    int32  PendingSteps = 0;
    bool   bPaused = true;

    /** Se pone a true al final de OnWorldBeginPlay; gatea el Tick para no correr
        antes de que el relieve y el visualizador estén listos. */
    bool   bWorldReady = false;

    float  SecondsPerTick = 0.5f;
    float  YearsPerTick = 1.f;
    int32  MaxStepsPerFrame = 4;

    // RNG determinista
    FEcosystemRng Rng;

    // Relieve y campos de recursos (Fase 1)
    FHeightField      HeightField;
    FWaterField       WaterField;
    FNutrientField    NutrientField;
    FLightFieldCoarse LightField;

    /** Último campo de vigor pintado (misma geometría que HeightField.Field). */
    FField2D VigorField;

    /** Qué se está mostrando en el heatmap, para poder re-pintar tras cambios. */
    enum class EActiveHeatmap : uint8 { None, TestField, Water, Nutrient, Light, Vigor };
    EActiveHeatmap ActiveHeatmap = EActiveHeatmap::None;
    int32 ActiveSpeciesIndex = 0;

    // Debug agents
    UPROPERTY(Transient)
    TArray<FEcoDebugAgent> DebugAgents;

    /** Ápices (mundo, cm) de las copas de prueba que depositan sombra. */
    TArray<FVector> TestTreeApex;

    // Heatmap
    UPROPERTY(Transient)
    TObjectPtr<UFieldVisualizer> FieldViz = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ADecalActor> HeatmapDecal = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> HeatmapMID = nullptr;
};