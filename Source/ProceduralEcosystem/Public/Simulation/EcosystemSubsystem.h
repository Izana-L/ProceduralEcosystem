#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/EcoCore.h"
#include "Terrain/HeightField.h"
#include "Terrain/WaterField.h"
#include "Terrain/NutrientField.h"
#include "Terrain/LightFieldCoarse.h"
#include "Ecology/TreePopulation.h"
#include "Ecology/SpatialHash.h"
#include "Ecology/ResourcePool.h"
#include "Ecology/TickScratch.h"
#include "EcosystemSubsystem.generated.h"

class UFieldVisualizer;
class ADecalActor;
class UMaterialInstanceDynamic;
class USpeciesData;

/**
 * Motor de la simulacion de ecosistema.
 *
 *  Fase 0: tick desacoplado del render, RNG determinista, relieve muestreable,
 *          framework de debug.
 *  Fase 1: campos de recursos base (agua/TWI, nutrientes/Perlin, luz gruesa).
 *  Fase 2: poblacion de arboles en SoA, spatial hash, competencia por
 *          recursos compartidos, crecimiento/estres/mortalidad/reproduccion
 *          deterministas bajo paralelismo.
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

    /** Alpha de interpolacion de render [0..1] dentro del tick actual (para Fase 5). */
    float GetInterpolationAlpha() const;

    // --- Terreno ---
    const FHeightField& GetHeightField() const { return HeightField; }

    // --- Debug agents (Fase 0: sondas manuales, no forman parte de la simulacion) ---
    void AddDebugAgent(const FVector& WorldPos, const FColor& Color, float Radius);
    void AddRandomDebugAgent();
    void ClearDebugAgents();

    // --- Heatmaps ---
    void PaintTestField();
    void PaintWaterField();
    void PaintNutrientField();

    // --- Poblacion (Fase 2) ---
    /** Siembra Count plantulas aleatorias sobre el terreno (consola: Eco.SeedForest). */
    void SeedInitialPopulation(int32 Count);

    /** Nº de arboles vivos actualmente (para HUD/consola/tests). */
    int32 GetLivePopulationCount() const { return Agents_Read.Num(); }

private:
    void SimulateTick(float DtYears);
    void DrawDebug();
    void EnsureHeatmapDecal();
    void LogPopulationStats() const;
    const USpeciesData* ResolveSpecies(uint16 SpeciesId) const;

    // --- Estado de tiempo ---
    double Accumulator = 0.0;
    int64  TickCount = 0;
    int32  PendingSteps = 0;
    bool   bPaused = true;

    /** Se pone a true al final de OnWorldBeginPlay; gatea el Tick para no correr
        antes de que el relieve y los campos esten listos. */
    bool   bWorldReady = false;

    float  SecondsPerTick = 0.5f;
    float  YearsPerTick = 1.f;
    int32  MaxStepsPerFrame = 4;

    // RNG determinista
    FEcosystemRng Rng;

    // --- Relieve (Fase 0) ---
    FHeightField HeightField;

    // --- Campos base (Fase 1): potencial del terreno, se calculan UNA VEZ ---
    FWaterField WaterBase;
    FNutrientField NutrientBase;
    FLightFieldCoarse LightCoarse; // este SI se recalcula entero cada tick (serial, ver SimulateTick)

    // --- Estado runtime (Fase 2): doble buffer de disponibilidad actual ---
    FResourcePool WaterPool;
    FResourcePool NutrientPool;

    // --- Poblacion y aceleracion espacial (Fase 2) ---
    FTreePopulation Agents_Read;
    FTreePopulation Agents_Write;
    FSpatialHash Hash;

    /** Scratch por-tarea del paso paralelo. PERSISTENTE: se dimensiona y se
        pone a cero una vez y se reutiliza cada tick (ResetForNextTick) en vez
        de reasignar arrays del tamano del campo en cada SimulateTick. */
    TArray<FTickScratch> TickContexts;

    /** Cache de especies resueltas: evita LoadSynchronous miles de veces por tick. */
    UPROPERTY(Transient)
    TArray<TObjectPtr<USpeciesData>> ResolvedSpecies;

    // --- Debug agents (Fase 0) ---
    UPROPERTY(Transient)
    TArray<FEcoDebugAgent> DebugAgents;

    // --- Heatmap ---
    UPROPERTY(Transient)
    TObjectPtr<UFieldVisualizer> FieldViz = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ADecalActor> HeatmapDecal = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> HeatmapMID = nullptr;
};