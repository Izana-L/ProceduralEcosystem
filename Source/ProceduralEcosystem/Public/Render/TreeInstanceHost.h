#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TreeInstanceHost.generated.h"

/**
 * Actor contenedor de los componentes ISM/HISM de la libreria (Fase 4).
 *
 * Existe por una razon puramente tecnica: en UE un UPrimitiveComponent necesita
 * un AActor propietario para registrarse y renderizarse, y un UWorldSubsystem no
 * lo es. Este actor no tiene logica: solo posee los componentes que crea
 * UTreeLibrary.
 *
 * IMPORTANTE: se coloca SIEMPRE en la identidad (origen del mundo, sin rotacion
 * ni escala). Asi el espacio local del componente coincide con el mundo y las
 * transformaciones de instancia se pueden pasar tal cual, sin conversiones ni
 * el flag bWorldSpace de AddInstances.
 */
UCLASS(NotPlaceable, Transient)
class PROCEDURALECOSYSTEM_API ATreeInstanceHost : public AActor
{
    GENERATED_BODY()

public:
    ATreeInstanceHost();
};
