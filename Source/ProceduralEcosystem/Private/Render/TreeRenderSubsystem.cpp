#include "Render/TreeRenderSubsystem.h"
#include "Render/TreeLibrary.h"
#include "Render/TreeInstanceHost.h"

#include "Config/EcosystemSettings.h"
#include "Simulation/EcosystemSubsystem.h"
#include "Species/SpeciesData.h"
#include "Geometry/HeroTreeActor.h"

#include "Ecology/TreePopulation.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
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

static UTreeRenderSubsystem* GetRender(UWorld* World)
{
    return World ? World->GetSubsystem<UTreeRenderSubsystem>() : nullptr;
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

    Library = NewObject<UTreeLibrary>(this);
    Library->Initialize(Host, Eco->GetSpeciesList(), Cfg);

    bEnabled = S->bEnableTreeRendering;

    if (S->bPrebakeLibraryOnStart && bEnabled)
    {
        Library->BakeAll();
    }

    bInitialized = true;

    UE_LOG(LogEcoRender, Log, TEXT("[Eco/LOD] Capa de render lista (%d especies, %d buckets)."),
        Eco->GetSpeciesList().Num(), S->NumAgeBuckets);
    return true;
}

void UTreeRenderSubsystem::ReleaseEverything()
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
    HeroSet.Reset();
    States.Reset();
    Pending.Reset();

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

