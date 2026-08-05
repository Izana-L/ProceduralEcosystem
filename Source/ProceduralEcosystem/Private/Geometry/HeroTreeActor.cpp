#include "Geometry/HeroTreeActor.h"
#include "Geometry/SpaceColonization.h"
#include "Simulation/EcosystemSubsystem.h" // luz gruesa FRESCA en cada BuildNow
#include "Config/EcosystemSettings.h"      // WindBoundsScale (fuente unica)
#include "Species/SpeciesData.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

AHeroTreeActor::AHeroTreeActor()
{
    // El Tick de este actor SOLO sirve para el debug draw del esqueleto y los
    // atractores. Optimizacion C7: arranca DESACTIVADO y lo enciende bDrawDebug.
    // Con la cache LRU del gestor de LOD puede haber hasta 2*HeroBudget actores
    // vivos (48 por defecto), y antes todos tickeaban cada frame solo para hacer
    // un early-out. No son milisegundos, pero es coste puro y sale en stat game.
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TreeMesh"));
    SetRootComponent(Mesh);
}

void AHeroTreeActor::SetDrawDebug(bool bInDrawDebug)
{
    bDrawDebug = bInDrawDebug;
    SetActorTickEnabled(bDrawDebug);
}

void AHeroTreeActor::BeginPlay()
{
    Super::BeginPlay();
    SetActorTickEnabled(bDrawDebug);
}

void AHeroTreeActor::Generate(const USpeciesData* InSpecies, uint32 Seed,
    const FLightFieldCoarse* CoarseLight, const FVector& WorldTrunkBase)
{
    if (!InSpecies)
    {
        return;
    }

    SpeciesPtr = InSpecies;
    GenSeed = Seed;
    // Solo se recuerda SI hay contexto de luz; el puntero al grid se pide fresco
    // al subsistema en cada BuildNow (ver nota en el .h).
    bUseCoarseLight = (CoarseLight != nullptr);
    TrunkBaseWorld = WorldTrunkBase;
    SetActorLocation(WorldTrunkBase);

    BuildNow();
}

void AHeroTreeActor::Regenerate()
{
    if (SpeciesPtr.IsValid())
    {
        // Modo runtime: los parametros ya los fijo Generate().
        BuildNow();
        return;
    }

    if (DebugSpecies)
    {
        // Uso suelto en editor: sin ecosistema, sin sombra de vecinos. Se
        // releen especie/semilla/posicion cada vez (asi mover el actor o
        // cambiar la semilla en el editor surte efecto).
        SpeciesPtr = DebugSpecies;
        GenSeed = (uint32)DebugSeed;
        bUseCoarseLight = false;
        TrunkBaseWorld = GetActorLocation();

        BuildNow();

        SpeciesPtr = nullptr; // no "pega" el estado runtime: proximo Regenerate re-lee
    }
}

void AHeroTreeActor::BuildNow()
{
    const USpeciesData* Sp = SpeciesPtr.Get();
    if (!Sp || !Mesh)
    {
        return;
    }

    // Fase 6 (6.1): margen de bounds para que el viento (WPO) no provoque
    // culling prematuro. MISMA fuente (settings) que los componentes de
    // instancing, para que hero e instancia lleven identico margen.
    Mesh->SetBoundsScale(FMath::Max(1.f, UEcosystemSettings::Get()->WindBoundsScale));

    uint32 Rng = GenSeed; // stream local: la misma semilla da el mismo arbol

    FSpaceColonizationConfig Config;
    Config.bEnableSelfPruning = bEnableSelfPruning;
    Config.bEnablePhototropism = bEnablePhototropism;

    // Luz gruesa FRESCA del subsistema vivo (nunca un puntero cacheado de otro
    // frame: el dueño del grid es el subsistema y este actor puede sobrevivirle
    // en la cache LRU del gestor de LOD).
    const FLightFieldCoarse* CoarseLight = nullptr;
    if (bUseCoarseLight)
    {
        if (UWorld* World = GetWorld())
        {
            if (const UEcosystemSubsystem* Eco = World->GetSubsystem<UEcosystemSubsystem>())
            {
                CoarseLight = &Eco->GetLightCoarse();
            }
        }
    }

    SpaceColonization::GrowTree(*Sp, Rng, TrunkBaseWorld, CoarseLight, Config,
        Skeleton, FineLight, Attractors);

    // Fase 6 (6.2): se le pasa al mallador la rejilla de luz FINA que acaba de
    // dejar el SCA. De ahi sale el AO de copa por vertice: los mismos voxels que
    // decidieron la autopoda oscurecen ahora el interior de la copa. Es la
    // coherencia "material atado al campo de la simulacion" que pide el doc.
    TreeMeshBuilder::BuildMesh(Skeleton, *Sp, Rng, MeshData, &FineLight);

    UploadSection(0, MeshData.Wood, BarkMaterial);
    UploadSection(1, MeshData.Leaves, LeafMaterial);
}

