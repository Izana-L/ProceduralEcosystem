#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Render/TreeArchetype.h"
#include "TreeRenderSubsystem.generated.h"

class AHeroTreeActor;
class ATreeInstanceHost;
class UEcosystemSubsystem;
class USpeciesData;
class UTreeLibrary;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialParameterCollection; // Fase 5 (ciclo estacional)

/** Nivel de representacion de un arbol (doc. Fase 4, 4.1). */
UENUM()
enum class ETreeRenderTier : uint8
{
    None,      // fuera de rango: no se dibuja (pero SIGUE simulandose)
    Hero,      // SCA en vivo, geometria unica: decenas
    Instance,  // malla de libreria via ISM/HISM: miles
    Impostor   // tarjetas cruzadas: campo lejano
};

/**
 * Estado de render de UN arbol (doc. 4.3). Vive en la capa de render, nunca en
 * la simulacion: la poblacion es la unica fuente de verdad y el render es solo
 * una vista (doc. 0, "Propiedad de los datos").
 */
struct FTreeRenderState
{
    ETreeRenderTier Tier = ETreeRenderTier::None;
    uint32 PackedKey = 0;      // arquetipo actual -> detecta cambios de bucket/variante
    int32  InstanceIndex = -1; // indice dentro del ISM correspondiente
    int32  Bucket = -1;        // ultimo bucket (para la histeresis)
    float  LastScale = 0.f;    // ultima escala subida (umbral de actualizacion)
    uint32 Stamp = 0;          // pasada de re-nivelado en que se vio vivo por ultima vez
    uint8  LastVitalityQ = 255; // Fase 5: ultima "sequedad" cuantizada escrita en float1 (255 = nunca)
};

/** Hero pendiente de generar (la cola amortigua el coste, doc. 4.4). */
struct FPendingHero
{
    FArchetypeKey Key;
    FVector  Position = FVector::ZeroVector;
    float    Scale = 1.f;
};

/** Ranura de hero cacheado (reentrar en el nivel hero debe ser instantaneo). */
USTRUCT()
struct FHeroSlot
{
    GENERATED_BODY()

    UPROPERTY(Transient) TObjectPtr<AHeroTreeActor> Actor = nullptr;
    uint32 GeneratedKey = 0; // arquetipo con el que se genero: si cambia, hay que regenerar
    uint32 LastUsedStamp = 0;
    bool   bActive = false;

    // Fase 5 (bosque vivo): escala objetivo hacia la que UpdateHeroInterpolation
    // acerca la escala del actor cada frame, para que el crecimiento se vea
    // continuo y no a saltos entre re-nivelados.
    float  TargetScale = 1.f;
};

