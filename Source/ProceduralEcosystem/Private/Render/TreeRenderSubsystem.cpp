#include "Render/TreeRenderSubsystem.h"
#include "Render/TreeLibrary.h"
#include "Render/TreeInstanceHost.h"

#include "Config/EcosystemSettings.h"
#include "Core/EcoStats.h"                 // Fase 6: instrumentacion
#include "Simulation/EcosystemSubsystem.h"
#include "Species/SpeciesData.h"
#include "Geometry/HeroTreeActor.h"
#include "Terrain/LightFieldCoarse.h"      // Fase 6: AO de copa por instancia

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

// Fase 5 (bosque vivo): toggle en vivo del suavizado de crecimiento de los hero.
static TAutoConsoleVariable<int32> CVarSmoothHero(TEXT("Eco.LOD.SmoothHero"), 1,
    TEXT("1 = interpola la escala de los hero trees entre ticks (bosque vivo, Fase 5)."));

// Fase 5 (estacional): fuerza la estacion para demos/capturas; -1 = avance automatico.
static TAutoConsoleVariable<float> CVarSeason(TEXT("Eco.Season"), -1.f,
    TEXT("Fuerza la estacion [0,1) (0=primavera, .25=verano, .5=otono, .75=invierno). -1 = auto."));

// --- Fase 6 (6.1): viento ---
static TAutoConsoleVariable<int32> CVarWind(TEXT("Eco.Wind.Enable"), 1,
    TEXT("1 = viento activo. 0 = fuerza 0 (el material deja de desplazar vertices: util para medir el coste del WPO)."));

static TAutoConsoleVariable<float> CVarWindStrength(TEXT("Eco.Wind.Strength"), -1.f,
    TEXT("Fuerza del viento [0..4]. -1 = usar Project Settings."));

static TAutoConsoleVariable<float> CVarWindDir(TEXT("Eco.Wind.Dir"), -1.f,
    TEXT("Direccion del viento en grados [0..360). -1 = usar Project Settings."));

// --- Fase 6 (6.2): AO de copa por instancia ---
static TAutoConsoleVariable<int32> CVarCanopyAO(TEXT("Eco.CanopyAO"), 1,
    TEXT("1 = escribe la apertura de copa en PerInstanceCustomData[2] (AO por instancia). 0 = ablacion."));

static UTreeRenderSubsystem* GetRender(UWorld* World)
{
    return World ? World->GetSubsystem<UTreeRenderSubsystem>() : nullptr;
}

