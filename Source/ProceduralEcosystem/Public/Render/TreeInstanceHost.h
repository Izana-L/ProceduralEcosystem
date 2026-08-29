/**
 * @file TreeInstanceHost.h
 * @author Juan Luque Roldán
 * @brief Actor contenedor de los componentes de instancing y fábrica común de éstos.
 *
 * Existe por una razón de motor: un UPrimitiveComponent necesita un AActor propietario para
 * registrarse y dibujarse, y un subsistema de mundo no lo es. El actor no tiene lógica ni
 * tick; solo posee los componentes que le cuelgan la librería de árboles y la capa de suelo, y
 * se sitúa siempre en la identidad para que el espacio local del componente coincida con el
 * mundo. Declara además la fábrica estática que ambos usan para crear un componente
 * instanciado con la configuración compartida: movilidad, colisión, navegación, sombra y
 * canales de datos por instancia.
 *
 * @ingroup eco_render
 * @see @ref bib_instancing
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TreeInstanceHost.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;

/**
 * Actor que aloja los componentes de instancing del proyecto.
 *
 * No tiene lógica: solo es el propietario que el motor exige para registrar y dibujar un
 * componente primitivo. Se coloca siempre en la identidad —origen del mundo, sin rotación ni
 * escala—, de modo que el espacio local del componente coincide con el mundo y las
 * transformaciones de instancia se pasan tal cual, sin conversiones.
 */
UCLASS(NotPlaceable, Transient)
class PROCEDURALECOSYSTEM_API ATreeInstanceHost : public AActor
{
    GENERATED_BODY()

public:
    /** Actor sin tick, sin colisión y con una raíz movible de la que cuelgan los componentes. */
    ATreeInstanceHost();

    /**
     * Crea el actor en la identidad, con su componente raíz garantizado.
     *
     * @param EditorLabel Etiqueta con la que aparece en el editor; ignorada en otras builds.
     * @return El actor, o nullptr si el mundo es nulo o el spawn falla; los llamantes usan
     *         ese nullptr como condición para no inicializar su capa.
     */
    static ATreeInstanceHost* SpawnHost(UWorld* World, const TCHAR* EditorLabel);

    /**
     * Crea y registra un componente de instancing colgado de Host con la configuración común
     * a todo el proyecto: movible, porque las instancias se dan de alta y de baja en runtime;
     * sin colisión; fuera de la navegación, ya que si no cada alta o baja pediría reconstruir
     * el navmesh; y con la sombra que se le indique.
     *
     * Es el núcleo compartido por la librería de árboles y por la capa de suelo. Lo que sí
     * difiere entre ellas —distancias de cull, viento— lo aplica el llamante sobre el
     * componente devuelto.
     *
     * @param NumCustomDataFloats Canales de datos por instancia. Va como parámetro, y no como
     *                            ajuste posterior, porque dimensiona el buffer por instancia y
     *                            debe quedar fijado antes de registrar el componente.
     * @param OverrideMaterial    Material que sustituye al de la ranura 0; opcional.
     * @return El componente ya registrado, o nullptr si faltan Host o Mesh.
     */
    static UHierarchicalInstancedStaticMeshComponent* CreateInstancedComponent(
        AActor* Host, UStaticMesh* Mesh, FName ComponentName, bool bCastShadow,
        int32 NumCustomDataFloats = 0, UMaterialInterface* OverrideMaterial = nullptr);
};
