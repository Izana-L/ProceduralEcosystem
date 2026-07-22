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
 * grueso) -> mallado -> los vertices se pasan a LOCAL (base del tronco = origen
 * del actor) y se suben a un UProceduralMeshComponent con dos secciones
 * (0 = madera, 1 = follaje), cada una con su material.
 *
 * Dos formas de uso:
 *   - Runtime, desde el ecosistema: UEcosystemSubsystem::SpawnHeroTree llama a
 *     Generate() con la especie, la semilla del arbol y la luz gruesa actual.
 *   - Suelto en editor: coloca el actor, asigna DebugSpecies y pulsa
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
     * puede ser nullptr (demo sin ecosistema -> sin sombra de vecinos).
     */
    void Generate(const USpeciesData* InSpecies, uint32 Seed,
        const FLightFieldCoarse* CoarseLight, const FVector& WorldTrunkBase);

    /** Nº de nodos del ultimo esqueleto generado (para logs/tests). */
    int32 GetNodeCount() const { return Skeleton.Num(); }

    /**
     * Re-genera con los parametros actuales. Si nunca se llamo a Generate
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

    // --- Debug draw (esqueleto + atractores) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hero Tree|Debug")
    bool bDrawDebug = false;

protected:
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
    const FLightFieldCoarse* CoarseLightPtr = nullptr; // struct del subsistema (no reflejado)
    uint32 GenSeed = 0;
    FVector TrunkBaseWorld = FVector::ZeroVector;

    // Buffers de trabajo (tambien alimentan el debug draw).
    FTreeSkeleton Skeleton;
    FTreeLightGridFine FineLight;
    FAttractorCloud Attractors;
    FTreeMeshData MeshData;
};