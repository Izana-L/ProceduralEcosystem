#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Render/TreeArchetype.h"
#include "TreeLibrary.generated.h"

class AActor;
class UStaticMesh;
class USpeciesData;
class UHierarchicalInstancedStaticMeshComponent;

/** Ajustes de la libreria (los rellena el gestor de LOD desde UEcosystemSettings). */
struct FTreeLibraryConfig
{
    int32 NumAgeBuckets = 5;
    /** Fase 5 usa DOS: [0] fase estacional estable por arbol, [1] sequedad. La
        Fase 6 anade [2] apertura de copa (AO). Con menos de 3 el material se
        queda sin alguno de esos canales y los ve como 0, que es un degradado
        silencioso pero no rompe nada. */
    int32 NumInstanceCustomDataFloats = 3;
    bool  bInstancesCastShadow = true;
    bool  bImpostorsCastShadow = false;    // doc. 4.6: que la sombra lejana la ponga el HLOD
    float InstanceEndCullDistanceCm = 0.f; // 0 = sin cull por distancia del propio ISM
    float ImpostorEndCullDistanceCm = 0.f;

    // --- Fase 6 (doc. 6.1): viento en los componentes de instancing ---
    /** Las instancias cercanas evaluan el World Position Offset (el balanceo). */
    bool  bWindOnInstances = true;
    /** Los impostors, por defecto NO (campo lejano; ver doc. 6.1). */
    bool  bWindOnImpostors = false;
    /**
     * Distancia (cm) a partir de la cual el componente deja de evaluar el WPO.
     * Es el caveat de rendimiento del doc. 6.1 hecho ajuste: a 120 m el balanceo
     * es sub-pixel y evaluarlo solo cuesta vertex shader. 0 = sin corte.
     */
    float WindWpoCutoffCm = 12000.f;
    /**
     * Margen de la caja envolvente de los componentes con viento. El WPO mueve
     * vertices que el culling no ve; sin margen, un arbol al borde del encuadre
     * desaparece de golpe con las ramas todavia dentro.
     */
    float WindBoundsScale = 1.15f;
};

/**
 * Una entrada de la libreria: la malla horneada de un arquetipo, su impostor y
 * los dos componentes de instancing (malla e impostor) con su bookkeeping.
 *
 * MeshMapping[i] = StableId del arbol que ocupa la instancia i del componente.
 * Es el inverso de FTreeRenderState::InstanceIndex y lo que permite arreglar los
 * indices tras un borrado por lotes (ver TreeInstancing::CompactMappingAfterRemoval).
 */
USTRUCT()
struct FTreeArchetypeEntry
{
    GENERATED_BODY()

    UPROPERTY(Transient) TObjectPtr<UStaticMesh> Mesh = nullptr;
    UPROPERTY(Transient) TObjectPtr<UStaticMesh> ImpostorMesh = nullptr;
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> MeshISM = nullptr;
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> ImpostorISM = nullptr;

    // Bookkeeping (no reflejado: son datos calientes de la capa de render).
    TArray<uint32> MeshMapping;
    TArray<uint32> ImpostorMapping;

    float BakedHeightCm = 0.f;
    int32 WoodTriangles = 0;
    int32 LeafTriangles = 0;
    bool  bBaked = false;
};

