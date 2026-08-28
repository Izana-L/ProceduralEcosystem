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
class UMaterialParameterCollection; // Fase 5 (estacional) y Fase 6 (viento)

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

    /** Fase 5: ultima "sequedad" cuantizada escrita en float1 (255 = nunca). */
    uint8  LastVitalityQ = 255;

    /** Fase 6: ultima "apertura de copa" cuantizada escrita en float2 (255 = nunca).
        Se cuantiza a 16 bandas por el mismo motivo que la sequedad: sin banda, el
        AO cambiaria unas milesimas en cada re-nivelado y escribiriamos custom data
        de TODAS las instancias cada vez, que es justo el trap del doc. 4.4. */
    uint8  LastCanopyQ = 255;

    /** El arbol esta en HeroQueue esperando su malla, pero SIGUE dibujado con su
        representacion anterior (instancia/impostor). El cambio de nivel se
        consuma en ProcessHeroQueue, cuando el actor ya tiene geometria. */
    bool   bHeroPending = false;

    /**
     * true cuando PackedKey describe de verdad la representacion actual.
     *
     * Correccion B8: antes se usaba PackedKey == 0 como centinela de "no
     * representado", pero 0 es tambien una clave PERFECTAMENTE VALIDA
     * (Species=0, Bucket=0, Variant=0), es decir la plantula mas comun de la
     * simulacion. Funcionaba de milagro porque el cambio de nivel lo tapaba;
     * era una trampa esperando a que alguien reordenara las comprobaciones.
     */
    bool   bHasRepresentation = false;
};

/** Hero pendiente de generar (la cola amortigua el coste, doc. 4.4). */
struct FPendingHero
{
    FArchetypeKey Key;
    FVector  Position = FVector::ZeroVector;
    float    Scale = 1.f;          // escala final de instancia (bucket * jitter)
    float    ScaleInBucket = 1.f;  // sin jitter: es lo que compara el umbral de re-escalado
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
 *   - Los hero trees se generan amortizados (MaxHeroPerFrame), se cachean y el
 *     cambio de nivel a hero es DIFERIDO: no se suelta la representacion
 *     anterior hasta que la malla nueva esta lista.
 *   - El re-nivelado completo (que incluye la seleccion de hero) corre cada
 *     RelevelEveryNFrames frames: los arboles se mueven despacio respecto a la
 *     camara. Lo unico que corre cada frame es la interpolacion de escala de los
 *     hero, la cola de generacion y los relojes globales (estacion y viento).
 *
 * FASE 6: este subsistema es tambien el que empuja los PARAMETROS GLOBALES DE
 * MATERIAL (estacion, nieve, viento) a sus Material Parameter Collections. Son
 * dos escrituras por frame para TODO el bosque: el movimiento y el cambio de
 * estacion salen gratis en CPU, y lo que cuesta -el vertex shader del viento- se
 * acota por distancia en los propios componentes (ver UTreeLibrary).
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
    void RebuildAll();
    void BakeLibraryNow();
    void LogStats() const;

    /** Fase 6: reaplica a los componentes ya creados los ajustes de viento de
        Project Settings (WPO on/off, distancia de corte, margen de bounds), sin
        reconstruir la capa de render. Lo usa Eco.Wind.Apply. */
    void ApplyWindSettings();

    /** Fase 6: estado del viento para el HUD/log de profiling. */
    void LogWindState() const;

    /** Fase 6 (6.4): reparto de la ultima pasada, para el HUD y el CSV de frame. */
    void GetTierCounts(int32& OutHero, int32& OutInstance, int32& OutImpostor, int32& OutCulled) const
    {
        OutHero = NumHero; OutInstance = NumInstance; OutImpostor = NumImpostor; OutCulled = NumCulled;
    }

    /** Fase 6 (6.4): coste (ms) del ultimo re-nivelado completo. */
    double GetLastRelevelMs() const { return LastRelevelMs; }

private:
    void HandleStateLoaded();
    bool EnsureInitialized();
    void ReleaseEverything();

    void UpdateLOD(const FVector& ViewLocation);
    void EnterTier(uint32 StableId, FTreeRenderState& State, ETreeRenderTier Want,
        const FArchetypeKey& Key, const FTransform& Xform, float ScaleInBucket,
        float Dryness, float CanopyAO);
    void LeaveTier(uint32 StableId, FTreeRenderState& State);
    void FlushInstanceOps();

    void ProcessHeroQueue(int32 MaxThisFrame);
    void CommitHeroTier(uint32 StableId, FTreeRenderState& State, const FPendingHero& Info);
    void ReleaseHero(uint32 StableId);
    void EvictOldHeroes();

    /** Destruye los actores hero cacheados y vacia HeroActors/HeroQueue/HeroInfo.
        Lo comparten RebuildAll y ReleaseEverything (antes cada uno tenia su copia
        del bucle, con el riesgo de que una olvidase alguno de los Reset). */
    void DestroyAllHeroActors();

    /** Fase 5 (bosque vivo): acerca cada frame la escala de los hero trees a su
        FHeroSlot::TargetScale con un suavizado exponencial (barato: son decenas). */
    void UpdateHeroInterpolation(float DeltaTime);

