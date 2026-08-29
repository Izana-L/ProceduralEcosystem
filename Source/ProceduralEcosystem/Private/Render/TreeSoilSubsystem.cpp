/**
 * @file TreeSoilSubsystem.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de la capa de suelo: tocones que caen, madera muerta y hojarasca.
 *
 * Contiene los comandos de consola Eco.Soil.Clear y Eco.Soil.Stats, la inicialización perezosa
 * que crea los dos componentes de instancing y deduce de sus bounds las dimensiones reales de
 * las mallas, el consumo de eventos de muerte mediante un cursor monótono, el encolado de
 * tocones y hojarasca con su política de anillo, la aplicación en lote de las altas y el avance
 * en tiempo real de la línea temporal Standing -> Falling -> Log -> Gone, con la caída resuelta
 * por interpolación esférica entre la orientación en pie y la tumbada.
 *
 * @ingroup eco_render
 * @see @ref bib_harmon1986
 * @see @ref bib_shoemake1985
 */

#include "Render/TreeSoilSubsystem.h"

#include "Config/EcosystemSettings.h"
#include "Simulation/EcosystemSubsystem.h"
#include "Render/TreeInstanceHost.h"
#include "Render/TreeArchetype.h"   // TreeArchetype::YawOf: orientación estable por árbol
#include "Core/EcoCore.h"           // EcoRand: scatter de la hojarasca

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogEcoSoil, Log, All);

// ---------------------------------------------------------------------------
//  Comandos de consola
// ---------------------------------------------------------------------------

/** Devuelve la capa de suelo del mundo dado, o nullptr si el mundo no la tiene. */
static UTreeSoilSubsystem* GetSoil(UWorld* World)
{
    return World ? World->GetSubsystem<UTreeSoilSubsystem>() : nullptr;
}

static FAutoConsoleCommandWithWorld GSoilClear(TEXT("Eco.Soil.Clear"),
    TEXT("Borra todos los tocones y hojarasca de la capa de suelo."),
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
 * Inicialización perezosa, el mismo patrón que UTreeRenderSubsystem: el orden de arranque
 * entre subsistemas no está garantizado, así que no se crea nada hasta que el ecosistema
 * declara IsWorldReady().
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

    // El spawn del host y su colocación en la identidad son idénticos a los de la capa de
    // árboles, así que la única copia vive en ATreeInstanceHost::SpawnHost.
    Host = ATreeInstanceHost::SpawnHost(World, TEXT("TreeSoilHost"));
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
        // Las dimensiones salen de los bounds reales de la malla, no de constantes: asignar
        // una malla propia de tocón sigue dando troncos del tamaño correcto.
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

    // El cursor arranca en el contador actual: las muertes anteriores a existir la capa no
    // generan tocones retroactivos, que aparecerían todos de golpe al inicializarse.
    DeathCursor = Eco->GetDeathEventCounter();

    // Cargar un estado sustituye el bosque entero: los restos de la corrida anterior no
    // corresponden a ningún árbol del bosque recién cargado.
    Eco->OnStateLoaded.AddUObject(this, &UTreeSoilSubsystem::Clear);

    bInitialized = true;
    UE_LOG(LogEcoSoil, Log, TEXT("[Eco/Suelo] Capa de suelo lista."));
    return true;
}