/**
 * PUENTE DE ESCALA (doc. Fase 4): el gestor de LOD.
 *
 * Es la pieza que hace que 20.000 arboles simulados existan en pantalla a
 * framerate interactivo. Reparte la poblacion en cuatro niveles segun la
 * distancia a camara:
 *
 *   Camara -> [Hero: SCA en vivo, decenas]
 *          -> [Libreria: ISM/HISM, miles]
 *          -> [Impostors: tarjetas, campo lejano]
 *          -> [HLOD de World Partition, celdas lejanas]   (configuracion de nivel)
 *
 * Reglas que se respetan aqui y que son el nucleo de la fase:
 *   - La simulacion NO se toca: este subsistema solo LEE la poblacion.
 *   - Nunca se anade/quita instancia a instancia en un bucle: se acumulan los
 *     cambios y se aplican AddInstances/RemoveInstances (plural) una vez por
 *     flush y por componente (doc. 4.4, riesgo Alto del Apendice B).
 *   - Un arbol cambia de nivel o de bucket rara vez; lo frecuente es solo su
 *     escala al crecer, que va en lote y con umbral.
 *   - Los hero trees se generan amortizados (MaxHeroPerFrame) y se cachean.
 *   - El re-nivelado completo corre cada N frames (los arboles se mueven
 *     despacio respecto a la camara); la seleccion de hero, cada frame.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UTreeRenderSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // --- UWorldSubsystem ---
    virtual void Deinitialize() override;
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

    // --- FTickableGameObject ---
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    // --- Control (consola) ---
    void SetEnabled(bool bInEnabled);
    bool IsEnabled() const { return bEnabled; }
    void SetFrozen(bool bInFrozen) { bFrozen = bInFrozen; }
    void ForceRelevel() { bForceRelevel = true; }
    void RebuildAll();
    void BakeLibraryNow();
    void LogStats() const;

    UTreeLibrary* GetLibrary() const { return Library; }

private:
    bool EnsureInitialized();
    void ReleaseEverything();

    void UpdateLOD(const FVector& ViewLocation);
    void EnterTier(uint32 StableId, FTreeRenderState& State, ETreeRenderTier Want,
        const FArchetypeKey& Key, const FTransform& Xform, float ScaleInBucket);
    void LeaveTier(uint32 StableId, FTreeRenderState& State);
    void FlushInstanceOps();

    void ProcessHeroQueue(int32 MaxThisFrame);
    void ReleaseHero(uint32 StableId);
    void EvictOldHeroes();

    /** Fase 5 (bosque vivo): acerca cada frame la escala de los hero trees a su
        FHeroSlot::TargetScale con un suavizado exponencial (barato: son decenas). */
    void UpdateHeroInterpolation(float DeltaTime);

    /** Fase 5 (estacional): avanza la fase de estacion global y la empuja al
        Material Parameter Collection que leen los materiales de follaje. */
    void UpdateSeason(float DeltaTime);

    void DrawTierDebug(const FVector& ViewLocation) const;
    bool GetViewLocation(FVector& OutLocation) const;

    static FORCEINLINE uint64 MakeComponentKey(uint32 PackedKey, bool bImpostor)
    {
        return (static_cast<uint64>(PackedKey) << 1) | (bImpostor ? 1ull : 0ull);
    }

    /** Cambios acumulados de UN componente, para aplicarlos en lote. */
    struct FPendingComponentOps
    {
        TArray<int32>      Removes;
        TArray<uint32>     AddIds;
        TArray<FTransform> AddXforms;
        TArray<TPair<uint32, FTransform>> Updates; // (StableId, transform): el indice se resuelve tras las bajas
        TArray<TPair<uint32, float>>      CustomData1; // Fase 5: (StableId, sequedad) para PerInstanceCustomData[1]
    };

    UPROPERTY(Transient) TObjectPtr<UTreeLibrary> Library = nullptr;
    UPROPERTY(Transient) TObjectPtr<ATreeInstanceHost> Host = nullptr;
    UPROPERTY(Transient) TObjectPtr<UEcosystemSubsystem> Eco = nullptr;
    UPROPERTY(Transient) TMap<uint32, FHeroSlot> HeroActors;

    /** StableId -> estado de render. Clave ESTABLE: los indices de la poblacion
        cambian al compactar los muertos (FTreePopulation::CompactDead). */
    TMap<uint32, FTreeRenderState> States;

    TMap<uint64, FPendingComponentOps> Pending;

    TArray<uint32> HeroQueue;
    TMap<uint32, FPendingHero> HeroInfo;
    TSet<uint32> HeroSet;
    TArray<TPair<float, int32>> HeroCandidates; // (distancia^2, indice de poblacion)

    uint32 VisitStamp = 0;
    int32  FramesSinceRelevel = 0;
    bool   bEnabled = true;
    bool   bFrozen = false;
    bool   bForceRelevel = false;
    bool   bInitialized = false;

    // Contadores de la ultima pasada (Eco.LOD.Stats).
    int32 NumHero = 0;
    int32 NumInstance = 0;
    int32 NumImpostor = 0;
    int32 NumCulled = 0;
    double LastRelevelMs = 0.0;

    // Fase 5 (estacional): fase de estacion [0,1) empujada al MPC cada frame.
    float SeasonPhase = 0.f;
};
