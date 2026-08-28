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
    if (Eco)
    {
        Eco->OnStateLoaded.RemoveAll(this);
        Eco = nullptr;
    }
    if (Host)
    {
        Host->Destroy();
        Host = nullptr;
    }
    WoodISM = nullptr;
    LitterISM = nullptr;
    Snags.Reset();
    PendingSnagAdds.Reset();
    PendingSnagSlots.Reset();
    PendingLitterAdds.Reset();
    NewDeaths.Reset();
    SnagCursor = 0;
    LitterCount = 0;
    LitterCursor = 0;
    bInitialized = false;
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

    // Spawn del host + garantia de root: identico al de la capa de arboles, asi
    // que vive una sola vez en ATreeInstanceHost::SpawnHost.
    Host = ATreeInstanceHost::SpawnHost(World, TEXT("TreeSoilHost (Fase 5)"));
    if (!Host)
    {
        return false;
    }

    UStaticMesh* SnagMesh = S->SnagMesh.LoadSynchronous();
    UStaticMesh* LitterMesh = S->LitterMesh.LoadSynchronous();
    UMaterialInterface* SnagMat = S->SnagMaterial.LoadSynchronous();
    UMaterialInterface* LitterMat = S->LitterMaterial.LoadSynchronous();

    if (SnagMesh)
    {
        // Correccion B3: las dimensiones salen de los BOUNDS REALES de la malla,
        // no de constantes que asumian /Engine/BasicShapes/Cylinder. Asi, asignar
        // una malla propia sigue dando troncos del tamano correcto.
        const FVector Extent = SnagMesh->GetBoundingBox().GetExtent();
        SnagMeshHeightCm = FMath::Max(1.f, static_cast<float>(Extent.Z * 2.0));
        SnagMeshRadiusCm = FMath::Max(1.f, static_cast<float>(FMath::Max(Extent.X, Extent.Y)));

        WoodISM = CreateISM(SnagMesh, SnagMat, S->bSnagsCastShadow, TEXT("ISM_Snag"));
        UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Malla de tocon '%s': alto %.0f cm, radio %.0f cm."),
            *SnagMesh->GetName(), SnagMeshHeightCm, SnagMeshRadiusCm);
    }
    else
    {
        UE_LOG(LogEcoSoil, Warning, TEXT("[Eco/Suelo] Sin SnagMesh en Project Settings: no habra tocones/troncos."));
    }

    if (LitterMesh)
    {
        const FVector Extent = LitterMesh->GetBoundingBox().GetExtent();
        LitterMeshSizeCm = FMath::Max(1.f, static_cast<float>(FMath::Max(Extent.X, Extent.Y) * 2.0));

        LitterISM = CreateISM(LitterMesh, LitterMat, /*bCastShadow*/ false, TEXT("ISM_Litter"));
    }
    else
    {
        UE_LOG(LogEcoSoil, Warning, TEXT("[Eco/Suelo] Sin LitterMesh en Project Settings: no habra hojarasca."));
    }

    // No generamos tocones retroactivos para las muertes anteriores a existir la
    // capa (evita un estallido de tocones al arrancar). Solo cuentan las nuevas.
    DeathCursor = Eco->GetDeathEventCounter();

    // Un Eco.Load sustituye el bosque: los tocones y la hojarasca de la corrida
    // anterior no corresponden a ningun arbol del bake.
    Eco->OnStateLoaded.AddUObject(this, &UTreeSoilSubsystem::Clear);

    bInitialized = true;
    UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Capa de suelo lista."));
    return true;
}

