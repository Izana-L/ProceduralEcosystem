#include "Render/TreeInstanceHost.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"

ATreeInstanceHost::ATreeInstanceHost()
{
    PrimaryActorTick.bCanEverTick = false;

    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable); // los HISM hijos se modifican en runtime
    SetRootComponent(Root);

    SetActorEnableCollision(false);
    SetCanBeDamaged(false);
}

ATreeInstanceHost* ATreeInstanceHost::SpawnHost(UWorld* World, const TCHAR* EditorLabel)
{
    if (!World)
    {
        return nullptr;
    }

    ATreeInstanceHost* Host = World->SpawnActor<ATreeInstanceHost>(
        ATreeInstanceHost::StaticClass(), FTransform::Identity);
    if (!Host)
    {
        return nullptr;
    }

#if WITH_EDITOR
    if (EditorLabel)
    {
        Host->SetActorLabel(EditorLabel);
    }
#else
    (void)EditorLabel;
#endif

    // Red de seguridad: los ISM se cuelgan del root, y sin el no se registran.
    // El constructor ya lo crea, pero un host que llegue de otra ruta (o de una
    // subclase futura) no tiene por que traerlo.
    if (!Host->GetRootComponent())
    {
        USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("InstanceHostRoot"));
        Root->RegisterComponent();
        Host->SetRootComponent(Root);
    }

    return Host;
}

UHierarchicalInstancedStaticMeshComponent* ATreeInstanceHost::CreateInstancedComponent(
    AActor* Host, UStaticMesh* Mesh, FName ComponentName, bool bCastShadow,
    int32 NumCustomDataFloats, UMaterialInterface* OverrideMaterial)
{
    if (!Host || !Mesh)
    {
        return nullptr;
    }

    UHierarchicalInstancedStaticMeshComponent* Comp =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(Host, ComponentName);
    if (!Comp)
    {
        return nullptr;
    }

    // Movable: se anaden y quitan instancias en runtime. Con Static, UE asume
    // que el buffer de instancias no cambia tras registrar el componente.
    Comp->SetMobility(EComponentMobility::Movable);
    Comp->SetStaticMesh(Mesh);
    if (OverrideMaterial)
    {
        Comp->SetMaterial(0, OverrideMaterial);
    }
    Comp->NumCustomDataFloats = FMath::Max(0, NumCustomDataFloats);
    Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Comp->SetCanEverAffectNavigation(false); // si no, cada alta/baja pide rebuild de navmesh
    Comp->SetCastShadow(bCastShadow);

    Comp->SetupAttachment(Host->GetRootComponent());
    Comp->RegisterComponent();
    Host->AddInstanceComponent(Comp);
    return Comp;
}
