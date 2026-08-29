/**
 * @file TreeRenderSubsystem.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del gestor de niveles de representación del bosque.
 *
 * Contiene los CVars y comandos de consola `Eco.LOD.*`, `Eco.Season` y
 * `Eco.Wind.*`, la inicialización perezosa de la capa de render, el re-nivelado
 * `UpdateLOD` en tres pasadas (selección de los hero más cercanos, reparto de
 * niveles con sus datos por instancia y barrido de los árboles desaparecidos),
 * el volcado por lotes de altas, bajas y actualizaciones sobre los componentes
 * HISM, la cola amortizada de hero con swap diferido y caché LRU, y los relojes
 * de estación y de viento que se publican a Material Parameter Collections.
 * Toda la capa es una vista de solo lectura sobre @ref FTreePopulation: nunca
 * escribe en la simulación.
 *
 * @ingroup eco_render
 * @see @ref bib_clarkjh1976
 * @see @ref bib_instancing
 */

#include "Render/TreeRenderSubsystem.h"
#include "Render/TreeLibrary.h"
#include "Render/TreeInstanceHost.h"

#include "Config/EcosystemSettings.h"
#include "Core/EcoStats.h"                 // contadores y ámbitos de perfilado
#include "Simulation/EcosystemSubsystem.h"
#include "Species/SpeciesData.h"
#include "Geometry/HeroTreeActor.h"
#include "Terrain/LightFieldCoarse.h"      // AO de copa por instancia

#include "Ecology/TreePopulation.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "Algo/Unique.h"

DEFINE_LOG_CATEGORY_STATIC(LogEcoRender, Log, All);

// ---------------------------------------------------------------------------
//  CVars y comandos de consola
// ---------------------------------------------------------------------------
static TAutoConsoleVariable<float> CVarHeroRadius(TEXT("Eco.LOD.HeroRadius"), -1.f,
    TEXT("Radio (cm) del nivel hero. -1 = usar Project Settings."));

static TAutoConsoleVariable<float> CVarImpostorRadius(TEXT("Eco.LOD.ImpostorRadius"), -1.f,
    TEXT("Distancia (cm) a partir de la cual se usan impostors. -1 = usar Project Settings."));

static TAutoConsoleVariable<float> CVarCullRadius(TEXT("Eco.LOD.CullRadius"), -1.f,
    TEXT("Distancia (cm) a partir de la cual no se dibuja nada. -1 = usar Project Settings."));

static TAutoConsoleVariable<int32> CVarDrawTiers(TEXT("Eco.LOD.DrawTiers"), 0,
    TEXT("Dibuja cada arbol con el color de su nivel (rojo=hero, verde=instancia, azul=impostor)."));

// Interruptor en vivo del suavizado de crecimiento de los hero.
static TAutoConsoleVariable<int32> CVarSmoothHero(TEXT("Eco.LOD.SmoothHero"), 1,
    TEXT("1 = interpola la escala de los hero trees entre ticks (bosque vivo)."));

// Fija la estación a mano para demos y capturas; -1 = avance automático.
static TAutoConsoleVariable<float> CVarSeason(TEXT("Eco.Season"), -1.f,
    TEXT("Fuerza la estacion [0,1) (0=primavera, .25=verano, .5=otono, .75=invierno). -1 = auto."));

// --- Viento ---
static TAutoConsoleVariable<int32> CVarWind(TEXT("Eco.Wind.Enable"), 1,
    TEXT("1 = viento activo. 0 = fuerza 0 (el material deja de desplazar vertices: util para medir el coste del WPO)."));

static TAutoConsoleVariable<float> CVarWindStrength(TEXT("Eco.Wind.Strength"), -1.f,
    TEXT("Fuerza del viento [0..4]. -1 = usar Project Settings."));

static TAutoConsoleVariable<float> CVarWindDir(TEXT("Eco.Wind.Dir"), -1.f,
    TEXT("Direccion del viento en grados [0..360). -1 = usar Project Settings."));

// --- Oclusión ambiental de copa por instancia ---
static TAutoConsoleVariable<int32> CVarCanopyAO(TEXT("Eco.CanopyAO"), 1,
    TEXT("1 = escribe la apertura de copa en PerInstanceCustomData[2] (AO por instancia). 0 = ablacion."));

static UTreeRenderSubsystem* GetRender(UWorld* World)
{
    return World ? World->GetSubsystem<UTreeRenderSubsystem>() : nullptr;
}

/**
 * Resuelve la convención común a los CVars numéricos de esta capa: un valor
 * negativo significa «no lo fuerzo, usa Project Settings».
 *
 * @param CVar          Anulación por consola; negativa = sin anular.
 * @param SettingsValue Valor de Project Settings que rige si no hay anulación.
 * @return El valor efectivo del parámetro.
 */
static float CVarOverrideOr(const TAutoConsoleVariable<float>& CVar, float SettingsValue)
{
    const float Override = CVar.GetValueOnGameThread();
    return (Override >= 0.f) ? Override : SettingsValue;
}

/**
 * Sequedad del follaje que se publica en `PerInstanceCustomData[1]`.
 *
 * 0 es follaje sano y verde, 1 seco o senescente. La escala va en ese sentido y
 * no al revés porque los hero trees no llevan custom data y leen 0 por defecto.
 *
 * @param State  Estado ecológico del árbol.
 * @param Stress Estrés acumulado, en [0,1].
 * @return Sequedad en [0,1].
 */
static float DrynessOf(ETreeState State, float Stress)
{
    switch (State)
    {
    case ETreeState::Senescent: return FMath::Clamp(0.6f + 0.4f * Stress, 0.f, 1.f);

    // Un suprimido está apagado pero vivo y puede recuperarse: se dibuja según su
    // estrés, con algo más de peso que un sano, y no como un senescente. Sin este
    // caso caería en el default y saldría completamente seco.
    case ETreeState::Suppressed: return FMath::Clamp(0.5f * Stress, 0.f, 1.f);

    case ETreeState::Sapling:
    case ETreeState::Mature:    return FMath::Clamp(0.35f * Stress, 0.f, 1.f);
    default:                    return 1.f; // Dead: no debería llegar a dibujarse
    }
}

static FAutoConsoleCommandWithWorld GLodStats(TEXT("Eco.LOD.Stats"),
    TEXT("Loguea el reparto de arboles por nivel de detalle y el coste de la libreria."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UTreeRenderSubsystem* S = GetRender(W)) S->LogStats(); }));

static FAutoConsoleCommandWithWorld GLodBakeAll(TEXT("Eco.LOD.BakeAll"),
    TEXT("Hornea de golpe toda la libreria de arquetipos (evita hitches durante una demo)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UTreeRenderSubsystem* S = GetRender(W)) S->BakeLibraryNow(); }));

static FAutoConsoleCommandWithWorld GLodRebuild(TEXT("Eco.LOD.Rebuild"),
    TEXT("Tira todas las instancias y hero trees y vuelve a repartir niveles desde cero."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UTreeRenderSubsystem* S = GetRender(W)) S->RebuildAll(); }));

static FAutoConsoleCommandWithWorldAndArgs GLodEnable(TEXT("Eco.LOD.Enable"),
    TEXT("Activa (1) o desactiva (0) la capa de render instanciada. Uso: Eco.LOD.Enable [0|1]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* W)
        {
            if (UTreeRenderSubsystem* S = GetRender(W))
            {
                S->SetEnabled(Args.Num() > 0 ? (FCString::Atoi(*Args[0]) != 0) : true);
            }
        }));

static FAutoConsoleCommandWithWorldAndArgs GLodFreeze(TEXT("Eco.LOD.Freeze"),
    TEXT("Congela (1) el re-nivelado: util para inspeccionar un reparto de LOD. Uso: Eco.LOD.Freeze [0|1]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* W)
        {
            if (UTreeRenderSubsystem* S = GetRender(W))
            {
                S->SetFrozen(Args.Num() > 0 ? (FCString::Atoi(*Args[0]) != 0) : true);
            }
        }));

// --- Viento ---
static FAutoConsoleCommandWithWorld GWindApply(TEXT("Eco.Wind.Apply"),
    TEXT("Reaplica a los componentes ISM los ajustes de viento de Project Settings (WPO, distancia de corte, bounds)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UTreeRenderSubsystem* S = GetRender(W)) S->ApplyWindSettings(); }));

static FAutoConsoleCommandWithWorld GWindLog(TEXT("Eco.Wind.Log"),
    TEXT("Loguea el estado actual del viento (direccion, fuerza, rafaga) y la configuracion de WPO."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UTreeRenderSubsystem* S = GetRender(W)) S->LogWindState(); }));

