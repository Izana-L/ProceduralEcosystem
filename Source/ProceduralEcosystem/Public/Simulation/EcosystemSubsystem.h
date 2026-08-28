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
#include "Ecology/CarbonModel.h"   // Fase 6 (6.3): multiplicador de CO2
#include "EcosystemSubsystem.generated.h"

class UFieldVisualizer;
class ADecalActor;
class UMaterialInstanceDynamic;
class USpeciesData;
class AHeroTreeActor;
class UEcosystemSettings;
class FArchive;
// Fase 5 (Paso 6): serializacion del bake
/** Se emite cuando LoadState sustituye la poblacion entera. Las capas que
    mantienen estado indexado por StableId (render: instancias y heroes cacheados;
    suelo: tocones y hojarasca) DEBEN tirarlo: los StableId del bake son de otra
    corrida y reutilizarlos coloca representaciones de arboles viejos. */
DECLARE_MULTICAST_DELEGATE(FOnEcoStateLoaded);

/**
 * Desglose del coste de UN tick, por etapas (comando Eco.Profile).
 *
 * El doc. 6.4 es tajante: "medir primero, optimizar despues". Antes de decidir
 * si algo se sube a compute shader (doc. 6.5) hay que saber DONDE se va el
 * tiempo, y a ojo no se sabe. Esto instrumenta las cinco etapas del bucle de
 * 2.5 con una media exponencial (barata y estable, sin acumular historico).
 *
 * Coste de la propia instrumentacion: 6 llamadas a FPlatformTime::Seconds() por
 * tick. Despreciable frente a un tick de milisegundos.
 *
 * Fase 6: ademas de esta media propia, las mismas etapas se publican como
 * contadores del motor (`stat EcoSim`) y como ambitos de Unreal Insights, para
 * poder verlas junto al resto del frame. Ver Core/EcoStats.h.
 */
struct FEcoTickProfile
{
    double HashMs = 0.0;      // reconstruccion del spatial hash
    double LightMs = 0.0;     // ClearShadow + deposito de copas
    double ParallelMs = 0.0;  // ParallelFor: crecimiento/estres/mortalidad/semillas
    double ReduceMs = 0.0;    // reduccion serial de los scratch
    double RegenMs = 0.0;     // recarga + difusion de los campos
    double GerminationMs = 0.0; // pulsos de muerte + germinacion
    double TotalMs = 0.0;

    /** Media exponencial: Sample pesa Alpha, el historico 1-Alpha. */
    static void Accumulate(double& InOutAvg, double SampleMs, double Alpha = 0.1)
    {
        InOutAvg = (InOutAvg <= 0.0) ? SampleMs : (InOutAvg * (1.0 - Alpha) + SampleMs * Alpha);
    }
};

