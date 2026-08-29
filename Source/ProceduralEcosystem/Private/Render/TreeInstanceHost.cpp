/**
 * @file TreeInstanceHost.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del actor contenedor y de la fábrica de componentes de instancing.
 *
 * Contiene el constructor, que deja el actor sin tick, sin colisión y con una raíz movible; el
 * spawn en la identidad con su etiqueta de editor; y la creación del componente instanciado
 * con la configuración común, fijando los canales de datos por instancia antes de registrarlo
 * y colgándolo de la raíz.
 *
 * @ingroup eco_render
 */

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
    Root->SetMobility(EComponentMobility::Movable); // los componentes hijos cambian en runtime
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

    // Los componentes de instancing se cuelgan de la raíz, y sin ella no se registran. El
    // constructor la crea, pero una subclase no tiene por qué traerla.
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

    // Movable porque se añaden y quitan instancias en runtime: con Static, el motor asume que
    // el buffer de instancias no cambia una vez registrado el componente.
    Comp->SetMobility(EComponentMobility::Movable);
    Comp->SetStaticMesh(Mesh);
    if (OverrideMaterial)
    {
        Comp->SetMaterial(0, OverrideMaterial);
    }
    Comp->NumCustomDataFloats = FMath::Max(0, NumCustomDataFloats);
    Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Comp->SetCanEverAffectNavigation(false); // si no, cada alta o baja pide rebuild del navmesh
    Comp->SetCastShadow(bCastShadow);

    Comp->SetupAttachment(Host->GetRootComponent());
    Comp->RegisterComponent();
    Host->AddInstanceComponent(Comp);
    return Comp;
}