// ---------------------------------------------------------------------------
//  Ciclo de vida
// ---------------------------------------------------------------------------
bool UTreeRenderSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UTreeRenderSubsystem::Deinitialize()
{
    ReleaseEverything();
    Super::Deinitialize();
}

TStatId UTreeRenderSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTreeRenderSubsystem, STATGROUP_Tickables);
}

/** Copia a la configuración de la librería los ajustes de viento de Project Settings. */
static void FillWindConfig(const UEcosystemSettings& S, FTreeLibraryConfig& Cfg)
{
    const bool bWind = S.bEnableWind;
    Cfg.bWindOnInstances = bWind;
    Cfg.bWindOnImpostors = bWind && S.bWindOnImpostors;
    Cfg.WindWpoCutoffCm = S.WindWpoCutoffCm;
    Cfg.WindBoundsScale = S.WindBoundsScale; // misma fuente que los hero trees
}

/**
 * Inicialización perezosa de la capa de render.
 *
 * El orden de `OnWorldBeginPlay` entre subsistemas del mismo mundo no está
 * garantizado, así que en lugar de dar por hecho que el ecosistema ya generó el
 * relieve se espera a que declare `IsWorldReady()`. Deja listos el actor host,
 * la librería de arquetipos y los dos Material Parameter Collections.
 *
 * @return true si la capa está inicializada y se puede usar en este tick.
 */
bool UTreeRenderSubsystem::EnsureInitialized()
{
    if (bInitialized)
    {
        return true;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    Eco = World->GetSubsystem<UEcosystemSubsystem>();
    if (!Eco || !Eco->IsWorldReady())
    {
        return false;
    }

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // El host es idéntico al de la capa de suelo: se crea con la misma fábrica.
    Host = ATreeInstanceHost::SpawnHost(World, TEXT("TreeInstanceHost"));
    if (!Host)
    {
        return false;
    }

    FTreeLibraryConfig Cfg;
    Cfg.NumAgeBuckets = S->NumAgeBuckets;
    Cfg.NumInstanceCustomDataFloats = S->NumInstanceCustomDataFloats;
    Cfg.bInstancesCastShadow = S->bInstancesCastShadow;
    Cfg.bImpostorsCastShadow = S->bImpostorsCastShadow;
    // El propio ISM culla por distancia; se le da un 20 % de margen sobre el radio
    // del gestor para que el cull no compita con la conmutación de nivel.
    Cfg.InstanceEndCullDistanceCm = S->ImpostorRadiusCm * 1.2f;
    Cfg.ImpostorEndCullDistanceCm = S->CullRadiusCm * 1.2f;
    FillWindConfig(*S, Cfg);

    Library = NewObject<UTreeLibrary>(this);
    Library->Initialize(Host, Eco->GetSpeciesList(), Cfg);

    bEnabled = S->bEnableTreeRendering;

    if (S->bPrebakeLibraryOnStart && bEnabled)
    {
        Library->BakeAll();
    }

    // Un Eco.Load sustituye la población entera: States, HeroActors y los mapeos
    // instancia -> StableId quedan referidos a árboles de otra ejecución.
    Eco->OnStateLoaded.AddUObject(this, &UTreeRenderSubsystem::HandleStateLoaded);

    // Los dos Material Parameter Collections se resuelven UNA vez aquí:
    // LoadSynchronous en cada frame desde UpdateSeason/UpdateWind es inaceptable.
    SeasonMPCCached = S->SeasonMPC.LoadSynchronous();
    if (!SeasonMPCCached)
    {
        UE_LOG(LogEcoRender, Log,
            TEXT("[Eco/LOD] Sin SeasonMPC en Project Settings: el ciclo estacional no se aplicara."));
    }

    WindMPCCached = S->WindMPC.LoadSynchronous();
    if (!WindMPCCached)
    {
        UE_LOG(LogEcoRender, Log,
            TEXT("[Eco/Viento] Sin WindMPC en Project Settings: el viento no se aplicara. "
                "Puedes asignar el MISMO asset que SeasonMPC (los nombres de parametro no chocan)."));
    }

    bInitialized = true;

    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Capa de render lista (%d especies, %d buckets, %d floats por instancia)."),
        Eco->GetSpeciesList().Num(), S->NumAgeBuckets, S->NumInstanceCustomDataFloats);
    return true;
}


/**
 * Destruye los actores hero cacheados y vacía el bookkeeping asociado.
 *
 * @warning Los tres contenedores (`HeroActors`, `HeroQueue`, `HeroInfo`) se
 *          vacían juntos: una entrada superviviente apunta a un actor ya
 *          destruido y el fallo aparece varios frames después.
 */
void UTreeRenderSubsystem::DestroyAllHeroActors()
{
    for (TPair<uint32, FHeroSlot>& It : HeroActors)
    {
        if (It.Value.Actor)
        {
            It.Value.Actor->Destroy();
        }
    }
    HeroActors.Reset();
    HeroQueue.Reset();
    HeroInfo.Reset();
}

/** Deshace la inicialización: actores, estado de render, librería y actor host. */
void UTreeRenderSubsystem::ReleaseEverything()
{
    if (Eco)
    {
        Eco->OnStateLoaded.RemoveAll(this); // el delegate sobrevive a este subsistema
    }

    DestroyAllHeroActors();
    HeroSet.Reset();
    HeroBest.Reset();
    DistSqCache.Reset();
    States.Reset();
    Pending.Reset();
    ResolvedUpdates.Reset();
    BatchXforms.Reset();
    SeasonMPCCached = nullptr;
    WindMPCCached = nullptr;

    if (Library)
    {
        Library->Shutdown();
        Library = nullptr;
    }
    if (Host)
    {
        Host->Destroy();
        Host = nullptr;
    }
    bInitialized = false;
}

void UTreeRenderSubsystem::SetEnabled(bool bInEnabled)
{
    if (bEnabled == bInEnabled)
    {
        return;
    }
    bEnabled = bInEnabled;

    if (!bEnabled)
    {
        RebuildAll(); // suelta instancias y heroes; la simulación sigue intacta
    }
    else
    {
        bForceRelevel = true;
    }
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Capa de render %s."), bEnabled ? TEXT("ACTIVADA") : TEXT("DESACTIVADA"));
}

void UTreeRenderSubsystem::RebuildAll()
{
    // Suelta el estado de render, no la librería: las mallas horneadas se
    // conservan, que es lo caro de reconstruir.
    Pending.Reset();
    States.Reset();
    if (Library)
    {
        Library->ClearAllInstances();
    }

    DestroyAllHeroActors();

    bForceRelevel = true;
}

/** Reacción a la carga de un bake: la población cargada no tiene nada que ver con la anterior. */
void UTreeRenderSubsystem::HandleStateLoaded()
{
    RebuildAll();
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Bake cargado: estado de render reconstruido."));
}

void UTreeRenderSubsystem::BakeLibraryNow()
{
    if (EnsureInitialized() && Library)
    {
        Library->BakeAll();
        bForceRelevel = true;
    }
}

void UTreeRenderSubsystem::ApplyWindSettings()
{
    if (!EnsureInitialized() || !Library)
    {
        return;
    }

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    FTreeLibraryConfig Cfg;
    FillWindConfig(*S, Cfg);
    Library->ApplyWindSettings(Cfg);

    UE_LOG(LogEcoRender, Log, TEXT("[Eco/Viento] Viento reaplicado: instancias=%s impostors=%s corte=%.0f cm."),
        Cfg.bWindOnInstances ? TEXT("si") : TEXT("no"),
        Cfg.bWindOnImpostors ? TEXT("si") : TEXT("no"),
        Cfg.WindWpoCutoffCm);
}

void UTreeRenderSubsystem::LogWindState() const
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/Viento] Viento: dir %.0f deg | fuerza %.3f (rafaga %.2f) | reloj %.1f s | MPC %s"),
        WindDirDegNow, WindStrengthNow, WindGustNow, WindTime,
        WindMPCCached ? TEXT("asignado") : TEXT("NO ASIGNADO"));
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/Viento] WPO: corte %.0f cm | impostors %s | AO por instancia %s"),
        S->WindWpoCutoffCm,
        S->bWindOnImpostors ? TEXT("con viento") : TEXT("estaticos"),
        (S->bCanopyAOInstanceData && CVarCanopyAO.GetValueOnGameThread() != 0) ? TEXT("ON") : TEXT("OFF"));
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------

