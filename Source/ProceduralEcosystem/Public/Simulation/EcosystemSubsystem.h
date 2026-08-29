/**
 * @file EcosystemSubsystem.h
 * @author Juan Luque Roldán
 * @brief Motor del ecosistema: reloj del bosque, etapas del tick y API de solo lectura
 *        sobre el estado de la simulación.
 *
 * Declara el subsistema de mundo que avanza el tiempo ecológico y custodia la única fuente
 * de verdad del proyecto: la población de árboles en SoA y los pools de agua y nutrientes.
 * Modela la demografía individuo a individuo —vigor, crecimiento, estrés, senescencia,
 * mortalidad, dispersión, germinación y claros— acoplada a campos de recursos que los
 * propios árboles agotan y sombrean. El tiempo ecológico (años por tick) va desacoplado del
 * frame: un acumulador de paso fijo dispara los ticks, un presupuesto en milisegundos corta
 * el bucle cuando se agota y el alfa de interpolación da la fracción de tick ya consumida.
 * Hacia fuera el estado se expone solo como lectura, junto a la instrumentación del tick.
 *
 * @ingroup eco_simulation
 * @see @ref bib_gapmodels
 * @see @ref bib_fiedler2004
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/EcoCore.h"
#include "Terrain/HeightField.h"
#include "Terrain/WaterField.h"
#include "Terrain/NutrientField.h"
#include "Terrain/LightFieldCoarse.h"
#include "Ecology/TreePopulation.h"
#include "Ecology/SpatialHash.h"
#include "Ecology/ResourcePool.h"
#include "Ecology/TickScratch.h"
#include "Ecology/TreeDeathEvent.h"
#include "Ecology/CarbonModel.h"   // multiplicador analítico de CO2 del vigor
#include "Ecology/Vigor.h"         // EcoVigor::FSpeciesResponses (caché por especie y tick)
#include "EcosystemSubsystem.generated.h"

class UFieldVisualizer;
class ADecalActor;
class UMaterialInstanceDynamic;
class USpeciesData;
class AHeroTreeActor;
class UEcosystemSettings;
class FArchive;

/**
 * Se emite cuando LoadState sustituye la población entera por la de un bake.
 *
 * Las capas que mantienen estado indexado por StableId (instancias y hero trees cacheados
 * en el render; tocones y hojarasca en el suelo) deben tirarlo al recibirlo: los StableId
 * de un bake vienen de otra corrida y reutilizarlos coloca representaciones de árboles
 * que ya no existen.
 */
DECLARE_MULTICAST_DELEGATE(FOnEcoStateLoaded);

/**
 * Desglose del coste de un tick por etapas, en milisegundos (consola: Eco.Profile).
 *
 * Responde a la pregunta que hay que contestar antes de optimizar nada: dónde se va el
 * tiempo. Siete marcas de reloj por tick delimitan las seis etapas del bucle y cada una se
 * suaviza con una media exponencial, barata y estable, que no acumula histórico. Las mismas
 * etapas se publican además como contadores del motor y como ámbitos de Unreal Insights,
 * para poder leerlas junto al resto del frame.
 *
 * @note GerminationMs cubre desde el final de la regeneración hasta el final del tick:
 *       pulsos de muerte, germinación, perturbación, compactación de muertos e
 *       intercambio de buffers.
 * @see EcoStats.h
 */
struct FEcoTickProfile
{
    double HashMs = 0.0;        ///< Reconstrucción del spatial hash sobre el snapshot de lectura.
    double LightMs = 0.0;       ///< Limpieza de la rejilla de luz y depósito de las copas.
    double ParallelMs = 0.0;    ///< Paso paralelo: crecimiento, estrés, mortalidad y semillas.
    double ReduceMs = 0.0;      ///< Reducción serial de los scratch por tarea.
    double RegenMs = 0.0;       ///< Recarga hacia el campo base y difusión de los pools.
    double GerminationMs = 0.0; ///< Pulsos de muerte, germinación, perturbación y cierre del tick.
    double TotalMs = 0.0;       ///< Tick completo.

