/**
 * @file TreeRenderSubsystem.h
 * @author Juan Luque Roldán
 * @brief Gestor de niveles de representación: reparte la población entre hero, instancia,
 *        impostor y fuera de rango, y publica los relojes globales de estación y viento.
 *
 * Es el puente de escala entre la simulación y la pantalla: una vista de solo lectura sobre la
 * población que elige, por distancia a cámara, con qué se dibuja cada árbol, y traduce su
 * tamaño continuo al arquetipo horneado que le corresponde. Su regla de diseño es no tocar
 * nunca un componente instanciado instancia a instancia ni cada frame: altas, bajas,
 * transformaciones y datos por instancia se acumulan por componente y se aplican en lote. El
 * re-nivelado completo corre cada N frames y los trabajos caros —horneado de mallas y
 * generación de hero trees— se drenan con presupuesto fijo por frame. Lo único que corre
 * siempre son los relojes de estación y de viento: dos escrituras de Material Parameter
 * Collection para todo el bosque.
 *
 * @ingroup eco_render
 * @see @ref bib_clarkjh1976
 * @see @ref bib_funkhouser1993
 * @see @ref bib_instancing
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Render/TreeArchetype.h"
#include "TreeRenderSubsystem.generated.h"

class AHeroTreeActor;
class ATreeInstanceHost;
class UEcosystemSubsystem;
class USpeciesData;
class UTreeLibrary;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialParameterCollection; // colecciones de parámetros de estación y de viento

/** Nivel de representación con el que se dibuja un árbol, elegido por distancia a cámara. */
UENUM()
enum class ETreeRenderTier : uint8
{
    None,      ///< Fuera de rango: no se dibuja, pero sigue simulándose.
    Hero,      ///< Geometría propia generada en vivo por colonización del espacio: decenas.
    Instance,  ///< Malla horneada de librería vía ISM/HISM: miles.
    Impostor   ///< Dos tarjetas cruzadas para el campo lejano.
};

/**
 * Estado de render de un árbol, indexado por su identificador estable.
 *
 * Vive solo en la capa de presentación: la población es la única fuente de verdad y el render
 * es una vista sobre ella. Guarda con qué está dibujado el árbol ahora mismo y los centinelas
 * que evitan reescribir lo que no ha cambiado.
 */
struct FTreeRenderState
{
    /** Nivel de representación con el que está dibujado. */
    ETreeRenderTier Tier = ETreeRenderTier::None;

    /** Arquetipo dibujado (FArchetypeKey::Pack): comparado con el recalculado, delata un
        cambio de bucket o de variante. */
    uint32 PackedKey = 0;

    /** Índice dentro del componente instanciado, o -1 si no hay instancia. Lo asigna
        FlushInstanceOps y lo reescribe el remapeo posterior a cada baja. */
    int32  InstanceIndex = -1;

    /** Último bucket de tamaño: es la entrada de la histéresis de
        TreeArchetype::BucketWithHysteresis, que evita oscilar en la frontera. */
    int32  Bucket = -1;

    /** Última escala dentro del bucket subida al componente; umbral de re-escalado. */
    float  LastScale = 0.f;

    /** Sello del último re-nivelado en que se vio vivo: lo que no lo lleva, se libera. */
    uint32 Stamp = 0;

    /** Última sequedad cuantizada escrita en PerInstanceCustomData[1]; 255 = nunca escrita. */
    uint8  LastVitalityQ = 255;

    /** Última apertura de copa cuantizada escrita en PerInstanceCustomData[2]; 255 = nunca
        escrita. Ambos valores se cuantizan a 16 bandas porque la banda es el umbral de
        reescritura: sin ella cambiarían unas milésimas en cada re-nivelado y habría que tocar
        los datos por instancia de todas las instancias cada vez. */
    uint8  LastCanopyQ = 255;

    /** El árbol espera su malla en HeroQueue y sigue dibujado con su representación anterior
        (instancia o impostor). El cambio de nivel se consuma en ProcessHeroQueue, cuando el
        actor ya tiene geometría. */
    bool   bHeroPending = false;

    /**
     * true cuando PackedKey describe de verdad la representación actual.
     *
     * Hace falta un booleano aparte porque la clave 0 no sirve de centinela: especie 0,
     * bucket 0 y variante 0 es una clave perfectamente válida, la de la plántula más común
     * del bosque. Un arquetipo aún no horneado deja el estado sin representar, y así el
     * siguiente re-nivelado lo ve como cambio y lo reintenta.
     */
    bool   bHasRepresentation = false;
};

