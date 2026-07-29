#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Ecology/TreeDeathEvent.h"
#include "TreeSoilSubsystem.generated.h"

class UEcosystemSubsystem;
class UStaticMesh;
class UMaterialInterface;
class UHierarchicalInstancedStaticMeshComponent;

/**
 * Linea temporal de la muerte visible (doc. 5.4): el arbol muere -> queda un TOCON
 * EN PIE un tiempo (fase ecologicamente relevante, no se desploma al instante) ->
 * CAE -> se queda como TRONCO/madera muerta -> se RETIRA, en paralelo al pulso de
 * nutrientes y a la mancha de descomposicion del terreno.
 */
enum class ESnagPhase : uint8
{
    Standing, // en pie, aun no ha empezado a caer
    Falling,  // volcando sobre su base
    Log,      // tronco tumbado (madera muerta)
    Gone      // retirado: la instancia sigue existiendo pero con escala ~0 y su
              // ranura es la primera candidata a reutilizarse
};

/**
 * Un tocon/tronco: su instancia en el ISM de madera + su fase y el tiempo que
 * lleva en ella. No se refleja (son datos calientes de la capa de render).
 */
struct FSoilSnag
{
    int32   InstanceIndex = -1;
    FVector Base = FVector::ZeroVector; // base del tronco, en el suelo (cm)
    float   HeightCm = 100.f;
    float   RadiusCm = 10.f;
    float   Yaw = 0.f;   // orientacion y direccion de caida (grados)
    float   FallT = 0.f; // 0 = en pie, 1 = tumbado
    float   PhaseSeconds = 0.f;                    // tiempo acumulado en la fase actual
    ESnagPhase Phase = ESnagPhase::Standing;
};

/**
 * CAPA DE SUELO (doc. Fase 5): hace visible la muerte y "vende el realismo del
 * conjunto". Consume los eventos de muerte de la simulacion (NO la toca: es una
 * vista, doc. 0 "Propiedad de los datos") y genera, por cada arbol muerto:
 *   - un TOCON/snag que cae poco a poco y queda como TRONCO (madera muerta),
 *   - unas cards de HOJARASCA esparcidas a su alrededor.
 *
 * Acotado por diseno: snags y hojarasca viven en ANILLOS (MaxSnags / MaxLitter);
 * al llenarse se REUTILIZA la instancia mas vieja (UpdateInstanceTransform), de
 * modo que NUNCA se borran instancias -> se evita el baile de indices de
 * RemoveInstances (el riesgo Alto del Apendice B) y el coste esta acotado.
 *
 * Las plantulas ("plantulas" del doc.) ya salen de la capa de render normal: son
 * los arboles en estado Sapling, que la libreria dibuja en los buckets pequenos.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UTreeSoilSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // --- UWorldSubsystem ---
    virtual void Deinitialize() override;
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

    // --- FTickableGameObject ---
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    // --- Control (consola) ---
    void Clear();
    void LogStats() const;

private:
    bool EnsureInitialized();
    UHierarchicalInstancedStaticMeshComponent* CreateISM(UStaticMesh* Mesh, UMaterialInterface* Mat,
        bool bCastShadow, const TCHAR* Name);

    void SpawnSnag(const FTreeDeathEvent& Death);
    void SpawnLitterAround(const FVector& Base, float SpreadCm, int32 Count, uint32& RngState);
    /** Avanza la linea temporal Standing -> Falling -> Log -> Gone de cada tocon. */
    void UpdateSnags(float DeltaTime);
    FTransform SnagTransform(const FSoilSnag& Snag) const;

    UPROPERTY(Transient) TObjectPtr<AActor> Host = nullptr;
    UPROPERTY(Transient) TObjectPtr<UEcosystemSubsystem> Eco = nullptr;
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> WoodISM = nullptr;
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> LitterISM = nullptr;

    TArray<FSoilSnag> Snags;
    int32 SnagCursor = 0;   // anillo de tocones
    int32 LitterCount = 0;  // cuantas cards de hojarasca hay ya (hasta MaxLitter)
    int32 LitterCursor = 0; // anillo de hojarasca

    int64 DeathCursor = 0;  // ultimo evento de muerte consumido de la simulacion
    bool  bInitialized = false;
};