/**
 * Orquesta un frame de la capa de render.
 *
 * El orden es: relojes globales de estación y viento (siempre, aun con la capa
 * desactivada), horneado amortizado de la librería, cola de hero, re-nivelado
 * cada `RelevelEveryNFrames` frames, dibujo de depuración opcional, suavizado de
 * la escala de los hero y volcado de contadores.
 *
 * @param DeltaTime Segundos de tiempo real desde el frame anterior.
 */
void UTreeRenderSubsystem::Tick(float DeltaTime)
{
    if (!EnsureInitialized())
    {
        return;
    }

    // Los relojes GLOBALES de material (estación y viento) se publican siempre,
    // aunque la capa instanciada esté apagada:
    //   - Los hero trees sueltos (Eco.GrowHeroTree) viven al margen del gestor de
    //     LOD y también tienen que moverse y cambiar de estación.
    //   - Con Eco.LOD.Enable 0 se comparan capturas, y el bosque de referencia
    //     debe quedar en la misma estación.
    // Cuesta dos escrituras de parámetro por frame.
    UpdateSeason(DeltaTime);
    UpdateWind(DeltaTime);

    if (!bEnabled)
    {
        return;
    }

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // Horneado amortizado: unos pocos arquetipos por frame, nunca todos de golpe.
    Library->ProcessBakeQueue(S->MaxBakesPerFrame);

    // Generación de hero trees amortizada: unos milisegundos de golpe en el game
    // thread se ven como un hitch.
    ProcessHeroQueue(S->MaxHeroPerFrame);

    // Cadencia: el re-nivelado completo cada N frames. Los árboles se mueven
    // despacio respecto a la cámara, así que no hace falta cada frame.
    ++FramesSinceRelevel;
    const int32 Every = FMath::Max(1, S->RelevelEveryNFrames);
    if (!bFrozen && (bForceRelevel || FramesSinceRelevel >= Every))
    {
        FVector ViewLocation;
        if (GetViewLocation(ViewLocation))
        {
            const double T0 = FPlatformTime::Seconds();
            UpdateLOD(ViewLocation);
            LastRelevelMs = (FPlatformTime::Seconds() - T0) * 1000.0;

            FramesSinceRelevel = 0;
            bForceRelevel = false;
        }
    }

    if (CVarDrawTiers.GetValueOnGameThread() != 0)
    {
        FVector ViewLocation;
        if (GetViewLocation(ViewLocation))
        {
            DrawTierDebug(ViewLocation);
        }
    }

    // Cada frame acerca la escala de los hero trees a su objetivo para que crezcan
    // de forma continua; son decenas de actores, así que sale barato.
    UpdateHeroInterpolation(DeltaTime);

    // Contadores para «stat EcoRender» y para el CSV de perfilado.
    SET_DWORD_STAT(STAT_EcoNumHero, NumHero);
    SET_DWORD_STAT(STAT_EcoNumInstance, NumInstance);
    SET_DWORD_STAT(STAT_EcoNumImpostor, NumImpostor);
    SET_DWORD_STAT(STAT_EcoNumCulled, NumCulled);
    CSV_CUSTOM_STAT(Eco, TreesHero, NumHero, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, TreesInstance, NumInstance, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, TreesImpostor, NumImpostor, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, RelevelMs, (float)LastRelevelMs, ECsvCustomStatOp::Set);
}

/**
 * Punto de vista contra el que se miden las distancias del re-nivelado.
 *
 * @param OutLocation Recibe la posición de la cámara, en coordenadas de mundo.
 * @return false si aún no hay ningún punto de vista disponible; en ese caso el
 *         re-nivelado se pospone al siguiente frame.
 */
