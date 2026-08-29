/**
 * @file TreeSoilSubsystem.h
 * @author Juan Luque Roldán
 * @brief Capa de suelo: convierte los eventos de muerte en tocones que caen y en hojarasca.
 *
 * Declara la línea temporal de la muerte visible (ESnagPhase), el registro de un tocón
 * (FSoilSnag) y el subsistema que los mantiene. Es una vista de solo lectura sobre la
 * simulación: consume sus eventos de muerte con un cursor monótono y los materializa en dos
 * componentes de instancing, uno de madera y otro de hojarasca. Ambos se gestionan como
 * anillos acotados (MaxSnags / MaxLitter) en los que nunca se borra una instancia: lo
 * retirado se reduce a escala ~0 y su ranura se reutiliza, así que los índices de instancia
 * no se desplazan nunca y el coste queda acotado por configuración.
 *
 * @ingroup eco_render
 * @see @ref bib_harmon1986
 * @see @ref bib_instancing
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Ecology/TreeDeathEvent.h"
#include "TreeSoilSubsystem.generated.h"

class UEcosystemSubsystem;
class UEcosystemSettings;
class UStaticMesh;
class UMaterialInterface;
class UHierarchicalInstancedStaticMeshComponent;

/**
 * Línea temporal de la muerte visible, en orden.
 *
 * Traduce la mortalidad arbórea como proceso con duración y no como desaparición instantánea:
 * el árbol muerto queda en pie un tiempo, vuelca, permanece tumbado como madera muerta y
 * acaba retirado. Las cuatro fases se miden en tiempo real de render porque son animación, no
 * ecología: el pulso de nutrientes lo aplica la simulación en el tick de la muerte.
 *
 * @see @ref bib_harmon1986
 */
enum class ESnagPhase : uint8
{
    Standing, ///< En pie: aún no ha empezado a caer.
    Falling,  ///< Volcando sobre su base.
    Log,      ///< Tronco tumbado (madera muerta).
    Gone      ///< Retirado: la instancia sigue existiendo con escala ~0 y su ranura es la
              ///< primera candidata a reutilizarse.
};

/**
 * Un tocón o tronco: su instancia en el componente de madera, la fase en que está y el tiempo
 * que lleva en ella.
 *
 * No se refleja a Blueprint: son datos calientes de la capa de render, recorridos enteros en
 * cada tick para avanzar la línea temporal.
 */
struct FSoilSnag
{
    int32   InstanceIndex = -1;         ///< Instancia en el HISM de madera; -1 si aún no existe.
    FVector Base = FVector::ZeroVector; ///< Base del tronco, apoyada en el suelo, en cm de mundo.
    float   HeightCm = 100.f;           ///< Altura del tocón en cm.
    float   RadiusCm = 10.f;            ///< Radio del tronco en cm; su altura una vez tumbado.
    float   Yaw = 0.f;                  ///< Orientación y dirección de caída, en grados.
    float   FallT = 0.f;                ///< Progreso de la caída: 0 en pie, 1 tumbado.
    float   PhaseSeconds = 0.f;         ///< Tiempo real acumulado en la fase actual, en segundos.
    ESnagPhase Phase = ESnagPhase::Standing; ///< Fase actual de la línea temporal.
};

/**
 * Capa de suelo: la vista que hace visible la muerte del bosque.
 *
 * Consume los eventos de muerte de la simulación sin escribir nunca en ella y genera, por
 * cada árbol muerto, un tocón que cae y queda como madera muerta más unas tarjetas de hojarasca
 * esparcidas a su alrededor. Mantiene para ello dos componentes de instancing, uno de madera
 * y otro de hojarasca, colgados de un ATreeInstanceHost propio.
 *
 * Su coste está acotado por dos reglas. La primera: tocones y hojarasca viven en anillos
 * (MaxSnags / MaxLitter) y al llenarse se reutiliza la instancia más vieja con
 * UpdateInstanceTransform, sin borrar nunca ninguna, de modo que los índices de instancia no
 * se desplazan. La segunda: las altas se acumulan durante el tick y se aplican con una sola
 * llamada a AddInstances por componente y una sola invalidación, nunca instancia a instancia.
 *
 * Los árboles vivos, plántulas incluidas, los dibuja la capa de render; aquí solo se
 * representa la muerte.
 *
 * @see UTreeRenderSubsystem
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

    // --- Control desde consola ---

    /**
     * Vacía tocones y hojarasca y resincroniza el cursor de muertes con el contador de la
     * simulación, para que lo ya ocurrido no vuelva a materializarse.
     *
     * @note Se ejecuta también al cargar una instantánea del bosque: los restos de la corrida
     *       anterior no corresponden a ningún árbol del estado recién cargado.
     */
    void Clear();

    /** Vuelca en el log el reparto de tocones por fase, la hojarasca y el cursor de muertes. */
    void LogStats() const;

