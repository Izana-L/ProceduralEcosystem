#include "Geometry/HeroTreeActor.h"
#include "Geometry/SpaceColonization.h"
#include "Species/SpeciesData.h"
#include "ProceduralMeshComponent.h"
#include "DrawDebugHelpers.h"

AHeroTreeActor::AHeroTreeActor()
{
    PrimaryActorTick.bCanEverTick = true; // solo para el debug draw

    Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("TreeMesh"));
    SetRootComponent(Mesh);
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
    CoarseLightPtr = CoarseLight;
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
        CoarseLightPtr = nullptr;
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

    uint32 Rng = GenSeed; // stream local: la misma semilla da el mismo arbol

    FSpaceColonizationConfig Config;
    Config.bEnableSelfPruning = bEnableSelfPruning;
    Config.bEnablePhototropism = bEnablePhototropism;

    SpaceColonization::GrowTree(*Sp, Rng, TrunkBaseWorld, CoarseLightPtr, Config,
        Skeleton, FineLight, Attractors);

    TreeMeshBuilder::BuildMesh(Skeleton, *Sp, Rng, MeshData);

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
    // la ubicacion del actor), no en coordenadas absolutas.
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

    Mesh->CreateMeshSection_LinearColor(
        SectionIndex, LocalVerts, Buffers.Triangles, Buffers.Normals, Buffers.UVs,
        TArray<FLinearColor>(), Tangents, /*bCreateCollision*/ false);

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

    // Al tocar cualquier parametro en el editor, re-genera si hay especie de debug.
    if (DebugSpecies)
    {
        Regenerate();
    }
}
#endif