bool UTreeRenderSubsystem::GetViewLocation(FVector& OutLocation) const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    if (APlayerController* PC = World->GetFirstPlayerController())
    {
        if (PC->PlayerCameraManager)
        {
            OutLocation = PC->PlayerCameraManager->GetCameraLocation();
            return true;
        }
    }

    // Sin PlayerController (p. ej. en el viewport de simulación) se usa el punto
    // de vista que el render dibujó el último frame.
    if (World->ViewLocationsRenderedLastFrame.Num() > 0)
    {
        OutLocation = World->ViewLocationsRenderedLastFrame[0];
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Re-nivelado
// ---------------------------------------------------------------------------

/**
 * Reasigna a cada árbol vivo su nivel de representación y su arquetipo.
 *
 * Recorre la población en tres pasadas:
 * @li 1. Selecciona los `HeroBudget` árboles más cercanos dentro del radio de
 *        hero y cachea la distancia al cuadrado de toda la población.
 * @li 2. Decide el nivel de cada árbol por distancia, calcula sus datos por
 *        instancia y su arquetipo, y encola lo que haya cambiado.
 * @li 3. Libera los estados que no llevan el sello de esta pasada: árboles que
 *        han muerto y que la simulación ya ha compactado.
 *
 * Termina aplicando el lote acumulado (FlushInstanceOps) y desalojando los hero
 * más antiguos.
 *
 * @param ViewLocation Posición de la cámara en coordenadas de mundo.
 * @note Corre una vez cada `RelevelEveryNFrames` frames, no cada frame.
 */
void UTreeRenderSubsystem::UpdateLOD(const FVector& ViewLocation)
{
    SCOPE_CYCLE_COUNTER(STAT_EcoRelevel);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_UpdateLOD);

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const FTreePopulation& Pop = Eco->GetPopulation();
    const int32 PopNum = Pop.Num();

    const float HeroR = CVarOverrideOr(CVarHeroRadius, S->HeroRadiusCm);
    const float ImpR = CVarOverrideOr(CVarImpostorRadius, S->ImpostorRadiusCm);
    const float CullR = CVarOverrideOr(CVarCullRadius, S->CullRadiusCm);

    const double HeroR2 = static_cast<double>(HeroR) * HeroR;
    const double ImpR2 = static_cast<double>(ImpR) * ImpR;
    const double CullR2 = static_cast<double>(CullR) * CullR;

    const int32 NumBuckets = FMath::Max(1, S->NumAgeBuckets);
    ++VisitStamp;

    // Oclusión ambiental de copa por instancia: se muestrea la rejilla de luz GRUESA
    // a media altura de la copa de cada árbol INSTANCIADO, no de los impostores (a
    // esa distancia no se aprecia y son la mayoría). Es un muestreo trilineal por
    // árbol dibujado y por re-nivelado, no por frame ni por vértice.
    const FLightFieldCoarse& Light = Eco->GetLightCoarse();
    const bool bCanopyAO = S->bCanopyAOInstanceData
        && CVarCanopyAO.GetValueOnGameThread() != 0
        && Light.IsValid();

    // --- 1) Selección de hero: los HeroBudget más cercanos dentro de R_hero ---
    // Selección PARCIAL, no una ordenación completa: dentro de HeroRadiusCm hay
    // cientos de árboles en bosque denso y solo interesan HeroBudget. Se mantiene
    // un array acotado y ya ordenado por distancia, así que un candidato más lejano
    // que el peor del array se descarta en O(1), que es el caso común.
    // De paso se cachea la distancia al cuadrado para que la pasada 2 no la repita.
    const int32 HeroBudget = FMath::Max(0, S->HeroBudget);
    HeroBest.Reset();
    DistSqCache.SetNumUninitialized(PopNum, EAllowShrinking::No);

    for (int32 i = 0; i < PopNum; ++i)
    {
        // En double de principio a fin: las posiciones de mundo de UE5 son double
        // y a estas distancias (al cuadrado ~1e10) el float pierde precisión.
        const double D2 = FVector::DistSquared(Pop.Position[i], ViewLocation);
        DistSqCache[i] = D2;

        if (HeroBudget == 0 || Pop.State[i] == ETreeState::Dead || D2 >= HeroR2) { continue; }
        if (HeroBest.Num() == HeroBudget && D2 >= HeroBest.Last().Key) { continue; } // no entra

        int32 Insert = HeroBest.Num();
        while (Insert > 0 && HeroBest[Insert - 1].Key > D2) { --Insert; }
        HeroBest.Insert(TPair<double, int32>(D2, i), Insert);
        if (HeroBest.Num() > HeroBudget) { HeroBest.Pop(EAllowShrinking::No); }
    }

    HeroSet.Reset();
    for (const TPair<double, int32>& Cand : HeroBest)
    {
        HeroSet.Add(Pop.StableId[Cand.Value]);
    }

    // --- 2) Nivel deseado y arquetipo de cada árbol ---
    NumHero = NumInstance = NumImpostor = NumCulled = 0;

    for (int32 i = 0; i < PopNum; ++i)
    {
        if (Pop.State[i] == ETreeState::Dead) { continue; }

        const uint32 StableId = Pop.StableId[i];
        const double D2 = DistSqCache[i];
        const bool bWantsHero = HeroSet.Contains(StableId);

        // SALIDA TEMPRANA DE LOS CULLADOS, antes de tocar el TMap de estados y de
        // calcular el arquetipo (tres hashes y una FTransform). Con decenas de
        // miles de árboles, la mayoría cae más allá de CullRadiusCm: para ellos el
        // coste queda en un test de distancia, y States se mantiene proporcional a
        // lo que de verdad se dibuja en vez de crecer hasta el tamaño de la
        // población entera.
        if (!bWantsHero && D2 >= CullR2)
        {
            ++NumCulled;
            if (FTreeRenderState* Existing = States.Find(StableId))
            {
                LeaveTier(StableId, *Existing);
                States.Remove(StableId);
            }
            continue;
        }

        const USpeciesData* Sp = Eco->GetSpeciesById(Pop.SpeciesId[i]);
        if (!Sp) { continue; }

        FTreeRenderState& State = States.FindOrAdd(StableId);
        State.Stamp = VisitStamp;

        const ETreeRenderTier Want =
            bWantsHero ? ETreeRenderTier::Hero :
            (D2 < ImpR2) ? ETreeRenderTier::Instance :
            ETreeRenderTier::Impostor;

        switch (Want)
        {
        case ETreeRenderTier::Hero:     ++NumHero; break;
        case ETreeRenderTier::Instance: ++NumInstance; break;
        default:                        ++NumImpostor; break;
        }

        // --- Datos por instancia ---
        // Se calculan aquí porque los necesitan tanto la rama estable (actualizar)
        // como EnterTier (sembrar la custom data de una instancia recién creada).
        const float Dryness = DrynessOf(Pop.State[i], Pop.Stress[i]);

        float CanopyAO = 1.f;
        if (bCanopyAO && Want == ETreeRenderTier::Instance)
        {
            // A media altura de copa: ni el suelo (siempre sombrío) ni el ápice
            // (siempre al sol). Es la banda donde de verdad se distingue si el
            // árbol está bajo el dosel de un vecino o es el que domina.
            const FVector Probe = Pop.Position[i] + FVector(0.f, 0.f, Pop.Height[i] * 0.6f);
            CanopyAO = FMath::Clamp(Light.SampleLightSmooth(Probe) / FLightFieldCoarse::FullSunlight, 0.f, 1.f);
        }
        const FVector2f CustomData(Dryness, CanopyAO);

        // Estaba encolado como hero y ha dejado de serlo mientras esperaba: se
        // cancela la generación. Si no, ProcessHeroQueue le montaría un actor a un
        // árbol que ya no es hero, y además por delante de los que sí lo son.
        if (State.bHeroPending && Want != ETreeRenderTier::Hero)
        {
            HeroQueue.Remove(StableId);
            HeroInfo.Remove(StableId);
            State.bHeroPending = false;
        }

        // Arquetipo: bucket de tamaño con histéresis más variante estable.
        const float Ratio = TreeArchetype::HeightRatio(Pop.Biomass[i], Sp->MaxBiomass);
        const int32 Bucket = TreeArchetype::BucketWithHysteresis(Ratio, State.Bucket, NumBuckets, S->BucketHysteresis);
        const uint8 Variant = TreeArchetype::VariantOf(StableId, Sp->NumLodVariants);
        const FArchetypeKey Key(Pop.SpeciesId[i], static_cast<uint8>(Bucket), Variant);

        const float ScaleInBucket = TreeArchetype::ScaleWithinBucket(Ratio, Bucket, NumBuckets);
        const FTransform Xform = TreeArchetype::InstanceTransform(
            Pop.Position[i], StableId, ScaleInBucket, S->InstanceScaleJitter);

        // Mantiene al día la escala objetivo del hero para que la interpolación por
        // frame (UpdateHeroInterpolation) lo siga suavemente aunque esta pasada de
        // re-nivelado no lo reorganice.
        if (Want == ETreeRenderTier::Hero)
        {
            if (FHeroSlot* HSlot = HeroActors.Find(StableId))
            {
                HSlot->TargetScale = static_cast<float>(Xform.GetScale3D().X);
            }
        }

        const bool bTierChanged = (State.Tier != Want);
        // Si no hay representación válida (p. ej. el arquetipo aún no estaba
        // horneado), PackedKey no describe nada y hay que reintentar. El centinela
        // es un bool aparte, y no PackedKey == 0, porque esa clave es la legítima
        // especie 0 / bucket 0 / variante 0: la plántula más común del bosque.
        const bool bKeyChanged = !State.bHasRepresentation || (State.PackedKey != Key.Pack());

        if (!bTierChanged && !bKeyChanged)
        {
            // Caso común con diferencia: nada que reorganizar, solo la escala del
            // que ha crecido. Con umbral, porque la mayoría no cambia de forma
            // perceptible entre dos re-nivelados.
            if (FMath::Abs(ScaleInBucket - State.LastScale) > S->ScaleUpdateThreshold)
            {
                if (State.Tier == ETreeRenderTier::Hero)
                {
                    // Con suavizado, TargetScale ya se actualizó arriba y la
                    // interpolación por frame lo alcanza; sin él, salto directo.
                    const bool bSmooth = S->bSmoothHeroGrowth && (CVarSmoothHero.GetValueOnGameThread() != 0);
                    if (!bSmooth)
                    {
                        if (FHeroSlot* Slot = HeroActors.Find(StableId))
                        {
                            if (Slot->Actor) { Slot->Actor->SetActorScale3D(Xform.GetScale3D()); }
                        }
                    }
                }
                else if (State.InstanceIndex >= 0)
                {
                    const uint64 CompKey = MakeComponentKey(State.PackedKey, State.Tier == ETreeRenderTier::Impostor);
                    Pending.FindOrAdd(CompKey).Updates.Add(TPair<uint32, FTransform>(StableId, Xform));
                }
                State.LastScale = ScaleInBucket;
            }

            // Sequedad y apertura de copa. Solo aquí, en la rama estable (sin
            // cambio de nivel ni de arquetipo), donde el índice de instancia no se
            // mueve en este flush; la instancia recién creada la cubren EnterTier y
            // FlushInstanceOps.
            //
            // Se reescriben SOLO si alguno cruza de banda (16 niveles): sin ese
            // umbral se estaría tocando la custom data de todas las instancias en
            // cada re-nivelado.
            if (State.InstanceIndex >= 0 &&
                (State.Tier == ETreeRenderTier::Instance || State.Tier == ETreeRenderTier::Impostor))
            {
                const uint8 Qd = Quantize16(Dryness);
                const uint8 Qc = Quantize16(CanopyAO);
                if (Qd != State.LastVitalityQ || Qc != State.LastCanopyQ)
                {
                    const uint64 CompKey = MakeComponentKey(State.PackedKey, State.Tier == ETreeRenderTier::Impostor);
                    Pending.FindOrAdd(CompKey).CustomUpdates.Add(TPair<uint32, FVector2f>(StableId, CustomData));
                    State.LastVitalityQ = Qd;
                    State.LastCanopyQ = Qc;
                }
            }

            State.Bucket = Bucket;
            continue;
        }

        if (Want == ETreeRenderTier::Hero)
        {
            FPendingHero Info;
            Info.Key = Key;
            Info.Position = Xform.GetLocation();
            Info.Scale = static_cast<float>(Xform.GetScale3D().X);
            Info.ScaleInBucket = ScaleInBucket;
            HeroInfo.Add(StableId, Info);      // sobrescribe: refresca posición y escala
            HeroQueue.AddUnique(StableId);
            State.bHeroPending = true;
            State.Bucket = Bucket;
            continue;                          // la representación actual sigue en pantalla
        }

        LeaveTier(StableId, State);
        EnterTier(StableId, State, Want, Key, Xform, ScaleInBucket, Dryness, CanopyAO);
        State.Bucket = Bucket;
    }

    // --- 3) Árboles que ya no están: muertos y compactados por la simulación ---
    for (TMap<uint32, FTreeRenderState>::TIterator It(States); It; ++It)
    {
        if (It.Value().Stamp != VisitStamp)
        {
            LeaveTier(It.Key(), It.Value());
            It.RemoveCurrent();
        }
    }

    // --- 4) Aplicar los cambios acumulados, agrupados por componente ---
    FlushInstanceOps();
    EvictOldHeroes();
}