/**
 * Petición de hero tree encolada: lo que ProcessHeroQueue necesita para materializar el actor.
 *
 * La cola amortigua el coste de generar geometría única, que se mide en milisegundos de game
 * thread y produciría un hitch si varios árboles ascendieran a hero en el mismo frame.
 */
struct FPendingHero
{
    /** Arquetipo con el que se genera la geometría. */
    FArchetypeKey Key;

    /** Posición de mundo del árbol. */
    FVector  Position = FVector::ZeroVector;

    /** Escala final aplicada al actor: escala dentro del bucket por el jitter estable. */
    float    Scale = 1.f;

    /** Escala dentro del bucket, sin jitter: es la que compara el umbral de re-escalado. */
    float    ScaleInBucket = 1.f;
};

/**
 * Ranura de la caché de actores hero.
 *
 * Salir del nivel hero oculta el actor en vez de destruirlo, de modo que volver a entrar es
 * inmediato mientras el arquetipo no cambie. EvictOldHeroes destruye las ranuras inactivas más
 * antiguas cuando la caché crece por encima de su límite.
 */
USTRUCT()
struct FHeroSlot
{
    GENERATED_BODY()

    /** Actor generado; se conserva oculto cuando el árbol sale del nivel hero. */
    UPROPERTY(Transient) TObjectPtr<AHeroTreeActor> Actor = nullptr;

    /** Arquetipo con el que se generó la geometría: si cambia, hay que regenerarla. */
    uint32 GeneratedKey = 0;

    /** Sello del último re-nivelado que la usó; ordena el desalojo por antigüedad de uso. */
    uint32 LastUsedStamp = 0;

    /** true mientras el árbol ocupa el nivel hero. */
    bool   bActive = false;

    /** Escala objetivo hacia la que UpdateHeroInterpolation acerca la escala del actor cada
        frame, para que el crecimiento se vea continuo y no a saltos entre re-nivelados. */
    float  TargetScale = 1.f;
};

/**
 * Gestor de niveles de representación: el puente de escala del simulador.
 *
 * Es la pieza que hace que decenas de miles de árboles simulados existan en pantalla a
 * framerate interactivo. Reparte la población por distancia a cámara —hero, instancia de
 * librería, impostor y fuera de rango— y, más allá del radio de corte, el HLOD de World
 * Partition se ocupa de las celdas lejanas. Las reglas que sostienen su coste:
 *
 * @li La simulación no se toca: este subsistema solo lee la población.
 * @li Nunca se añade ni se quita instancia a instancia dentro de un bucle: los cambios se
 *     acumulan y se aplican con AddInstances/RemoveInstances una vez por componente y flush.
 * @li Un árbol cambia de nivel o de bucket rara vez; lo frecuente es que solo crezca, y eso
 *     viaja como actualización de transform en lote y con umbral.
 * @li Los hero trees se generan con presupuesto por frame, se cachean, y el ascenso a hero es
 *     diferido: la representación anterior no se suelta hasta que la geometría está lista.
 * @li El re-nivelado completo, que incluye la selección de hero, corre cada
 *     RelevelEveryNFrames frames, porque los árboles se mueven despacio respecto a la cámara.
 *     Cada frame corren solo la interpolación de escala de los hero, la cola de generación y
 *     los relojes de estación y de viento.
 *
 * Es además el único punto donde se publican los parámetros globales de material —estación,
 * nieve, dirección y fuerza del viento— a sus Material Parameter Collections: dos escrituras
 * por frame para todo el bosque. El movimiento lo resuelve el vertex shader, y su coste se
 * acota por distancia en los propios componentes, que configura
 * UTreeLibrary::ApplyWindSettings.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UTreeRenderSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // --- UWorldSubsystem ---

    virtual void Deinitialize() override;

    /** @return true solo en mundos de juego y de PIE: no hay capa de render en el editor. */
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

    // --- FTickableGameObject ---

    /** Avanza los relojes de material, drena las colas amortizadas y, según la cadencia
        configurada, lanza el re-nivelado completo. @see UpdateLOD */
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    // --- Control (consola) ---

    /** Activa o desactiva la capa instanciada. Al desactivarla se sueltan las instancias y los
        hero trees; la simulación sigue corriendo intacta, lo que permite comparar capturas con
        y sin capa de presentación. */
    void SetEnabled(bool bInEnabled);

    bool IsEnabled() const { return bEnabled; }

    /** Congela el re-nivelado: cada árbol conserva su nivel aunque la cámara se mueva. */
    void SetFrozen(bool bInFrozen) { bFrozen = bInFrozen; }

    /** Suelta todo el estado de render y fuerza un reparto de niveles desde cero. Conserva la
        librería: las mallas ya horneadas son lo caro de reconstruir. */
    void RebuildAll();

    /** Hornea de golpe la librería de arquetipos completa, para no pagar picos de horneado
        durante una demostración o una captura. */
    void BakeLibraryNow();

    /** Vuelca al log el reparto por nivel, el estado de la librería y el coste del último
        re-nivelado. */
    void LogStats() const;

    /** Reaplica a los componentes ya creados los ajustes de viento de la configuración del
        proyecto (WPO activo o no, distancia de corte, margen de bounds), sin reconstruir la
        capa de render. */
    void ApplyWindSettings();

    /** Vuelca al log el estado del viento: dirección, fuerza, ráfaga y reloj. */
    void LogWindState() const;

    /** Reparto de árboles por nivel en la última pasada, para el HUD y el CSV de frame. */
    void GetTierCounts(int32& OutHero, int32& OutInstance, int32& OutImpostor, int32& OutCulled) const
    {
        OutHero = NumHero; OutInstance = NumInstance; OutImpostor = NumImpostor; OutCulled = NumCulled;
    }

    /** @return Coste en milisegundos del último re-nivelado completo. */
    double GetLastRelevelMs() const { return LastRelevelMs; }