    /** Media exponencial: la muestra pesa Alpha y el histórico 1-Alpha. */
    static void Accumulate(double& InOutAvg, double SampleMs, double Alpha = 0.1)
    {
        InOutAvg = (InOutAvg <= 0.0) ? SampleMs : (InOutAvg * (1.0 - Alpha) + SampleMs * Alpha);
    }
};

/**
 * Una fila del histórico demográfico: el estado de UNA especie en UN tick
 * (consola: Eco.Demografia.CSV).
 *
 * Se muestrea por especie y no por árbol porque lo que hace falta graficar para
 * diagnosticar una exclusión competitiva es la trayectoria de cada especie. El recuento
 * dice que una especie se está yendo; el vigor medio, el limitante y los flujos del tick
 * dicen por qué: si recluta poco, si se le mueren las plántulas o si no llega a madurez.
 * El reparto por estados distingue además un bosque maduro de un vivero, que es justo lo
 * que el recuento no ve.
 *
 * La muestra ocupa unas decenas de bytes y se toma cada 20 ticks, así que el histórico de
 * una corrida larga con tres especies se queda en el orden de decenas de kilobytes: puede
 * dejarse activo siempre.
 */
struct FEcoDemoSample
{
    int64 Tick = 0;              ///< Tick de simulación en que se tomó la muestra.
    int32 SpeciesIndex = 0;      ///< Índice de la especie en la lista resuelta.
    int32 Count = 0;             ///< Árboles vivos de esta especie.
    float MeanBiomass = 0.f;     ///< Biomasa media en fracción de la máxima de la especie.
    float MeanVigor = 0.f;       ///< Vigor medio de la especie, en [0,1].
    float MeanStress = 0.f;      ///< Estrés medio acumulado, en [0,1].
    int32 LimitedByLight = 0;    ///< Individuos cuyo argmin de Liebig es la luz.
    int32 LimitedByWater = 0;    ///< Individuos limitados por el agua.
    int32 LimitedByNutrient = 0; ///< Individuos limitados por los nutrientes.

    // --- Flujos del tick ---
    FEcoSpeciesFlow Flow;        ///< Embudo de reclutamiento y muertes por canal de este tick.

    // --- Estructura de tamaños ---
    int32 Saplings = 0;          ///< Individuos en estado Sapling.
    int32 Suppressed = 0;        ///< Individuos suprimidos: el banco de plántulas.
    int32 Senescent = 0;         ///< Individuos senescentes.
};

