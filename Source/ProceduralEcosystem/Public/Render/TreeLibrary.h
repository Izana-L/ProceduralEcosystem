/**
 * @file TreeLibrary.h
 * @author Juan Luque Roldán
 * @brief Librería de arquetipos: mallas horneadas, impostores y componentes de instancing.
 *
 * Posee los activos con los que se dibuja el bosque a escala. Por cada arquetipo deriva una
 * especie con la morfología escalada al bucket y perturbada por la variante, hornea una sola
 * vez su malla y su impostor, y sirve los dos componentes instanciados que los dibujan junto
 * con el mapeo instancia → árbol que permite dar de baja sin corromper índices. Un componente
 * por malla convierte decenas de miles de árboles en un puñado de draw calls. El horneado es
 * amortizado: quien pide un arquetipo que todavía no existe lo encola, y cada frame se hornea
 * un número fijo, de modo que la librería se llena sin bloquear el hilo de juego. No decide
 * con qué se dibuja cada árbol —eso es del gestor de niveles de representación—: aquí solo
 * hay activos y su ciclo de vida.
 *
 * @ingroup eco_render
 * @see @ref bib_deussen1998
 * @see @ref bib_instancing
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Render/TreeArchetype.h"
#include "TreeLibrary.generated.h"

class AActor;
class UStaticMesh;
class USpeciesData;
class UHierarchicalInstancedStaticMeshComponent;

/** Ajustes de la librería; los rellena el gestor de niveles desde UEcosystemSettings. */
struct FTreeLibraryConfig
{
    /** Buckets de tamaño por especie. Con más buckets, la silueta salta menos al crecer y
        la librería tiene más mallas que hornear. */
    int32 NumAgeBuckets = 5;
    /**
     * Canales de datos por instancia que dimensionan los componentes. Se usan tres: [0] fase
     * estacional estable por árbol, [1] sequedad del follaje y [2] apertura de copa. Con
     * menos de tres, el material lee 0 en los que falten: degrada en silencio, no rompe.
     */
    int32 NumInstanceCustomDataFloats = 3;

    /** Las instancias cercanas proyectan sombra. */
    bool  bInstancesCastShadow = true;
    /** Los impostores no: la sombra del campo lejano la aporta el HLOD. */
    bool  bImpostorsCastShadow = false;
    /** Cull por distancia del propio componente de instancias; 0 lo desactiva. */
    float InstanceEndCullDistanceCm = 0.f;
    /** Ídem para el componente de impostores. */
    float ImpostorEndCullDistanceCm = 0.f;

    // --- Viento en los componentes de instancing ---
    /** Las instancias cercanas evalúan el World Position Offset, es decir, el balanceo. */
    bool  bWindOnInstances = true;
    /** Los impostores no se mueven: a esa distancia el balanceo no se aprecia y sí se paga. */
    bool  bWindOnImpostors = false;
    /**
     * Distancia (cm) a partir de la cual el componente deja de evaluar el World Position
     * Offset. Más allá de unos 120 m el balanceo es sub-píxel y evaluarlo solo cuesta vertex
     * shader. 0 = sin corte.
     */
    float WindWpoCutoffCm = 12000.f;
    /**
     * Margen de la caja envolvente de los componentes con viento. El desplazamiento de
     * vértices mueve geometría que el culling no ve; sin margen, un árbol al borde del
     * encuadre desaparece de golpe con las ramas todavía dentro.
     */
    float WindBoundsScale = 1.15f;
};

/**
 * Entrada de la librería: la malla horneada de un arquetipo, su impostor y los dos
 * componentes de instancing que los dibujan, con su contabilidad de índices.
 *
 * `MeshMapping[i]` es el StableId del árbol que ocupa la instancia `i` del componente. Es el
 * inverso de `FTreeRenderState::InstanceIndex` y lo que permite reparar los índices tras un
 * borrado por lotes.
 *
 * @see TreeInstancing::CompactMappingAfterRemoval
 */
USTRUCT()
struct FTreeArchetypeEntry
{
    GENERATED_BODY()

    /** Malla horneada del arquetipo. */
    UPROPERTY(Transient) TObjectPtr<UStaticMesh> Mesh = nullptr;
    /** Tarjetas cruzadas que la sustituyen en el campo lejano. */
    UPROPERTY(Transient) TObjectPtr<UStaticMesh> ImpostorMesh = nullptr;
    /** Componente que dibuja la malla; se crea la primera vez que se pide. */
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> MeshISM = nullptr;
    /** Componente que dibuja el impostor. */
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> ImpostorISM = nullptr;