/**
 * Suelta la representación actual de un árbol y deja su estado sin representar.
 *
 * En los niveles instanciados no borra nada de inmediato: encola la baja para el
 * siguiente FlushInstanceOps. En el nivel hero oculta el actor sin destruirlo.
 *
 * @param StableId Identificador estable del árbol.
 * @param State    Estado de render, que queda en `None` y sin representación.
 */
void UTreeRenderSubsystem::LeaveTier(uint32 StableId, FTreeRenderState& State)
{
    switch (State.Tier)
    {
    case ETreeRenderTier::Hero:
        ReleaseHero(StableId);
        break;

    case ETreeRenderTier::Instance:
    case ETreeRenderTier::Impostor:
        if (State.InstanceIndex >= 0)
        {
            const uint64 CompKey = MakeComponentKey(State.PackedKey, State.Tier == ETreeRenderTier::Impostor);
            Pending.FindOrAdd(CompKey).Removes.Add(State.InstanceIndex);
        }
        break;

    default:
        break;
    }

    State.InstanceIndex = -1;
    State.Tier = ETreeRenderTier::None;
    State.bHasRepresentation = false; // la clave guardada ya no describe nada
    // La próxima instancia será otra (posiblemente en otro componente) y nacerá
    // con la custom data a 0. Sin devolver los centinelas a «nunca escrito», el
    // comparador de banda (Q != LastQ) daría falso y los datos por instancia no se
    // volverían a escribir nunca.
    State.LastVitalityQ = 255;
    State.LastCanopyQ = 255;
    State.bHeroPending = false;
}

/**
 * Da de alta la representación de un árbol en un nivel instanciado.
 *
 * Encola el alta y su custom data inicial en el lote del componente que
 * corresponde al arquetipo; el índice de instancia lo asigna el flush. Si el
 * arquetipo aún no está horneado, el estado queda sin representación y el
 * siguiente re-nivelado lo reintenta.
 *
 * @param StableId      Identificador estable del árbol.
 * @param State         Estado de render que se actualiza.
 * @param Want          Nivel destino; `Instance`, `Impostor` o `None`.
 * @param Key           Arquetipo (especie, bucket, variante) que lo representa.
 * @param Xform         Transformación de la instancia, en espacio local del host.
 * @param ScaleInBucket Escala continua dentro del bucket, para el umbral de update.
 * @param Dryness       Sequedad inicial de la instancia, en [0,1].
 * @param CanopyAO      Apertura de copa inicial, en [0,1].
 * @pre `Want` nunca es `Hero`: esa transición es diferida y la consuma
 *      CommitHeroTier.
 */
void UTreeRenderSubsystem::EnterTier(uint32 StableId, FTreeRenderState& State, ETreeRenderTier Want,
    const FArchetypeKey& Key, const FTransform& Xform, float ScaleInBucket,
    float Dryness, float CanopyAO)
{
    State.LastScale = ScaleInBucket;

    if (Want == ETreeRenderTier::None)
    {
        State.Tier = ETreeRenderTier::None;
        State.PackedKey = Key.Pack();
        State.bHasRepresentation = true; // «no dibujar» es una decisión válida y estable
        return;
    }

    // El nivel hero no entra por aquí: UpdateLOD lo encola y ProcessHeroQueue lo
    // consuma cuando la malla está lista. Es un swap diferido, para que el árbol
    // no desaparezca mientras se genera su geometría.
    checkf(Want != ETreeRenderTier::Hero,
        TEXT("EnterTier no debe recibir Hero: la transicion a hero es diferida."));

    // Instancia o impostor: hace falta que el arquetipo esté horneado.
    const bool bImpostor = (Want == ETreeRenderTier::Impostor);
    FTreeArchetypeEntry* Entry = Library->FindOrRequestBake(Key);
    if (!Entry)
    {
        // Aún no horneado: se queda sin representar y se reintenta en el próximo
        // re-nivelado (bHasRepresentation = false fuerza que se vea como cambio).
        State.Tier = ETreeRenderTier::None;
        State.bHasRepresentation = false;
        return;
    }

    if (!Library->GetOrCreateComponent(Key, bImpostor))
    {
        State.Tier = ETreeRenderTier::None;
        State.bHasRepresentation = false;
        return;
    }

    FPendingComponentOps& Ops = Pending.FindOrAdd(MakeComponentKey(Key.Pack(), bImpostor));
    Ops.AddIds.Add(StableId);
    Ops.AddXforms.Add(Xform);
    Ops.AddCustom.Add(FVector2f(Dryness, CanopyAO)); // nace ya con su sequedad y su AO

    State.Tier = Want;
    State.PackedKey = Key.Pack();
    State.bHasRepresentation = true;
    State.InstanceIndex = -1; // lo asigna el flush
    // Sincroniza los centinelas de banda con lo que el flush va a escribir, para
    // que el siguiente re-nivelado no reescriba lo mismo.
    State.LastVitalityQ = Quantize16(Dryness);
    State.LastCanopyQ = Quantize16(CanopyAO);
}

/**
 * Vuelca al motor los cambios acumulados: una sola llamada de alta, una de baja y
 * una sola invalidación de render state por componente.
 *
 * Todas las escrituras intermedias van con `bMarkRenderStateDirty = false`
 * precisamente para que la invalidación sea única.
 *
 * @warning El orden por componente es obligatorio:
 * @li 1. Bajas (`RemoveInstances` por lote) y remapeo del bookkeeping.
 *        `RemoveInstances` borra con semántica `RemoveAt`, es decir desplaza
 *        hacia abajo todas las instancias de índice mayor: sin remapear, los
 *        índices guardados pasan a apuntar al árbol equivocado.
 * @li 2. Altas (`AddInstances` por lote), que devuelven los índices nuevos.
 * @li 3. Actualizaciones de transform, ya con los índices correctos.
 * @li 4. Datos por instancia (sequedad y apertura de copa).
 */