// Fase 5 (estacional): "sequedad" [0,1] por arbol para PerInstanceCustomData[1].
// 0 = follaje sano y verde; 1 = seco/senescente. Los hero trees no tienen custom
// data, asi que leen 0 (sano) por defecto: por eso 0 = sano y no al reves.
static float DrynessOf(ETreeState State, float Stress)
{
    switch (State)
    {
    case ETreeState::Senescent: return FMath::Clamp(0.6f + 0.4f * Stress, 0.f, 1.f);
    case ETreeState::Sapling:
    case ETreeState::Mature:    return FMath::Clamp(0.35f * Stress, 0.f, 1.f);
    default:                    return 1.f; // Dead: no deberia dibujarse
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

// --- Fase 6 ---
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

/** Rellena la parte de viento de la config de la libreria desde los settings. */
static void FillWindConfig(const UEcosystemSettings& S, FTreeLibraryConfig& Cfg)
{
    const bool bWind = S.bEnableWind;
    Cfg.bWindOnInstances = bWind;
    Cfg.bWindOnImpostors = bWind && S.bWindOnImpostors;
    Cfg.WindWpoCutoffCm = S.WindWpoCutoffCm;
    Cfg.WindBoundsScale = S.WindBoundsScale; // misma fuente que los hero trees
}

/**
 * Inicializacion PEREZOSA: el orden de OnWorldBeginPlay entre subsistemas del
 * mismo mundo no esta garantizado, asi que en vez de asumir que el ecosistema ya
 * genero el relieve, se espera a que declare IsWorldReady().
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

    Host = World->SpawnActor<ATreeInstanceHost>(ATreeInstanceHost::StaticClass(), FTransform::Identity);
    if (!Host)
    {
        return false;
    }
#if WITH_EDITOR
    Host->SetActorLabel(TEXT("TreeInstanceHost (Fase 4)"));
#endif

    FTreeLibraryConfig Cfg;
    Cfg.NumAgeBuckets = S->NumAgeBuckets;
    Cfg.NumInstanceCustomDataFloats = S->NumInstanceCustomDataFloats;
    Cfg.bInstancesCastShadow = S->bInstancesCastShadow;
    Cfg.bImpostorsCastShadow = S->bImpostorsCastShadow;
    // El propio ISM culla por distancia; le damos un poco de margen sobre el
    // radio del gestor para que el cambio de nivel no compita con el cull.
    Cfg.InstanceEndCullDistanceCm = S->ImpostorRadiusCm * 1.2f;
    Cfg.ImpostorEndCullDistanceCm = S->CullRadiusCm * 1.2f;
    FillWindConfig(*S, Cfg); // Fase 6

    Library = NewObject<UTreeLibrary>(this);
    Library->Initialize(Host, Eco->GetSpeciesList(), Cfg);

    bEnabled = S->bEnableTreeRendering;

    if (S->bPrebakeLibraryOnStart && bEnabled)
    {
        Library->BakeAll();
    }

    // Un Eco.Load sustituye la poblacion entera: States, HeroActors y los mapeos
    // instancia->StableId quedan referidos a arboles de otra corrida.
    Eco->OnStateLoaded.AddUObject(this, &UTreeRenderSubsystem::HandleStateLoaded);

    // MPC de estacion resuelto UNA vez (correccion B6): antes UpdateSeason hacia
    // LoadSynchronous en cada frame. Fase 6: lo mismo para el MPC de viento.
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
            TEXT("[Eco/F6] Sin WindMPC en Project Settings: el viento no se aplicara. "
                "Puedes asignar el MISMO asset que SeasonMPC (los nombres de parametro no chocan)."));
    }

    bInitialized = true;

    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Capa de render lista (%d especies, %d buckets, %d floats por instancia)."),
        Eco->GetSpeciesList().Num(), S->NumAgeBuckets, S->NumInstanceCustomDataFloats);
    return true;
}


void UTreeRenderSubsystem::ReleaseEverything()
{
    if (Eco)
    {
        Eco->OnStateLoaded.RemoveAll(this); // el delegate sobrevive a este subsistema
    }

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
        RebuildAll(); // suelta instancias y heroes; la simulacion sigue intacta
    }
    else
    {
        bForceRelevel = true;
    }
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Capa de render %s."), bEnabled ? TEXT("ACTIVADA") : TEXT("DESACTIVADA"));
}

void UTreeRenderSubsystem::RebuildAll()
{
    // Suelta el estado de render (no la libreria: las mallas horneadas se
    // conservan, que es lo caro de reconstruir).
    Pending.Reset();
    States.Reset();
    if (Library)
    {
        Library->ClearAllInstances();
    }

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

    bForceRelevel = true;
}

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

    UE_LOG(LogEcoRender, Log, TEXT("[Eco/F6] Viento reaplicado: instancias=%s impostors=%s corte=%.0f cm."),
        Cfg.bWindOnInstances ? TEXT("si") : TEXT("no"),
        Cfg.bWindOnImpostors ? TEXT("si") : TEXT("no"),
        Cfg.WindWpoCutoffCm);
}

void UTreeRenderSubsystem::LogWindState() const
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/F6] Viento: dir %.0f deg | fuerza %.3f (rafaga %.2f) | reloj %.1f s | MPC %s"),
        WindDirDegNow, WindStrengthNow, WindGustNow, WindTime,
        WindMPCCached ? TEXT("asignado") : TEXT("NO ASIGNADO"));
    UE_LOG(LogEcoRender, Log, TEXT("[Eco/F6] WPO: corte %.0f cm | impostors %s | AO por instancia %s"),
        S->WindWpoCutoffCm,
        S->bWindOnImpostors ? TEXT("con viento") : TEXT("estaticos"),
        (S->bCanopyAOInstanceData && CVarCanopyAO.GetValueOnGameThread() != 0) ? TEXT("ON") : TEXT("OFF"));
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------
void UTreeRenderSubsystem::Tick(float DeltaTime)
{
    if (!EnsureInitialized())
    {
        return;
    }

    // FASE 6: los relojes GLOBALES de material (estacion y viento) se empujan
    // SIEMPRE, aunque la capa instanciada este apagada. Motivos:
    //   - Los hero trees sueltos (Eco.GrowHeroTree) existen al margen del gestor
    //     de LOD y tambien tienen que moverse y cambiar de estacion.
    //   - En la ablacion de la Fase 7 (Eco.LOD.Enable 0) se comparan capturas: el
    //     bosque de referencia debe estar en la misma estacion.
    // Coste: dos escrituras de parametro por frame. Literalmente nada.
    UpdateSeason(DeltaTime);
    UpdateWind(DeltaTime);

    if (!bEnabled)
    {
        return;
    }

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // Horneado amortizado: unos pocos arquetipos por frame, nunca todos de golpe.
    Library->ProcessBakeQueue(S->MaxBakesPerFrame);

    // Generacion de hero trees amortizada (doc. 4.4: "milisegundos en el game
    // thread son hitches").
    ProcessHeroQueue(S->MaxHeroPerFrame);

    // Cadencia: el re-nivelado completo cada N frames. Los arboles se mueven
    // despacio respecto a la camara, asi que no hace falta cada frame.
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

    // Fase 5 (bosque vivo): cada frame acerca la escala de los hero trees a su
    // objetivo para que crezcan de forma continua (barato: son decenas).
    UpdateHeroInterpolation(DeltaTime);

    // Fase 6 (6.4): contadores para `stat EcoRender` y para el CSV de la Fase 7.
    SET_DWORD_STAT(STAT_EcoNumHero, NumHero);
    SET_DWORD_STAT(STAT_EcoNumInstance, NumInstance);
    SET_DWORD_STAT(STAT_EcoNumImpostor, NumImpostor);
    SET_DWORD_STAT(STAT_EcoNumCulled, NumCulled);
    CSV_CUSTOM_STAT(Eco, TreesHero, NumHero, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, TreesInstance, NumInstance, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, TreesImpostor, NumImpostor, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, RelevelMs, (float)LastRelevelMs, ECsvCustomStatOp::Set);
}

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

    // Sin PlayerController (p.ej. viewport de simulacion): usa el punto de vista
    // que el render dibujo el ultimo frame.
    if (World->ViewLocationsRenderedLastFrame.Num() > 0)
    {
        OutLocation = World->ViewLocationsRenderedLastFrame[0];
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Re-nivelado (doc. 4.3, UpdateLOD)
// ---------------------------------------------------------------------------
void UTreeRenderSubsystem::UpdateLOD(const FVector& ViewLocation)
{
    SCOPE_CYCLE_COUNTER(STAT_EcoRelevel);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_UpdateLOD);

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const FTreePopulation& Pop = Eco->GetPopulation();
    const int32 PopNum = Pop.Num();

    const float HeroR = CVarHeroRadius.GetValueOnGameThread() >= 0.f ? CVarHeroRadius.GetValueOnGameThread() : S->HeroRadiusCm;
    const float ImpR = CVarImpostorRadius.GetValueOnGameThread() >= 0.f ? CVarImpostorRadius.GetValueOnGameThread() : S->ImpostorRadiusCm;
    const float CullR = CVarCullRadius.GetValueOnGameThread() >= 0.f ? CVarCullRadius.GetValueOnGameThread() : S->CullRadiusCm;

    const double HeroR2 = static_cast<double>(HeroR) * HeroR;
    const double ImpR2 = static_cast<double>(ImpR) * ImpR;
    const double CullR2 = static_cast<double>(CullR) * CullR;

    const int32 NumBuckets = FMath::Max(1, S->NumAgeBuckets);
    ++VisitStamp;

    // Fase 6 (6.2): AO de copa por instancia. Se muestrea el grid de luz GRUESO a
    // media altura de la copa de cada arbol INSTANCIADO (no de los impostors: a
    // esa distancia no se aprecia y son la mayoria). Es un muestreo trilineal por
    // arbol dibujado y por re-nivelado -o sea, cada RelevelEveryNFrames frames-,
    // no por frame ni por vertice.
    const FLightFieldCoarse& Light = Eco->GetLightCoarse();
    const bool bCanopyAO = S->bCanopyAOInstanceData
        && CVarCanopyAO.GetValueOnGameThread() != 0
        && Light.IsValid();

    // --- 1) Seleccion de hero: los HeroBudget mas cercanos dentro de R_hero ---
    // SELECCION PARCIAL (C4), no un sort completo: en bosque denso dentro de
    // HeroRadiusCm hay cientos de arboles y solo interesan HeroBudget (24). Se
    // mantiene un array acotado y ordenado; un candidato mas lejano que el peor
    // del array se descarta en O(1), que es el caso comun.
    // De paso se cachea D2 para que la pasada 2 no lo recalcule.
    const int32 HeroBudget = FMath::Max(0, S->HeroBudget);
    HeroBest.Reset();
    DistSqCache.SetNumUninitialized(PopNum, EAllowShrinking::No);

    for (int32 i = 0; i < PopNum; ++i)
    {
        // En double de principio a fin: las posiciones de mundo de UE5 son
        // double y a estas distancias (^2 ~ 1e10) el float pierde precision.
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

    // --- 2) Nivel deseado y arquetipo de cada arbol ---
    NumHero = NumInstance = NumImpostor = NumCulled = 0;

    for (int32 i = 0; i < PopNum; ++i)
    {
        if (Pop.State[i] == ETreeState::Dead) { continue; }

        const uint32 StableId = Pop.StableId[i];
        const double D2 = DistSqCache[i];
        const bool bWantsHero = HeroSet.Contains(StableId);

        // SALIDA TEMPRANA DE LOS CULLADOS (optimizacion C3).
        // Antes se hacia States.FindOrAdd() -es decir, un insert en un TMap- y se
        // calculaba el arquetipo completo (3 hashes + una FTransform) para CADA
        // arbol de la poblacion, incluidos los miles que estan mas alla de
        // CullRadiusCm y no se dibujan. Peor: quedaban estampados, asi que States
        // crecia hasta el tamano de la poblacion entera y nunca se limpiaba.
        // Ahora el caso comun a 20k es un test de distancia y nada mas, y States
        // queda proporcional a lo que de verdad se dibuja.
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

        // --- Datos por instancia (Fase 5 + Fase 6) ---
        // Se calculan aqui porque los necesitan TANTO la rama estable (actualizar)
        // COMO EnterTier (sembrar la custom data de una instancia nueva).
        const float Dryness = DrynessOf(Pop.State[i], Pop.Stress[i]);

        float CanopyAO = 1.f;
        if (bCanopyAO && Want == ETreeRenderTier::Instance)
        {
            // A media altura de copa: ni el suelo (siempre sombrio) ni el apice
            // (siempre al sol). Es la banda donde de verdad se nota si el arbol
            // esta bajo el dosel de un vecino o es el que domina.
            const FVector Probe = Pop.Position[i] + FVector(0.f, 0.f, Pop.Height[i] * 0.6f);
            CanopyAO = FMath::Clamp(Light.SampleLightSmooth(Probe) / FLightFieldCoarse::FullSunlight, 0.f, 1.f);
        }
        const FVector2f CustomData(Dryness, CanopyAO);

        // Estaba encolado como hero y ha dejado de serlo mientras esperaba: cancela
        // la generacion. Si no, ProcessHeroQueue le montaria un actor a un arbol que
        // ya no es hero (y ademas se colaria delante de los que si lo son).
        if (State.bHeroPending && Want != ETreeRenderTier::Hero)
        {
            HeroQueue.Remove(StableId);
            HeroInfo.Remove(StableId);
            State.bHeroPending = false;
        }

        // Arquetipo: bucket con histeresis + variante estable.
        const float Ratio = TreeArchetype::HeightRatio(Pop.Biomass[i], Sp->MaxBiomass);
        const int32 Bucket = TreeArchetype::BucketWithHysteresis(Ratio, State.Bucket, NumBuckets, S->BucketHysteresis);
        const uint8 Variant = TreeArchetype::VariantOf(StableId, Sp->NumLodVariants);
        const FArchetypeKey Key(Pop.SpeciesId[i], static_cast<uint8>(Bucket), Variant);

        const float ScaleInBucket = TreeArchetype::ScaleWithinBucket(Ratio, Bucket, NumBuckets);
        const FTransform Xform = TreeArchetype::InstanceTransform(
            Pop.Position[i], StableId, ScaleInBucket, S->InstanceScaleJitter);

        // Fase 5 (bosque vivo): manten al dia la escala objetivo del hero para que
        // la interpolacion por frame (UpdateHeroInterpolation) lo siga suavemente
        // aunque esta pasada de re-nivelado no lo reorganice.
        if (Want == ETreeRenderTier::Hero)
        {
            if (FHeroSlot* HSlot = HeroActors.Find(StableId))
            {
                HSlot->TargetScale = static_cast<float>(Xform.GetScale3D().X);
            }
        }

        const bool bTierChanged = (State.Tier != Want);
        // B8: si no hay representacion valida (p.ej. el arquetipo aun no estaba
        // horneado), PackedKey no significa nada y hay que reintentar. Antes esto
        // se codificaba poniendo PackedKey = 0, que choca con la clave LEGITIMA
        // Species0/Bucket0/Variant0 -la plantula mas comun de la simulacion-.
        const bool bKeyChanged = !State.bHasRepresentation || (State.PackedKey != Key.Pack());

        if (!bTierChanged && !bKeyChanged)
        {
            // Caso COMUN con diferencia: nada que reorganizar, solo (quiza) la
            // escala del que ha crecido. Con umbral, porque la mayoria no cambia
            // de forma perceptible entre dos re-nivelados.
            if (FMath::Abs(ScaleInBucket - State.LastScale) > S->ScaleUpdateThreshold)
            {
                if (State.Tier == ETreeRenderTier::Hero)
                {
                    // Con suavizado (bosque vivo) TargetScale ya se actualizo arriba y
                    // la interpolacion por frame lo alcanza; sin suavizado, snap directo.
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

            // Datos por instancia (Fase 5: sequedad; Fase 6: apertura de copa).
            // Solo aqui, en la rama estable (sin cambio de nivel/bucket), donde el
            // indice de instancia no se va a mover en este flush. El caso
            // "instancia recien creada" lo cubre EnterTier/FlushInstanceOps.
            //
            // Se reescribe SOLO si alguno de los dos cruza de banda (16 niveles):
            // sin ese umbral estariamos tocando la custom data de todas las
            // instancias en cada re-nivelado, que es el trap del doc. 4.4.
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
            HeroInfo.Add(StableId, Info);      // sobrescribe: refresca pos/escala
            HeroQueue.AddUnique(StableId);
            State.bHeroPending = true;
            State.Bucket = Bucket;
            continue;                          // la representacion actual sigue en pantalla
        }

        LeaveTier(StableId, State);
        EnterTier(StableId, State, Want, Key, Xform, ScaleInBucket, Dryness, CanopyAO);
        State.Bucket = Bucket;
    }

    // --- 3) Arboles que ya no estan (muertos y compactados por la Fase 2) ---
    for (TMap<uint32, FTreeRenderState>::TIterator It(States); It; ++It)
    {
        if (It.Value().Stamp != VisitStamp)
        {
            LeaveTier(It.Key(), It.Value());
            It.RemoveCurrent();
        }
    }

    // --- 4) Aplicar TODOS los cambios agrupados por componente ---
    FlushInstanceOps();
    EvictOldHeroes();
}

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
    State.bHasRepresentation = false; // B8: la clave guardada ya no describe nada
    // La proxima instancia sera OTRA instancia (posiblemente en otro componente)
    // y nacera con custom data a 0. Sin este reset, el comparador de banda
    // (Q != LastQ) da falso y los datos por instancia no se vuelven a escribir jamas.
    State.LastVitalityQ = 255;
    State.LastCanopyQ = 255;
    State.bHeroPending = false;
}

void UTreeRenderSubsystem::EnterTier(uint32 StableId, FTreeRenderState& State, ETreeRenderTier Want,
    const FArchetypeKey& Key, const FTransform& Xform, float ScaleInBucket,
    float Dryness, float CanopyAO)
{
    State.LastScale = ScaleInBucket;

    if (Want == ETreeRenderTier::None)
    {
        State.Tier = ETreeRenderTier::None;
        State.PackedKey = Key.Pack();
        State.bHasRepresentation = true; // "no dibujar" es una decision valida y estable
        return;
    }

    // El nivel hero NO entra por aqui: UpdateLOD lo encola y ProcessHeroQueue lo
    // consuma cuando la malla esta lista (swap diferido, para que el arbol no
    // desaparezca mientras se genera). Ver la rama Want==Hero de UpdateLOD.
    checkf(Want != ETreeRenderTier::Hero,
        TEXT("EnterTier no debe recibir Hero: la transicion a hero es diferida."));

    // Instancia o impostor: hace falta que el arquetipo este horneado.
    const bool bImpostor = (Want == ETreeRenderTier::Impostor);
    FTreeArchetypeEntry* Entry = Library->FindOrRequestBake(Key);
    if (!Entry)
    {
        // Aun no horneado: se queda sin representar y se reintentara en el proximo
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
    Ops.AddCustom.Add(FVector2f(Dryness, CanopyAO)); // nace ya con su estado sanitario y su AO

    State.Tier = Want;
    State.PackedKey = Key.Pack();
    State.bHasRepresentation = true;
    State.InstanceIndex = -1; // lo asigna el flush
    // Sincroniza los centinelas con lo que el flush va a escribir, para que el
    // siguiente re-nivelado no reescriba lo mismo.
    State.LastVitalityQ = Quantize16(Dryness);
    State.LastCanopyQ = Quantize16(CanopyAO);
}

/**
 * Aplica los cambios acumulados: UNA llamada de alta, UNA de baja y UNA
 * invalidacion de render state por componente (doc. 4.4).
 *
 * El ORDEN importa:
 *   1. Bajas (RemoveInstances por lote) y REMAPEO del bookkeeping. UE borra con
 *      RemoveAt, o sea desplaza hacia abajo todo lo que estaba por encima: si no
 *      se re-mapea, los indices guardados apuntan al arbol equivocado.
 *   2. Altas (AddInstances por lote), que devuelven los indices nuevos.
 *   3. Actualizaciones de transform, ya con los indices correctos.
 *   4. Datos por instancia (sequedad + apertura de copa).
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
                //   [0] = fase estacional estable por arbol -> el material desincroniza
                //         el cambio de estacion para que no cambien todos al unisono.
                //   [1] = sequedad (0 sano, 1 seco/senescente).
                //   [2] = apertura de copa para el AO (Fase 6): 1 a pleno sol,
                //         0 bajo dosel cerrado.
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
        // El doc. 5.2 nombra la API explicitamente ("aplicado en lote con
        // BatchUpdateInstancesTransforms") y el 4.4 lo repite. Antes esto era un
        // UpdateInstanceTransform por instancia. Como los indices no son contiguos
        // por casualidad, se resuelven, se ORDENAN y se agrupan las tiradas
        // consecutivas: cada tirada es una sola llamada por lote (correccion B5).
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

        // --- 4) DATOS POR INSTANCIA: sequedad (Fase 5) + apertura de copa (Fase 6) ---
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

        // --- 5) UNA sola invalidacion por componente ---
        Comp->MarkRenderStateDirty();
    }

    Pending.Reset();
}

// ---------------------------------------------------------------------------
//  Hero trees (doc. 4.4: amortizar y cachear)
// ---------------------------------------------------------------------------
void UTreeRenderSubsystem::ProcessHeroQueue(int32 MaxThisFrame)
{
    UWorld* World = GetWorld();
    if (!World || !Library)
    {
        return;
    }
    if (HeroQueue.Num() == 0 && Pending.Num() == 0)
    {
        return; // nada que hacer: ni siquiera abrimos el ambito de profiling
    }

    SCOPE_CYCLE_COUNTER(STAT_EcoHeroGen);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_ProcessHeroQueue);

    // Se consume por la cabeza con un contador y se compacta con UN solo
    // RemoveAt al final: el RemoveAt(0) por elemento desplazaba todo el resto
    // de la cola en cada extraccion. (Nada dentro del bucle re-encola ni borra
    // de HeroQueue: las altas las hace UpdateLOD y ReleaseHero solo se alcanza
    // desde LeaveTier de un arbol cuyo nivel actual no es Hero.)
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

        // Puede haber dejado de ser hero mientras esperaba en la cola. Ojo: con el
        // swap diferido, un hero pendiente TODAVIA tiene Tier == Instance/Impostor.
        FTreeRenderState* State = States.Find(StableId);
        if (!State || (!State->bHeroPending && State->Tier != ETreeRenderTier::Hero))
        {
            continue;
        }

        FHeroSlot& Slot = HeroActors.FindOrAdd(StableId);
        Slot.LastUsedStamp = VisitStamp;

        // Cacheado y con el MISMO arquetipo: reentrar es instantaneo.
        if (Slot.Actor && Slot.GeneratedKey == Info.Key.Pack())
        {
            Slot.Actor->SetActorLocation(Info.Position);  // el actor cacheado puede venir de otra corrida (Eco.Load)
            Slot.Actor->SetActorHiddenInGame(false);
            Slot.Actor->SetActorScale3D(FVector(Info.Scale));
            Slot.TargetScale = Info.Scale; // Fase 5 (bosque vivo): arranca a su tamano
            Slot.bActive = true;
            CommitHeroTier(StableId, *State, Info);  // AHORA se suelta la instancia anterior
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

        // AQUI esta el pago de la arquitectura en dos escalas (doc. 3.5): al
        // hero SI se le pasa el grid de luz grueso, asi que sus atractores en
        // sombra de un vecino grande se podan y la copa crece ladeada. La
        // libreria instanciada no puede hacer esto (es generica); por eso los
        // arboles cercanos se ven "conscientes de su sitio" y los lejanos no
        // hace falta que lo esten.
        //
        // Fase 6: de esa misma rejilla fina sale ademas el AO de copa por
        // vertice del hero (ver AHeroTreeActor::BuildNow), asi que un hero
        // apretado contra un vecino no solo crece ladeado: tambien se ve mas
        // oscuro por el lado que le da sombra.
        Slot.Actor->Generate(ArchetypeSp, EcoRand::Hash32(StableId), &Eco->GetLightCoarse(), Info.Position);
        Slot.Actor->SetActorRotation(FRotator::ZeroRotator); // ver nota de A3 en el SpawnActor
        Slot.Actor->SetActorScale3D(FVector(Info.Scale));
        Slot.TargetScale = Info.Scale; // Fase 5 (bosque vivo): arranca a su tamano
        Slot.Actor->SetActorHiddenInGame(false);
        Slot.GeneratedKey = Info.Key.Pack();
        Slot.bActive = true;

        CommitHeroTier(StableId, *State, Info);  // la malla ya esta: suelta la anterior

        ++Done;
    }

    if (Consumed > 0)
    {
        HeroQueue.RemoveAt(0, Consumed, EAllowShrinking::No); // un solo desplazamiento
    }

    // Las bajas que acaba de encolar CommitHeroTier se aplican YA. Si esperasen al
    // FlushInstanceOps del proximo re-nivelado, la instancia y el hero se dibujarian
    // SUPERPUESTOS durante hasta RelevelEveryNFrames frames.
    if (Pending.Num() > 0)
    {
        FlushInstanceOps();
    }
}

/**
 * Consuma la transicion a hero: ahora que el actor tiene geometria y esta visible,
 * se suelta la instancia/impostor que lo venia representando y se pasa el estado
 * de render a Hero. Es la segunda mitad del swap diferido (la primera es la rama
 * Want==Hero de UpdateLOD).
 */
void UTreeRenderSubsystem::CommitHeroTier(uint32 StableId, FTreeRenderState& State, const FPendingHero& Info)
{
    if (State.Tier != ETreeRenderTier::Hero)
    {
        LeaveTier(StableId, State);   // encola RemoveInstance de la representacion anterior
    }
    State.Tier = ETreeRenderTier::Hero;
    State.PackedKey = Info.Key.Pack();
    State.bHasRepresentation = true; // B8
    State.InstanceIndex = -1;
    State.LastScale = Info.ScaleInBucket;
    State.bHeroPending = false;
}

void UTreeRenderSubsystem::ReleaseHero(uint32 StableId)
{
    HeroQueue.Remove(StableId);
    HeroInfo.Remove(StableId);

    if (FHeroSlot* Slot = HeroActors.Find(StableId))
    {
        if (Slot->Actor)
        {
            Slot->Actor->SetActorHiddenInGame(true); // se conserva: reentrar debe ser instantaneo
        }
        Slot->bActive = false;
    }
}

/** Cache LRU: se guardan hasta 2x HeroBudget arboles hero ocultos. */
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
//  Debug e instrumentacion
// ---------------------------------------------------------------------------
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
//  Bosque vivo (Fase 5): interpolacion de escala de los hero trees
// ---------------------------------------------------------------------------
//
// Los hero trees son actores (decenas como mucho), asi que escalarlos cada
// frame es barato y NO viola la regla 4.4 de "no tocar el HISM cada frame":
// las instancias masivas siguen actualizandose en lote y con umbral. Aqui solo
// suavizamos los pocos arboles cercanos, que son los que se ven crecer de cerca.
//
// El re-nivelado (UpdateLOD) fija FHeroSlot::TargetScale a la escala que le toca
// al arbol por su biomasa actual; este metodo acerca la escala real del actor a
// ese objetivo con un suavizado exponencial, estable a cualquier framerate.
void UTreeRenderSubsystem::UpdateHeroInterpolation(float DeltaTime)
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S->bSmoothHeroGrowth || CVarSmoothHero.GetValueOnGameThread() == 0 || DeltaTime <= 0.f)
    {
        return;
    }

    // Suavizado exponencial: K = 1 - e^(-dt/tau). Independiente del framerate,
    // asi que el crecimiento se ve igual de suave a 30 o 120 fps.
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
//  Ciclo estacional (Fase 5) + nieve (Fase 6): estacion -> MPC
// ---------------------------------------------------------------------------
//
// La estacion es un escalar [0,1) que avanza solo con el tiempo real (o se fija
// con la consola Eco.Season). Se empuja al MPC que leen TODOS los materiales de
// follaje (heros e instancias), asi que el bosque entero cambia de estacion a la
// vez, sin coste por arbol. La DESINCRONIZACION por arbol la da el material
// sumando PerInstanceCustomData[0] (fase estable por instancia, ya escrita en el
// alta de la instancia).
void UTreeRenderSubsystem::UpdateSeason(float DeltaTime)
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UWorld* World = GetWorld();
    if (!World) { return; }

    const float Override = CVarSeason.GetValueOnGameThread();
    if (Override >= 0.f)
    {
        // Consola: estacion fijada a mano para demos/capturas.
        SeasonPhase = FMath::Frac(Override);
    }
    else if (S->bSeasonFollowsSimClock && Eco && !Eco->IsPaused())
    {
        // MODO BOSQUE VIVO. La estacion es EL ANO SIMULADO, no el reloj de pared:
        // TickCount da los anos enteros y el alpha de interpolacion del doc. 5.2 el
        // resto, asi que la fase avanza de forma continua a 60 fps aunque el tick
        // sea discreto. Sin esto, el follaje y la ecologia cuentan anos distintos.
        const double YearsPerTick = Eco->GetYearsPerTick();
        const double Years = Eco->GetTickCount() * YearsPerTick
            + Eco->GetInterpolationAlpha() * YearsPerTick;
        SeasonPhase = static_cast<float>(FMath::Frac(Years));
    }
    else if (S->bAutoAdvanceSeason && DeltaTime > 0.f)
    {
        // MODO BAKE ESTATICO (o sim pausada): no hay anos simulados que seguir, asi
        // que la estacion corre con su propio reloj para poder animar el beauty shot.
        const float Period = FMath::Max(0.1f, S->VisualYearSeconds);
        SeasonPhase = FMath::Frac(SeasonPhase + DeltaTime / Period);
    }

    // MPC resuelto una vez en EnsureInitialized (B6). Sin MPC asignado, el ciclo
    // estacional simplemente no se aplica: no rompe nada.
    if (!SeasonMPCCached) { return; }

    if (UMaterialParameterCollectionInstance* Inst = World->GetParameterCollectionInstance(SeasonMPCCached))
    {
        Inst->SetScalarParameterValue(TEXT("Season"), SeasonPhase);

        // Fase 6 (6.2): nieve derivada de la estacion. Pico en invierno (0.75) y
        // cero el resto del ano; la curva al cuadrado hace que llegue y se vaya
        // deprisa en vez de haber medio invierno permanente. El material la mezcla
        // segun la normal hacia arriba, asi que basta con este escalar global.
        const float Winter = FMath::Cos(2.f * PI * (SeasonPhase - 0.75f));
        const float Snow = FMath::Clamp(S->MaxSnowAmount, 0.f, 1.f)
            * FMath::Square(FMath::Max(0.f, Winter));
        Inst->SetScalarParameterValue(TEXT("Snow"), Snow);
    }
}