/**
 * Libreria de arquetipos (doc. Fase 4, 4.2).
 *
 * Responsabilidades:
 *   1. Derivar, por arquetipo, una USpeciesData con la morfologia escalada al
 *      bucket y perturbada por la variante (fabrica de parametros).
 *   2. Hornear su malla con la Fase 3 (SCA -> mallador -> UStaticMesh) UNA vez,
 *      con semilla fija -> libreria reproducible.
 *   3. Crear y servir el componente ISM/HISM de cada arquetipo (un componente
 *      por malla = un draw call por arquetipo, no por arbol).
 *
 * NO decide que arbol se dibuja como que: eso es del gestor de LOD
 * (UTreeRenderSubsystem). Aqui solo hay activos y su ciclo de vida.
 *
 * El horneado es AMORTIZADO: FindOrRequestBake encola y ProcessBakeQueue hornea
 * como mucho N por frame, para no meter un hitch de segundos al arrancar
 * (Apendice B, riesgo Medio de generacion en el game thread).
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UTreeLibrary : public UObject
{
    GENERATED_BODY()

public:
    /** InHost debe estar en la identidad (ver ATreeInstanceHost). */
    void Initialize(AActor* InHost, const TArray<TObjectPtr<USpeciesData>>& InSpecies,
        const FTreeLibraryConfig& InConfig);
    void Shutdown();

    /**
     * Especie DERIVADA del arquetipo: duplicado transitorio de la especie base
     * con la morfologia escalada al bucket y con el jitter estable de la
     * variante. La usan tanto el horneado de la libreria como los hero trees,
     * de modo que un hero y su instancia son "la misma familia" de arbol.
     */
    const USpeciesData* GetArchetypeSpecies(const FArchetypeKey& Key);

    /** Entrada ya horneada, o nullptr. */
    FTreeArchetypeEntry* Find(const FArchetypeKey& Key);

    /** Igual que Find, pero si no existe encola el horneado (devuelve nullptr esta vez). */
    FTreeArchetypeEntry* FindOrRequestBake(const FArchetypeKey& Key);

    /** Hornea como mucho MaxThisFrame arquetipos pendientes. Devuelve cuantos hizo. */
    int32 ProcessBakeQueue(int32 MaxThisFrame);

    /** Hornea de golpe toda la libreria (especies x buckets x variantes). Devuelve cuantas mallas. */
    int32 BakeAll();

    /** Componente de instancing del arquetipo, creandolo la primera vez. */
    UHierarchicalInstancedStaticMeshComponent* GetOrCreateComponent(const FArchetypeKey& Key, bool bImpostor);

    /** Vacia todas las instancias de todos los componentes (las mallas se conservan). */
    void ClearAllInstances();

    int32 GetNumPendingBakes() const { return BakeQueue.Num() - BakeQueueHead; }

    /** Recuento agregado para Eco.LOD.Stats. */
    void GetStats(int32& OutMeshes, int32& OutComponents, int32& OutInstances, int32& OutTriangles) const;

    /**
     * Fase 6: reaplica los ajustes de viento (evaluar WPO, distancia de corte,
     * margen de bounds) a TODOS los componentes ya creados. Lo llama el gestor de
     * LOD cuando cambian los ajustes en vivo, para no tener que reconstruir la
     * capa de render entera solo por mover un slider.
     */
    void ApplyWindSettings(const FTreeLibraryConfig& InConfig);

private:
    bool BakeArchetype(const FArchetypeKey& Key);
    /** Devuelve no-const a proposito: GetArchetypeSpecies necesita duplicar el
        asset (DuplicateObject) y asi se evita el const_cast en el punto de uso.
        Los TObjectPtr del array ya son no-const. */
    USpeciesData* GetBaseSpecies(uint16 SpeciesId) const;

    /** Aplica a un componente los ajustes de viento/bounds de la Fase 6. */
    void ConfigureWind(UHierarchicalInstancedStaticMeshComponent* Comp, bool bImpostor) const;

    /** Semilla fija del arquetipo: misma libreria en cada arranque (doc. 3.8). */
    static uint32 ArchetypeSeed(const FArchetypeKey& Key)
    {
        return EcoRand::Hash32(0xB5297A4Du ^ (Key.Pack() * 2654435761u));
    }

    UPROPERTY(Transient) TObjectPtr<AActor> Host = nullptr;
    UPROPERTY(Transient) TArray<TObjectPtr<USpeciesData>> BaseSpecies;

    /** Clave = FArchetypeKey::Pack(). */
    UPROPERTY(Transient) TMap<uint32, FTreeArchetypeEntry> Entries;
    UPROPERTY(Transient) TMap<uint32, TObjectPtr<USpeciesData>> ArchetypeSpecies;

    FTreeLibraryConfig Config;

    /** Cola FIFO de horneados con CURSOR de lectura: extraer con RemoveAt(0)
        desplazaba todos los elementos restantes en cada extraccion (drenar N
        era O(N^2)). El cursor avanza y la cola se compacta solo al vaciarse. */
    TArray<uint32> BakeQueue;
    int32          BakeQueueHead = 0;
    TSet<uint32>   BakeQueued;
};