void UTreeRenderSubsystem::FlushInstanceOps()
{
    SCOPE_CYCLE_COUNTER(STAT_EcoFlushInstances);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_FlushInstances);

    if (!Library)
    {
        Pending.Reset();
        return;
    }

    for (TPair<uint64, FPendingComponentOps>& It : Pending)
    {
        const bool bImpostor = (It.Key & 1ull) != 0;
        const FArchetypeKey Key = FArchetypeKey::Unpack(static_cast<uint32>(It.Key >> 1));
        FPendingComponentOps& Ops = It.Value;

        FTreeArchetypeEntry* Entry = Library->Find(Key);
        if (!Entry) { continue; }

        UHierarchicalInstancedStaticMeshComponent* Comp = bImpostor ? Entry->ImpostorISM.Get() : Entry->MeshISM.Get();
        TArray<uint32>& Mapping = bImpostor ? Entry->ImpostorMapping : Entry->MeshMapping;
        if (!Comp) { continue; }

        // --- 1) BAJAS ---
        if (Ops.Removes.Num() > 0)
        {
            Ops.Removes.Sort(TGreater<int32>());
            int32 Unique = Algo::Unique(Ops.Removes);
            Ops.Removes.SetNum(Unique, EAllowShrinking::No);

            Comp->RemoveInstances(Ops.Removes);

            TreeInstancing::CompactMappingAfterRemoval(Mapping, Ops.Removes,
                [this](uint32 MovedId, int32 NewIndex)
                {
                    if (FTreeRenderState* Moved = States.Find(MovedId))
                    {
                        Moved->InstanceIndex = NewIndex;
                    }
                });
        }

        // --- 2) ALTAS ---
        if (Ops.AddXforms.Num() > 0)
        {
            const TArray<int32> NewIndices = Comp->AddInstances(Ops.AddXforms, /*bShouldReturnIndices*/ true);
            for (int32 k = 0; k < NewIndices.Num() && k < Ops.AddIds.Num(); ++k)
            {
                const uint32 Id = Ops.AddIds[k];
                const int32 Index = NewIndices[k];

                if (!Mapping.IsValidIndex(Index))
                {
                    Mapping.SetNum(Index + 1, EAllowShrinking::No);
                }
                Mapping[Index] = Id;

                if (FTreeRenderState* Added = States.Find(Id))
                {
                    Added->InstanceIndex = Index;
                }

                // Datos por instancia (PerInstanceCustomData, sin draw calls extra):
                //   [0] = desfase estacional estable por árbol; con él, el material
                //         evita que todos cambien de estación al unísono.
                //   [1] = sequedad (0 sano, 1 seco o senescente).
                //   [2] = apertura de copa para el AO: 1 a pleno sol, 0 bajo un
                //         dosel cerrado.
                if (Comp->NumCustomDataFloats > 0)
                {
                    Comp->SetCustomDataValue(Index, 0,
                        TreeArchetype::StableUnit(Id, TreeArchetype::SaltPhase), /*bMarkRenderStateDirty*/ false);
                }
                if (Ops.AddCustom.IsValidIndex(k))
                {
                    if (Comp->NumCustomDataFloats > 1)
                    {
                        Comp->SetCustomDataValue(Index, 1, Ops.AddCustom[k].X, /*bMarkRenderStateDirty*/ false);
                    }
                    if (Comp->NumCustomDataFloats > 2)
                    {
                        Comp->SetCustomDataValue(Index, 2, Ops.AddCustom[k].Y, /*bMarkRenderStateDirty*/ false);
                    }
                }
            }
        }

        // --- 3) ACTUALIZACIONES DE TRANSFORM, EN LOTE ---
        // Se guardan como (StableId, transform) porque el índice de instancia no se
        // conoce hasta después de las bajas. Aquí se resuelven a índices, se ORDENAN
        // y se agrupan en tiradas de índices consecutivos: cada tirada es una sola
        // BatchUpdateInstancesTransforms.
        if (Ops.Updates.Num() > 0)
        {
            ResolvedUpdates.Reset(Ops.Updates.Num());
            for (const TPair<uint32, FTransform>& Update : Ops.Updates)
            {
                if (const FTreeRenderState* Found = States.Find(Update.Key))
                {
                    if (Found->InstanceIndex >= 0)
                    {
                        ResolvedUpdates.Add(TPair<int32, FTransform>(Found->InstanceIndex, Update.Value));
                    }
                }
            }
            ResolvedUpdates.Sort([](const TPair<int32, FTransform>& A, const TPair<int32, FTransform>& B)
                { return A.Key < B.Key; });

            int32 Run = 0;
            while (Run < ResolvedUpdates.Num())
            {
                int32 End = Run + 1;
                while (End < ResolvedUpdates.Num() && ResolvedUpdates[End].Key == ResolvedUpdates[End - 1].Key + 1)
                {
                    ++End;
                }

                BatchXforms.Reset(End - Run);
                for (int32 k = Run; k < End; ++k) { BatchXforms.Add(ResolvedUpdates[k].Value); }

                Comp->BatchUpdateInstancesTransforms(ResolvedUpdates[Run].Key, BatchXforms,
                    /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
                Run = End;
            }
        }

        // --- 4) DATOS POR INSTANCIA: sequedad y apertura de copa ---
        if (Comp->NumCustomDataFloats > 1)
        {
            const bool bHasAO = Comp->NumCustomDataFloats > 2;
            for (const TPair<uint32, FVector2f>& CD : Ops.CustomUpdates)
            {
                if (const FTreeRenderState* Found = States.Find(CD.Key))
                {
                    if (Found->InstanceIndex >= 0)
                    {
                        Comp->SetCustomDataValue(Found->InstanceIndex, 1, CD.Value.X, /*bMarkRenderStateDirty*/ false);
                        if (bHasAO)
                        {
                            Comp->SetCustomDataValue(Found->InstanceIndex, 2, CD.Value.Y, /*bMarkRenderStateDirty*/ false);
                        }
                    }
                }
            }
        }

        // --- 5) UNA sola invalidación por componente ---
        Comp->MarkRenderStateDirty();
    }

    Pending.Reset();
}

// ---------------------------------------------------------------------------
//  Hero trees: generación amortizada y caché
// ---------------------------------------------------------------------------

/**
 * Genera o reactiva unos pocos hero trees por frame y consuma su swap diferido.
 *
 * Cada entrada de la cola se resuelve reactivando la ranura cacheada si conserva
 * el mismo arquetipo, o generando geometría nueva con la rejilla de luz del sitio.
 * Solo entonces CommitHeroTier suelta la instancia que representaba al árbol, de
 * modo que nunca hay un hueco en pantalla.
 *
 * @param MaxThisFrame Presupuesto de hero a resolver en este frame; se trata como
 *                     mínimo 1. Es lo que evita el hitch de generar varios árboles
 *                     completos en el game thread.
 * @see @ref bib_funkhouser1993
 */
void UTreeRenderSubsystem::ProcessHeroQueue(int32 MaxThisFrame)
{
    UWorld* World = GetWorld();
    if (!World || !Library)
    {
        return;
    }
    if (HeroQueue.Num() == 0 && Pending.Num() == 0)
    {
        return; // nada que hacer: ni siquiera se abre el ámbito de perfilado
    }

    SCOPE_CYCLE_COUNTER(STAT_EcoHeroGen);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_ProcessHeroQueue);

    // La cola se consume por la cabeza con un contador y se compacta con UN solo
    // RemoveAt al final; un RemoveAt(0) por elemento desplazaría todo el resto en
    // cada extracción. Nada dentro del bucle reencola ni borra de HeroQueue: las
    // altas las hace UpdateLOD, y ReleaseHero solo se alcanza desde LeaveTier de un
    // árbol cuyo nivel actual no es Hero.
    int32 Done = 0;
    int32 Consumed = 0;
    while (Consumed < HeroQueue.Num() && Done < FMath::Max(1, MaxThisFrame))
    {
        const uint32 StableId = HeroQueue[Consumed++];

        FPendingHero Info;
        if (!HeroInfo.RemoveAndCopyValue(StableId, Info))
        {
            continue;
        }

        // Puede haber dejado de ser hero mientras esperaba en la cola. Con el swap
        // diferido, un hero pendiente conserva Tier == Instance o Impostor.
        FTreeRenderState* State = States.Find(StableId);
        if (!State || (!State->bHeroPending && State->Tier != ETreeRenderTier::Hero))
        {
            continue;
        }

        FHeroSlot& Slot = HeroActors.FindOrAdd(StableId);
        Slot.LastUsedStamp = VisitStamp;

        // Cacheado y con el MISMO arquetipo: reentrar es instantáneo.
        if (Slot.Actor && Slot.GeneratedKey == Info.Key.Pack())
        {
            Slot.Actor->SetActorLocation(Info.Position);  // el actor cacheado puede venir de otra ejecución (Eco.Load)
            Slot.Actor->SetActorHiddenInGame(false);
            Slot.Actor->SetActorScale3D(FVector(Info.Scale));
            Slot.TargetScale = Info.Scale; // arranca a su tamaño, sin interpolar desde el anterior
            Slot.bActive = true;
            CommitHeroTier(StableId, *State, Info);  // ahora sí se suelta la instancia anterior
            ++Done;
            continue;
        }

        const USpeciesData* Base = Eco->GetSpeciesById(Info.Key.Species);
        const USpeciesData* ArchetypeSp = Library->GetArchetypeSpecies(Info.Key);
        if (!Base || !ArchetypeSp)
        {
            continue;
        }

        if (!Slot.Actor)
        {
            Slot.Actor = World->SpawnActor<AHeroTreeActor>(Info.Position, FRotator::ZeroRotator);
        }
        if (!Slot.Actor)
        {
            HeroActors.Remove(StableId);
            continue;
        }

        Slot.Actor->BarkMaterial = Base->BarkMaterial;
        Slot.Actor->LeafMaterial = Base->LeafMaterial;

        // Aquí se materializa la arquitectura en dos escalas: al hero sí se le pasa
        // la rejilla de luz gruesa, así que sus atractores en sombra de un vecino
        // grande se podan y la copa crece ladeada; y de la rejilla fina que deja
        // ese crecimiento sale además su AO de copa por vértice (ver
        // AHeroTreeActor::BuildNow). La librería instanciada es genérica y no puede
        // hacerlo: por eso los árboles cercanos se ven conscientes de su sitio y a
        // los lejanos no les hace falta estarlo.
        //
        // La curvatura del tronco viaja por la VARIANTE y no por la semilla del
        // árbol: el hero regenera con Hash32(StableId) -morfología única, luz de su
        // sitio- pero se dobla exactamente igual que la instancia a la que
        // sustituye, de modo que acercarse a un árbol arqueado no lo endereza de
        // golpe (véase UTreeLibrary::VariantDeformSeed).
        Slot.Actor->DeformSeedOverride =
            static_cast<int64>(UTreeLibrary::VariantDeformSeed(Info.Key.Species, Info.Key.Variant));

        Slot.Actor->Generate(ArchetypeSp, EcoRand::Hash32(StableId), &Eco->GetLightCoarse(), Info.Position);
        Slot.Actor->SetActorRotation(FRotator::ZeroRotator); // el hero no lleva el yaw estable de las instancias
        Slot.Actor->SetActorScale3D(FVector(Info.Scale));
        Slot.TargetScale = Info.Scale; // arranca a su tamaño, sin interpolar desde el anterior
        Slot.Actor->SetActorHiddenInGame(false);
        Slot.GeneratedKey = Info.Key.Pack();
        Slot.bActive = true;

        CommitHeroTier(StableId, *State, Info);  // la malla ya está: suelta la anterior

        ++Done;
    }

    if (Consumed > 0)
    {
        HeroQueue.RemoveAt(0, Consumed, EAllowShrinking::No); // un solo desplazamiento
    }

    // Las bajas que acaba de encolar CommitHeroTier se aplican YA. Si esperasen al
    // FlushInstanceOps del próximo re-nivelado, la instancia y el hero se dibujarían
    // superpuestos durante hasta RelevelEveryNFrames frames.
    if (Pending.Num() > 0)
    {
        FlushInstanceOps();
    }
}

