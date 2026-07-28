#include "Render/TreeSoilSubsystem.h"

#include "Config/EcosystemSettings.h"
#include "Simulation/EcosystemSubsystem.h"
#include "Render/TreeInstanceHost.h"
#include "Render/TreeArchetype.h"   // TreeArchetype::YawOf (orientacion estable del tocon)
#include "Core/EcoCore.h"           // EcoRand (scatter de hojarasca)

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogEcoSoil, Log, All);

// ---------------------------------------------------------------------------
//  Consola
// ---------------------------------------------------------------------------
static UTreeSoilSubsystem* GetSoil(UWorld* World)
{
    return World ? World->GetSubsystem<UTreeSoilSubsystem>() : nullptr;
}

static FAutoConsoleCommandWithWorld GSoilClear(TEXT("Eco.Soil.Clear"),
    TEXT("Borra todos los tocones y hojarasca de la capa de suelo (Fase 5)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UTreeSoilSubsystem* S = GetSoil(W)) S->Clear(); }));

static FAutoConsoleCommandWithWorld GSoilStats(TEXT("Eco.Soil.Stats"),
    TEXT("Loguea cuantos tocones y cards de hojarasca hay en la capa de suelo."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UTreeSoilSubsystem* S = GetSoil(W)) S->LogStats(); }));

// ---------------------------------------------------------------------------
//  Ciclo de vida
// ---------------------------------------------------------------------------
bool UTreeSoilSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UTreeSoilSubsystem::Deinitialize()
{
    if (Host)
    {
        Host->Destroy();
        Host = nullptr;
    }
    Super::Deinitialize();
}

TStatId UTreeSoilSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTreeSoilSubsystem, STATGROUP_Tickables);
}

/**
 * Inicializacion PEREZOSA (igual patron que UTreeRenderSubsystem): el orden de
 * arranque entre subsistemas no esta garantizado, asi que esperamos a que el
 * ecosistema declare IsWorldReady() antes de crear nada.
 */
bool UTreeSoilSubsystem::EnsureInitialized()
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
    Host->SetActorLabel(TEXT("TreeSoilHost (Fase 5)"));
#endif
    // Garantiza un root al que colgar los ISM (por si el host no lo trae).
    if (!Host->GetRootComponent())
    {
        USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("SoilRoot"));
        Root->RegisterComponent();
        Host->SetRootComponent(Root);
    }

    UStaticMesh* SnagMesh = S->SnagMesh.LoadSynchronous();
    UStaticMesh* LitterMesh = S->LitterMesh.LoadSynchronous();
    UMaterialInterface* SnagMat = S->SnagMaterial.LoadSynchronous();
    UMaterialInterface* LitterMat = S->LitterMaterial.LoadSynchronous();

    if (SnagMesh)
    {
        WoodISM = CreateISM(SnagMesh, SnagMat, S->bSnagsCastShadow, TEXT("ISM_Snag"));
    }
    else
    {
        UE_LOG(LogEcoSoil, Warning, TEXT("[Eco/Suelo] Sin SnagMesh en Project Settings: no habra tocones/troncos."));
    }

    if (LitterMesh)
    {
        LitterISM = CreateISM(LitterMesh, LitterMat, /*bCastShadow*/ false, TEXT("ISM_Litter"));
    }
    else
    {
        UE_LOG(LogEcoSoil, Warning, TEXT("[Eco/Suelo] Sin LitterMesh en Project Settings: no habra hojarasca."));
    }

    // No generamos tocones retroactivos para las muertes anteriores a existir la
    // capa (evita un estallido de tocones al arrancar). Solo cuentan las nuevas.
    DeathCursor = Eco->GetDeathEventCounter();

    bInitialized = true;
    UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Capa de suelo lista."));
    return true;
}