private:
    /** Reacciona a la carga de un bosque horneado: la población entera cambia, así que el
        estado de render y los mapeos instancia-árbol de la corrida anterior ya no valen. */
    void HandleStateLoaded();

    /** Inicialización perezosa: monta host, librería y colecciones de material la primera vez
        que el ecosistema declara que el mundo está listo.
        @return true cuando la capa ya puede trabajar.
        @note El orden de arranque entre subsistemas de un mismo mundo no está garantizado, de
              ahí que la inicialización se intente en cada tick en vez de al empezar el juego. */
    bool EnsureInitialized();

    /** Libera todo lo creado por EnsureInitialized: actores, componentes, mapas y librería. */
    void ReleaseEverything();

    /** Pasada de re-nivelado: selecciona los hero más cercanos, asigna a cada árbol vivo su
        nivel y su arquetipo, encola los cambios y libera los estados de los árboles que ya no
        aparecen en la población. */
    void UpdateLOD(const FVector& ViewLocation);

    /** Encola el alta de un árbol en el nivel pedido, con su transformación y sus datos por
        instancia iniciales.
        @param Want Nivel al que pasa el árbol.
        @param ScaleInBucket Escala sin jitter, la que guarda el estado como umbral.
        @param Dryness Sequedad del follaje [0,1].
        @param CanopyAO Apertura de copa [0,1], 1 a pleno sol.
        @pre Want no puede ser Hero: ese ascenso lo consuma CommitHeroTier cuando el actor ya
             tiene geometría.
        @note Si el arquetipo todavía no está horneado, el árbol se queda sin representar y el
              siguiente re-nivelado lo reintenta. */
    void EnterTier(uint32 StableId, FTreeRenderState& State, ETreeRenderTier Want,
        const FArchetypeKey& Key, const FTransform& Xform, float ScaleInBucket,
        float Dryness, float CanopyAO);

    /** Encola la baja de la representación actual y devuelve el estado a "no representado",
        incluidos los centinelas de cuantización: la próxima instancia será otra y nacerá con
        sus datos por instancia a cero. */
    void LeaveTier(uint32 StableId, FTreeRenderState& State);

    /** Aplica los cambios acumulados en Pending. Por componente y en este orden: bajas y
        remapeo del bookkeeping, altas, actualizaciones de transform agrupadas en tiradas
        contiguas, datos por instancia y una única invalidación del render state. */
    void FlushInstanceOps();

    /** Genera o reactiva hasta MaxThisFrame hero trees de la cola y consuma su cambio de
        nivel. Vacía las bajas que ese cambio deja pendientes sin esperar al próximo
        re-nivelado, para no dibujar el hero y su instancia superpuestos. */
    void ProcessHeroQueue(int32 MaxThisFrame);

    /** Cierra el ascenso a hero: suelta la representación anterior y pasa el estado a Hero. */
    void CommitHeroTier(uint32 StableId, FTreeRenderState& State, const FPendingHero& Info);

    /** Saca al árbol del nivel hero ocultando su actor, que queda cacheado en su ranura. */
    void ReleaseHero(uint32 StableId);

    /** Destruye las ranuras inactivas más antiguas hasta devolver la caché a su límite. */
    void EvictOldHeroes();

    /** Destruye los actores hero cacheados y vacía HeroActors, HeroQueue y HeroInfo. Copia
        única del bucle que comparten RebuildAll y ReleaseEverything: los tres contenedores
        tienen que vaciarse juntos, o quedan entradas apuntando a actores ya destruidos. */
    void DestroyAllHeroActors();

    /** Acerca cada frame la escala de los hero trees a su FHeroSlot::TargetScale con un
        suavizado exponencial, de modo que el crecimiento se vea continuo entre re-nivelados.
        Barato porque son decenas de actores; las instancias masivas nunca se tocan por frame. */
    void UpdateHeroInterpolation(float DeltaTime);

    /** Avanza la fase de estación global y la empuja, junto al escalar de nieve derivado de
        ella, a la colección de parámetros que leen todos los materiales de árbol. */
    void UpdateSeason(float DeltaTime);

    /** Avanza el reloj propio del viento, compone las ráfagas y empuja dirección y fuerza a la
        colección de parámetros del viento. Coste constante por frame para todo el bosque. */
    void UpdateWind(float DeltaTime);

    /** Dibuja un punto sobre cada árbol con el color de su nivel de representación. */
    void DrawTierDebug(const FVector& ViewLocation) const;

    /** Posición desde la que se mide la distancia: la cámara del jugador o, si no hay
        controlador, el punto de vista que el render dibujó el último frame.
        @return false si no hay ninguno de los dos, y entonces el re-nivelado se pospone. */
    bool GetViewLocation(FVector& OutLocation) const;

    /** Clave de componente: el arquetipo más el bit de impostor, porque cada arquetipo tiene
        un componente de malla y otro de impostor. */
    static FORCEINLINE uint64 MakeComponentKey(uint32 PackedKey, bool bImpostor)
    {
        return (static_cast<uint64>(PackedKey) << 1) | (bImpostor ? 1ull : 0ull);
    }

    /** Cuantiza un valor [0,1] a 16 bandas: es el umbral de reescritura de los datos por
        instancia. */
    static FORCEINLINE uint8 Quantize16(float Value01)
    {
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value01 * 15.f), 0, 15));
    }

    /** Cambios acumulados de un componente, a la espera de aplicarse en lote. */
    struct FPendingComponentOps
    {
        /** Índices de instancia dados de baja. */
        TArray<int32>      Removes;

        /** Identificadores estables de las altas, en paralelo a AddXforms y AddCustom. */
        TArray<uint32>     AddIds;

        /** Transformaciones de las altas. */
        TArray<FTransform> AddXforms;

        /** Actualizaciones de transform como (identificador estable, transformación): el
            índice de instancia no se conoce hasta después de aplicar las bajas. */
        TArray<TPair<uint32, FTransform>> Updates;

        /**
         * Datos por instancia iniciales de cada alta, empaquetados en un par:
         * X es la sequedad del follaje (0 sano, 1 seco o senescente) e Y la apertura de copa
         * (1 a pleno sol, 0 bajo dosel cerrado).
         *
         * Va en paralelo a AddIds y AddXforms y es imprescindible: una instancia recién creada
         * nace con todos sus datos por instancia a cero, así que sin esto un árbol senescente
         * que acaba de cambiar de bucket se dibujaría verde y sin oclusión hasta que alguno de
         * los dos valores cambiase de banda, cosa que puede no ocurrir nunca.
         */
        TArray<FVector2f>                   AddCustom;

        /** Reescrituras de datos por instancia: (identificador estable, (sequedad, apertura)). */
        TArray<TPair<uint32, FVector2f>>    CustomUpdates;
    };

    /** Librería de arquetipos horneados y dueña de los componentes instanciados. */
    UPROPERTY(Transient) TObjectPtr<UTreeLibrary> Library = nullptr;

    /** Actor que aloja los componentes instanciados de la capa de árboles. */
    UPROPERTY(Transient) TObjectPtr<ATreeInstanceHost> Host = nullptr;

    /** Subsistema de simulación, consultado siempre en solo lectura. */
    UPROPERTY(Transient) TObjectPtr<UEcosystemSubsystem> Eco = nullptr;

    /** Caché de actores hero, indexada por identificador estable. */
    UPROPERTY(Transient) TMap<uint32, FHeroSlot> HeroActors;

    /** Identificador estable -> estado de render. La clave es el identificador y no el índice
        de población porque los índices se desplazan cuando FTreePopulation::CompactDead
        compacta los muertos. */
    TMap<uint32, FTreeRenderState> States;

    /** Cambios acumulados por componente, con la clave que compone MakeComponentKey. */
    TMap<uint64, FPendingComponentOps> Pending;

    /** Buffers de trabajo de FlushInstanceOps, miembros para no reservar memoria en cada
        flush. ResolvedUpdates lleva (índice de instancia, transformación) ya resuelto y
        ordenado; BatchXforms es la tirada contigua que se pasa a
        BatchUpdateInstancesTransforms. */
    TArray<TPair<int32, FTransform>> ResolvedUpdates;
    TArray<FTransform> BatchXforms;

    /** Cola de ascensos a hero pendientes, consumida por la cabeza con presupuesto por frame. */
    TArray<uint32> HeroQueue;

    /** Datos de generación de cada entrada de HeroQueue. */
    TMap<uint32, FPendingHero> HeroInfo;

    /** Identificadores que el último re-nivelado eligió como hero. */
    TSet<uint32> HeroSet;

    /**
     * Los HeroBudget árboles más cercanos, como pares (distancia al cuadrado, índice de
     * población) en orden ascendente.
     *
     * Se mantiene por selección parcial y no ordenando la lista completa de candidatos: en
     * bosque denso caben cientos de árboles dentro del radio de hero y solo interesan las
     * primeras decenas. El caso común se descarta en O(1), cuando el candidato está más lejos
     * que el peor de los que ya están dentro.
     */
    TArray<TPair<double, int32>> HeroBest;

    /** Distancia al cuadrado a cámara de cada árbol, cacheada en la pasada de selección de
        hero para que la del reparto de niveles no la recalcule. En double porque con un radio
        de corte de 1,2 km la distancia al cuadrado ronda 1,4e10 y desborda la mantisa de 24
        bits de un float; además las coordenadas de mundo del motor ya son double. */
    TArray<double> DistSqCache;

    /** Reloj lógico de pasadas de re-nivelado: sella los estados vistos y ordena el desalojo
        de la caché de hero. */
    uint32 VisitStamp = 0;

    /** Frames transcurridos desde el último re-nivelado; se compara con RelevelEveryNFrames. */
    int32  FramesSinceRelevel = 0;

    /** Capa instanciada activa. */
    bool   bEnabled = true;

    /** Re-nivelado congelado: cada árbol conserva el nivel que tiene. */
    bool   bFrozen = false;

    /** Fuerza un re-nivelado en el próximo tick, sin esperar a la cadencia. */
    bool   bForceRelevel = false;

    /** La inicialización perezosa ya se completó. */
    bool   bInitialized = false;

    // Reparto de la última pasada, publicado como stats y en el CSV de frame.
    int32 NumHero = 0;
    int32 NumInstance = 0;
    int32 NumImpostor = 0;
    int32 NumCulled = 0;

    /** Coste en milisegundos del último re-nivelado completo. */
    double LastRelevelMs = 0.0;

    /** Fase de estación [0,1) empujada a la colección de material cada frame. */
    float SeasonPhase = 0.f;

    /** Colección de parámetros de estación, resuelta una sola vez en EnsureInitialized: cargar
        el asset dentro de UpdateSeason lo haría en cada frame. */
    UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> SeasonMPCCached = nullptr;

    // --- Viento ---

    /** Colección de parámetros del viento, resuelta una vez igual que la de estación. */
    UPROPERTY(Transient) TObjectPtr<UMaterialParameterCollection> WindMPCCached = nullptr;

    /** Reloj propio del viento, en segundos. No usa el reloj de la simulación a propósito: con
        la simulación pausada el bosque tiene que seguir moviéndose, y con la simulación
        acelerada el viento no debe acelerarse. */
    float WindTime = 0.f;

    // Últimos valores publicados en la colección de material; los lee LogWindState.
    float WindStrengthNow = 0.f;
    float WindGustNow = 0.f;
    float WindDirDegNow = 0.f;
};