void AHeroTreeActor::UploadSection(int32 SectionIndex, const FTreeMeshBuffers& Buffers, UMaterialInterface* Material)
{
    if (!Mesh)
    {
        return;
    }

    if (Buffers.IsEmpty())
    {
        Mesh->ClearMeshSection(SectionIndex);
        return;
    }

    // Mundo -> local: la malla se guarda relativa a la base del tronco (que es
    // la ubicacion del actor), no en coordenadas absolutas. Los pivotes de rama
    // de la Fase 6 ya vienen en ESE mismo espacio local (ver TreeWindData.h), asi
    // que no hay nada que convertir en ellos.
    const FVector Origin = TrunkBaseWorld;
    TArray<FVector> LocalVerts;
    LocalVerts.SetNumUninitialized(Buffers.Vertices.Num());
    for (int32 i = 0; i < Buffers.Vertices.Num(); ++i)
    {
        LocalVerts[i] = Buffers.Vertices[i] - Origin;
    }

    // FVector -> FProcMeshTangent (la conversion a la API de PMC vive aqui, no
    // en el mallador, que es neutro).
    TArray<FProcMeshTangent> Tangents;
    Tangents.SetNumUninitialized(Buffers.Tangents.Num());
    for (int32 i = 0; i < Buffers.Tangents.Num(); ++i)
    {
        Tangents[i] = FProcMeshTangent(Buffers.Tangents[i], false);
    }

    // FASE 6: se sube la malla con los CUATRO canales UV y el color de vertice.
    // El UProceduralMeshComponent soporta exactamente UV0..UV3, que es justo el
    // motivo de haber empaquetado los datos de viento en 4 canales y no en mas:
    // asi el hero tree y la instancia horneada comparten material y un arbol no
    // cambia de aspecto -ni deja de moverse- al cruzar de nivel de detalle.
    Mesh->CreateMeshSection_LinearColor(
        SectionIndex,
        LocalVerts,
        Buffers.Triangles,
        Buffers.Normals,
        Buffers.UVs,   // UV0: textura
        Buffers.UV1,   // UV1: pivote.XY de la rama (metros, local)
        Buffers.UV2,   // UV2: (pivote.Z, nivel de rama)
        Buffers.UV3,   // UV3: (peso de balanceo, desfase)
        Buffers.Colors,// (AO de copa, tinte, nivel, 1)
        Tangents,
        /*bCreateCollision*/ false);

    if (Material)
    {
        Mesh->SetMaterial(SectionIndex, Material);
    }
}

void AHeroTreeActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!bDrawDebug)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // Esqueleto: linea de cada nodo a su padre (posiciones en mundo).
    for (int32 i = 0; i < Skeleton.Num(); ++i)
    {
        const int32 P = Skeleton.Nodes[i].Parent;
        if (P >= 0)
        {
            DrawDebugLine(World, Skeleton.Nodes[P].Pos, Skeleton.Nodes[i].Pos,
                FColor::Orange, false, -1.f, 0, 2.f);
        }
    }

    // Atractores: verde = vivo (aun perseguido), rojo = alcanzado o en sombra.
    for (const FAttractor& A : Attractors.Attractors)
    {
        DrawDebugPoint(World, A.Pos, 4.f, A.bAlive ? FColor::Green : FColor::Red, false, -1.f, 0);
    }
}

#if WITH_EDITOR
void AHeroTreeActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // C7: el Tick solo existe para el debug draw, asi que sigue al flag tambien
    // cuando se marca desde el panel de detalles del editor.
    SetActorTickEnabled(bDrawDebug);

    // Al tocar cualquier parametro en el editor, re-genera si hay especie de debug.
    if (DebugSpecies)
    {
        Regenerate();
    }
}
#endif
