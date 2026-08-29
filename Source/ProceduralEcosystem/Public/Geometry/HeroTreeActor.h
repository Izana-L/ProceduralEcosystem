/**
 * @file HeroTreeActor.h
 * @author Juan Luque Roldán
 * @brief Actor de un hero tree: árbol de geometría única generado en vivo sobre malla procedural.
 *
 * Declara AHeroTreeActor, la puerta de entrada del módulo de geometría al mundo del juego.
 * Encadena la colonización del espacio y el mallado para un árbol concreto y sube el
 * resultado a un UProceduralMeshComponent con dos secciones, madera y follaje, cada una
 * con su material. El árbol crece en coordenadas de mundo, que es lo que le permite leer
 * la sombra de los vecinos en la rejilla de luz gruesa, y la malla se guarda relativa a la
 * base del tronco. Es el extremo de detalle de la representación en dos escalas: unos
 * pocos árboles cercanos con malla propia frente a la masa del bosque, que son instancias
 * de la librería de arquetipos.
 *
 * @ingroup eco_geometry
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeLightGridFine.h"
#include "Geometry/AttractorCloud.h"
#include "Geometry/TreeMeshBuilder.h"
#include "HeroTreeActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class USpeciesData;
struct FLightFieldCoarse;

/**
 * Hero tree: un árbol con geometría única, generada en vivo por la colonización del
 * espacio en lugar de instanciada desde la librería de arquetipos.
 *
 * Generate() encadena tres pasos: crecimiento en coordenadas de mundo, para poder leer la
 * sombra de los vecinos en la rejilla gruesa; mallado; y subida de los vértices, pasados a
 * local respecto a la base del tronco, a las dos secciones del componente procedural
 * (0 = madera, 1 = follaje).
 *
 * @li Uso en runtime: @ref UEcosystemSubsystem::SpawnHeroTree y el gestor de niveles de
 *     representación llaman a Generate() con la especie, la semilla del árbol y la luz
 *     gruesa del momento.
 * @li Uso suelto en el editor: se coloca el actor, se le asigna DebugSpecies y se pulsa
 *     Regenerate; el árbol crece sin contexto de vecinos.
 *
 * @note El Tick solo existe para el dibujo de depuración y arranca desactivado.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API AHeroTreeActor : public AActor
{
    GENERATED_BODY()

public:
    AHeroTreeActor();

    virtual void Tick(float DeltaTime) override;

    /**
     * Genera el árbol y sube la malla.
     *
     * @param Seed            Semilla del árbol: la misma semilla da exactamente la misma
     *                        geometría.
     * @param CoarseLight     Rejilla de luz gruesa de la que sale la sombra de los
     *                        vecinos; nullptr para crecer sin ese contexto.
     * @param WorldTrunkBase  Base del tronco en mundo. El actor se coloca ahí y la malla
     *                        queda relativa a ese punto.
     */
    void Generate(const USpeciesData* InSpecies, uint32 Seed,
        const FLightFieldCoarse* CoarseLight, const FVector& WorldTrunkBase);

    /** Nodos del esqueleto generado. */
    int32 GetNodeCount() const { return Skeleton.Num(); }

    /**
     * Regenera el árbol con los parámetros actuales. Si nunca se llamó a Generate —el
     * caso del árbol suelto en el editor— toma especie, semilla y posición de
     * DebugSpecies, DebugSeed y la ubicación del actor, sin sombra de vecinos.
     */
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "Hero Tree")
    void Regenerate();

    // ==== USO SUELTO EN EL EDITOR, SIN ECOSISTEMA ====
    UPROPERTY(EditAnywhere, Category = "Hero Tree")
    TObjectPtr<USpeciesData> DebugSpecies;

    UPROPERTY(EditAnywhere, Category = "Hero Tree")
    int32 DebugSeed = 12345;

    // ==== MATERIALES: si son nulos, el componente usa el material por defecto ====
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    TObjectPtr<UMaterialInterface> BarkMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    TObjectPtr<UMaterialInterface> LeafMaterial;

    // ==== INTERRUPTORES DE LA GENERACIÓN: el resto de los ajustes van por defecto ====
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    bool bEnableSelfPruning = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    bool bEnablePhototropism = true;

    /**
     * Identidad de la deformación de tronco, con -1 como centinela de «derivarla de la
     * semilla del árbol», que es lo correcto para un hero suelto en el editor.
     *
     * El gestor de niveles de representación sí lo rellena al promocionar un árbol del
     * bosque, con la semilla de la variante de su instancia: sin eso, una instancia
     * arqueada se enderezaría al convertirse en hero delante del jugador, que es el salto
     * más visible que puede producir este sistema.
     *
     * @note Es int64 y no uint32 para que el centinela -1 no colisione con una semilla
     *       válida, y no es UPROPERTY editable porque no es un parámetro de diseño sino
     *       una conexión entre capas.
     * @see FSpaceColonizationConfig::DeformSeedOverride
     */
    int64 DeformSeedOverride = -1;

    // ==== DIBUJO DE DEPURACIÓN: ESQUELETO Y ATRACTORES ====
    /** Marcarlo desde código con SetDrawDebug(), que enciende también el Tick del actor. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree|Debug")
    bool bDrawDebug = false;

    /** Enciende o apaga a la vez el dibujo de depuración y el Tick que lo alimenta. */
    UFUNCTION(BlueprintCallable, Category = "Hero Tree|Debug")
    void SetDrawDebug(bool bInDrawDebug);

protected:
    virtual void BeginPlay() override;

    /** Hace que el actor tickee también en el viewport del editor, sin darle a Play, para
        que el dibujo de depuración se vea al pulsar Regenerate. Sin esto un AActor solo
        tickea con el juego en marcha. */
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    /** Genera el esqueleto y la malla con el estado actual y sube las dos secciones. */
    void BuildNow();

    /** Vuelca una sección de la malla en el componente procedural: pasa los vértices a
        local, convierte las tangentes al tipo de la API y le asigna el material. */
    void UploadSection(int32 SectionIndex, const FTreeMeshBuffers& Buffers, UMaterialInterface* Material);

    UPROPERTY(VisibleAnywhere, Category = "Hero Tree")
    TObjectPtr<UProceduralMeshComponent> Mesh;

    // ==== ESTADO DE LA ÚLTIMA GENERACIÓN, PARA PODER REGENERAR ====
    TWeakObjectPtr<const USpeciesData> SpeciesPtr;

    /** true si el árbol se generó con el contexto de luz gruesa del ecosistema.
        Se guarda el hecho y no el puntero a la rejilla: es una struct propiedad del
        subsistema, sin reflexión, y este actor puede sobrevivirle en la caché del gestor
        de niveles de representación, con lo que un puntero cacheado quedaría colgante.
        BuildNow() se lo pide fresco al subsistema vivo. */
    bool bUseCoarseLight = false;

    /** Semilla con la que se generó el árbol. */
    uint32 GenSeed = 0;

    /** Base del tronco y origen local de la malla. */
    FVector TrunkBaseWorld = FVector::ZeroVector;

    // ==== BUFFERS DE TRABAJO: SALIDA DE LA GENERACIÓN Y FUENTE DEL DIBUJO DE DEPURACIÓN ====
    FTreeSkeleton Skeleton;
    FTreeLightGridFine FineLight;
    FAttractorCloud Attractors;
    FTreeMeshData MeshData;
};