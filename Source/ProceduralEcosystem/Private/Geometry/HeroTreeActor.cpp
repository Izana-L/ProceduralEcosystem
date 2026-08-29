/**
 * @file HeroTreeActor.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del actor de hero tree: generación, subida de la malla y depuración.
 *
 * Orquesta el pipeline de geometría para un árbol concreto: fija el margen de bounds que
 * el viento necesita, pide la luz gruesa al subsistema vivo, encadena crecimiento y
 * mallado, y sube las dos secciones al componente procedural pasando los vértices a
 * coordenadas locales y convirtiendo las tangentes al tipo que espera esa API. Contiene
 * también el dibujo de depuración del esqueleto y de la nube de atractores, único motivo
 * por el que el actor tiene Tick.
 *
 * @ingroup eco_geometry
 */

#include "Geometry/HeroTreeActor.h"
#include "Geometry/SpaceColonization.h"
#include "Simulation/EcosystemSubsystem.h" // luz gruesa fresca en cada BuildNow
#include "Config/EcosystemSettings.h"      // WindBoundsScale: fuente única del margen
#include "Species/SpeciesData.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

AHeroTreeActor::AHeroTreeActor()
{
    // El Tick de este actor solo sirve para el dibujo de depuración, así que arranca
    // desactivado y lo enciende bDrawDebug. La caché del gestor de niveles de
    // representación mantiene vivos hasta el doble del presupuesto de hero trees, y un
    // Tick por actor que solo hace un early-out es coste puro que se ve en el perfilado.
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
    // Solo se recuerda si hay contexto de luz: el puntero a la rejilla se pide fresco al
    // subsistema en cada BuildNow (ver la nota de bUseCoarseLight en la cabecera).
    bUseCoarseLight = (CoarseLight != nullptr);
    TrunkBaseWorld = WorldTrunkBase;
    SetActorLocation(WorldTrunkBase);

    BuildNow();
}

void AHeroTreeActor::Regenerate()
{
    if (SpeciesPtr.IsValid())
    {
        // Modo runtime: los parámetros ya los fijó Generate().
        BuildNow();
        return;
    }

    if (DebugSpecies)
    {
        // Uso suelto en el editor: sin ecosistema y sin sombra de vecinos. Especie,
        // semilla y posición se releen cada vez, para que mover el actor o cambiar la
        // semilla en el panel de detalles surta efecto.
        SpeciesPtr = DebugSpecies;
        GenSeed = (uint32)DebugSeed;
        bUseCoarseLight = false;
        TrunkBaseWorld = GetActorLocation();

        BuildNow();

        SpeciesPtr = nullptr; // no deja pegado el estado de runtime: el siguiente vuelve a leer
    }
}

void AHeroTreeActor::BuildNow()
{
    const USpeciesData* Sp = SpeciesPtr.Get();
    if (!Sp || !Mesh)
    {
        return;
    }

    // Margen de bounds para que el desplazamiento de vértices del viento no provoque
    // culling prematuro. Sale de los mismos ajustes que usan los componentes de
    // instancing, para que hero e instancia lleven idéntico margen.
    Mesh->SetBoundsScale(FMath::Max(1.f, UEcosystemSettings::Get()->WindBoundsScale));

    uint32 Rng = GenSeed; // stream local: la misma semilla da el mismo árbol

    FSpaceColonizationConfig Config;
    Config.bEnableSelfPruning = bEnableSelfPruning;
    Config.bEnablePhototropism = bEnablePhototropism;
    // Con -1, el caso del hero suelto en el editor, la curvatura se deriva de la semilla
    // del propio árbol. El gestor de niveles de representación lo rellena con la semilla
    // de la variante para que el hero doble exactamente como doblaba su instancia.
    Config.DeformSeedOverride = DeformSeedOverride;

    // Luz gruesa fresca del subsistema vivo, nunca un puntero cacheado de otro frame: la
    // rejilla es propiedad del subsistema y este actor puede sobrevivirle en la caché del
    // gestor de niveles de representación.
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

    // Al mallador se le pasa la rejilla de luz fina tal y como la deja el crecimiento: de
    // ahí sale la oclusión de copa por vértice, con lo que los mismos vóxeles que
    // decidieron la autopoda oscurecen el interior de la copa y el material queda atado al
    // campo de la simulación.
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

    // Mundo -> local: la malla se guarda relativa a la base del tronco, que es la
    // ubicación del actor, y no en coordenadas absolutas. Los pivotes de rama ya vienen en
    // ese mismo espacio local (ver FTreeWindNode), así que en ellos no hay nada que
    // convertir.
    const FVector Origin = TrunkBaseWorld;
    TArray<FVector> LocalVerts;
    LocalVerts.SetNumUninitialized(Buffers.Vertices.Num());
    for (int32 i = 0; i < Buffers.Vertices.Num(); ++i)
    {
        LocalVerts[i] = Buffers.Vertices[i] - Origin;
    }

    // FVector -> FProcMeshTangent: la conversión a la API del componente vive aquí y no en
    // el mallador, cuyos buffers son de formato neutro para servir también al horneado.
    TArray<FProcMeshTangent> Tangents;
    Tangents.SetNumUninitialized(Buffers.Tangents.Num());
    for (int32 i = 0; i < Buffers.Tangents.Num(); ++i)
    {
        Tangents[i] = FProcMeshTangent(Buffers.Tangents[i], false);
    }

    // La malla sube con los cuatro canales UV y el color de vértice. El
    // UProceduralMeshComponent soporta exactamente UV0..UV3, que es el motivo de haber
    // empaquetado los datos de viento en cuatro canales y no en más: así el hero tree y la
    // instancia horneada comparten material y un árbol no cambia de aspecto, ni deja de
    // moverse, al cruzar de nivel de representación.
    Mesh->CreateMeshSection_LinearColor(
        SectionIndex,
        LocalVerts,
        Buffers.Triangles,
        Buffers.Normals,
        Buffers.UVs,   // UV0: textura de corteza u hoja
        Buffers.UV1,   // UV1: pivote XY de la rama, en metros y local
        Buffers.UV2,   // UV2: pivote Z y nivel de rama
        Buffers.UV3,   // UV3: peso de balanceo y desfase
        Buffers.Colors,// oclusión de copa, tinte, nivel y 1
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

    // Esqueleto: una línea de cada nodo a su padre, con las posiciones en mundo.
    for (int32 i = 0; i < Skeleton.Num(); ++i)
    {
        const int32 P = Skeleton.Nodes[i].Parent;
        if (P >= 0)
        {
            DrawDebugLine(World, Skeleton.Nodes[P].Pos, Skeleton.Nodes[i].Pos,
                FColor::Orange, false, -1.f, 0, 2.f);
        }
    }

    // Atractores: verde el que sigue vivo, rojo el alcanzado por una rama o en sombra.
    for (const FAttractor& A : Attractors.Attractors)
    {
        DrawDebugPoint(World, A.Pos, 4.f, A.bAlive ? FColor::Green : FColor::Red, false, -1.f, 0);
    }
}

#if WITH_EDITOR
void AHeroTreeActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    // El Tick solo existe para el dibujo de depuración, así que sigue al flag también
    // cuando se marca desde el panel de detalles del editor.
    SetActorTickEnabled(bDrawDebug);

    // Cualquier cambio de parámetro en el editor regenera el árbol si hay especie puesta.
    if (DebugSpecies)
    {
        Regenerate();
    }
}
#endif