private:
    /**
     * Crea el host y los componentes la primera vez que el ecosistema está listo.
     *
     * @return true si la capa quedó operativa; false si aún no hay mundo o ecosistema.
     * @note El orden de arranque entre subsistemas no está garantizado, de ahí la
     *       inicialización perezosa desde Tick en vez de desde Initialize.
     */
    bool EnsureInitialized();

    /** Crea y registra un componente de instancing del host con la malla y el material dados. */
    UHierarchicalInstancedStaticMeshComponent* CreateISM(UStaticMesh* Mesh, UMaterialInterface* Mat,
        bool bCastShadow, const TCHAR* Name);

    /** Reserva la ranura del tocón de una muerte y encola su alta; no toca aún el componente. */
    void QueueSnag(const FTreeDeathEvent& Death, const UEcosystemSettings& S);

    /**
     * Encola las tarjetas de hojarasca de una muerte, esparcidas en disco alrededor de la base.
     *
     * @param Base     Base del tronco del árbol muerto, en cm de mundo.
     * @param RngState Estado del generador, sembrado por muerte: la misma muerte reparte
     *                 siempre la misma hojarasca. Avanza con cada tarjeta.
     */
    void QueueLitterAround(const FVector& Base, const UEcosystemSettings& S, uint32& RngState);

    /** Aplica en lote las altas del tick: una AddInstances y una invalidación por componente. */
    void FlushSpawns();

    /** Avanza la línea temporal Standing -> Falling -> Log -> Gone de cada tocón. */
    void UpdateSnags(float DeltaTime);

    /** Transformación de instancia de un tocón según su fase y su progreso de caída. */
    FTransform SnagTransform(const FSoilSnag& Snag) const;

    UPROPERTY(Transient) TObjectPtr<AActor> Host = nullptr;             ///< Actor de los ISM.
    UPROPERTY(Transient) TObjectPtr<UEcosystemSubsystem> Eco = nullptr; ///< Origen de las muertes.

    // Un componente de instancing por tipo de resto: madera (tocones y troncos) y hojarasca.
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> WoodISM = nullptr;
    UPROPERTY(Transient) TObjectPtr<UHierarchicalInstancedStaticMeshComponent> LitterISM = nullptr;

    // --- Dimensiones reales de las mallas, leídas de sus bounds al inicializar ---
    // La escala de instancia se deriva de ellas y no de constantes, de modo que sustituir la
    // malla de tocón o de hojarasca por una propia sigue dando restos del tamaño configurado.
    float SnagMeshHeightCm = 100.f; ///< Altura de la malla de tocón, en cm.
    float SnagMeshRadiusCm = 50.f;  ///< Radio de la malla de tocón, en cm.
    float LitterMeshSizeCm = 100.f; ///< Lado de la malla de hojarasca, en cm.

    TArray<FSoilSnag> Snags; ///< Anillo de tocones; nunca crece por encima de MaxSnags.
    int32 SnagCursor = 0;   ///< Posición del anillo de tocones: siguiente candidato a reutilizarse.
    int32 LitterCount = 0;  ///< Tarjetas de hojarasca ya creadas, hasta MaxLitter.
    int32 LitterCursor = 0; ///< Posición del anillo de hojarasca.

    // --- Altas acumuladas durante el tick, para aplicarlas en lote ---
    TArray<FTransform> PendingSnagAdds;   ///< Transformaciones de los tocones aún sin instancia.
    TArray<int32>      PendingSnagSlots;  ///< Ranura de Snags que recibe cada índice nuevo.
    TArray<FTransform> PendingLitterAdds; ///< Transformaciones de la hojarasca sin instancia.
    bool bWoodDirty = false;   ///< Hay cambios de madera pendientes de una única invalidación.
    bool bLitterDirty = false; ///< Ídem para la hojarasca.

    TArray<FTreeDeathEvent> NewDeaths; ///< Buffer reutilizado por CollectNewDeathEvents.

    int64 DeathCursor = 0;  ///< Último evento de muerte consumido de la simulación.
    bool  bInitialized = false;
};
