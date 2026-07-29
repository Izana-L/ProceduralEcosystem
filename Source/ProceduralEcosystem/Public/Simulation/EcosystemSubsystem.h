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
#include "Ecology/TreeDeathEvent.h"
#include "EcosystemSubsystem.generated.h"

class UFieldVisualizer;
class ADecalActor;
class UMaterialInstanceDynamic;
class USpeciesData;
class AHeroTreeActor;
class FArchive; 
// Fase 5 (Paso 6): serializacion del bake
/** Se emite cuando LoadState sustituye la poblacion entera. Las capas que
    mantienen estado indexado por StableId (render: instancias y heroes cacheados;
    suelo: tocones y hojarasca) DEBEN tirarlo: los StableId del bake son de otra
    corrida y reutilizarlos coloca representaciones de arboles viejos. */
DECLARE_MULTICAST_DELEGATE(FOnEcoStateLoaded);
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

    /**Alpha de interpolacion de render[0..1] dentro del tick actual : cuanto se ha
        consumido ya del tick en curso.Es el puente tick(discreto) < ->frame(continuo)
        del doc. 5.2; lo usa el ciclo estacional para avanzar de forma suave. */
    float GetInterpolationAlpha() const;

    /** Años simulados que avanza cada tick (settings). Lo necesita el reloj
        estacional para contar el MISMO año que la ecologia. */
    float GetYearsPerTick() const { return YearsPerTick; }

    // --- Terreno ---
    const FHeightField& GetHeightField() const { return HeightField; }
    // --- Luz gruesa (Fase 3): la lee el hero tree para sembrar su rejilla fina
    //     con la sombra de los vecinos (conexion micro<-macro). Se rellena en
    //     cada SimulateTick, asi que refleja el estado del ultimo tick corrido. ---
    const FLightFieldCoarse& GetLightCoarse() const { return LightCoarse; }

    // --- Debug agents (Fase 0: sondas manuales, no forman parte de la simulacion) ---
    void AddDebugAgent(const FVector& WorldPos, const FColor& Color, float Radius);
    void AddRandomDebugAgent();
    void ClearDebugAgents();
    void LogStateFingerprint() const;
    void LogFiniteCheck() const;

    // --- Eventos de muerte (Fase 5, Paso 1): la capa de suelo los consume ---
    void  LogRecentDeaths() const;
    int64 GetDeathEventCounter() const { return DeathEventCounter; }
    /** Copia en Out las muertes con indice >= InOutCursor (limitado al anillo) y avanza el cursor. */
    void  CollectNewDeathEvents(int64& InOutCursor, TArray<FTreeDeathEvent>& Out) const;
    // --- Heatmaps ---
    void PaintTestField();
    void PaintWaterField();
    void PaintNutrientField();
    void PaintVigorField();
    void PaintLightField();
    void PaintDecompositionField(); // Fase 5 (Paso 5): puntos de muerte en el terreno
    // --- Poblacion (Fase 2) ---
    /** Siembra Count plantulas aleatorias sobre el terreno (consola: Eco.SeedForest). */
    void SeedInitialPopulation(int32 Count);

    /** Nº de arboles vivos actualmente (para HUD/consola/tests). */
    int32 GetLivePopulationCount() const { return Agents_Read.Num(); }

    // --- Bake a un año objetivo (Fase 5, Paso 6): guardar / cargar el estado ---
    void SaveState(const FString& FilePath);
    bool LoadState(const FString& FilePath);

    /** Notifica a las capas de vista que la poblacion se ha sustituido de golpe. */
    FOnEcoStateLoaded OnStateLoaded;
    // --- Hero trees (Fase 3): geometria SCA en vivo ---
    /** Genera un hero tree en WorldPos con la especie SpeciesIndex (indice en
        Species) y semilla Seed, usando la luz gruesa actual como contexto de
        vecinos. Devuelve el actor creado (o nullptr si falla). */
    AHeroTreeActor* SpawnHeroTree(const FVector& WorldPos, int32 SpeciesIndex, uint32 Seed);

    /** Destruye todos los hero trees generados. */
    void ClearHeroTrees();

    // --- Acceso de solo lectura para la capa de render (Fase 4) ---
    // La poblacion es la UNICA fuente de verdad (doc. 0, "Propiedad de los
    // datos"); el gestor de LOD elige representacion sin tocar la simulacion.
    const FTreePopulation& GetPopulation() const { return Agents_Read; }
    const TArray<TObjectPtr<USpeciesData>>& GetSpeciesList() const { return ResolvedSpecies; }
    const USpeciesData* GetSpeciesById(uint16 SpeciesId) const { return ResolveSpecies(SpeciesId); }

    /** true cuando el relieve y los campos ya estan listos (gatea el render). */
    bool IsWorldReady() const { return bWorldReady; }

private:
    void SimulateTick(float DtYears);
    /** Rehace el grid de luz grueso con la poblacion actual (ClearShadow + deposito
        de todas las copas). Lo llama SimulateTick al principio del tick, y LoadState
        despues de cargar: sin esto un bake cargado deja la luz del bosque ANTERIOR,
        y como LoadState deja la sim en pausa nadie la refresca -> los hero trees que
        generes para el beauty shot crecerian contra la sombra de vecinos que ya no
        existen, que es justo la feature que vende el doc. 3.5. */
    void RebuildCoarseLight();
    void DrawDebug();
    void EnsureHeatmapDecal();
    void LogPopulationStats() const;
    void RecordDeathEvent(const FPendingDeathPulse& Pulse); // Fase 5 (Paso 1)
    /** Serializa/deserializa un bake completo sobre Payload. Deserializar sobre un
        objeto APARTE (y no directamente sobre los miembros vivos) es lo que permite
        VALIDAR antes de pisar el estado: un fichero corrupto o de otra resolucion de
        relieve dejaba la simulacion a medio cargar y luego indexaba fuera de rango. */
    void SerializeState(FArchive& Ar, struct FEcoBakePayload& Payload);

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

    // --- Fase 5 (Paso 5): campo de "descomposicion reciente". Recibe un pulso al
    //     morir un arbol y decae con el tiempo; se pinta en el terreno. Solo
    //     visualizacion (no entra en el vigor). Un solo buffer (se actualiza serial). ---
    FField2D DecompositionField;
    int64    LastDecompPaintTick = -1; // ultimo tick repintado en modo "live"

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
    TArray<FVector> NewbornPositions;

    /** Cache de especies resueltas: evita LoadSynchronous miles de veces por tick. */
    UPROPERTY(Transient)
    TArray<TObjectPtr<USpeciesData>> ResolvedSpecies;

    // --- Eventos de muerte (Fase 5, Paso 1): anillo circular + cursor monotono ---
    TArray<FTreeDeathEvent> RecentDeaths;
    int64 DeathEventCounter = 0;

    // --- Debug agents (Fase 0) ---
    UPROPERTY(Transient)
    TArray<FEcoDebugAgent> DebugAgents;

    // --- Hero trees generados (Fase 3) ---
    UPROPERTY(Transient)
    TArray<TObjectPtr<AHeroTreeActor>> HeroTrees;

    // --- Heatmap ---
    UPROPERTY(Transient)
    TObjectPtr<UFieldVisualizer> FieldViz = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ADecalActor> HeatmapDecal = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> HeatmapMID = nullptr;
};