UHierarchicalInstancedStaticMeshComponent* UTreeSoilSubsystem::CreateISM(UStaticMesh* Mesh,
    UMaterialInterface* Mat, bool bCastShadow, const TCHAR* Name)
{
    if (!Mesh || !Host)
    {
        return nullptr;
    }

    UHierarchicalInstancedStaticMeshComponent* Comp =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(Host, Name);
    if (!Comp)
    {
        return nullptr;
    }

    Comp->SetMobility(EComponentMobility::Movable);
    Comp->SetStaticMesh(Mesh);
    if (Mat)
    {
        Comp->SetMaterial(0, Mat);
    }
    Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Comp->SetCanEverAffectNavigation(false);
    Comp->SetCastShadow(bCastShadow);
    Comp->SetupAttachment(Host->GetRootComponent());
    Comp->RegisterComponent();
    Host->AddInstanceComponent(Comp);
    return Comp;
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::Tick(float DeltaTime)
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S->bEnableSoilLayer)
    {
        return;
    }
    if (!EnsureInitialized())
    {
        return;
    }

    // 1) Consumir las muertes NUEVAS de la simulacion (cursor monotono).
    TArray<FTreeDeathEvent> NewDeaths;
    Eco->CollectNewDeathEvents(DeathCursor, NewDeaths);
    for (const FTreeDeathEvent& Death : NewDeaths)
    {
        SpawnSnag(Death);
        // Scatter estable por arbol: la misma muerte esparce siempre la misma hojarasca.
        uint32 Rng = EcoRand::Hash32(Death.StableId ^ 0xA53CB123u);
        SpawnLitterAround(Death.Position, S->LitterRadiusCm, S->LitterPerDeath, Rng);
    }

    // 2) Animar la caida de los tocones que aun estan de pie.
    UpdateFallingSnags(DeltaTime);
}

// ---------------------------------------------------------------------------
//  Tocones / troncos
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::SpawnSnag(const FTreeDeathEvent& Death)
{
    if (!WoodISM)
    {
        return;
    }
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const int32 Cap = FMath::Max(0, S->MaxSnags);
    if (Cap == 0)
    {
        return;
    }

    FSoilSnag Snag;
    Snag.Base = Death.Position;
    Snag.HeightCm = FMath::Max(20.f, Death.HeightCm * S->SnagHeightFraction);
    Snag.RadiusCm = FMath::Max(4.f, Snag.HeightCm * 0.06f); // tronco corto y relativamente grueso
    Snag.Yaw = TreeArchetype::YawOf(Death.StableId);        // orientacion estable por arbol
    Snag.FallT = 0.f;
    Snag.bFalling = true;

    const FTransform Xform = SnagTransform(Snag);

    if (Snags.Num() < Cap)
    {
        Snag.InstanceIndex = WoodISM->AddInstance(Xform, /*bWorldSpace*/ false);
        Snags.Add(Snag);
    }
    else
    {
        // Anillo lleno: reutiliza la instancia del tocon mas viejo (no se borra
        // ninguna instancia -> sin baile de indices de RemoveInstances).
        FSoilSnag& Old = Snags[SnagCursor];
        Snag.InstanceIndex = Old.InstanceIndex;
        Old = Snag;
        WoodISM->UpdateInstanceTransform(Snag.InstanceIndex, Xform, false, /*bMarkDirty*/ true, true);
        SnagCursor = (SnagCursor + 1) % Cap;
    }
}

/**
 * Transforma un tocon segun su estado de caida FallT (0=en pie, 1=tumbado).
 *
 * Malla de referencia: cilindro basico del motor (100 cm de alto, radio 50,
 * pivote en el CENTRO). Si usas otra malla con pivote distinto, ajusta las dos
 * constantes de abajo y, si hace falta, el offset vertical.
 */
FTransform UTreeSoilSubsystem::SnagTransform(const FSoilSnag& Snag) const
{
    static constexpr float EngineCylHeight = 100.f;
    static constexpr float EngineCylRadius = 50.f;

    const float HalfH = Snag.HeightCm * 0.5f;
    const FVector Scale(Snag.RadiusCm / EngineCylRadius,
        Snag.RadiusCm / EngineCylRadius,
        Snag.HeightCm / EngineCylHeight);

    // En pie: eje +Z vertical, base en el suelo. Tumbado: eje +Z horizontal en la
    // direccion Yaw, tronco apoyado sobre el suelo (levantado su radio).
    const FQuat QUp(FRotator(0.f, Snag.Yaw, 0.f));
    const FQuat QDown(FRotator(90.f, Snag.Yaw, 0.f));
    const FQuat Rot = FQuat::Slerp(QUp, QDown, Snag.FallT).GetNormalized();

    const FVector UpLoc = Snag.Base + FVector(0.f, 0.f, HalfH);
    const FVector DownLoc = Snag.Base + FVector(0.f, 0.f, Snag.RadiusCm);
    const FVector Loc = FMath::Lerp(UpLoc, DownLoc, Snag.FallT);

    return FTransform(Rot, Loc, Scale);
}

