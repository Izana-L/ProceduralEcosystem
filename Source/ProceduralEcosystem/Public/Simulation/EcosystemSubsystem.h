#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/EcoCore.h"
#include "Terrain/HeightField.h"
#include "EcosystemSubsystem.generated.h"

class UFieldVisualizer;
class ADecalActor;
class UMaterialInstanceDynamic;

/**
 * Motor de la simulación de ecosistema (Fase 0: andamiaje).
 *
 *  - Tick de simulación desacoplado del frame de render (avance por "años").
 *  - RNG determinista por subsistema.
 *  - Relieve muestreable (FHeightField) como fuente de verdad.
 *  - Debug: agentes como esferas + heatmap de un campo de prueba.
 *
 * En Fase 0, SimulateTick() está intencionadamente vacío: solo avanza el
 * contador de años. En Fase 2 se llenará con el bucle de ecología.
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

    // --- Terreno ---
    const FHeightField& GetHeightField() const { return HeightField; }

    // --- Debug agents ---
    void AddDebugAgent(const FVector& WorldPos, const FColor& Color, float Radius);
    void AddRandomDebugAgent();
    void ClearDebugAgents();

    // --- Heatmap de prueba ---
    void PaintTestField();

private:
    void SimulateTick(float DtYears);  // un paso de ecología (VACÍO en Fase 0)
    void DrawDebug();
    void EnsureHeatmapDecal();

    // Estado de tiempo
    double Accumulator = 0.0;
    int64  TickCount   = 0;
    int32  PendingSteps = 0;
    bool   bPaused = true;

    float  SecondsPerTick   = 0.5f;
    float  YearsPerTick     = 1.f;
    int32  MaxStepsPerFrame = 4;

    // RNG determinista
    FEcosystemRng Rng;

    // Relieve
    FHeightField HeightField;

    // Debug agents
    UPROPERTY(Transient)
    TArray<FEcoDebugAgent> DebugAgents;

    // Heatmap
    UPROPERTY(Transient)
    TObjectPtr<UFieldVisualizer> FieldViz = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ADecalActor> HeatmapDecal = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> HeatmapMID = nullptr;
};
