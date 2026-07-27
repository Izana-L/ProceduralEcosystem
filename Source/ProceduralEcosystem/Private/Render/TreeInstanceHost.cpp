#include "Render/TreeInstanceHost.h"
#include "Components/SceneComponent.h"

ATreeInstanceHost::ATreeInstanceHost()
{
    PrimaryActorTick.bCanEverTick = false;

    USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable); // los HISM hijos se modifican en runtime
    SetRootComponent(Root);

    SetActorEnableCollision(false);
    SetCanBeDamaged(false);
}