    /** Fase 5 (estacional): avanza la fase de estacion global y la empuja al
        Material Parameter Collection que leen todos los materiales de arbol.
        Fase 6: escribe ademas el escalar "Snow" derivado de la estacion. */
    void UpdateSeason(float DeltaTime);

    /** Fase 6 (6.1): avanza el reloj del viento, compone las rafagas y empuja
        direccion/fuerza al MPC de viento. O(1) por frame para todo el bosque. */
    void UpdateWind(float DeltaTime);

    void DrawTierDebug(const FVector& ViewLocation) const;
    bool GetViewLocation(FVector& OutLocation) const;

    static FORCEINLINE uint64 MakeComponentKey(uint32 PackedKey, bool bImpostor)
    {
        return (static_cast<uint64>(PackedKey) << 1) | (bImpostor ? 1ull : 0ull);
    }

    /** Cuantizacion a 16 bandas de un valor [0,1] (umbral de reescritura de custom data). */
    static FORCEINLINE uint8 Quantize16(float Value01)
    {
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value01 * 15.f), 0, 15));
    }

    /** Cambios acumulados de UN componente, para aplicarlos en lote. */
    struct FPendingComponentOps
    {
        TArray<int32>      Removes;
        TArray<uint32>     AddIds;
        TArray<FTransform> AddXforms;
        TArray<TPair<uint32, FTransform>> Updates; // (StableId, transform): el indice se resuelve tras las bajas

        /**
         * Datos por instancia (Fase 5 + Fase 6), empaquetados juntos:
         *   X = sequedad     [1] 0 sano, 1 seco/senescente
         *   Y = apertura     [2] 1 a pleno sol, 0 bajo dosel cerrado (AO)
         *
         * AddCustom va en PARALELO a AddIds/AddXforms (valor inicial de cada alta).
         * Es imprescindible: una instancia recien creada nace con TODA su custom
         * data a 0, asi que sin esto un arbol senescente que acaba de cambiar de
         * bucket se dibujaria verde y sin AO hasta que alguno de los dos valores
         * cambiase de banda... que puede no pasar nunca.
         */
        TArray<FVector2f>                   AddCustom;
        TArray<TPair<uint32, FVector2f>>    CustomUpdates; // (StableId, (sequedad, apertura))
    };

    UPROPERTY(Transient) TObjectPtr<UTreeLibrary> Library = nullptr;
    UPROPERTY(Transient) TObjectPtr<ATreeInstanceHost> Host = nullptr;
    UPROPERTY(Transient) TObjectPtr<UEcosystemSubsystem> Eco = nullptr;
    UPROPERTY(Transient) TMap<uint32, FHeroSlot> HeroActors;

    /** StableId -> estado de render. Clave ESTABLE: los indices de la poblacion
        cambian al compactar los muertos (FTreePopulation::CompactDead). */
    TMap<uint32, FTreeRenderState> States;

    TMap<uint64, FPendingComponentOps> Pending;

    /** Buffers de trabajo de FlushInstanceOps (miembros: cero allocations por
        flush). ResolvedUpdates lleva (indice de instancia, transform) ya resuelto
        y ordenado; BatchXforms es la tirada contigua que se pasa a
        BatchUpdateInstancesTransforms. */
    TArray<TPair<int32, FTransform>> ResolvedUpdates;
    TArray<FTransform> BatchXforms;

    TArray<uint32> HeroQueue;
    TMap<uint32, FPendingHero> HeroInfo;
    TSet<uint32> HeroSet;

    /**
     * Los HeroBudget arboles mas cercanos, mantenido por SELECCION PARCIAL
     * (optimizacion C4): un array acotado y ordenado de a lo sumo HeroBudget
     * elementos, no la lista completa de candidatos ordenada entera. En bosque
     * denso, dentro de HeroRadiusCm caben cientos de arboles y solo interesan 24.
     * Cada candidato se descarta en O(1) si esta mas lejos que el peor actual.
     */
    TArray<TPair<double, int32>> HeroBest; // (distancia^2, indice de poblacion), ascendente

    /** Distancia^2 a camara de cada arbol, cacheada en la pasada de seleccion de
        hero para que la de reparto de niveles no la recalcule (C4). En double:
        con CullRadius de 1,2 km, la distancia^2 (~1.4e10) excede la precision de
        un float (mantisa de 24 bits) y las coordenadas de mundo de UE5 son double. */
    TArray<double> DistSqCache;

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

    /** MPC de estacion resuelto UNA vez en EnsureInitialized (correccion B6).
        Antes se hacia SeasonMPC.LoadSynchronous() dentro de UpdateSeason, o sea
        en cada frame. */
    UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> SeasonMPCCached = nullptr;

    // --- Fase 6 (viento) ---
    /** MPC de viento, resuelto una vez igual que el de estacion. */
    UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> WindMPCCached = nullptr;

    /** Reloj PROPIO del viento (s). No usa el reloj de simulacion a proposito:
        con la sim pausada (bake estatico / beauty shot) el bosque tiene que
        seguir moviendose, y con la sim acelerada el viento no debe acelerarse. */
    float WindTime = 0.f;

    // Ultimos valores empujados al MPC (los lee el log/HUD de profiling).
    float WindStrengthNow = 0.f;
    float WindGustNow = 0.f;
    float WindDirDegNow = 0.f;
};