/**
 * Consuma la transición a hero: con el actor ya generado y visible, suelta la
 * instancia o el impostor que venía representando al árbol y pasa su estado de
 * render al nivel Hero.
 *
 * Es la segunda mitad del swap diferido; la primera es la rama `Want == Hero` de
 * UpdateLOD, que encola sin soltar nada.
 */
void UTreeRenderSubsystem::CommitHeroTier(uint32 StableId, FTreeRenderState& State, const FPendingHero& Info)
{
    if (State.Tier != ETreeRenderTier::Hero)
    {
        LeaveTier(StableId, State);   // encola la baja de la representación anterior
    }
    State.Tier = ETreeRenderTier::Hero;
    State.PackedKey = Info.Key.Pack();
    State.bHasRepresentation = true; // la clave vuelve a describir lo que se dibuja
    State.InstanceIndex = -1;
    State.LastScale = Info.ScaleInBucket;
    State.bHeroPending = false;
}

/** Saca a un árbol del nivel hero: cancela lo que tuviera en cola y oculta su actor. */
void UTreeRenderSubsystem::ReleaseHero(uint32 StableId)
{
    HeroQueue.Remove(StableId);
    HeroInfo.Remove(StableId);

    if (FHeroSlot* Slot = HeroActors.Find(StableId))
    {
        if (Slot->Actor)
        {
            Slot->Actor->SetActorHiddenInGame(true); // el actor se conserva: reentrar es instantáneo
        }
        Slot->bActive = false;
    }
}

/**
 * Desalojo LRU de la caché de hero: conserva hasta 2 * HeroBudget ranuras y
 * destruye las inactivas más antiguas.
 *
 * @note La antigüedad es `FHeroSlot::LastUsedStamp`, el sello del último
 *       re-nivelado que usó la ranura: un reloj lógico de pasadas, no de frames.
 *       Una ranura activa no se desaloja aunque sea la más antigua.
 */
void UTreeRenderSubsystem::EvictOldHeroes()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const int32 MaxCached = FMath::Max(1, S->HeroBudget * 2);
    if (HeroActors.Num() <= MaxCached)
    {
        return;
    }

    TArray<TPair<uint32, uint32>> Inactive; // (stamp, id)
    for (const TPair<uint32, FHeroSlot>& It : HeroActors)
    {
        if (!It.Value.bActive)
        {
            Inactive.Add(TPair<uint32, uint32>(It.Value.LastUsedStamp, It.Key));
        }
    }
    Inactive.Sort([](const TPair<uint32, uint32>& A, const TPair<uint32, uint32>& B) { return A.Key < B.Key; });

    int32 ToRemove = HeroActors.Num() - MaxCached;
    for (const TPair<uint32, uint32>& It : Inactive)
    {
        if (ToRemove-- <= 0) { break; }
        if (FHeroSlot* Slot = HeroActors.Find(It.Value))
        {
            if (Slot->Actor) { Slot->Actor->Destroy(); }
        }
        HeroActors.Remove(It.Value);
    }
}

// ---------------------------------------------------------------------------
//  Depuración e instrumentación
// ---------------------------------------------------------------------------

/** Marca cada árbol con un punto del color de su nivel: hero rojo, instancia verde, impostor azul. */
void UTreeRenderSubsystem::DrawTierDebug(const FVector& ViewLocation) const
{
    UWorld* World = GetWorld();
    if (!World || !Eco) { return; }

    const FTreePopulation& Pop = Eco->GetPopulation();
    for (int32 i = 0; i < Pop.Num(); ++i)
    {
        if (Pop.State[i] == ETreeState::Dead) { continue; }

        const FTreeRenderState* State = States.Find(Pop.StableId[i]);
        if (!State) { continue; }

        FColor Color = FColor::Silver;
        switch (State->Tier)
        {
        case ETreeRenderTier::Hero:     Color = FColor::Red; break;
        case ETreeRenderTier::Instance: Color = FColor::Green; break;
        case ETreeRenderTier::Impostor: Color = FColor::Blue; break;
        default: continue; // los cullados no se dibujan
        }

        DrawDebugPoint(World, Pop.Position[i] + FVector(0, 0, Pop.Height[i]), 8.f, Color, false, -1.f, 0);
    }
}

/** Vuelca al log el reparto por niveles, el estado de la librería y la caché de hero. */
void UTreeRenderSubsystem::LogStats() const
{
    if (!bInitialized || !Library || !Eco)
    {
        UE_LOG(LogEcoRender, Warning, TEXT("[Eco/LOD] La capa de render aun no esta inicializada."));
        return;
    }

    int32 Meshes = 0, Components = 0, Instances = 0, Triangles = 0;
    Library->GetStats(Meshes, Components, Instances, Triangles);

    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Poblacion %d | hero %d, instancia %d, impostor %d, fuera de rango %d"),
        Eco->GetPopulation().Num(), NumHero, NumInstance, NumImpostor, NumCulled);
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Libreria: %d mallas horneadas (%d pendientes), %d componentes ISM, %d instancias, %d triangulos unicos"),
        Meshes, Library->GetNumPendingBakes(), Components, Instances, Triangles);
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Hero actores en cache: %d (%d en cola) | ultimo re-nivelado: %.2f ms"),
        HeroActors.Num(), HeroQueue.Num(), LastRelevelMs);
}

// ---------------------------------------------------------------------------
//  Crecimiento continuo de los hero trees
// ---------------------------------------------------------------------------

/**
 * Acerca cada frame la escala real de los actores hero a su escala objetivo, de
 * manera que el árbol se vea crecer de forma continua entre dos re-nivelados.
 *
 * UpdateLOD fija `FHeroSlot::TargetScale` a la escala que le corresponde al árbol
 * por su biomasa actual. El factor de mezcla es @f$ K = 1 - e^{-\Delta t/\tau} @f$
 * con @f$ \tau @f$ = `HeroGrowthSmoothingSeconds`, la discretización exacta de
 * @f$ dx/dt = (objetivo - x)/\tau @f$: el resultado es el mismo a 30 o a 120 fps.
 *
 * @param DeltaTime Segundos de tiempo real desde el frame anterior.
 * @note Solo se aplica a los hero, que son decenas de actores. Las instancias
 *       masivas se siguen actualizando en lote y con umbral, nunca cada frame.
 */