// ---------------------------------------------------------------------------
//  VIENTO (Fase 6, doc. 6.1): estado global -> Material Parameter Collection
// ---------------------------------------------------------------------------
//
// Todo el movimiento lo hace el material en el vertex shader. Desde C++ solo se
// publica, una vez por frame, el ESTADO del viento; no hay ni un bucle sobre
// arboles. Las dos escalas de movimiento del documento se reparten asi:
//
//   - Balanceo de baja frecuencia del arbol: el material rota cada rama sobre el
//     pivote que trae en UV1/UV2, con la amplitud de UV3.x y el desfase de UV3.y.
//     La jerarquia (subramas heredando el movimiento del padre) sale de que el
//     nivel de rama viaja en UV2.y: los niveles altos suman el desplazamiento de
//     los bajos.
//   - Aleteo de alta frecuencia de las hojas: la seccion de follaje usa el mismo
//     UV3 pero con una frecuencia mucho mayor y amplitud pequena.
//
// Y las tres dependencias que pide el doc:
//   - TIEMPO: reloj propio (WindTime), no el de simulacion.
//   - POSICION DE MUNDO: la anade el material (dot(WorldPosition, WindDirection)
//     dentro del seno) para que la racha "viaje" por el bosque y no se muevan
//     todos al unisono.
//   - DIRECCION/FUERZA GLOBAL + RAFAGAS: lo que se escribe aqui.
void UTreeRenderSubsystem::UpdateWind(float DeltaTime)
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UWorld* World = GetWorld();
    if (!World) { return; }

    WindTime += FMath::Max(0.f, DeltaTime);

    const bool bWindOn = S->bEnableWind && (CVarWind.GetValueOnGameThread() != 0);

    const float StrengthOverride = CVarWindStrength.GetValueOnGameThread();
    const float BaseStrength = (StrengthOverride >= 0.f) ? StrengthOverride : S->WindStrength;

    const float DirOverride = CVarWindDir.GetValueOnGameThread();
    const float BaseDirDeg = (DirOverride >= 0.f) ? DirOverride : S->WindDirectionDeg;

    // --- Rafagas: ruido temporal barato y REPRODUCIBLE ---
    // Dos senos de periodos inconmensurables (P y P*0.37): la suma no se repite
    // en ningun ciclo audible/visible pero es puramente analitica -sin RNG, sin
    // estado, sin textura de ruido- y por tanto igual en cada corrida, que es lo
    // que pide el proyecto para poder comparar capturas.
    const float P = FMath::Max(0.1f, S->WindGustPeriodSeconds);
    const float Raw = 0.5f * (FMath::Sin(2.f * PI * WindTime / P)
        + FMath::Sin(2.f * PI * WindTime / (P * 0.37f)));   // [-1, 1]
    const float Gust01 = 0.5f + 0.5f * Raw;                  // [0, 1]

    const float Amp = FMath::Clamp(S->WindGustAmplitude, 0.f, 1.f);
    const float Strength = bWindOn
        ? FMath::Max(0.f, BaseStrength * (1.f + Amp * (2.f * Gust01 - 1.f)))
        : 0.f;

    // --- Deriva lenta de la direccion ---
    // Un viento de direccion perfectamente fija canta a artificial en cuanto lo
    // miras diez segundos. Un vaiven lento y pequeno lo arregla.
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