/**
 * Motor de la simulación del ecosistema.
 *
 * Monta el mundo al arrancar siguiendo el orden de dependencias que va del relieve a la
 * población, avanza el bosque un paso discreto por tick y expone el resultado como solo
 * lectura a las capas de presentación, suelo y diagnóstico. El estado se mantiene en doble
 * buffer: el paso paralelo lee siempre un snapshot inmutable y escribe en el buffer de
 * escritura, y los productos que no se pueden aplicar en el acto convergen en una reducción
 * serial de orden fijo. Con eso, y con streams de RNG por subsistema y por árbol, dos
 * corridas de la misma semilla dan el mismo fingerprint con y sin paralelismo. Concentra
 * también los comandos de consola, la instrumentación experimental y el bake del bosque.
 *
 * @note El orden de las etapas del tick es parte del contrato de determinismo y no es
 *       reordenable.
 * @see SimulateTick
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UEcosystemSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    // --- Ciclo de vida del subsistema de mundo ---
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    /** Monta relieve, campos base, pools, especies, rejilla de luz y hash, en ese orden. */
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;
    /** Limita el subsistema a los mundos de juego y de PIE: no corre en el del editor. */
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

    // --- Tick del motor ---
    /** Reparte los ticks pendientes dentro del presupuesto de milisegundos del frame. */
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    // --- Control (consola) ---
    void SetPaused(bool bInPaused) { bPaused = bInPaused; }
    bool IsPaused() const { return bPaused; }
    /** Encola N ticks manuales, que se ejecutan aunque la simulación esté en pausa. */
    void StepN(int32 N) { PendingSteps += FMath::Max(1, N); }
    int64 GetTickCount() const { return TickCount; }

    /**
     * Fracción del tick en curso ya consumida, en [0,1].
     *
     * Es el puente entre el tick discreto y el frame continuo: el render la usa para
     * interpolar entre dos estados, por ejemplo para que el reloj estacional avance de
     * forma suave en vez de a saltos de un tick.
     */
    float GetInterpolationAlpha() const;

    /** Años simulados que avanza cada tick. Lo necesita el reloj estacional para contar
        el mismo año que la ecología. */
    float GetYearsPerTick() const { return YearsPerTick; }

    // --- Terreno y luz ---
    const FHeightField& GetHeightField() const { return HeightField; }

    /**
     * Rejilla de luz gruesa del bosque, con el área foliar acumulada por encima de cada vóxel.
     *
     * La lee el hero tree para sembrar su rejilla fina con la sombra de los vecinos —la
     * conexión de la escala macro con la micro— y el gestor de niveles de representación
     * para el AO de copa por instancia. Se rellena durante el tick, así que refleja el
     * último tick corrido.
     */
    const FLightFieldCoarse& GetLightCoarse() const { return LightCoarse; }

    // --- Agentes de depuración: sondas manuales, ajenas a la simulación ---
    void AddDebugAgent(const FVector& WorldPos, const FColor& Color, float Radius);
    /** Coloca una sonda en un punto aleatorio del terreno, gastando el stream de RNG de
        depuración para no perturbar la reproducibilidad del bosque. */
    void AddRandomDebugAgent();
    void ClearDebugAgents();

    /** CRC32 encadenado de la población y de los pools: el test operativo del
        determinismo, con el que se comparan dos corridas de la misma semilla. */
    void LogStateFingerprint() const;

    /** Busca el primer valor no finito en población, pools y rejilla de luz. */
    void LogFiniteCheck() const;

    /**
     * Estructura demográfica del bosque por especie (consola: Eco.Demografia): reparto por
     * estado, edades, qué fracción de la población ya está crecida y muertes acumuladas.
     *
     * Es el instrumento con el que se calibra la longevidad, porque contrasta la mediana
     * de vida nominal del canal de edad con la mediana realizada que se deduce del hazard
     * medido en los flujos del tick. LogPopulationStats dice cuántos árboles hay, que es
     * justo lo que no distingue un bosque maduro de un vivero.
     *
     * @note Solo lee: no toca el estado de la simulación ni consume RNG, así que llamarlo
     *       no altera el fingerprint ni la evolución de la partida.
     */
    void LogDemographics() const;

    /**
     * Vuelca los rasgos de cada especie resuelta más los derivados que no aparecen en el
     * asset (consola: Eco.Especies.Volcado).
     *
     * Calcula lo que no se ve en ninguna parte: anchura absoluta de cada campana de nicho,
     * factor de luz a pleno sol y en penumbra, lluvia de semillas de un adulto crecido con
     * la saturación aplicada, radio radicular efectivo a varias fracciones de biomasa y
     * edad de senescencia frente a la mediana esperada. Sin esto la calibración es a
     * ciegas, porque los rasgos viven en un binario y ningún log los imprime.
     */
    void LogSpeciesDump() const;

    /**
     * Para cada celda del terreno, qué especie tendría más vigor si estuviera allí, y qué
     * fracción del mapa gana cada una (consola: Eco.Nicho.Mapa). Pinta el argmax como heatmap.
     *
     * Es la prueba directa de si el modelo puede sostener coexistencia, y se obtiene sin
     * simular un solo tick: si una especie es la mejor en casi todo el mapa, la exclusión
     * competitiva ya está escrita en la forma de las curvas.
     *
     * @note Hornea la idoneidad con las mismas respuestas, modo de combinación y parámetros
     *       de CO2 que usa el tick, para que el mapa represente la función que de verdad
     *       hace crecer al bosque.
     */
    void LogNicheWinnerMap();

    /**
     * Perfil vertical de luz de una columna (consola: Eco.Luz.Perfil X Y): área foliar
     * acumulada y luz disponible por capa, más lo que devuelve el muestreador a ras de
     * suelo, a media altura y en el ápice.
     *
     * Confirma o refuta la pregunta de la que depende todo el modelo de sucesión: si
     * existe de verdad un gradiente de luz entre el dosel y el suelo.
     *
     * @param Xcm Coordenada X de la columna, en cm de mundo.
     * @param Ycm Coordenada Y de la columna, en cm de mundo.
     */
    void LogLightProfile(double Xcm, double Ycm) const;

    /**
     * Vuelca el histórico demográfico acumulado a un CSV (consola: Eco.Demografia.CSV).
     *
     * El histórico se toma durante el tick, así que el fichero refleja la partida entera y
     * no el instante en que se pide. Una columna por métrica y una fila por tick y especie:
     * entra tal cual en una hoja de cálculo para sacar las curvas de población y de vigor.
     *
     * @param FullPath Ruta absoluta del fichero de destino.
     */
    void SaveDemographyCsv(const FString& FullPath) const;

    /** Vacía el histórico, para separar corridas sin reiniciar el editor. */
    void ClearDemographyHistory() { DemoHistory.Reset(); }

    /** Imprime el desglose del coste del tick por etapas y la memoria de las estructuras
        que vigila la optimización (consola: Eco.Profile). */
    void LogTickProfile() const;

    /** Perfil del tick acumulado. Lo lee el perfilador de frame para expresar la
        simulación como fracción del frame. */
    const FEcoTickProfile& GetTickProfile() const { return Profile; }

    /**
     * Parámetros del multiplicador de CO2, ya resueltos contra los ajustes y la consola.
     *
     * Es el único punto donde se resuelven, y de él los leen las tres cosas que evalúan
     * vigor: el tick, la germinación y el heatmap de idoneidad. Si cada una los montara por
     * su cuenta, el mapa dejaría de representar la función que hace crecer al bosque en
     * cuanto alguien tocase un valor.
     *
     * @see EcoCarbon
     */
    EcoCarbon::FCO2Params GetCO2Params() const;

    // --- Eventos de muerte: los consume la capa de suelo ---
    /** Imprime el total de muertes y las últimas conservadas en el anillo. */
    void  LogRecentDeaths() const;

    /** Muertes registradas desde el inicio de la partida; sirve de cursor global. */
    int64 GetDeathEventCounter() const { return DeathEventCounter; }

    /**
     * Copia en Out las muertes posteriores al cursor del consumidor y lo avanza.
     *
     * El anillo solo conserva las últimas muertes, así que la lectura arranca como pronto
     * en la ranura más antigua todavía viva: nunca se devuelve una ranura sin escribir, y
     * un consumidor que se retrasa pierde eventos en silencio en vez de leer basura.
     *
     * @param InOutCursor Cursor monótono del consumidor; a la vuelta queda al día.
     * @param Out Destino al que se añaden los eventos nuevos, en orden cronológico.
     */
    void  CollectNewDeathEvents(int64& InOutCursor, TArray<FTreeDeathEvent>& Out) const;

    /**
     * Percentiles de los campos de agua y nutrientes, en absoluto y en fracción del máximo
     * de salida (consola: Eco.PercentilesCampos).
     *
     * Es lo que permite colocar los óptimos de nicho de las especies sin adivinar. El TWI
     * del agua sale muy sesgado hacia valores bajos —la mayor parte del mapa está seca y
     * solo unos pocos fondos de barranco llegan arriba—, así que fijar el óptimo de una
     * especie en una fracción elegida a ojo puede dejarla sin un solo sitio donde ganar y
     * extinguirla por una razón ajena a la competencia. Los óptimos van en los percentiles
     * 25 / 50 / 75 que imprime este comando.
     *
     * @note La anchura sugerida es min(p50-p25, p75-p50) y no la semidistancia
     *       intercuartílica, porque con un campo sesgado la fórmula simétrica solapa entre
     *       sí a las especies del extremo seco.
     */
    void LogFieldPercentiles() const;

    /**
     * Compara las propiedades de configuración del proyecto con las claves realmente
     * escritas en el .ini (consola: Eco.Config.Auditar).
     *
     * Avisa de las propiedades ausentes del .ini, que corren con el valor por defecto de
     * C++, y de las claves huérfanas que ya no mapean a ninguna propiedad. Un .ini que solo
     * fija parte de los parámetros deja de ser el registro reproducible de la corrida: un
     * cambio de valor por defecto altera la ecología sin que nada visible cambie, y el
     * resultado son calibraciones híbridas entre dos versiones del modelo.
     *
     * @note Lee el .ini como texto y no a través de la caché de configuración, cuyo
     *       fusionado con los .ini del motor enmascara justo lo que se quiere detectar.
     */
    static void LogConfigCoverage();

    // --- Heatmaps proyectados sobre el terreno ---
    void PaintTestField();
    void PaintWaterField();
    void PaintNutrientField();
    void PaintVigorField();
    void PaintLightField();

    /** Pinta el campo de descomposición: los puntos de muerte recientes.
        @param bLogResult A false para el repintado automático del modo continuo, que si no
               llenaría el log a varias líneas por segundo. */
    void PaintDecompositionField(bool bLogResult = true);

    // --- Población ---
    /**
     * Siembra Count plántulas en puntos aleatorios del terreno (consola: Eco.SeedForest).
     *
     * Las edades se escalonan en vez de arrancar todas a cero: una cohorte única
     * envejecería y moriría en bloque, abriendo un claro simultáneo en todo el mapa. La
     * biomasa inicial acompaña a la edad, y el tope del escalonado, 0,35 de la longevidad,
     * coincide con la fracción en que entra la senescencia por defecto; como el sorteo es
     * semiabierto, ningún fundador arranca ya senescente.
     */
    void SeedInitialPopulation(int32 Count);

    /** Árboles vivos ahora mismo, para HUD, consola y pruebas. */
    int32 GetLivePopulationCount() const { return Agents_Read.Num(); }

    // --- Bake del bosque: guardar y cargar el estado ---
    /** Escribe una instantánea del bosque: población, pools, campo de descomposición,
        streams de RNG y contador de ticks. Los campos base no se guardan porque se
        regeneran idénticos a partir de la semilla maestra. */
    void SaveState(const FString& FilePath);

    /**
     * Carga una instantánea y sustituye el estado vivo por ella.
     *
     * Deserializa sobre un objeto aparte y valida antes de pisar nada —tamaños de los
     * arrays contra lo que queda de fichero, geometría de los campos, coherencia del SoA e
     * índices de especie—, de modo que un fichero corrupto o de otra resolución de relieve
     * se rechaza entero en vez de dejar la simulación a medio cargar. Al terminar rehace la
     * luz gruesa, limpia los hero trees, emite OnStateLoaded y deja la simulación en pausa.
     *
     * @return true si el fichero era válido y el estado se sustituyó.
     */
    bool LoadState(const FString& FilePath);

    /** Notifica a las capas de vista que la población se ha sustituido de golpe. */
    FOnEcoStateLoaded OnStateLoaded;

    // --- Hero trees: geometría completa generada en vivo ---
    /**
     * Genera un hero tree y devuelve su actor, o nullptr si el mundo aún no está listo.
     *
     * Le pasa la luz gruesa actual como contexto de sombra de vecinos, que es lo que hace
     * que la copa crezca hacia el hueco que le dejan los árboles de alrededor.
     *
     * @param WorldPos Base del tronco, en cm de mundo.
     * @param SpeciesIndex Índice en la lista de especies resueltas; se recorta al rango.
     * @param Seed Semilla del generador de geometría.
     */
    AHeroTreeActor* SpawnHeroTree(const FVector& WorldPos, int32 SpeciesIndex, uint32 Seed);

    /** Destruye todos los hero trees generados. */
    void ClearHeroTrees();

    // --- Acceso de solo lectura para las capas de presentación ---
    // La población es la única fuente de verdad del estado: el gestor de niveles de
    // representación elige con qué se dibuja cada árbol, pero no escribe en la simulación.
    const FTreePopulation& GetPopulation() const { return Agents_Read; }
    const TArray<TObjectPtr<USpeciesData>>& GetSpeciesList() const { return ResolvedSpecies; }
    const USpeciesData* GetSpeciesById(uint16 SpeciesId) const { return ResolveSpecies(SpeciesId); }

    /** true cuando el relieve y los campos ya están listos; es lo que gatea el render. */
    bool IsWorldReady() const { return bWorldReady; }