void UTreeRenderSubsystem::UpdateHeroInterpolation(float DeltaTime)
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S->bSmoothHeroGrowth || CVarSmoothHero.GetValueOnGameThread() == 0 || DeltaTime <= 0.f)
    {
        return;
    }

    const float Tau = FMath::Max(0.01f, S->HeroGrowthSmoothingSeconds);
    const float K = 1.f - FMath::Exp(-DeltaTime / Tau);

    for (TPair<uint32, FHeroSlot>& It : HeroActors)
    {
        FHeroSlot& Slot = It.Value;
        if (!Slot.bActive || !Slot.Actor) { continue; }

        const float Cur = static_cast<float>(Slot.Actor->GetActorScale3D().X);
        if (FMath::IsNearlyEqual(Cur, Slot.TargetScale, 1e-3f)) { continue; }

        const float NewScale = FMath::Lerp(Cur, Slot.TargetScale, K);
        Slot.Actor->SetActorScale3D(FVector(NewScale));
    }
}

// ---------------------------------------------------------------------------
//  Ciclo estacional y nieve: estado global -> Material Parameter Collection
// ---------------------------------------------------------------------------

/**
 * Avanza la fase estacional y la publica, junto con la cantidad de nieve, en el
 * Material Parameter Collection de estación.
 *
 * La estación es un escalar en [0,1) —0 primavera, 0.25 verano, 0.5 otoño, 0.75
 * invierno— que leen todos los materiales de follaje, hero e instanciados, así
 * que el bosque entero cambia a la vez y sin coste por árbol. La
 * desincronización entre árboles la aporta el material sumando
 * `PerInstanceCustomData[0]`, escrito una sola vez en el alta de la instancia.
 *
 * @param DeltaTime Segundos de tiempo real desde el frame anterior.
 */
void UTreeRenderSubsystem::UpdateSeason(float DeltaTime)
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UWorld* World = GetWorld();
    if (!World) { return; }

    const float Override = CVarSeason.GetValueOnGameThread();
    if (Override >= 0.f)
    {
        // Estación fijada a mano desde consola, para demos y capturas.
        SeasonPhase = FMath::Frac(Override);
    }
    else if (S->bSeasonFollowsSimClock && Eco && !Eco->IsPaused())
    {
        // MODO BOSQUE VIVO: la estación es el año SIMULADO, no el reloj de pared.
        // TickCount da los años enteros y el alfa de interpolación del tick el
        // resto, así que la fase avanza de forma continua a 60 fps aunque el tick
        // sea discreto. Sin esto, el follaje y la ecología contarían años distintos.
        const double YearsPerTick = Eco->GetYearsPerTick();
        const double Years = Eco->GetTickCount() * YearsPerTick
            + Eco->GetInterpolationAlpha() * YearsPerTick;
        SeasonPhase = static_cast<float>(FMath::Frac(Years));
    }
    else if (S->bAutoAdvanceSeason && DeltaTime > 0.f)
    {
        // MODO BAKE ESTÁTICO, o simulación pausada: no hay años simulados que
        // seguir, así que la estación corre con su propio reloj.
        const float Period = FMath::Max(0.1f, S->VisualYearSeconds);
        SeasonPhase = FMath::Frac(SeasonPhase + DeltaTime / Period);
    }

    // Sin MPC asignado el ciclo estacional simplemente no se aplica.
    if (!SeasonMPCCached) { return; }

    if (UMaterialParameterCollectionInstance* Inst = World->GetParameterCollectionInstance(SeasonMPCCached))
    {
        Inst->SetScalarParameterValue(TEXT("Season"), SeasonPhase);

        // Nieve derivada de la estación: pico en el invierno (estación 0.75) y cero
        // el resto del año. El cuadrado hace que llegue y se vaya deprisa en vez de
        // dejar medio invierno permanente. El material la mezcla según lo vertical
        // que sea la normal, así que basta con este escalar global.
        const float Winter = FMath::Cos(2.f * PI * (SeasonPhase - 0.75f));
        const float Snow = FMath::Clamp(S->MaxSnowAmount, 0.f, 1.f)
            * FMath::Square(FMath::Max(0.f, Winter));
        Inst->SetScalarParameterValue(TEXT("Snow"), Snow);
    }
}

// ---------------------------------------------------------------------------
//  Viento: estado global -> Material Parameter Collection
// ---------------------------------------------------------------------------

/**
 * Avanza el reloj de viento y publica su estado global en el Material Parameter
 * Collection de viento: dirección, fuerza, ráfaga, tiempo y distancia de corte
 * del World Position Offset.
 *
 * Todo el movimiento lo hace el material en el vertex shader; desde C++ se
 * escriben cinco parámetros por frame y no se recorre ni un árbol. El material
 * reparte ese estado en dos escalas: balanceo de baja frecuencia de cada rama
 * sobre el pivote que trae en UV1/UV2, con la amplitud de UV3.x, el desfase de
 * UV3.y y la jerarquía dada por el nivel de rama en UV2.y; y aleteo de alta
 * frecuencia del follaje, con el mismo UV3 a mucha más frecuencia y poca
 * amplitud. La dependencia de la posición de mundo —la que hace que la racha
 * viaje por el bosque en vez de mover todo al unísono— también la añade él.
 *
 * @param DeltaTime Segundos de tiempo real desde el frame anterior.
 * @note El reloj es propio (`WindTime`) y no el de la simulación: el bosque debe
 *       seguir moviéndose con la simulación pausada y no acelerarse cuando ésta
 *       corre rápido.
 * @see @ref bib_vientovegetacion
 */
void UTreeRenderSubsystem::UpdateWind(float DeltaTime)
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UWorld* World = GetWorld();
    if (!World) { return; }

    WindTime += FMath::Max(0.f, DeltaTime);

    const bool bWindOn = S->bEnableWind && (CVarWind.GetValueOnGameThread() != 0);

    const float BaseStrength = CVarOverrideOr(CVarWindStrength, S->WindStrength);
    const float BaseDirDeg = CVarOverrideOr(CVarWindDir, S->WindDirectionDeg);

    // --- Ráfagas: ruido temporal barato y REPRODUCIBLE ---
    // Dos senos de periodos inconmensurables (P y 0.37*P): la suma no se repite a
    // la vista, pero es puramente analítica -sin RNG, sin estado y sin textura de
    // ruido- y por tanto idéntica en cada ejecución, que es lo que permite comparar
    // dos capturas del mismo instante.
    const float P = FMath::Max(0.1f, S->WindGustPeriodSeconds);
    const float Raw = 0.5f * (FMath::Sin(2.f * PI * WindTime / P)
        + FMath::Sin(2.f * PI * WindTime / (P * 0.37f)));   // [-1, 1]
    const float Gust01 = 0.5f + 0.5f * Raw;                  // [0, 1]

    const float Amp = FMath::Clamp(S->WindGustAmplitude, 0.f, 1.f);
    const float Strength = bWindOn
        ? FMath::Max(0.f, BaseStrength * (1.f + Amp * (2.f * Gust01 - 1.f)))
        : 0.f;

    // --- Deriva lenta de la dirección ---
    // Una dirección perfectamente fija se lee como artificial a los pocos
    // segundos; un vaivén lento y de poca amplitud la rompe.
    const float WanderDeg = FMath::Clamp(S->WindDirectionWanderDeg, 0.f, 90.f)
        * FMath::Sin(2.f * PI * WindTime / (P * 2.9f));
    const float DirDeg = BaseDirDeg + WanderDeg;
    const float DirRad = FMath::DegreesToRadians(DirDeg);

    WindStrengthNow = Strength;
    WindGustNow = Gust01;
    WindDirDegNow = DirDeg;

    if (!WindMPCCached) { return; }

    if (UMaterialParameterCollectionInstance* Inst = World->GetParameterCollectionInstance(WindMPCCached))
    {
        Inst->SetVectorParameterValue(TEXT("WindDirection"),
            FLinearColor(FMath::Cos(DirRad), FMath::Sin(DirRad), 0.f, 0.f));
        Inst->SetScalarParameterValue(TEXT("WindStrength"), Strength);
        Inst->SetScalarParameterValue(TEXT("WindGust"), Gust01);
        Inst->SetScalarParameterValue(TEXT("WindTime"), WindTime);
        Inst->SetScalarParameterValue(TEXT("WindWpoCutoff"), S->WindWpoCutoffCm);
    }
}