    // Sin reflexión: son datos calientes de la capa de presentación, no estado de objeto.
    /** Instancia → StableId en MeshISM. */
    TArray<uint32> MeshMapping;
    /** Instancia → StableId en ImpostorISM. */
    TArray<uint32> ImpostorMapping;

    /** Alto de la caja local horneada; es lo que dimensiona el impostor. */
    float BakedHeightCm = 0.f;
    /** Triángulos de la sección de madera. */
    int32 WoodTriangles = 0;
    /** Triángulos de la sección de follaje. */
    int32 LeafTriangles = 0;
    /** La entrada puede existir a medias: solo es utilizable si está horneada. */
    bool  bBaked = false;
};

/**
 * Librería de arquetipos: dueña de las mallas, los impostores y los componentes de instancing
 * con los que se dibuja el bosque.
 *
 * Tres responsabilidades:
 * @li Derivar por arquetipo una USpeciesData con la morfología escalada al bucket y
 *     perturbada por la variante: es la fábrica de parámetros de todo lo que se hornea.
 * @li Hornear su malla una sola vez —colonización del espacio, mallador y malla estática— con
 *     semilla fija, de modo que la librería sale idéntica en cada arranque.
 * @li Crear y servir el componente instanciado de cada arquetipo: un componente por malla es
 *     un draw call por arquetipo, no por árbol.
 *
 * El horneado es amortizado: FindOrRequestBake encola y ProcessBakeQueue hornea como mucho N
 * arquetipos por frame, para que llenar la librería no cueste un parón de segundos.
 *
 * @see UTreeRenderSubsystem, que es quien decide con qué se dibuja cada árbol.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UTreeLibrary : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Toma posesión del actor que alojará los componentes y de las especies base.
     * @param InHost Actor contenedor; debe estar en la identidad.
     * @pre Llamada obligatoria antes de cualquier otra operación.
     * @see ATreeInstanceHost
     */
    void Initialize(AActor* InHost, const TArray<TObjectPtr<USpeciesData>>& InSpecies,
        const FTreeLibraryConfig& InConfig);

    /** Destruye los componentes y suelta mallas, especies derivadas y cola de horneado. */
    void Shutdown();

    /**
     * Especie derivada del arquetipo: duplicado transitorio de la especie base con la
     * morfología escalada al bucket y el jitter estable de la variante. Se cachea por clave.
     *
     * La usan tanto el horneado de la librería como los hero trees, de modo que un hero y su
     * instancia son el mismo árbol y no hay salto de forma al acercarse.
     *
     * @return La especie derivada, o nullptr si la especie base no existe.
     */
    const USpeciesData* GetArchetypeSpecies(const FArchetypeKey& Key);

    /** @return La entrada del arquetipo si ya está horneada; nullptr en cualquier otro caso. */
    FTreeArchetypeEntry* Find(const FArchetypeKey& Key);

    /**
     * Igual que Find, pero si el arquetipo no está horneado lo encola.
     * @return La entrada si ya estaba lista; nullptr esta vez, y en algún frame posterior
     *         la misma llamada devolverá la entrada horneada.
     */
    FTreeArchetypeEntry* FindOrRequestBake(const FArchetypeKey& Key);

    /**
     * Drena la cola de horneado con presupuesto acotado.
     * @param MaxThisFrame Tope de arquetipos a hornear en esta llamada; se fuerza a 1 como
     *                     mínimo para que la cola siempre avance.
     * @return Cuántos arquetipos se hornearon con éxito.
     * @see @ref bib_funkhouser1993
     */
    int32 ProcessBakeQueue(int32 MaxThisFrame);

    /**
     * Hornea de una vez la librería entera —especies × buckets × variantes— y vacía la cola.
     * @return Número de mallas horneadas en esta llamada.
     * @warning Bloquea el hilo de juego mientras dura; su sitio es el arranque o una
     *          herramienta, no el bucle de frame.
     */
    int32 BakeAll();

    /**
     * Componente de instancing del arquetipo, creándolo la primera vez con la configuración
     * común, el viento y el cull por distancia.
     * @param bImpostor Elige entre el componente de la malla y el del impostor.
     * @return El componente, o nullptr si el arquetipo aún no está horneado o falta el host.
     */
    UHierarchicalInstancedStaticMeshComponent* GetOrCreateComponent(const FArchetypeKey& Key, bool bImpostor);

    /** Vacía las instancias y su mapeo en todos los componentes; las mallas se conservan. */
    void ClearAllInstances();

    /** @return Arquetipos encolados y todavía sin hornear. */
    int32 GetNumPendingBakes() const { return BakeQueue.Num() - BakeQueueHead; }

    /** Recuento agregado de la librería para el diagnóstico en consola. */
    void GetStats(int32& OutMeshes, int32& OutComponents, int32& OutInstances, int32& OutTriangles) const;

    /**
     * Reaplica los ajustes de viento —evaluar el desplazamiento de vértices, distancia de
     * corte y margen de la caja envolvente— a todos los componentes ya creados. Permite
     * mover esos ajustes en vivo sin reconstruir la capa de presentación entera.
     */
    void ApplyWindSettings(const FTreeLibraryConfig& InConfig);

    /**
     * Semilla de la deformación del tronco de una variante.
     *
     * El bucket se ignora deliberadamente: los buckets de una variante son las etapas de un
     * mismo árbol, así que si entrase en la semilla un individuo arqueado se enderezaría al
     * cambiar de bucket. Es la misma razón por la que el jitter de morfología de
     * GetArchetypeSpecies tampoco lo mira.
     *
     * Pública y estática porque la necesitan dos rutas que no se conocen entre sí, el
     * horneado de la librería y la promoción a hero. Con una sola copia de la fórmula, el
     * hero y su instancia se curvan igual y no hay salto de forma al acercarse.
     *
     * @see FSpaceColonizationConfig::DeformSeedOverride
     */
    static uint32 VariantDeformSeed(uint16 InSpecies, uint8 InVariant)
    {
        return EcoRand::Hash32(FArchetypeKey(InSpecies, 0, InVariant).Pack() * 0x9E3779B9u ^ 0x0DEF0B75u);
    }