UHierarchicalInstancedStaticMeshComponent* UTreeSoilSubsystem::CreateISM(UStaticMesh* Mesh,
    UMaterialInterface* Mat, bool bCastShadow, const TCHAR* Name)
{
    // Configuracion comun (movilidad, colision, navmesh, sombra, registro): la
    // MISMA fabrica que usa la libreria de arboles. Lo unico propio del suelo es
    // el material, que aqui viene por parametro en vez de venir en la malla, y
    // que la fabrica aplica antes de registrar el componente.
    return ATreeInstanceHost::CreateInstancedComponent(Host, Mesh, FName(Name), bCastShadow,
        /*NumCustomDataFloats*/ 0, Mat);
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::Tick(float DeltaTime)
{
    const UEcosystemSettings& S = *UEcosystemSettings::Get();
    if (!S.bEnableSoilLayer)
    {
        return;
    }
    // Coherencia con la ablacion de la Fase 7 (correccion B11): tocones y
    // hojarasca son parte de la capa de render, asi que por defecto se apagan con
    // ella (Eco.LOD.Enable 0 / bEnableTreeRendering). Antes seguian dibujandose y
    // la comparacion "con y sin capa de render" no era limpia.
    if (S.bSoilFollowsTreeRendering && !S.bEnableTreeRendering)
    {
        return;
    }
    if (!EnsureInitialized())
    {
        return;
    }

    // 1) Consumir las muertes NUEVAS de la simulacion (cursor monotono) y ENCOLAR
    //    su representacion. Nada toca el ISM todavia: ver FlushSpawns (B4).
    NewDeaths.Reset();
    Eco->CollectNewDeathEvents(DeathCursor, NewDeaths);
    for (const FTreeDeathEvent& Death : NewDeaths)
    {
        QueueSnag(Death, S);
        // Scatter estable por arbol: la misma muerte esparce siempre la misma hojarasca.
        uint32 Rng = EcoRand::Hash32(Death.StableId ^ 0xA53CB123u);
        QueueLitterAround(Death.Position, S, Rng);
    }

    // 2) Aplicar en LOTE todas las altas del tick.
    FlushSpawns();

    // 3) Avanzar la linea temporal en pie -> caida -> tronco -> retirada.
    UpdateSnags(DeltaTime);
}

// ---------------------------------------------------------------------------
//  Tocones / troncos
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::QueueSnag(const FTreeDeathEvent& Death, const UEcosystemSettings& S)
{
    if (!WoodISM)
    {
        return;
    }
    const int32 Cap = FMath::Max(0, S.MaxSnags);
    if (Cap == 0)
    {
        return;
    }

    FSoilSnag Snag;
    Snag.Base = Death.Position;
    Snag.HeightCm = FMath::Max(20.f, Death.HeightCm * S.SnagHeightFraction);
    Snag.RadiusCm = FMath::Max(4.f, Snag.HeightCm * 0.06f); // tronco corto y relativamente grueso
    Snag.Yaw = TreeArchetype::YawOf(Death.StableId);        // orientacion estable por arbol
    Snag.FallT = 0.f;
    Snag.PhaseSeconds = 0.f;
    Snag.Phase = ESnagPhase::Standing;   // arranca EN PIE (doc. 5.4), no cayendo

    // 1) Ranura ya retirada (Gone): es la reutilizacion natural, y ademas mantiene
    //    coherente el criterio "se va el mas viejo por EDAD, no por presion".
    for (FSoilSnag& Candidate : Snags)
    {
        if (Candidate.Phase == ESnagPhase::Gone && Candidate.InstanceIndex >= 0)
        {
            Snag.InstanceIndex = Candidate.InstanceIndex;
            Candidate = Snag;
            WoodISM->UpdateInstanceTransform(Snag.InstanceIndex, SnagTransform(Snag),
                /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            bWoodDirty = true; // una sola invalidacion, en FlushSpawns
            return;
        }
    }

    if (Snags.Num() < Cap)
    {
        // Alta NUEVA: se encola. El indice de instancia se rellena en FlushSpawns,
        // que es quien llama a AddInstances (plural) una sola vez por tick.
        const int32 Slot = Snags.Add(Snag);
        PendingSnagSlots.Add(Slot);
        PendingSnagAdds.Add(SnagTransform(Snag));
    }
    else
    {
        // Anillo lleno y nadie retirado todavia: reutiliza la instancia del tocon mas
        // viejo (no se borra ninguna instancia -> sin baile de indices de
        // RemoveInstances). Es un backstop de memoria, NO el mecanismo de retirada.
        //
        // OJO: solo puede reciclarse una ranura YA VOLCADA (InstanceIndex >= 0).
        // Las anadidas ESTE MISMO tick siguen con InstanceIndex == -1 hasta
        // FlushSpawns: si se sobrescribiera una de ellas, el indice que devuelva
        // AddInstances se asignaria a datos ya pisados y la instancia horneada
        // quedaria con el transform de un arbol y los datos logicos de otro
        // (tocon descuadrado en mortandades masivas del tick de primer llenado).
        int32 Slot = INDEX_NONE;
        for (int32 Step = 0; Step < Cap; ++Step)
        {
            const int32 Candidate = (SnagCursor + Step) % Cap;
            if (Snags[Candidate].InstanceIndex >= 0)
            {
                Slot = Candidate;
                SnagCursor = (Candidate + 1) % Cap;
                break;
            }
        }

        if (Slot != INDEX_NONE)
        {
            FSoilSnag& Old = Snags[Slot];
            Snag.InstanceIndex = Old.InstanceIndex;
            Old = Snag;
            WoodISM->UpdateInstanceTransform(Snag.InstanceIndex, SnagTransform(Snag),
                /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            bWoodDirty = true; // una sola invalidacion, en FlushSpawns
        }
        else
        {
            // Todas las ranuras son altas de este mismo tick sin volcar: se
            // sustituye la mas vieja pendiente EN SITIO, incluida su transform
            // encolada, para que FlushSpawns cree la instancia ya con los datos
            // del tocon nuevo (InstanceIndex sigue en -1 y lo rellena el flush).
            const int32 Reuse = SnagCursor;
            Snags[Reuse] = Snag;
            const int32 PendingIdx = PendingSnagSlots.Find(Reuse);
            if (PendingIdx != INDEX_NONE && PendingSnagAdds.IsValidIndex(PendingIdx))
            {
                PendingSnagAdds[PendingIdx] = SnagTransform(Snag);
            }
            SnagCursor = (SnagCursor + 1) % Cap;
        }
    }
}

/**
 * Transforma un tocon segun su fase y su estado de caida FallT (0=en pie,
 * 1=tumbado). Las dimensiones de la malla (SnagMeshHeightCm/RadiusCm) se leen de
 * sus bounds reales al inicializar, asi que esto funciona con cualquier malla
 * cuyo pivote este en el centro (correccion B3).
 */
FTransform UTreeSoilSubsystem::SnagTransform(const FSoilSnag& Snag) const
{
    // Retirado: escala ~0 en vez de RemoveInstance, para no mover los indices del
    // resto (mismo criterio de diseno que el anillo: aqui NUNCA se borra una
    // instancia). La ranura queda libre para el proximo tocon.
    if (Snag.Phase == ESnagPhase::Gone)
    {
        return FTransform(FQuat::Identity, Snag.Base, FVector(KINDA_SMALL_NUMBER));
    }

    const float HalfH = Snag.HeightCm * 0.5f;
    const FVector Scale(Snag.RadiusCm / SnagMeshRadiusCm,
        Snag.RadiusCm / SnagMeshRadiusCm,
        Snag.HeightCm / SnagMeshHeightCm);

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

/**
 * Linea temporal de la muerte (doc. 5.4), desacoplada del sim (que ya avanzo):
 *   Standing (SnagStandingSeconds) -> Falling (SnagFallSeconds) -> Log (SnagLogSeconds) -> Gone
 * Se mide en tiempo REAL porque es una animacion de render, no ecologia: el
 * pulso de nutrientes ya lo aplico la simulacion en el tick de la muerte.
 */
void UTreeSoilSubsystem::UpdateSnags(float DeltaTime)
{
    if (!WoodISM || DeltaTime <= 0.f)
    {
        return;
    }
    // Los settings se leen UNA vez por tick, no una por tocon (correccion B7).
    const UEcosystemSettings& S = *UEcosystemSettings::Get();
    const float FallSeconds = FMath::Max(0.1f, S.SnagFallSeconds);
    const float StandingSeconds = S.SnagStandingSeconds;
    const float LogSeconds = S.SnagLogSeconds;

    bool bAnyMoved = false;
    for (FSoilSnag& Snag : Snags)
    {
        if (Snag.InstanceIndex < 0 || Snag.Phase == ESnagPhase::Gone)
        {
            continue;
        }

        Snag.PhaseSeconds += DeltaTime;
        bool bDirty = false;

        switch (Snag.Phase)
        {
        case ESnagPhase::Standing:
            if (Snag.PhaseSeconds >= StandingSeconds)
            {
                Snag.Phase = ESnagPhase::Falling;
                Snag.PhaseSeconds = 0.f;
            }
            break;

        case ESnagPhase::Falling:
            Snag.FallT = FMath::Clamp(Snag.PhaseSeconds / FallSeconds, 0.f, 1.f);
            bDirty = true;
            if (Snag.FallT >= 1.f)
            {
                Snag.Phase = ESnagPhase::Log;
                Snag.PhaseSeconds = 0.f;
            }
            break;

        case ESnagPhase::Log:
            // SnagLogSeconds == 0 -> la madera muerta se queda para siempre
            // (util para un beauty shot estatico).
            if (LogSeconds > 0.f && Snag.PhaseSeconds >= LogSeconds)
            {
                Snag.Phase = ESnagPhase::Gone;
                bDirty = true;
            }
            break;

        default:
            break;
        }

        if (bDirty)
        {
            WoodISM->UpdateInstanceTransform(Snag.InstanceIndex, SnagTransform(Snag),
                /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            bAnyMoved = true;
        }
    }
    if (bAnyMoved)
    {
        WoodISM->MarkRenderStateDirty(); // una sola invalidacion por frame
    }
}

// ---------------------------------------------------------------------------
//  Hojarasca
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::QueueLitterAround(const FVector& Base, const UEcosystemSettings& S, uint32& RngState)
{
    const int32 Count = S.LitterPerDeath;
    const int32 Cap = FMath::Max(0, S.MaxLitter);
    if (!LitterISM || Count <= 0 || Cap == 0)
    {
        return;
    }

    // Escala derivada de los bounds reales de LitterMesh (B3): LitterCardCm es el
    // tamano que quieres ver en mundo, sea cual sea la malla asignada.
    const float Sc = S.LitterCardCm / LitterMeshSizeCm;
    const float SpreadCm = S.LitterRadiusCm;

    for (int32 k = 0; k < Count; ++k)
    {
        // Disco uniforme por area: LA MISMA funcion que dispersa las semillas
        // (EcoRand::SampleDiscOffsetCm). Antes estaba reescrita aqui a mano, asi
        // que la hojarasca y las semillas podian acabar con kernels distintos.
        const FVector2D Offset = EcoRand::SampleDiscOffsetCm(RngState, SpreadCm);
        FVector P = Base + FVector(Offset.X, Offset.Y, 0.f);
        if (Eco)
        {
            P.Z = Eco->GetHeightField().SampleHeight(P.X, P.Y);
        }
        P.Z += S.LitterGroundOffsetCm; // evita z-fighting con el material del suelo

        const float YawDeg = EcoRand::NextRange(RngState, 0.f, 360.f);
        const FTransform Xform(FRotator(0.f, YawDeg, 0.f), P, FVector(Sc, Sc, Sc));

        if (LitterCount + PendingLitterAdds.Num() < Cap)
        {
            PendingLitterAdds.Add(Xform); // alta nueva -> lote (B4)
        }
        else
        {
            // Anillo lleno: reutiliza la card mas vieja, sin borrar instancias.
            LitterISM->UpdateInstanceTransform(LitterCursor, Xform,
                /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            LitterCursor = (LitterCursor + 1) % Cap;
            bLitterDirty = true;
        }
    }
}

/**
 * Aplica en LOTE las altas encoladas este tick (correccion B4).
 *
 * El Apendice B marca como riesgo ALTO "reconstruir el HISM cada frame... nunca
 * instancia a instancia". La version anterior llamaba a AddInstance (singular)
 * una vez por tocon y una por card: en un tick de auto-aclareo con 200 muertes y
 * LitterPerDeath=6 eso son 1.400 llamadas sueltas al HISM en un frame, ademas de
 * un MarkRenderStateDirty por tocon. Aqui es UNA llamada AddInstances por
 * componente y UNA invalidacion, igual que hace FlushInstanceOps en la capa de
 * arboles.
 */
void UTreeSoilSubsystem::FlushSpawns()
{
    if (WoodISM && PendingSnagAdds.Num() > 0)
    {
        const TArray<int32> NewIndices = WoodISM->AddInstances(PendingSnagAdds, /*bShouldReturnIndices*/ true);
        for (int32 k = 0; k < NewIndices.Num() && k < PendingSnagSlots.Num(); ++k)
        {
            if (Snags.IsValidIndex(PendingSnagSlots[k]))
            {
                Snags[PendingSnagSlots[k]].InstanceIndex = NewIndices[k];
            }
        }
        bWoodDirty = true;
    }
    PendingSnagAdds.Reset();
    PendingSnagSlots.Reset();

    if (LitterISM && PendingLitterAdds.Num() > 0)
    {
        LitterISM->AddInstances(PendingLitterAdds, /*bShouldReturnIndices*/ false);
        LitterCount += PendingLitterAdds.Num();
        bLitterDirty = true;
    }
    PendingLitterAdds.Reset();

    if (bWoodDirty && WoodISM) { WoodISM->MarkRenderStateDirty(); }
    if (bLitterDirty && LitterISM) { LitterISM->MarkRenderStateDirty(); }
    bWoodDirty = false;
    bLitterDirty = false;
}

// ---------------------------------------------------------------------------
//  Control
// ---------------------------------------------------------------------------
void UTreeSoilSubsystem::Clear()
{
    if (WoodISM) { WoodISM->ClearInstances(); }
    if (LitterISM) { LitterISM->ClearInstances(); }
    Snags.Reset();
    PendingSnagAdds.Reset();
    PendingSnagSlots.Reset();
    PendingLitterAdds.Reset();
    bWoodDirty = false;
    bLitterDirty = false;
    SnagCursor = 0;
    LitterCount = 0;
    LitterCursor = 0;
    if (Eco) { DeathCursor = Eco->GetDeathEventCounter(); }
    UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Capa de suelo vaciada."));
}

void UTreeSoilSubsystem::LogStats() const
{
    int32 Standing = 0, Falling = 0, Logs = 0, Gone = 0;
    for (const FSoilSnag& S : Snags)
    {
        switch (S.Phase)
        {
        case ESnagPhase::Standing: ++Standing; break;
        case ESnagPhase::Falling:  ++Falling;  break;
        case ESnagPhase::Log:      ++Logs;     break;
        default:                   ++Gone;     break;
        }
    }
    UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Tocones: %d (en pie %d, cayendo %d, tumbados %d, retirados %d) | hojarasca: %d | cursor de muertes: %lld"),
        Snags.Num(), Standing, Falling, Logs, Gone, LitterCount, DeathCursor);
}
