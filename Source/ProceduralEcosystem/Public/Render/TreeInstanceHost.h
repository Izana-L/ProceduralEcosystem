#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TreeInstanceHost.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

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

    /**
     * Crea el actor host en la identidad y le pone etiqueta en el editor.
     *
     * Lo hacian por su cuenta, con el mismo bloque de cinco lineas, tanto
     * UTreeRenderSubsystem::EnsureInitialized como UTreeSoilSubsystem::
     * EnsureInitialized. Devuelve nullptr si el mundo es nulo o el spawn falla,
     * que es justo la condicion que ambos comprueban para no inicializarse.
     */
    static ATreeInstanceHost* SpawnHost(UWorld* World, const TCHAR* EditorLabel);

    /**
     * Crea y registra un componente de instancing colgado de Host, con la
     * configuracion COMUN a todo el proyecto: movible (se anaden y quitan
     * instancias en runtime), sin colision, fuera del navmesh -si no, cada
     * alta/baja pide un rebuild- y con la sombra que se le pida.
     *
     * Es el nucleo que compartian UTreeLibrary::GetOrCreateComponent (arboles e
     * impostors) y UTreeSoilSubsystem::CreateISM (tocones y hojarasca): siete
     * llamadas identicas duplicadas en dos ficheros. Lo que SI cambia entre
     * ellos -distancias de cull, viento- lo aplica el llamador sobre el
     * componente devuelto.
     *
     * NumCustomDataFloats y el material van como parametros, y no despues, a
     * proposito: los dos llamantes los fijaban ANTES de registrar el componente,
     * y NumCustomDataFloats en particular es una escritura directa de propiedad
     * (dimensiona el buffer de datos por instancia) que conviene dejar puesta de
     * entrada y no cuando el componente ya esta vivo.
     *
     * @return El componente ya registrado, o nullptr si faltan Host o Mesh.
     */
    static UHierarchicalInstancedStaticMeshComponent* CreateInstancedComponent(
        AActor* Host, UStaticMesh* Mesh, FName ComponentName, bool bCastShadow,
        int32 NumCustomDataFloats = 0, UMaterialInterface* OverrideMaterial = nullptr);
};