UHierarchicalInstancedStaticMeshComponent* UTreeSoilSubsystem::CreateISM(UStaticMesh* Mesh,
    UMaterialInterface* Mat, bool bCastShadow, const TCHAR* Name)
{
    // Movilidad, colisión, navmesh, sombra y registro salen de la misma fábrica que usa la
    // librería de árboles. Lo propio del suelo es el material: aquí viene por parámetro en vez
    // de venir en la malla, y la fábrica lo aplica antes de registrar el componente.
    return ATreeInstanceHost::CreateInstancedComponent(Host, Mesh, FName(Name), bCastShadow,
        /*NumCustomDataFloats*/ 0, Mat);
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------

/**
 * Un paso de la capa: consumir muertes y encolarlas, aplicar el lote y avanzar la línea
 * temporal de los tocones ya existentes. Ese orden importa: las altas del tick tienen ya su
 * índice de instancia antes de que UpdateSnags recorra el anillo.
 */
void UTreeSoilSubsystem::Tick(float DeltaTime)
{
    const UEcosystemSettings& S = *UEcosystemSettings::Get();
    if (!S.bEnableSoilLayer)
    {
        return;
    }
    // Tocones y hojarasca forman parte de la presentación, así que por defecto se apagan junto
    // con la capa de render (Eco.LOD.Enable 0): si siguieran dibujándose, la comparación de
    // rendimiento con y sin capa de render no mediría lo que dice medir.
    if (S.bSoilFollowsTreeRendering && !S.bEnableTreeRendering)
    {
        return;
    }
    if (!EnsureInitialized())
    {
        return;
    }

    // 1) Consumir las muertes nuevas de la simulación con el cursor monótono y encolar su
    //    representación. Ningún componente se toca todavía: de eso se encarga FlushSpawns.
    NewDeaths.Reset();
    Eco->CollectNewDeathEvents(DeathCursor, NewDeaths);
    for (const FTreeDeathEvent& Death : NewDeaths)
    {
        QueueSnag(Death, S);
        // Semilla derivada del identificador estable: la misma muerte esparce siempre la
        // misma hojarasca, aunque cambie el orden de los eventos o se recargue la partida.
        uint32 Rng = EcoRand::Hash32(Death.StableId ^ 0xA53CB123u);
        QueueLitterAround(Death.Position, S, Rng);
    }

    // 2) Aplicar en lote todas las altas del tick.
    FlushSpawns();

    // 3) Avanzar la línea temporal en pie -> caída -> tronco -> retirada.
    UpdateSnags(DeltaTime);
}

// ---------------------------------------------------------------------------
//  Tocones y troncos
// ---------------------------------------------------------------------------

/**
 * Coloca el tocón de una muerte en el anillo, por este orden de preferencia:
 * una ranura ya retirada, un alta nueva si el anillo no está lleno, la instancia del tocón más
 * viejo que ya exista en el componente y, si todas las ranuras son altas de este mismo tick, la
 * sustitución en sitio de la más antigua de ellas.
 *
 * Ninguna de las cuatro vías borra instancias, de modo que los índices ya repartidos siguen
 * siendo válidos. Las dimensiones del tocón son heurísticas sobre la altura al morir; la
 * orientación sale del identificador estable del árbol, así que es siempre la misma.
 */
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
    Snag.Yaw = TreeArchetype::YawOf(Death.StableId);        // orientación estable por árbol
    Snag.FallT = 0.f;
    Snag.PhaseSeconds = 0.f;
    Snag.Phase = ESnagPhase::Standing;   // el árbol muerto queda en pie, no se desploma al morir

    // 1) Ranura ya retirada: es la reutilización natural y mantiene el criterio de que un
    //    tocón desaparece por edad y no por presión de memoria.
    for (FSoilSnag& Candidate : Snags)
    {
        if (Candidate.Phase == ESnagPhase::Gone && Candidate.InstanceIndex >= 0)
        {
            Snag.InstanceIndex = Candidate.InstanceIndex;
            Candidate = Snag;
            WoodISM->UpdateInstanceTransform(Snag.InstanceIndex, SnagTransform(Snag),
                /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            bWoodDirty = true; // la invalidación, una sola, la hace FlushSpawns
            return;
        }
    }

    if (Snags.Num() < Cap)
    {
        // 2) Alta nueva: se encola. El índice de instancia lo rellena FlushSpawns, que es
        //    quien llama a AddInstances una sola vez por tick.
        const int32 Slot = Snags.Add(Snag);
        PendingSnagSlots.Add(Slot);
        PendingSnagAdds.Add(SnagTransform(Snag));
    }
    else
    {
        // 3) Anillo lleno y ninguna ranura retirada: se reutiliza la instancia del tocón más
        //    viejo. Es un tope de memoria, no el mecanismo de retirada.
        //
        //    Solo puede reciclarse una ranura cuya instancia ya exista (InstanceIndex >= 0).
        //    Las añadidas en este mismo tick siguen a -1 hasta FlushSpawns: sobrescribir una
        //    de ellas haría que el índice devuelto por AddInstances se asignara a datos ya
        //    pisados, y la instancia quedaría con la transformación de un árbol y el estado
        //    lógico de otro.
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
            bWoodDirty = true; // la invalidación, una sola, la hace FlushSpawns
        }
        else
        {
            // 4) Todas las ranuras son altas de este mismo tick: se sustituye en sitio la más
            //    antigua de ellas, transformación encolada incluida, para que FlushSpawns cree
            //    la instancia ya con los datos del tocón nuevo. InstanceIndex sigue a -1 y lo
            //    rellena el propio flush.
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
 * Compone la transformación de instancia de un tocón a partir de su fase y su progreso de
 * caída FallT (0 en pie, 1 tumbado).
 *
 * La rotación es una interpolación esférica entre la orientación vertical y la horizontal, que
 * gira a velocidad angular constante; la posición se interpola en paralelo desde el centro del
 * tronco en pie hasta su eje apoyado en el suelo. No hay dinámica de sólido rígido detrás: es
 * una aproximación visual barata de la caída.
 *
 * @pre Las dimensiones de la malla (SnagMeshHeightCm, SnagMeshRadiusCm) ya están leídas de sus
 *      bounds, y el pivote de la malla está en su centro.
 * @return Transformación en el espacio local del componente, que coincide con el mundo.
 */
FTransform UTreeSoilSubsystem::SnagTransform(const FSoilSnag& Snag) const
{
    // Retirado: escala ~0 en lugar de borrar la instancia, para no desplazar los índices del
    // resto. La ranura queda libre para el próximo tocón.
    if (Snag.Phase == ESnagPhase::Gone)
    {
        return FTransform(FQuat::Identity, Snag.Base, FVector(KINDA_SMALL_NUMBER));
    }

    const float HalfH = Snag.HeightCm * 0.5f;
    const FVector Scale(Snag.RadiusCm / SnagMeshRadiusCm,
        Snag.RadiusCm / SnagMeshRadiusCm,
        Snag.HeightCm / SnagMeshHeightCm);

    // En pie: eje +Z vertical, base en el suelo. Tumbado: eje +Z horizontal en la dirección
    // Yaw, con el tronco apoyado en el suelo, o sea levantado su propio radio.
    const FQuat QUp(FRotator(0.f, Snag.Yaw, 0.f));
    const FQuat QDown(FRotator(90.f, Snag.Yaw, 0.f));
    const FQuat Rot = FQuat::Slerp(QUp, QDown, Snag.FallT).GetNormalized();

    const FVector UpLoc = Snag.Base + FVector(0.f, 0.f, HalfH);
    const FVector DownLoc = Snag.Base + FVector(0.f, 0.f, Snag.RadiusCm);
    const FVector Loc = FMath::Lerp(UpLoc, DownLoc, Snag.FallT);

    return FTransform(Rot, Loc, Scale);
}

/**
 * Avanza la línea temporal de cada tocón, desacoplada del reloj de la simulación:
 * Standing (SnagStandingSeconds) -> Falling (SnagFallSeconds) -> Log (SnagLogSeconds) -> Gone.
 *
 * Se mide en tiempo real porque es animación de render y no ecología: el pulso de nutrientes
 * de la muerte lo aplicó la simulación en su propio tick.
 *
 * @param DeltaTime Segundos reales transcurridos desde el frame anterior.
 * @note Solo se reescribe la transformación de los tocones que cambian, y la invalidación del
 *       componente es una sola por frame.
 */
void UTreeSoilSubsystem::UpdateSnags(float DeltaTime)
{
    if (!WoodISM || DeltaTime <= 0.f)
    {
        return;
    }
    // La configuración se lee una vez por tick, no una por tocón.
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
            // SnagLogSeconds a 0 deja la madera muerta en el suelo indefinidamente.
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
        WoodISM->MarkRenderStateDirty(); // una sola invalidación por frame
    }
}

// ---------------------------------------------------------------------------
//  Hojarasca
// ---------------------------------------------------------------------------

/**
 * Encola las tarjetas de hojarasca de una muerte, repartidas en un disco de radio LitterRadiusCm
 * alrededor de la base del árbol.
 *
 * El reparto usa el mismo muestreo uniforme por área que la dispersión de semillas de la
 * simulación (EcoRand::SampleDiscOffsetCm), de modo que ambos comparten kernel. Cada tarjeta se
 * apoya en el relieve y se levanta LitterGroundOffsetCm para no entrar en z-fighting con el
 * material del suelo.
 */
void UTreeSoilSubsystem::QueueLitterAround(const FVector& Base, const UEcosystemSettings& S, uint32& RngState)
{
    const int32 Count = S.LitterPerDeath;
    const int32 Cap = FMath::Max(0, S.MaxLitter);
    if (!LitterISM || Count <= 0 || Cap == 0)
    {
        return;
    }

    // La escala se deriva de los bounds reales de la malla: LitterCardCm es el tamaño que la
    // tarjeta tiene en mundo, sea cual sea la malla asignada.
    const float Sc = S.LitterCardCm / LitterMeshSizeCm;
    const float SpreadCm = S.LitterRadiusCm;

    for (int32 k = 0; k < Count; ++k)
    {
        // Disco uniforme por área, con la misma función que dispersa las semillas: una única
        // copia del kernel para los dos usos.
        const FVector2D Offset = EcoRand::SampleDiscOffsetCm(RngState, SpreadCm);
        FVector P = Base + FVector(Offset.X, Offset.Y, 0.f);
        if (Eco)
        {
            P.Z = Eco->GetHeightField().SampleHeight(P.X, P.Y);
        }
        P.Z += S.LitterGroundOffsetCm; // separa la tarjeta del terreno para evitar z-fighting

        const float YawDeg = EcoRand::NextRange(RngState, 0.f, 360.f);
        const FTransform Xform(FRotator(0.f, YawDeg, 0.f), P, FVector(Sc, Sc, Sc));

        if (LitterCount + PendingLitterAdds.Num() < Cap)
        {
            PendingLitterAdds.Add(Xform); // alta nueva: va al lote del tick
        }
        else
        {
            // Anillo lleno: se reutiliza la tarjeta más vieja, sin borrar instancias.
            LitterISM->UpdateInstanceTransform(LitterCursor, Xform,
                /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ false, /*bTeleport*/ true);
            LitterCursor = (LitterCursor + 1) % Cap;
            bLitterDirty = true;
        }
    }
}

/**
 * Aplica en lote las altas acumuladas durante el tick: una llamada a AddInstances y una
 * invalidación por componente, nunca instancia a instancia.
 *
 * Un tick de autoaclareo con doscientas muertes y seis tarjetas por muerte son mil cuatrocientas
 * instancias nuevas en un solo frame; darlas de alta una a una y marcar el estado de render
 * sucio en cada una provoca un tirón visible. Es el mismo protocolo de flush que sigue
 * UTreeRenderSubsystem con los componentes de la librería.
 *
 * @post Todas las colas quedan vacías y cada tocón dado de alta tiene ya su índice de
 *       instancia asignado.
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
//  Control desde consola
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
    // El cursor se lleva al contador actual: las muertes ya ocurridas no vuelven a
    // materializarse en el siguiente tick.
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
