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
 * Hero tree: un arbol con geometria UNICA generada en vivo por el SCA (doc.
 * Fase 3). Es el hito visible de la fase y la punta del iceberg de la
 * arquitectura en dos escalas: pocos arboles cercanos con malla propia; la
 * masa (Fase 4) sera instancias de una libreria.
 *
 * Flujo de Generate(): SCA en MUNDO (para leer la sombra de vecinos del grid
 * grueso)  mallado  los vertices se pasan a LOCAL (base del tronco = origen
 * del actor) y se suben a un UProceduralMeshComponent con dos secciones
 * (0 = madera, 1 = follaje), cada una con su material.
 *
 * Dos formas de uso:
 *    Runtime, desde el ecosistema: UEcosystemSubsystem::SpawnHeroTree llama a
 *    Generate() con la especie, la semilla del arbol y la luz gruesa actual.
 *    Suelto en editor: coloca el actor, asigna DebugSpecies y pulsa
 *     "Regenerate" (crece sin contexto de vecinos, CoarseLight = nullptr).
 */
UCLASS()
class PROCEDURALECOSYSTEM_API AHeroTreeActor : public AActor
{
    GENERATED_BODY()

public:
    AHeroTreeActor();

    virtual void Tick(float DeltaTime) override;

    /**
     * Genera el arbol y sube la malla. WorldTrunkBase es la base del tronco en
     * mundo; el actor se coloca ahi y la malla queda relativa a el. CoarseLight
     * puede ser nullptr (demo sin ecosistema  sin sombra de vecinos).
     */
    void Generate(const USpeciesData* InSpecies, uint32 Seed,
        const FLightFieldCoarse* CoarseLight, const FVector& WorldTrunkBase);

   
    int32 GetNodeCount() const { return Skeleton.Num(); }

    /**
     * Regenera con los parametros actuales. Si nunca se llamo a Generate
     * (arbol suelto en editor), usa DebugSpecies/DebugSeed y la posicion del
     * actor, sin sombra de vecinos.
     */
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "Hero Tree")
    void Regenerate();

    // --- Uso suelto en editor (sin ecosistema) ---
    UPROPERTY(EditAnywhere, Category = "Hero Tree")
    TObjectPtr<USpeciesData> DebugSpecies;

    UPROPERTY(EditAnywhere, Category = "Hero Tree")
    int32 DebugSeed = 12345;

    // --- Materiales (si null, el PMC usa el material por defecto) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    TObjectPtr<UMaterialInterface> BarkMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    TObjectPtr<UMaterialInterface> LeafMaterial;

    // --- Toggles del SCA (el resto de la config va por defecto) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    bool bEnableSelfPruning = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree")
    bool bEnablePhototropism = true;

    /**
     * Identidad de la deformacion de tronco (ver
     * FSpaceColonizationConfig::DeformSeedOverride). -1 = derivarla de la semilla
     * del arbol, que es lo correcto para un hero suelto en editor o generado a
     * mano con Eco.GrowHeroTree.
     *
     * El gestor de LOD SI lo rellena al promocionar un arbol del bosque, con la
     * semilla de la variante de su instancia: sin eso, una instancia arqueada se
     * enderezaria al convertirse en hero delante del jugador, que es el pop mas
     * visible que puede producir este sistema.
     *
     * int64 (y no uint32) para que el centinela -1 no colisione con una semilla
     * valida; no es UPROPERTY editable porque no es un parametro de diseño sino
     * plumbing entre capas.
     */
    int64 DeformSeedOverride = -1;

    // --- Debug draw (esqueleto + atractores) ---
    /** Usa SetDrawDebug() en codigo: ademas de este flag, enciende/apaga el Tick
        del actor (que solo existe para este dibujo, ver C7 en el constructor). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree|Debug")
    bool bDrawDebug = false;

    /** Enciende/apaga el debug draw Y el Tick del actor a la vez. */
    UFUNCTION(BlueprintCallable, Category = "Hero Tree|Debug")
    void SetDrawDebug(bool bInDrawDebug);

protected:
    virtual void BeginPlay() override;

    /** Hace que el actor tambien tickee en el viewport del editor (sin darle a
        Play), para que el debug draw del esqueleto/atractores se vea al pulsar
        Regenerate. Sin esto, los AActor solo tickean en PIE. */
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    void BuildNow();
    void UploadSection(int32 SectionIndex, const FTreeMeshBuffers& Buffers, UMaterialInterface* Material);

    UPROPERTY(VisibleAnywhere, Category = "Hero Tree")
    TObjectPtr<UProceduralMeshComponent> Mesh;

    // Estado para Regenerate.
    TWeakObjectPtr<const USpeciesData> SpeciesPtr;
    /** true = el arbol se genero con contexto de luz gruesa del ecosistema.
        NO se cachea el puntero al grid (struct propiedad del subsistema, no
        reflejada): un actor cacheado que sobreviviera al subsistema lo
        deferenciaria colgante. BuildNow() lo pide FRESCO al subsistema vivo. */
    bool bUseCoarseLight = false;
    uint32 GenSeed = 0;
    FVector TrunkBaseWorld = FVector::ZeroVector;

    // Buffers de trabajo (tambien alimentan el debug draw).
    FTreeSkeleton Skeleton;
    FTreeLightGridFine FineLight;
    FAttractorCloud Attractors;
    FTreeMeshData MeshData;
};