private:
    /** Genera el árbol del arquetipo y lo hornea a malla estática más impostor.
        @return false si la especie no existe o el árbol no produjo geometría. */
    bool BakeArchetype(const FArchetypeKey& Key);

    /** Especie base del catálogo, o nullptr si el índice no existe. Devuelve no-const a
        propósito: GetArchetypeSpecies tiene que duplicar el activo, y así no hace falta un
        const_cast en el punto de uso. */
    USpeciesData* GetBaseSpecies(uint16 SpeciesId) const;

    /** Aplica a un componente el interruptor de viento, la distancia de corte del
        desplazamiento de vértices y el margen de la caja envolvente. */
    void ConfigureWind(UHierarchicalInstancedStaticMeshComponent* Comp, bool bImpostor) const;

    /** Semilla fija del arquetipo, derivada solo de su clave: la librería sale idéntica en
        cada arranque. */
    static uint32 ArchetypeSeed(const FArchetypeKey& Key)
    {
        return EcoRand::Hash32(0xB5297A4Du ^ (Key.Pack() * 2654435761u));
    }

    /** Actor del que cuelgan todos los componentes de instancing de la librería. */
    UPROPERTY(Transient) TObjectPtr<AActor> Host = nullptr;
    /** Catálogo de especies sin derivar, tal como llega de la configuración. */
    UPROPERTY(Transient) TArray<TObjectPtr<USpeciesData>> BaseSpecies;

    /** Entradas horneadas, indexadas por FArchetypeKey::Pack(). */
    UPROPERTY(Transient) TMap<uint32, FTreeArchetypeEntry> Entries;
    /** Especies derivadas ya calculadas, indexadas por la misma clave empaquetada. */
    UPROPERTY(Transient) TMap<uint32, TObjectPtr<USpeciesData>> ArchetypeSpecies;

    FTreeLibraryConfig Config;

    /** Cola FIFO de horneado con cursor de lectura: extraer con `RemoveAt(0)` desplazaría
        todos los elementos restantes en cada extracción, y drenar N sería cuadrático. El
        cursor avanza y la cola solo se compacta cuando la alcanza. */
    TArray<uint32> BakeQueue;
    /** Primer elemento de BakeQueue todavía sin consumir. */
    int32          BakeQueueHead = 0;
    /** Deduplicación: un arquetipo no se encola dos veces. */
    TSet<uint32>   BakeQueued;
};