void UTreeRenderSubsystem::BakeLibraryNow()
{
    if (EnsureInitialized() && Library)
    {
        Library->BakeAll();
        bForceRelevel = true;
    }
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------
void UTreeRenderSubsystem::Tick(float DeltaTime)
{
    if (!EnsureInitialized() || !bEnabled)
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

    // --- 1) Seleccion de hero: los HeroBudget mas cercanos dentro de R_hero ---
    HeroCandidates.Reset(PopNum);
    for (int32 i = 0; i < PopNum; ++i)
    {
        if (Pop.State[i] == ETreeState::Dead) { continue; }
        const double D2 = FVector::DistSquared(Pop.Position[i], ViewLocation);
        if (D2 < HeroR2)
        {
            HeroCandidates.Add(TPair<float, int32>(static_cast<float>(D2), i));
        }
    }
    HeroCandidates.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });

    HeroSet.Reset();
    const int32 HeroCount = FMath::Min(HeroCandidates.Num(), FMath::Max(0, S->HeroBudget));
    for (int32 k = 0; k < HeroCount; ++k)
    {
        HeroSet.Add(Pop.StableId[HeroCandidates[k].Value]);
    }

    // --- 2) Nivel deseado y arquetipo de cada arbol ---
    NumHero = NumInstance = NumImpostor = NumCulled = 0;

    for (int32 i = 0; i < PopNum; ++i)
    {
        if (Pop.State[i] == ETreeState::Dead) { continue; }

        const USpeciesData* Sp = Eco->GetSpeciesById(Pop.SpeciesId[i]);
        if (!Sp) { continue; }

        const uint32 StableId = Pop.StableId[i];
        const double D2 = FVector::DistSquared(Pop.Position[i], ViewLocation);

        FTreeRenderState& State = States.FindOrAdd(StableId);
        State.Stamp = VisitStamp;

        const ETreeRenderTier Want =
            HeroSet.Contains(StableId) ? ETreeRenderTier::Hero :
            (D2 < ImpR2) ? ETreeRenderTier::Instance :
            (D2 < CullR2) ? ETreeRenderTier::Impostor :
            ETreeRenderTier::None;

        switch (Want)
        {
        case ETreeRenderTier::Hero:     ++NumHero; break;
        case ETreeRenderTier::Instance: ++NumInstance; break;
        case ETreeRenderTier::Impostor: ++NumImpostor; break;
        default:                        ++NumCulled; break;
        }

        // Arquetipo: bucket con histeresis + variante estable.
        const float Ratio = TreeArchetype::HeightRatio(Pop.Biomass[i], Sp->MaxBiomass);
        const int32 Bucket = TreeArchetype::BucketWithHysteresis(Ratio, State.Bucket, NumBuckets, S->BucketHysteresis);
        const uint8 Variant = TreeArchetype::VariantOf(StableId, Sp->NumLodVariants);
        const FArchetypeKey Key(Pop.SpeciesId[i], static_cast<uint8>(Bucket), Variant);

        const float ScaleInBucket = TreeArchetype::ScaleWithinBucket(Ratio, Bucket, NumBuckets);
        const FTransform Xform = TreeArchetype::InstanceTransform(
            Pop.Position[i], StableId, ScaleInBucket, S->InstanceScaleJitter);

        const bool bTierChanged = (State.Tier != Want);
        const bool bKeyChanged = (State.PackedKey != Key.Pack());

        if (!bTierChanged && !bKeyChanged)
        {
            // Caso COMUN con diferencia: nada que reorganizar, solo (quiza) la
            // escala del que ha crecido. Con umbral, porque la mayoria no cambia
            // de forma perceptible entre dos re-nivelados.
            if (FMath::Abs(ScaleInBucket - State.LastScale) > S->ScaleUpdateThreshold)
            {
                if (State.Tier == ETreeRenderTier::Hero)
                {
                    if (FHeroSlot* Slot = HeroActors.Find(StableId))
                    {
                        if (Slot->Actor)
                        {
                            Slot->Actor->SetActorScale3D(Xform.GetScale3D());
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
            State.Bucket = Bucket;
            continue;
        }

        LeaveTier(StableId, State);
        EnterTier(StableId, State, Want, Key, Xform, ScaleInBucket);
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
}

void UTreeRenderSubsystem::EnterTier(uint32 StableId, FTreeRenderState& State, ETreeRenderTier Want,
    const FArchetypeKey& Key, const FTransform& Xform, float ScaleInBucket)
{
    State.LastScale = ScaleInBucket;

    if (Want == ETreeRenderTier::None)
    {
        State.Tier = ETreeRenderTier::None;
        State.PackedKey = Key.Pack();
        return;
    }

    if (Want == ETreeRenderTier::Hero)
    {
        State.Tier = ETreeRenderTier::Hero;
        State.PackedKey = Key.Pack();

        FPendingHero Info;
        Info.Key = Key;
        Info.Position = Xform.GetLocation();
        Info.Scale = static_cast<float>(Xform.GetScale3D().X);
        HeroInfo.Add(StableId, Info);
        HeroQueue.AddUnique(StableId);
        return;
    }

    // Instancia o impostor: hace falta que el arquetipo este horneado.
    const bool bImpostor = (Want == ETreeRenderTier::Impostor);
    FTreeArchetypeEntry* Entry = Library->FindOrRequestBake(Key);
    if (!Entry)
    {
        // Aun no horneado: se queda sin representar y se reintentara en el
        // proximo re-nivelado (PackedKey = 0 fuerza que se vea como cambio).
        State.Tier = ETreeRenderTier::None;
        State.PackedKey = 0;
        return;
    }

    if (!Library->GetOrCreateComponent(Key, bImpostor))
    {
        State.Tier = ETreeRenderTier::None;
        State.PackedKey = 0;
        return;
    }

    FPendingComponentOps& Ops = Pending.FindOrAdd(MakeComponentKey(Key.Pack(), bImpostor));
    Ops.AddIds.Add(StableId);
    Ops.AddXforms.Add(Xform);

    State.Tier = Want;
    State.PackedKey = Key.Pack();
    State.InstanceIndex = -1; // lo asigna el flush
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
 */
void UTreeRenderSubsystem::FlushInstanceOps()
{
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

                // Dato por instancia: fase [0,1) estable por arbol. No se usa aun;
                // es el gancho para desincronizar el ciclo estacional en la Fase 5
                // sin draw calls extra (PerInstanceCustomData).
                if (Comp->NumCustomDataFloats > 0)
                {
                    Comp->SetCustomDataValue(Index, 0,
                        TreeArchetype::StableUnit(Id, TreeArchetype::SaltPhase), /*bMarkRenderStateDirty*/ false);
                }
            }
        }

        // --- 3) ACTUALIZACIONES DE TRANSFORM ---
        for (const TPair<uint32, FTransform>& Update : Ops.Updates)
        {
            if (const FTreeRenderState* Found = States.Find(Update.Key))
            {
                if (Found->InstanceIndex >= 0)
                {
                    Comp->UpdateInstanceTransform(Found->InstanceIndex, Update.Value,
                        /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
                }
            }
        }

        // --- 4) UNA sola invalidacion por componente ---
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

    int32 Done = 0;
    while (HeroQueue.Num() > 0 && Done < FMath::Max(1, MaxThisFrame))
    {
        const uint32 StableId = HeroQueue[0];
        HeroQueue.RemoveAt(0, 1, EAllowShrinking::No);

        FPendingHero Info;
        if (!HeroInfo.RemoveAndCopyValue(StableId, Info))
        {
            continue;
        }

        // Puede haber dejado de ser hero mientras esperaba en la cola.
        const FTreeRenderState* State = States.Find(StableId);
        if (!State || State->Tier != ETreeRenderTier::Hero)
        {
            continue;
        }

        FHeroSlot& Slot = HeroActors.FindOrAdd(StableId);
        Slot.LastUsedStamp = VisitStamp;

        // Cacheado y con el MISMO arquetipo: reentrar es instantaneo.
        if (Slot.Actor && Slot.GeneratedKey == Info.Key.Pack())
        {
            Slot.Actor->SetActorHiddenInGame(false);
            Slot.Actor->SetActorScale3D(FVector(Info.Scale));
            Slot.bActive = true;
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
            Slot.Actor = World->SpawnActor<AHeroTreeActor>(Info.Position,
                FRotator(0.f, TreeArchetype::YawOf(StableId), 0.f));
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
        Slot.Actor->Generate(ArchetypeSp, EcoRand::Hash32(StableId), &Eco->GetLightCoarse(), Info.Position);
        Slot.Actor->SetActorRotation(FRotator(0.f, TreeArchetype::YawOf(StableId), 0.f));
        Slot.Actor->SetActorScale3D(FVector(Info.Scale));
        Slot.Actor->SetActorHiddenInGame(false);
        Slot.GeneratedKey = Info.Key.Pack();
        Slot.bActive = true;

        ++Done;
    }
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