/**
 * Motor de la simulacion de ecosistema.
 *
 *  Fase 0: tick desacoplado del render, RNG determinista, relieve muestreable,
 *          framework de debug.
 *  Fase 1: campos de recursos base (agua/TWI, nutrientes/Perlin, luz gruesa).
 *  Fase 2: poblacion de arboles en SoA, spatial hash, competencia por
 *          recursos compartidos, crecimiento/estres/mortalidad/reproduccion
 *          deterministas bajo paralelismo.
 *  Fase 5: senescencia, eventos de muerte y bake a un ano objetivo.
 *  Fase 6: multiplicador de CO2 en el vigor, presupuesto de tiempo del tick
 *          dentro del frame e instrumentacion para las herramientas del motor.
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
    //     con la sombra de los vecinos (conexion micro<-macro). Fase 6: tambien
    //     la lee el gestor de LOD para el AO de copa por instancia. Se rellena en
    //     cada SimulateTick, asi que refleja el estado del ultimo tick corrido. ---
    const FLightFieldCoarse& GetLightCoarse() const { return LightCoarse; }

    // --- Debug agents (Fase 0: sondas manuales, no forman parte de la simulacion) ---
    void AddDebugAgent(const FVector& WorldPos, const FColor& Color, float Radius);
    void AddRandomDebugAgent();
    void ClearDebugAgents();
    void LogStateFingerprint() const;
    void LogFiniteCheck() const;

    /**
     * Estructura DEMOGRAFICA del bosque por especie (consola: Eco.Demografia):
     * reparto por estado, edades, que fraccion de la poblacion ya esta crecida y
     * cuantas muertes lleva acumuladas.
     *
     * Es el instrumento con el que se calibra la longevidad. LogPopulationStats
     * dice CUANTOS arboles hay, que es justo lo que no distingue un bosque maduro
     * de un vivero: mil plantones y mil arboles de dosel son el mismo numero.
     *
     * Solo lee: no toca estado de simulacion ni consume RNG, asi que llamarlo no
     * cambia el fingerprint ni la evolucion de la partida.
     */
    void LogDemographics() const;

    /** Desglose del coste del tick por etapas + memoria de las estructuras
        (consola: Eco.Profile). Es el punto de partida obligatorio de la Fase 6. */
    void LogTickProfile() const;

    /** Fase 6 (6.4): lo lee el perfilador de frame para decir que fraccion del
        frame se lleva la simulacion. */
    const FEcoTickProfile& GetTickProfile() const { return Profile; }

    /** Fase 6 (6.3): parametros del multiplicador de CO2, ya resueltos contra la
        consola (Eco.CO2.Enable). Lo usan el tick, la germinacion y el heatmap de
        idoneidad, para que los tres evaluen EXACTAMENTE la misma funcion. */
    EcoCarbon::FCO2Params GetCO2Params() const;

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
    /** Fase 5 (Paso 5): puntos de muerte en el terreno. bLogResult=false para el
        repintado automatico del modo live, que si no llenaria el log a 2 lineas
        por segundo (correccion B9). */
    void PaintDecompositionField(bool bLogResult = true);
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

    // --- Etapas de SimulateTick ---
    // Extraidas a metodos con nombre para que el tick sea un orquestador legible
    // y cada etapa se pueda razonar (y en el futuro testear) por separado. El
    // ORDEN de llamada es fijo: es parte del contrato de determinismo del doc 2.5.

    /** Prepara el scratch por-tarea (chunks deterministas + reservas). Devuelve
        el nº de chunks, derivado SOLO de la poblacion y del grain de settings. */
    int32 PrepareTickScratch(const UEcosystemSettings& Settings);

    /** PASO 2 (paralelo): crecimiento/estres/mortalidad/semillas por chunk. Cada
        chunk solo lee del snapshot y escribe en su porcion de Agents_Write y en
        su propio FTickScratch. */
    void RunGrowthParallel(float DtYears, const UEcosystemSettings& Settings,
        const EcoCarbon::FCO2Params& CO2, int32 NumChunks);

    /** Decae el campo de descomposicion y aplica los pulsos de muerte del tick
        (nutrientes + evento para la capa de suelo + mancha visible). Serial. */
    void ApplyDeathPulses(float DtYears, const UEcosystemSettings& Settings);

    /** Germinacion serial de PendingSeeds: espaciado minimo (spatial hash +
        recien nacidas), sitio seguro y probabilidad por vigor local. */
    void RunGermination(float DtYears, const UEcosystemSettings& Settings,
        const EcoCarbon::FCO2Params& CO2);

    /** Rehace el grid de luz grueso con la poblacion actual (ClearShadow + deposito
        de todas las copas). Lo llama SimulateTick al principio del tick, y LoadState
        despues de cargar: sin esto un bake cargado deja la luz del bosque ANTERIOR,
        y como LoadState deja la sim en pausa nadie la refresca -> los hero trees que
        generes para el beauty shot crecerian contra la sombra de vecinos que ya no
        existen, que es justo la feature que vende el doc. 3.5. */
    void RebuildCoarseLight();
    void DrawDebug();
    void EnsureHeatmapDecal();

    /** Camino UNICO de todos los heatmaps (los seis Eco.Paint* tenian el mismo
        cuerpo copiado): sube Values al visualizador, garantiza el decal y loguea.
        bAutoRange=false fija la escala a [MinValue, MaxValue] en vez de usar el
        min/max del buffer. */
    void PaintField(const TArray<float>& Values, const TCHAR* LogLabel,
        bool bAutoRange = true, float MinValue = 0.f, float MaxValue = 1.f,
        bool bLogResult = true);

    /** Punto aleatorio sobre el terreno (XY uniforme en los limites, Z del
        relieve), consumiendo DOS valores del stream indicado. Lo comparten la
        siembra inicial (stream Colonization) y los agentes de debug (stream
        Debug), que antes lo calculaban cada uno por su lado. */
    FVector RandomPointOnTerrain(EEcoRngStream Stream);
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

    /** Fase 6 (6.4): ticks ejecutados en el frame actual (para stat/HUD). */
    int32  TicksLastFrame = 0;

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

    /** Scratch por-tarea del paso paralelo. PERSISTENTE: se reutiliza cada tick
        (ResetForNextTick) en vez de reasignar en cada SimulateTick. Desde la
        optimizacion C1 guarda listas DISPERSAS de (celda, cantidad) en vez de un
        campo denso por tarea: ver FCellDelta. */
    TArray<FTickScratch> TickContexts;

    /** Posiciones germinadas en el tick en curso (el hash indexa Agents_Read y no
        las ve). Miembro para conservar la capacidad entre ticks. */
    TArray<FVector> NewbornPositions;

    /** Salidas de la reduccion serial. Miembros, no locales: asi la capacidad
        sobrevive al tick y la germinacion masiva no vuelve a pedir heap (C5). */
    TArray<FPendingSeed> PendingSeeds;
    TArray<FPendingDeathPulse> PendingDeaths;

    /** Instrumentacion del tick (Eco.Profile). Ver FEcoTickProfile. */
    FEcoTickProfile Profile;

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