void UTreeSoilSubsystem::UpdateFallingSnags(float DeltaTime)
{
    if (!WoodISM || DeltaTime <= 0.f)
    {
        return;
    }
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const float Step = DeltaTime / FMath::Max(0.1f, S->SnagFallSeconds);

    bool bAnyMoved = false;
    for (FSoilSnag& Snag : Snags)
    {
        if (!Snag.bFalling || Snag.InstanceIndex < 0)
        {
            continue;
        }
        Snag.FallT = FMath::Min(1.f, Snag.FallT + Step);
        if (Snag.FallT >= 1.f)
        {
            Snag.bFalling = false; // ya es un tronco tumbado (madera muerta), se queda
        }
        WoodISM->UpdateInstanceTransform(Snag.InstanceIndex, SnagTransform(Snag),
            /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
        bAnyMoved = true;
    }
    if (bAnyMoved)
    {
        WoodISM->MarkRenderStateDirty(); // una sola invalidacion por frame
    }
}

// ---------------------------------------------------------------------------
//  Hojarasca
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::SpawnLitterAround(const FVector& Base, float SpreadCm, int32 Count, uint32& RngState)
{
    if (!LitterISM || Count <= 0)
    {
        return;
    }
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const int32 Cap = FMath::Max(0, S->MaxLitter);
    if (Cap == 0)
    {
        return;
    }

    static constexpr float EnginePlaneSize = 100.f; // /Engine/BasicShapes/Plane = 100x100 cm
    static constexpr float CardCm = 70.f;
    const float Sc = CardCm / EnginePlaneSize;

    for (int32 k = 0; k < Count; ++k)
    {
        // Disco uniforme por area (r = R*sqrt(U), igual criterio que la dispersion de semillas).
        const float Ang = EcoRand::NextRange(RngState, 0.f, 2.f * PI);
        const float Rad = SpreadCm * FMath::Sqrt(EcoRand::NextUnit(RngState));
        FVector P = Base + FVector(FMath::Cos(Ang) * Rad, FMath::Sin(Ang) * Rad, 0.f);
        if (Eco)
        {
            P.Z = Eco->GetHeightField().SampleHeight(P.X, P.Y);
        }
        P.Z += 3.f; // 3 cm sobre el suelo para evitar z-fighting

        const float YawDeg = EcoRand::NextRange(RngState, 0.f, 360.f);
        const FTransform Xform(FRotator(0.f, YawDeg, 0.f), P, FVector(Sc, Sc, Sc));

        if (LitterCount < Cap)
        {
            LitterISM->AddInstance(Xform, /*bWorldSpace*/ false);
            ++LitterCount;
        }
        else
        {
            LitterISM->UpdateInstanceTransform(LitterCursor, Xform, false, /*bMarkDirty*/ true, true);
            LitterCursor = (LitterCursor + 1) % Cap;
        }
    }
}

// ---------------------------------------------------------------------------
//  Control
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::Clear()
{
    if (WoodISM) { WoodISM->ClearInstances(); }
    if (LitterISM) { LitterISM->ClearInstances(); }
    Snags.Reset();
    SnagCursor = 0;
    LitterCount = 0;
    LitterCursor = 0;
    if (Eco) { DeathCursor = Eco->GetDeathEventCounter(); }
    UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Capa de suelo vaciada."));
}

void UTreeSoilSubsystem::LogStats() const
{
    UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Tocones: %d | hojarasca: %d | cursor de muertes: %lld"),
        Snags.Num(), LitterCount, DeathCursor);
}
