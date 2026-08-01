#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EcosystemSettings.generated.h"

class USpeciesData;
class UMaterialInterface;
class UStaticMesh;                  // Fase 5 (capa de suelo)
class UMaterialParameterCollection; // Fase 5 (ciclo estacional) y Fase 6 (viento)

/**
 * Configuración central del proyecto. Aparece en
 * Project Settings -> Game -> "Procedural Ecosystem".
 *
 * Punto ÚNICO para semilla maestra, tiempo de simulación, relieve, especies
 * y material del heatmap. Se irá ampliando en cada fase.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Procedural Ecosystem"))
class PROCEDURALECOSYSTEM_API UEcosystemSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    virtual FName GetCategoryName() const override { return TEXT("Game"); }

    // --- Reproducibilidad ---
    UPROPERTY(EditAnywhere, config, Category = "Reproducibilidad")
    int32 MasterSeed = 12345;

    // --- Tiempo de simulación (tick desacoplado del frame) ---
    /** Segundos reales por tick en modo vivo. */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "0.01"))
    float SecondsPerSimTick = 0.5f;

    /** Años simulados que avanza cada tick. */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "0.01"))
    float YearsPerTick = 1.0f;

    /** Tope de ticks por frame (evita el "spiral of death" si baja el framerate). */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "1"))
    int32 MaxStepsPerFrame = 4;

    UPROPERTY(EditAnywhere, config, Category = "Tiempo")
    bool bStartPaused = true;

    // --- Relieve (Fase 0/1) ---
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "2"))
    int32 HeightfieldResolution = 512; // muestras por lado

    /** cm por muestra. 512 * 200 cm = ~1 km de lado. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1"))
    float HeightfieldCellSizeCm = 200.f;

    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0"))
    float HeightScaleCm = 30000.f; // amplitud vertical

    // --- Recursos: agua (Fase 1) ---
    /** Rango de salida del TWI. Debe casar con NutrientOutputMax para que el vigor
        (Monod) reciba agua y nutrientes en escalas comparables. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Agua", meta = (ClampMin = "0.001"))
    float WaterOutputMax = 10.f;

    /** Rellena depresiones (priority-flood) antes del D8. Off = ablación. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Agua")
    bool bFillWaterSinks = true;

    // --- Recursos: nutrientes (Fase 1) ---
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "0.001"))
    float NutrientOutputMax = 10.f;

    /** Frecuencia base del Perlin parcheado: más baja = parches más grandes. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "0.0"))
    double NutrientPatchFrequency = 0.00015;

    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "1"))
    int32 NutrientOctaves = 3;

    // --- Recursos: luz gruesa (Fase 1/2) ---
    // NOTA (limpieza B1): aqui vivian LightCellSizeXYCm y LightCellSizeZCm, que NADIE
    // leia -- el tamano de voxel sale de LightCoarseCellSizeCm, mas abajo. Tambien
    // estaba LightCoarseLayers, que ahora se DERIVA (ver LightCanopyHeadroomCm).
    // Si actualizas desde una version anterior, borra esas tres claves y las tres de
    // TestTreeCanopy* de Config/DefaultGame.ini: son literales huerfanos.

    /** Margen de altura (cm) por encima del ARBOL MAS ALTO que cubre la rejilla de luz.
        El nº de capas se calcula solo a partir de la MaxHeightCm mayor de las especies
        + este margen: la rejilla es relativa al terreno (ver FLightFieldCoarse), asi
        que NO hace falta cubrir el desnivel del relieve. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Luz", meta = (ClampMin = "0"))
    float LightCanopyHeadroomCm = 1500.f;

    /** Margen (cm) POR DEBAJO del terreno que cubre la rejilla de luz. Da holgura en
        pendientes fuertes, donde la copa de un vecino cuesta abajo cae por debajo de
        la cota de esta columna. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Luz", meta = (ClampMin = "0"))
    float LightGroundClearanceCm = 800.f;

    /** Especie por defecto para el heatmap de idoneidad (índice en Species). */
    UPROPERTY(EditAnywhere, config, Category = "Vigor", meta = (ClampMin = "0"))
    int32 HeatmapSpeciesIndex = 0;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0.01"))
    float LightHalfSaturationMax = 5.f;

    /** S_THRESH: vigor por debajo del cual se acumula estrés. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float StressVigorThreshold = 0.3f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressAccumulationRate = 1.f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressRecoveryRate = 0.5f;

    /** Peso del estrés acumulado en la probabilidad de morir por tick. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressMortalityWeight = 0.2f;

    /** Semillas por unidad de biomasa y año simulado (media de la Poisson). */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float SeedRatePerBiomass = 0.1f;

    /** Multiplicador de germinación: prob = VigorEnDestino * GerminationRate. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float GerminationRate = 0.5f;

    /** Luz mínima en el punto de caída para considerarlo "sitio seguro". */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float MinLightForGermination = 0.5f;

    /** Fracción de la biomasa que vuelve como pulso de nutrientes al morir. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientDecompositionFactor = 0.3f;

    // --- Ecología (Fase 2): regeneración de campos ---
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float WaterRechargeRate = 0.3f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float WaterDiffusionRate = 0.1f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientRechargeRate = 0.15f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientDiffusionRate = 0.2f;

    // --- Ecología (Fase 2): grid de luz grueso (FLightFieldCoarse) ---
    /** Lado del voxel de luz, horizontal y vertical (cm). El nº de capas NO se
        configura: se deriva de la especie mas alta + LightCanopyHeadroomCm +
        LightGroundClearanceCm, porque la rejilla es relativa al terreno. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "50"))
    float LightCoarseCellSizeCm = 400.f;

    /**
     * Cada cuantos ticks se reconstruye el grid de luz grueso (optimizacion C6).
     * 1 = cada tick (comportamiento exacto, por defecto). Las copas cambian de
     * tamano despacio, asi que subirlo a 2-4 apenas altera el resultado y ahorra
     * la pasada serial de ClearShadow + deposito. SUBELO SOLO SI EL PROFILING LO
     * PIDE (doc. 6.4: medir primero) y anota el valor en la memoria, porque
     * cambia el resultado de la simulacion (no es una optimizacion neutra).
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1", ClampMax = "16"))
    int32 LightRebuildEveryNTicks = 1;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float MinGerminationSpacingCm = 100.f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "50"))
    float SpatialHashCellSizeCm = 500.f;

    /**
     * Nº de arboles por tarea del ParallelFor del tick. El nº de chunks se
     * deriva de este valor (ceil(Poblacion / Grain)), NO del nº de hilos de la
     * maquina: eso es lo que garantiza que la reduccion de deltas sea bit a bit
     * identica en cualquier CPU (ver nota de determinismo en SimulateTick).
     * Mas pequeno = mas paralelismo pero mas coste de reduccion; 512 es un
     * punto medio razonable para poblaciones de miles-decenas de miles.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1"))
    int32 TickChunkGrainSize = 512;

    // ================================================================
    // --- Render y LOD (Fase 4): el puente de escala ---
    // ================================================================

    /** Interruptor maestro de la capa de render instanciada. Apagado = solo
        simulación + esferas de debug (útil para la ablación de la Fase 7). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bEnableTreeRendering = true;

    /** Buckets de tamaño por especie (doc. Apéndice A: p.ej. 5). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1", ClampMax = "32"))
    int32 NumAgeBuckets = 5;

    /** Histéresis del cambio de bucket, en fracción de bucket. Evita que un
        árbol parado en el borde haga add/remove de instancia cada tick. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "0.49"))
    float BucketHysteresis = 0.15f;

    /** R_hero: radio (cm) dentro del cual un árbol puede ser hero (SCA en vivo). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float HeroRadiusCm = 6000.f;   // 60 m

    /** Nº máximo de hero trees simultáneos (working set pequeño: decenas). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    int32 HeroBudget = 24;

    /** Hero trees generados por frame: amortiza los ms de SCA para no dar hitches. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 MaxHeroPerFrame = 1;

    /** R_impostor: a partir de aquí (cm) se dibuja el impostor en vez de la malla. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float ImpostorRadiusCm = 25000.f;   // 250 m

    /** Más allá de esto (cm) el árbol no se dibuja (lo cubriría el HLOD de World Partition). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float CullRadiusCm = 120000.f;      // 1.2 km

    /** Cadencia del re-nivelado completo. Los árboles se mueven despacio
        respecto a la cámara: no hace falta cada frame (doc. §4.3). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 RelevelEveryNFrames = 5;

    /** Jitter de tamaño por instancia (doc. Apéndice A: 0.9-1.1 -> 0.1). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "0.5"))
    float InstanceScaleJitter = 0.1f;

    /** Cambio mínimo de escala para molestarse en actualizar la instancia. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float ScaleUpdateThreshold = 0.02f;

    /** Arquetipos horneados por frame cuando se piden bajo demanda. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 MaxBakesPerFrame = 2;

    /** Hornear TODA la librería al arrancar (hitch inicial de ~1 s, cero después).
        Recomendado para demos y para medir framerate sin ruido. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bPrebakeLibraryOnStart = false;

    /** Las instancias cercanas proyectan sombra (VSM). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bInstancesCastShadow = true;

    /** Los impostors NO deberían proyectar sombra: que lo haga el proxy HLOD (doc. §4.6). */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bImpostorsCastShadow = false;

    /** Floats de PerInstanceCustomData.
        [0] = fase estacional por arbol (Fase 5)
        [1] = sequedad, 0 sano / 1 seco-senescente (Fase 5)
        [2] = apertura de copa para el AO por instancia (Fase 6, doc. 6.2)
        Por eso el minimo util pasa a ser 3. Bajarlo a 2 no rompe nada: el
        material se queda sin el canal de AO y lo ve como 0. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "4"))
    int32 NumInstanceCustomDataFloats = 3;

    // --- Especies ---
    UPROPERTY(EditAnywhere, config, Category = "Especies")
    TArray<TSoftObjectPtr<USpeciesData>> Species;

    // --- Debug ---
    /** Material de decal (dominio Deferred Decal) con un parámetro de textura "FieldTex". */
    UPROPERTY(EditAnywhere, config, Category = "Debug")
    TSoftObjectPtr<UMaterialInterface> HeatmapDecalMaterial;

    // ================================================================
    // --- Fase 5: ciclo de vida y dinamica ---
    // ================================================================

    // --- Paso 0/2: bosque vivo (crecimiento continuo + buffer de muertes) ---
    /** Interpola la escala de los hero trees entre ticks para que el crecimiento
        se vea CONTINUO y no a saltos. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|BosqueVivo")
    bool bSmoothHeroGrowth = true;

    /** Constante de tiempo (s) del suavizado de escala del hero (exponencial). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|BosqueVivo", meta = (ClampMin = "0.01"))
    float HeroGrowthSmoothingSeconds = 0.6f;

    /** Nº de muertes recientes que la simulacion conserva (anillo) para la capa
        de suelo (tocones/hojarasca). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|BosqueVivo", meta = (ClampMin = "0"))
    int32 DeathEventBufferSize = 256;

    // --- Paso 3: ciclo estacional de follaje ---
    /** Material Parameter Collection con un escalar "Season" [0,1). El material
        de follaje lo lee para tintar/secar la hoja segun la estacion. Si es
        null, el ciclo estacional simplemente no se aplica (no rompe nada).
        Fase 6: aqui tambien se escribe el escalar "Snow" (ver MaxSnowAmount). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Estaciones")
    TSoftObjectPtr<UMaterialParameterCollection> SeasonMPC;

    /** Avanza la estacion sola con el tiempo real (modo bosque vivo). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Estaciones")
    bool bAutoAdvanceSeason = true;
    /** La estacion sigue el AÑO SIMULADO (TickCount*YearsPerTick + alpha) en vez del
       reloj de pared. Es lo coherente en modo bosque vivo: con los defaults
       (SecondsPerSimTick=0.5, YearsPerTick=1, VisualYearSeconds=24) el reloj de
       pared da UNA primavera mientras pasan 48 años simulados, o sea que follaje y
       ecologia cuentan calendarios distintos. Con la sim PAUSADA (bake estatico) se
       cae automaticamente al reloj de VisualYearSeconds, que es lo que se quiere
       para animar la estacion en un beauty shot congelado. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Estaciones")
    bool bSeasonFollowsSimClock = true;

    /** Segundos reales que dura un ciclo estacional completo (primavera->invierno). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Estaciones", meta = (ClampMin = "0.1"))
    float VisualYearSeconds = 24.f;

    // --- Paso 4: capa de suelo (tocones/snags, madera muerta, hojarasca) ---
    /** Interruptor maestro de la capa de suelo. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo")
    bool bEnableSoilLayer = true;

    /** Malla del tocon/tronco caido. Asigna aqui p.ej. /Engine/BasicShapes/Cylinder
        (o tu propia malla). Si es null, no se generan tocones. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo")
    TSoftObjectPtr<UStaticMesh> SnagMesh;

    /** Malla de la hojarasca (una card plana). Asigna p.ej. /Engine/BasicShapes/Plane.
        Si es null, no se genera hojarasca. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo")
    TSoftObjectPtr<UStaticMesh> LitterMesh;

    /** Material de la madera muerta (opcional; si null usa el de la malla). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo")
    TSoftObjectPtr<UMaterialInterface> SnagMaterial;

    /** Material de la hojarasca (opcional; ideal: el mismo LeafMaterial otoñal). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo")
    TSoftObjectPtr<UMaterialInterface> LitterMaterial;

    /** Maximo de tocones/troncos simultaneos (anillo: al llenarse se reutiliza el mas viejo). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0"))
    int32 MaxSnags = 512;

    /** Maximo de cards de hojarasca simultaneas (anillo). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0"))
    int32 MaxLitter = 4096;

    /** Altura del tocon como fraccion de la altura del arbol al morir. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0.05", ClampMax = "1"))
    float SnagHeightFraction = 0.45f;

    /** Segundos reales que tarda un tocon en caer y quedar como tronco tumbado. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0.1"))
    float SnagFallSeconds = 4.f;
    /** Segundos que el tocon aguanta EN PIE antes de empezar a caer. El doc. 5.4 lo
        pide explicitamente ("permanencia un tiempo como snag - ecologicamente
        relevante"): un arbol muerto no se desploma en el instante en que muere. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0"))
    float SnagStandingSeconds = 6.f;

    /** Segundos que el tronco tumbado permanece como madera muerta antes de
        retirarse (doc. 5.4: "pasado un tiempo se retira el snag/tronco,
        coincidiendo con el pulso de nutrientes"). 0 = no se retira nunca.
        Para que cuadre con la mancha de descomposicion del terreno: esta decae
        con exp(-DecompositionDecayPerYear * años) y un año simulado dura
        SecondsPerSimTick/YearsPerTick segundos reales; con los defaults
        (0.5 s/año, decay 0.5) la mancha se apaga en ~4 años = ~2 s reales, asi
        que sube DecompositionDecayPerYear o baja este valor si quieres que
        desaparezcan a la vez. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0"))
    float SnagLogSeconds = 20.f;
    /** Nº de cards de hojarasca esparcidas por cada muerte. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0"))
    int32 LitterPerDeath = 6;

    /** Lado (cm) de una card de hojarasca en mundo. Antes era una constante
        escondida en el .cpp junto al tamano de /Engine/BasicShapes/Plane; ahora la
        escala se deriva de los bounds REALES de LitterMesh, asi que este valor es
        el tamano que quieres ver, sea cual sea la malla que asignes. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "1"))
    float LitterCardCm = 70.f;

    /** Altura (cm) a la que se levanta la hojarasca sobre el terreno, para evitar
        z-fighting con el material del suelo. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0"))
    float LitterGroundOffsetCm = 3.f;

    /** La capa de suelo se apaga tambien cuando se apaga la capa de arboles
        (bEnableTreeRendering / Eco.LOD.Enable 0). Es lo coherente para la ablacion
        de la Fase 7: si comparas "con y sin capa de render", los tocones y la
        hojarasca son parte de esa capa. Ponlo a false si quieres estudiarlas por
        separado. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo")
    bool bSoilFollowsTreeRendering = true;

    /** Radio (cm) de dispersion de la hojarasca alrededor de la muerte. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo", meta = (ClampMin = "0"))
    float LitterRadiusCm = 300.f;

    UPROPERTY(EditAnywhere, config, Category = "Fase5|Suelo")
    bool bSnagsCastShadow = true;

    // --- Paso 5: descomposicion visible en el terreno ---
    /** Cuanto se desvanece por año la mancha de descomposicion (decaimiento exponencial). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Descomposicion", meta = (ClampMin = "0"))
    float DecompositionDecayPerYear = 0.5f;

    /** Escala del pulso de descomposicion depositado al morir un arbol. */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Descomposicion", meta = (ClampMin = "0"))
    float DecompositionPulseScale = 1.f;

    /** Valor que el heatmap pinta como "maximo" (rango FIJO, para que las manchas
        no cambien de intensidad al variar el maximo del campo entre ticks). */
    UPROPERTY(EditAnywhere, config, Category = "Fase5|Descomposicion", meta = (ClampMin = "0.001"))
    float DecompositionPaintMax = 20.f;

    // ================================================================
    // ================================================================
    // --- FASE 6: realismo y optimizacion final ---
    // ================================================================
    // ================================================================

    // ----------------------------------------------------------------
    // 6.1 VIENTO (doc. 6.1)
    // ----------------------------------------------------------------
    // El movimiento en si lo hace el MATERIAL en el vertex shader (World
    // Position Offset). Desde C++ solo se empuja, una vez por frame y para todo
    // el bosque, el estado global del viento a un Material Parameter Collection:
    // coste O(1), cero trabajo por arbol. La variedad por rama y por arbol ya
    // viaja horneada en los canales UV de la malla (ver Geometry/TreeWindData.h).

    /** MPC del viento. Escalares que escribe el subsistema de render:
          WindStrength   fuerza total ya modulada por las rafagas
          WindGust       [0,1] valor de rafaga crudo (por si el material lo quiere aparte)
          WindTime       reloj propio del viento en segundos
          WindWpoCutoff  distancia (cm) a partir de la cual no deberia haber sway
        Vectores:
          WindDirection  (X, Y, 0, 0) unitario en el plano horizontal
        Si es null, el viento simplemente no se aplica (no rompe nada). Puedes
        asignar el MISMO asset que SeasonMPC: los nombres de parametro no chocan. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento")
    TSoftObjectPtr<UMaterialParameterCollection> WindMPC;

    /** Interruptor maestro del viento. Apagado -> WindStrength = 0 (el material
        deja de desplazar vertices y desaparece su coste de WPO). */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento")
    bool bEnableWind = true;

    /** Direccion base del viento en grados (yaw de mundo). */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento", meta = (ClampMin = "-360", ClampMax = "360"))
    float WindDirectionDeg = 45.f;

    /** Oscilacion lenta de la direccion, en grados a cada lado. Un viento de
        direccion perfectamente fija se lee como artificial enseguida. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento", meta = (ClampMin = "0", ClampMax = "90"))
    float WindDirectionWanderDeg = 12.f;

    /** Fuerza base [0..1] (el material la escala a su amplitud en cm). */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento", meta = (ClampMin = "0", ClampMax = "4"))
    float WindStrength = 0.35f;

    /** Amplitud de las rafagas como fraccion de la fuerza base. 0 = viento
        constante (se nota falso), 1 = de calma total a el doble de fuerza. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento", meta = (ClampMin = "0", ClampMax = "1"))
    float WindGustAmplitude = 0.5f;

    /** Periodo (s) de la rafaga principal. El ruido temporal se compone con un
        segundo seno de periodo inconmensurable para que no se oiga el bucle. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento", meta = (ClampMin = "0.1"))
    float WindGustPeriodSeconds = 7.f;

    /**
     * CAVEAT DE RENDIMIENTO DEL DOC. 6.1, hecho ajuste:
     * "el world-position-offset sobre Nanite masivo tiene coste [...] desactiva
     *  el WPO de viento a distancia (solo se mueven los arboles cercanos; los
     *  impostors lejanos quedan estaticos)".
     * Distancia (cm) a partir de la cual los componentes de instancing dejan de
     * evaluar el WPO. A 120 m un balanceo de 20 cm es sub-pixel: no se pierde
     * nada visible y se recorta el coste del vertex shader en la mayor parte del
     * bosque. 0 = sin corte (evaluar siempre, solo para medir la diferencia).
     */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento", meta = (ClampMin = "0"))
    float WindWpoCutoffCm = 12000.f;   // 120 m

    /** Los impostors se mueven con el viento. Por defecto NO (doc. 6.1): son el
        campo lejano y su geometria es un crossboard, el sway se veria como un
        cizallamiento raro. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento")
    bool bWindOnImpostors = false;

    // ----------------------------------------------------------------
    // 6.2 MATERIALES
    // ----------------------------------------------------------------

    /**
     * Escribe en PerInstanceCustomData[2] la APERTURA DE COPA de cada arbol
     * (luz del grid grueso a media altura de su copa: 1 = a pleno sol, 0 = bajo
     * dosel cerrado). El material la usa como termino de AO, de modo que un
     * arbol del sotobosque se ve mas apagado que uno emergente SIN necesidad de
     * GI cara (doc. 6.2: "AO por densidad de copa [...] alimentando el termino
     * de AO con el campo de luz/copa gruesa").
     *
     * Coste: un muestreo trilineal por arbol INSTANCIADO y por re-nivelado (no
     * por frame, no por impostor). Apagalo si el profiling lo senala.
     */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Materiales")
    bool bCanopyAOInstanceData = true;

    /** Nieve maxima en pleno invierno [0..1]. Se escribe como escalar "Snow" en
        SeasonMPC y el material la mezcla segun la normal hacia arriba (doc. 6.2).
        0 = sin nieve (bosque templado/laurisilva: dejalo a 0). */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Materiales", meta = (ClampMin = "0", ClampMax = "1"))
    float MaxSnowAmount = 0.f;

    // ----------------------------------------------------------------
    // 6.3 CO2 (doc. 6.3): capa de realismo barata
    // ----------------------------------------------------------------
    // Multiplicador analitico del vigor, sin sim volumetrica ni campo nuevo.
    // Ver Ecology/CarbonModel.h para la formula y su justificacion.

    /** OJO: cambia el resultado de la simulacion. Apagalo (o Eco.CO2.Enable 0)
        para reproducir exactamente las corridas anteriores a la Fase 6 y para la
        ablacion de la Fase 7. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|CO2")
    bool bEnableCO2Factor = true;

    /** Reduccion maxima del vigor bajo dosel cerrado, en fraccion. El Apendice A
        lo marca como "~1, leve": 0.10-0.20 es el rango sensato. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|CO2", meta = (ClampMin = "0", ClampMax = "0.9"))
    float CO2MaxReduction = 0.15f;

    /** Altura (cm) por encima de la cual se considera aire bien mezclado y la
        penalizacion desaparece. Ponla en la altura del dosel dominante. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|CO2", meta = (ClampMin = "1"))
    float CO2FullMixingHeightCm = 2500.f;

    // ----------------------------------------------------------------
    // 6.4 PROFILING Y PRESUPUESTO DE FRAME (doc. 6.4)
    // ----------------------------------------------------------------

    /**
     * Presupuesto de tiempo (ms) que el TICK de simulacion puede consumir dentro
     * de un frame. Al agotarlo, los ticks que falten se dejan para el frame
     * siguiente aunque no se haya llegado a MaxStepsPerFrame.
     *
     * Es la traduccion literal del doc. 6.4: "fija un objetivo (16.6 ms para
     * 60 fps) y reparte; amortiza los ticks". MaxStepsPerFrame acota el NUMERO
     * de ticks, que no es lo mismo: con 20k arboles un solo tick puede pasarse
     * de presupuesto y con 200 caben veinte. Esto acota el TIEMPO, que es lo que
     * de verdad se reparte. 0 = sin limite (comportamiento anterior).
     */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Profiling", meta = (ClampMin = "0"))
    float TickBudgetMsPerFrame = 4.f;

    /** Objetivo de frame (ms) contra el que se compara en Eco.Frame. 16.6 = 60 fps. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Profiling", meta = (ClampMin = "1"))
    float FrameBudgetMs = 16.6f;

    /** Muestra en pantalla el reparto del frame y la poblacion (equivalente a
        Eco.Frame.HUD 1). Util para grabar video de la demo. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Profiling")
    bool bShowFrameBudgetHUD = false;

    static const UEcosystemSettings* Get() { return GetDefault<UEcosystemSettings>(); }
};