private:
    /**
     * Avanza el bosque DtYears: orquesta las etapas del tick en su orden fijo.
     *
     * El orden es hash espacial, rejilla de luz, preparación del doble buffer, paso
     * paralelo, reducción serial, regeneración de los pools, pulsos de muerte, germinación,
     * perturbación y, por último, compactación de muertos e intercambio de buffers. Ese
     * orden es parte del contrato de determinismo y no es reordenable: ninguna lectura del
     * tick puede ver escrituras del propio tick.
     */
    void SimulateTick(float DtYears);

    // --- Etapas del tick ---
    // Cada etapa vive en un método con nombre para que el tick quede como orquestador
    // legible y cada una se pueda razonar y probar por separado.

    /**
     * Reparte la población en chunks y deja el scratch por tarea listo para escribir.
     *
     * @return Número de chunks, derivado solo de la población y del grano configurado y
     *         nunca del número de hilos: es lo que hace que la reducción de deltas sea
     *         idéntica en cualquier CPU.
     */
    int32 PrepareTickScratch(const UEcosystemSettings& Settings);

    /**
     * Paso paralelo del tick: vigor, crecimiento, estrés, declive, consumo de recursos,
     * mortalidad y emisión de semillas, un chunk por tarea.
     *
     * Cada tarea lee solo del snapshot inmutable y escribe únicamente en su tramo del
     * buffer de escritura y en su propio scratch, así que no hacen falta ni bloqueos ni
     * atómicas; los deltas de recursos convergen después en una reducción serial de orden
     * fijo, porque la suma en coma flotante no es asociativa.
     *
     * @see EcologyRules::ReduceScratchInto
     */
    void RunGrowthParallel(float DtYears, const UEcosystemSettings& Settings,
        const EcoCarbon::FCO2Params& CO2, int32 NumChunks);

    /** Envejece el campo de descomposición y aplica los pulsos de muerte del tick: devuelve
        nutrientes al suelo, registra el evento para la capa de suelo y deja la mancha
        visible. Serial. */
    void ApplyDeathPulses(float DtYears, const UEcosystemSettings& Settings);

    /**
     * Germinación serial de las semillas pendientes.
     *
     * Aplica los cuatro filtros del embudo de reclutamiento —dentro del mapa, espaciado
     * mínimo frente a los vecinos y a las nacidas en este mismo tick, sitio seguro por luz
     * a ras de suelo e inhibición por conespecíficos— y sortea el arraigo con el vigor
     * local. Es serial y de orden fijo, de modo que el resultado es reproducible.
     */
    void RunGermination(float DtYears, const UEcosystemSettings& Settings,
        const EcoCarbon::FCO2Params& CO2);

    /**
     * Régimen de perturbación: abre claros matando a los árboles de un disco.
     *
     * Sin claros, cada árbol muere por su cuenta y el hueco que deja es del tamaño de un
     * árbol: no existe el episodio de alta luz en el que una especie pionera es la mejor,
     * o sea que falta la dimensión temporal del nicho. El número de claros por tick sale de
     * convertir una tasa de área en un número de eventos, y el área de cada claro se
     * muestrea de una ley potencia: muchos claros pequeños y una cola larga de eventos raros.
     *
     * @note Consume un stream de RNG propio, para que activar la perturbación no desplace
     *       los streams del resto de la simulación y las corridas sigan siendo comparables.
     * @see @ref bib_dinamicadeclaros
     * @see @ref bib_leypotenciaclaros
     */
    void RunDisturbance(float DtYears, const UEcosystemSettings& Settings);

    /** Rehace las curvas de respuesta, una por especie, al principio del tick. */
    void RefreshSpeciesResponses(const UEcosystemSettings& Settings);

    /** Campos con los que se hornea cualquier mapa de idoneidad: el pool si la simulación
        ya ha corrido y el potencial del terreno si no. El pool es lo que los árboles leen
        de verdad, y puede estar bastante por debajo del base: horneando sobre el base, el
        mapa declara un reparto de nicho que en el recurso disponible ya no existe. */
    const FField2D& SuitabilityWaterField() const;
    const FField2D& SuitabilityNutrientField() const;

    /**
     * Rehace la rejilla de luz gruesa con la población actual: la limpia, deposita el área
     * foliar de cada copa viva y acumula la extinción por columna.
     *
     * La llama el tick y también LoadState, porque la luz es estado derivado que no se
     * serializa: sin este refresco un bake cargado conservaría la luz del bosque anterior
     * y, como la carga deja la simulación en pausa, nadie la refrescaría, de modo que un
     * hero tree generado ahí crecería contra la sombra de vecinos que ya no existen.
     *
     * @see FLightFieldCoarse
     */
    void RebuildCoarseLight();
    void DrawDebug();
    void EnsureHeatmapDecal();

    /**
     * Camino único de todos los heatmaps: sube el buffer al visualizador, garantiza el
     * decal proyectado y deja constancia en el log.
     *
     * @param LogLabel Nombre del campo tal y como aparece en el log.
     * @param bAutoRange A false fija la escala en [MinValue, MaxValue] en vez de tomar el
     *        mínimo y el máximo del buffer, que harían latir el heatmap entre ticks.
     * @param bLogResult A false silencia el log, para los repintados automáticos.
     */
    void PaintField(const TArray<float>& Values, const TCHAR* LogLabel,
        bool bAutoRange = true, float MinValue = 0.f, float MaxValue = 1.f,
        bool bLogResult = true);

    /**
     * Punto aleatorio sobre el terreno: XY uniforme dentro de los límites y Z muestreada
     * del relieve.
     *
     * @param Stream Stream de RNG a consumir; la siembra inicial usa el de colonización y
     *        las sondas de depuración el suyo, para que depurar no altere el bosque.
     * @warning Consume exactamente dos valores del stream, primero X y luego Y. El orden
     *          forma parte del contrato de reproducibilidad de una semilla dada.
     */
    FVector RandomPointOnTerrain(EEcoRngStream Stream);
    void LogPopulationStats() const;

    /** Añade al histórico una fila por especie. La llama el tick con la misma cadencia que
        LogPopulationStats: solo lee la población y no consume RNG, así que no altera el
        fingerprint de la partida. */
    void RecordDemographySample();

    /** Escribe una muerte en el anillo circular y avanza el contador global. */
    void RecordDeathEvent(const FPendingDeathPulse& Pulse);

    /** Serializa o deserializa un bake completo sobre Payload. Trabajar sobre un objeto
        aparte, y no sobre los miembros vivos, es lo que permite validar el contenido antes
        de pisar el estado de la simulación. */
    void SerializeState(FArchive& Ar, struct FEcoBakePayload& Payload);

    /** Especie de un identificador, o nullptr si el índice está fuera de la lista resuelta. */
    const USpeciesData* ResolveSpecies(uint16 SpeciesId) const;

    // --- Reloj de la simulación ---
    double Accumulator = 0.0;   ///< Tiempo de reloj pendiente de convertir en ticks, en s.
    int64  TickCount = 0;       ///< Ticks corridos desde el inicio de la partida.
    int32  PendingSteps = 0;    ///< Ticks manuales encolados, que corren aunque haya pausa.
    bool   bPaused = true;      ///< Detiene el avance automático, no los ticks manuales.

    /** Ticks ejecutados en el frame actual; lo publican el stat y el HUD. */
    int32  TicksLastFrame = 0;

    /** Se pone a true al final de OnWorldBeginPlay. Gatea el Tick para que la simulación
        no arranque antes de que el relieve y los campos estén listos. */
    bool   bWorldReady = false;

    float  SecondsPerTick = 0.5f; ///< Cadencia de reloj de un tick, en segundos.
    float  YearsPerTick = 1.f;    ///< Tiempo ecológico que avanza un tick, en años.
    int32  MaxStepsPerFrame = 4;  ///< Tope de ticks por frame; acota también la deuda acumulada.

    /** Generador determinista con un stream por subsistema, derivados de la semilla maestra. */
    FEcosystemRng Rng;

    // --- Relieve ---
    FHeightField HeightField;

    // --- Campos base: potencial del terreno, calculados una sola vez al arrancar ---
    FWaterField WaterBase;
    FNutrientField NutrientBase;
    FLightFieldCoarse LightCoarse; ///< El único de los tres que se rehace entero cada tick.

    /** Campo de descomposición reciente: recibe una mancha al morir un árbol y decae con el
        tiempo. Es solo visualización y no entra en el vigor, y se actualiza en serie, así
        que le basta un buffer. */
    FField2D DecompositionField;
    int64    LastDecompPaintTick = -1; ///< Último tick repintado en el modo de repintado continuo.

    // --- Estado runtime: disponibilidad actual de cada recurso, en doble buffer ---
    FResourcePool WaterPool;
    FResourcePool NutrientPool;

    // --- Población y aceleración espacial ---
    FTreePopulation Agents_Read;  ///< Snapshot inmutable: de aquí sale toda lectura del tick.
    FTreePopulation Agents_Write; ///< Destino de las escrituras del tick; al final se intercambian.
    FSpatialHash Hash;            ///< Rejilla de vecindad reconstruida cada tick sobre Agents_Read.

    /** Scratch privado de cada tarea del paso paralelo. Es persistente: se reutiliza tick
        tras tick en vez de reasignarse, y guarda listas dispersas de celda y cantidad en
        lugar de un campo denso por tarea.
        @see FTickScratch */
    TArray<FTickScratch> TickContexts;

    /** Posiciones germinadas en el tick en curso. Hacen falta aparte porque el hash indexa
        el snapshot de lectura y no ve a las nacidas en este mismo tick; es miembro para
        conservar la capacidad entre ticks. */
    TArray<FVector> NewbornPositions;

    /** Salidas de la reducción serial. Son miembros y no variables locales para que la
        capacidad sobreviva al tick y una oleada de germinación no vuelva a pedir memoria. */
    TArray<FPendingSeed> PendingSeeds;
    TArray<FPendingDeathPulse> PendingDeaths;

    /** Embudo de reclutamiento y muertes de este tick, indexado por identificador de especie. */
    TArray<FEcoSpeciesFlow> SpeciesFlow;

    /**
     * Curvas de respuesta resueltas, una por especie y por tick.
     *
     * Se construyen fuera del paso paralelo porque son idénticas para todos los individuos
     * de una especie: rehacerlas por árbol repetiría el trabajo decenas de miles de veces y
     * además obligaría a leer el asset de especie desde dentro del bucle paralelo.
     */
    TArray<EcoVigor::FSpeciesResponses> SpeciesResponses;

    /** Tick en que cada especie llegó a cero vivos, o -1 si no ha ocurrido. Sin este
        registro una extinción solo se detecta por ausencia y su momento exacto, que es el
        dato más informativo del experimento, se pierde. */
    TArray<int64> SpeciesExtinctionTick;

    /** Coste del último tick por etapas. @see FEcoTickProfile */
    FEcoTickProfile Profile;

    /** Especies ya cargadas: evita resolver el asset miles de veces por tick. */
    UPROPERTY(Transient)
    TArray<TObjectPtr<USpeciesData>> ResolvedSpecies;

    /** Histórico demográfico volcable a CSV. Instrumentación pura: no interviene en la
        simulación. */
    TArray<FEcoDemoSample> DemoHistory;

    // --- Eventos de muerte: anillo circular de capacidad fija + cursor monótono ---
    /** Anillo predimensionado al arrancar el mundo: Num() es la capacidad, no cuántas
        muertes hay. No se serializa en el bake, porque es un buffer de eventos para la
        vista y no estado del bosque. */
    TArray<FTreeDeathEvent> RecentDeaths;
    int64 DeathEventCounter = 0; ///< Muertes totales; su módulo con la capacidad da la ranura.

    // --- Sondas de depuración ---
    UPROPERTY(Transient)
    TArray<FEcoDebugAgent> DebugAgents;

    // --- Hero trees generados ---
    UPROPERTY(Transient)
    TArray<TObjectPtr<AHeroTreeActor>> HeroTrees;

    // --- Heatmap: textura del campo y decal que la proyecta sobre el terreno ---
    UPROPERTY(Transient)
    TObjectPtr<UFieldVisualizer> FieldViz = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<ADecalActor> HeatmapDecal = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> HeatmapMID = nullptr;
};
