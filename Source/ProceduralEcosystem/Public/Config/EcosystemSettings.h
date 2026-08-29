#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EcosystemSettings.generated.h"

class USpeciesData;
class UMaterialInterface;
class UStaticMesh;                  // Fase 5 (capa de suelo)
class UMaterialParameterCollection; // Fase 5 (ciclo estacional) y Fase 6 (viento)

/**
 * Como se combinan los tres factores de recurso (luz, agua, nutrientes) en el
 * vigor. Vive aqui, y no en Ecology/Vigor.h, porque tiene que ser un UENUM para
 * poder configurarse; Vigor.h incluye esta cabecera solo por eso.
 */
UENUM(BlueprintType)
enum class EEcoVigorCombine : uint8
{
    /**
     * Ley del minimo de Liebig: vigor = min(fL, fW, fN). El recurso mas escaso
     * manda y los otros dos NO pesan nada.
     *
     * Es la ley clasica y la que el proyecto usaba en exclusiva, pero tiene una
     * consecuencia que conviene entender antes de elegirla: la derivada respecto a
     * los ejes NO limitantes es exactamente cero. Si el factor de luz esta topado
     * por debajo del pico de las campanas de nicho, el agua y los nutrientes dejan
     * de intervenir en el resultado por completo, y con ellos desaparece el unico
     * mecanismo por el que dos especies pueden repartirse el mapa. Dentro de esa
     * meseta, estar EN el optimo hidrico no vale mas que estar a media anchura de
     * el: el paisaje se vuelve plano.
     */
    Minimum,

    /**
     * Media geometrica: vigor = (fL * fW * fN)^(1/3). Los tres ejes pesan SIEMPRE,
     * asi que una desventaja de luz de factor r se puede compensar con una ventaja
     * de agua x nutrientes del mismo factor, y cada especie vuelve a tener sitios
     * donde gana.
     *
     * Conserva la ESCALA del vigor -tres factores de 0.6 dan 0.6, no 0.216-, que es
     * lo que permite cambiar de modo sin recalibrar StressVigorThreshold ni los
     * GrowthRate de los assets.
     */
    GeometricMean,

    /**
     * Hibrido: min() entre los factores de SUMINISTRO y multiplicacion aparte de
     * las respuestas de nicho. Es la forma defendible conceptualmente -y la de los
     * modelos de hueco clasicos (JABOWA, FORET, SORTIE)-, porque una campana de
     * nicho no es una disponibilidad de recurso sino una curva de TOLERANCIA, y
     * las tolerancias se multiplican.
     *
     * Con bUseNicheResponse=false se comporta exactamente como Minimum (no hay
     * respuestas de tolerancia que separar).
     */
    MinSupplyTimesNiche
};

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

    // --- Relieve (Fase 0/1; sintesis realista: ver FHeightField::Generate) ---
    // NOTA (relieve realista): aqui vivia HeightfieldBaseFrequency (cm^-1), que
    // producia formas de ~17 m con 300 m de desnivel (agujas) y octavas por
    // debajo del limite de Nyquist de la rejilla (pinchos por vertice). La
    // sustituye TerrainBaseWavelengthM, en metros. Si actualizas desde una
    // version anterior y tu DefaultGame.ini guarda HeightfieldBaseFrequency,
    // borra esa clave: es un literal huerfano.
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "2"))
    int32 HeightfieldResolution = 512; // muestras por lado

    /** cm por muestra. 512 * 200 cm = ~1 km de lado. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1"))
    float HeightfieldCellSizeCm = 200.f;

    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0"))
    float HeightScaleCm = 30000.f; // amplitud vertical (pico-valle)

    /** Octavas del fractal: mas octavas = mas detalle fino. El generador ademas
        recorta las que caen bajo el limite de Nyquist de la rejilla (longitud
        de onda < 2 * HeightfieldCellSizeCm), que solo meterian aliasing. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1", ClampMax = "12"))
    int32 HeightfieldOctaves = 8;

    /** Longitud de onda (m) de la octava base: el ancho de las formas GRANDES
        del relieve. Para un mapa de ~1 km, 600-1000 m da 1-2 macizos. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "10.0"))
    float TerrainBaseWavelengthM = 700.f;

    /** Amplitud conservada por octava (0..1). 0.5 reproduce el espectro ~1/f^2
        de los DEM reales; mas alto = mas rugoso, mas bajo = mas liso. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0.1", ClampMax = "0.9"))
    float TerrainPersistence = 0.5f;

    /** Multiplicador de frecuencia entre octavas (2 = estandar). */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1.5", ClampMax = "4.0"))
    float TerrainLacunarity = 2.f;

    /** Amplitud (m) del domain warp (Quilez): distorsiona el dominio con otro
        fBm para curvar valles y laderas de forma organica. 0 = desactivado. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0.0"))
    float TerrainWarpStrengthM = 150.f;

    /** Longitud de onda (m) de las formas del warp. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "10.0"))
    float TerrainWarpWavelengthM = 400.f;

    /** Peso de las crestas (ridged multifractal de Musgrave) en las zonas
        altas: 0 = solo colinas suaves, 1 = cordillera afilada. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TerrainRidgeWeight = 0.45f;

    // --- Relieve: erosion (bake unico al generar; ver TerrainErosion) ---
    /** Interruptor general de la erosion (hidraulica + termica). */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion")
    bool bTerrainErosion = true;

    /** Nº de gotas de la erosion hidraulica (0 = off). Mas gotas = red de
        drenaje mas marcada. ~120k para 512x512 tarda <1 s al generar. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "0"))
    int32 TerrainHydraulicDroplets = 120000;

    /** Intensidad de la erosion hidraulica: escala la tasa de arranque de
        material de cada gota (0..1). */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TerrainErosionStrength = 0.5f;

    /** Iteraciones de erosion termica (0 = off): relaja las pendientes que
        superan el angulo de talud (laderas de derrubios). */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "0"))
    int32 TerrainThermalIterations = 25;

    /** Angulo de talud (grados): pendiente maxima estable del material suelto.
        En la naturaleza, ~30-37. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "10.0", ClampMax = "80.0"))
    float TerrainTalusAngleDeg = 34.f;

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

    /**
     * Kl_max de la curva de luz: fL = Amax * Q/(Q + KlMax*(1-ShadeTolerance)).
     *
     * CUIDADO AL CALIBRARLO, porque decide si la luz limita por SOMBRA o por
     * decreto. Con Q normalizada a [0,1], un KlMax de 1.0 topa fL en 0.50 para una
     * heliofila incluso a pleno sol y en campo abierto: la luz pasa a ser el minimo
     * de Liebig en casi todo el mapa aunque no haya una sola copa, y como fL apenas
     * varia en el espacio (pero si mucho entre especies), el modelo se reduce a un
     * ranking global de tolerancia y la exclusion competitiva queda garantizada por
     * la calibracion.
     *
     * El valor correcto es el que hace que fL SATURE a pleno sol -de modo que en el
     * claro limiten agua y nutrientes, que es donde el modelo sabe repartir nicho- y
     * caiga en picado bajo dosel, que es donde la tolerancia debe decidir. Con
     * Q en [0,1] eso son ~0.10-0.25. Verificalo con Eco.AuditarEspecies: el bloque
     * de luz avisa si la varianza espacial de fL es despreciable frente a la
     * diferencia entre especies.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0.01"))
    float LightHalfSaturationMax = 0.12f;

    /**
     * Como se combinan los tres factores de recurso. Ver EEcoVigorCombine.
     *
     * POR QUE Minimum Y NO LA MEDIA GEOMETRICA, que es la opcion que parece mas
     * atractiva. La media geometrica arregla el problema del reparto de nicho -da
     * derivada no nula a los tres ejes- pero paga un precio que aqui es fatal:
     * la raiz cubica DILUYE un factor catastrofico. Con KlMax=0.12 y el umbral de
     * estres en 0.43, para que la falta de luz llegue a estresar a un arbol que
     * esta en su optimo hidrico haria falta Q < 0.010, y el piso de luz difusa es
     * 0.04: la luz NO PODRIA MATAR A NADIE NUNCA. Eso vacia justo el mecanismo que
     * el arreglo del dosel venia a construir.
     *
     * Con la ley del minimo y el KlMax corregido se obtienen las dos cosas: la luz
     * conserva todo su peso -la pionera entra en estres por debajo de Q=0.089 y la
     * climacica aguanta hasta Q=0.048, o sea existe una banda de penumbra donde una
     * sufre y la otra no-, y el reparto de nicho vuelve a estar activo porque f_L a
     * pleno sol ya no topa por debajo del pico de las campanas: el agua pasa a
     * limitar a solo 0.36-0.62 anchuras del optimo, frente a las 0.83 de antes.
     *
     * Cambia a GeometricMean solo si el limitante sigue saliendo "luz" en mas del
     * 40% de los arboles A PLENO SOL despues de arreglar el dosel.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    EEcoVigorCombine VigorCombineMode = EEcoVigorCombine::Minimum;

    /**
     * Coste de la tolerancia a la sombra: capacidad fotosintetica maxima
     * Amax(s) = 1 - esto * ShadeTolerance.
     *
     * Sin este termino, ShadeTolerance es una ventaja ESTRICTAMENTE MONOTONA y
     * gratis: sube fL a cualquier nivel de luz -tambien a pleno sol- y no cuesta
     * nada en ninguna otra ecuacion del proyecto. Un rasgo asi hace que la especie
     * mas tolerante gane en todas las celdas a la vez, que es exclusion competitiva
     * por construccion.
     *
     * Con el coste, la curva de la pionera y la de la climacica SE CRUZAN: la
     * pionera rinde mas en el claro y la tolerante en la penumbra, y ninguna gana en
     * todas partes. Esa es la primera diferencia estabilizadora real del modelo.
     * Con 0.40 el cruce cae en Q ~ 0.25-0.30. Ponlo a 0 para el comportamiento
     * anterior (tolerancia sin coste).
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "0.9"))
    float ShadeToleranceAssimilationCost = 0.40f;

    /**
     * true  = agua y nutrientes usan la respuesta UNIMODAL de nicho (optimo y
     *         anchura por especie, ver USpeciesData::WaterOptimum).
     * false = respuesta Monod monotona anterior (mas recurso siempre es mejor).
     *
     * Es un interruptor A/B deliberado: la curva Monod hace IMPOSIBLE que dos
     * especies se repartan un gradiente de recurso -solo cambia cuanto les gusta
     * el sitio bueno, no cual es su sitio-, asi que con ella la exclusion
     * competitiva esta garantizada por la FORMA de la funcion y no por los
     * numeros. Dejarlo en false reproduce bit a bit el comportamiento anterior.
     *
     * OJO: con true, WaterDemand y NutrientDemand dejan de intervenir en el vigor
     * y se quedan SOLO como tasa de consumo. Esa separacion es intencionada: que
     * el mismo numero fuera divisor de la respuesta Y multiplicador del consumo
     * daba a la especie poco exigente una ventaja doble sin ningun coste.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    bool bUseNicheResponse = true;

    /**
     * Anchura de la rama DERECHA de la campana de nicho (por encima del optimo),
     * como multiplo de la anchura declarada por la especie, cuando esa especie NO
     * penaliza el exceso (bWaterloggingPenalty / bNutrientExcessPenalty a false).
     *
     * POR QUE NO BASTA CON SATURAR EN 1. Recortando la respuesta a 1 por encima del
     * optimo, la curva deja de ser unimodal y se vuelve MONOTONA NO DECRECIENTE: en
     * toda la mitad rica del mapa fN vale 1.0000 exacto para cualquier especie, asi
     * que un optimo mas bajo es mejor-o-igual en el 100% de las celdas. Es
     * exactamente el eje monotono gratuito que la respuesta de nicho venia a
     * eliminar, reintroducido por la puerta de atras. Y en la mitad pobre reparte al
     * reves: gana la de campana mas ancha.
     *
     * Con una rama derecha ANCHA (2-3x) se conserva la idea biologica -"un suelo mas
     * rico no hace tanto dano como uno pobre"- sin perder la unimodalidad, que es lo
     * unico que reparte territorio. Ponlo a 0 para volver a la saturacion exacta.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "10"))
    float NicheExcessWidthScale = 2.5f;

    /** S_THRESH: vigor por debajo del cual se acumula estrés. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float StressVigorThreshold = 0.3f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressAccumulationRate = 1.f;

    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressRecoveryRate = 0.5f;

    /**
     * Decaimiento PROPORCIONAL del estres (-k*S por ano). Es el termino que
     * convierte el estres en una variable de estado graduada.
     *
     * Sin el, UpdateStress es un integrador puro con tope y su punto fijo es un
     * ESCALON: por encima del umbral de vigor el estres cae a 0 exacto y el arbol es
     * inmortal por ese canal; por debajo sube hasta 1 y muere al 20% anual. No hay
     * nada en medio. Dos sitios con vigor 0.44 y 0.90 dan identica demografia, y dos
     * especies separadas por 0.04 de vigor en el mismo pixel quedan separadas por
     * una diferencia de mortalidad infinita: es exclusion competitiva fabricada por
     * la forma de la funcion, no por la ecologia.
     *
     * Con k > 0 el punto fijo pasa a ser S* = (Umbral - vigor)*Acumulacion/k, una
     * rampa continua entre 0 y 1, y el vigor vuelve a mapear a mortalidad de forma
     * suave y monotona. 0 reproduce el comportamiento anterior.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressDecayRate = 0.2f;

    /** Peso del estrés acumulado en la probabilidad de morir por tick. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressMortalityWeight = 0.2f;

    /**
     * Cuanto REDUCE la longevidad de una especie su peso de mortalidad por estres:
     *
     *     peso efectivo = StressMortalityWeight * (LongevityStressRefYears / Longevity)^exponente
     *
     * POR QUE HACE FALTA. Los cuatro parametros de estres son GLOBALES e identicos
     * para todas las especies, mientras que GrowthRate, MaturityAge y MaxBiomass son
     * rasgos POR ESPECIE. Un impuesto de mortalidad en %/ano igual para todos
     * penaliza linealmente a quien necesita mas anos para crecer, y la unica
     * compensacion que el modelo ofrecia a la estrategia lenta -la longevidad- actua
     * sobre un canal (la mortalidad por edad) que en la practica aporta una fraccion
     * ridicula del riesgo total. Resultado: "lento" era sinonimo de "peor" y la
     * estrategia K resultaba matematicamente irrepresentable.
     *
     * Con este acoplamiento, una especie longeva paga en velocidad y COBRA en
     * resiliencia, que es el compromiso real. Exponente 0 lo desactiva.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "2"))
    float LongevityStressExponent = 0.5f;

    /** Longevidad de referencia del acoplamiento anterior (la especie "media"). */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1"))
    float LongevityStressRefYears = 300.f;

    /**
     * Histeresis de la supresion: un arbol suprimido no sale hasta que su estres
     * baja por debajo de esta FRACCION del umbral de su especie
     * (USpeciesData::SenescenceStressThreshold, que es el de entrada).
     *
     * La supresion por estres es REVERSIBLE, a diferencia de la senescencia por
     * edad: un arbol suprimido casi deja de crecer pero se recupera si mejoran sus
     * condiciones. Eso es lo que permite el BANCO DE PLANTULAS -plantulas tolerantes
     * que esperan decadas en penumbra y heredan el hueco cuando cae el dominante-,
     * que es el mecanismo de coexistencia de un bosque climacico. Antes las dos
     * causas compartian un unico estado irreversible, asi que unos pocos anos de
     * mala racha condenaban de por vida a la plantula y el banco no podia existir.
     *
     * Los umbrales de entrada y salida tienen que ser DISTINTOS: con uno solo el
     * estado parpadearia tick a tick alrededor del corte.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float SuppressionExitStressFraction = 0.4f;

    /**
     * Semillas que produce al año un adulto YA CRECIDO (media de la Poisson).
     * La lluvia real de un arbol escala con su biomasa RELATIVA, asi que un
     * arbol a media biomasa produce la mitad.
     *
     * SUSTITUYE a SeedRatePerBiomass, que iba con la biomasa ABSOLUTA y
     * convertia MaxBiomass en un multiplicador de fecundidad accidental (ver
     * EcologyRules::ComputeSeedCount). El renombre es deliberado: si tu
     * DefaultGame.ini conserva la clave vieja, queda huerfana y esta toma su
     * valor por defecto, que es el equivalente correcto de la calibracion
     * anterior (0.1 semillas x 100 de biomasa = 10 al año). O sea que olvidarse
     * de actualizar el .ini no rompe nada.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float SeedsPerAdultPerYear = 10.f;

    /**
     * Biomasa relativa (fraccion de MaxBiomass) a la que un adulto produce la MITAD
     * de su lluvia de semillas: lambda ~ SeedsPerAdultPerYear * r/(r + esto).
     *
     * Con proporcionalidad LINEAL a la biomasa alcanzada -el comportamiento
     * anterior- la fecundidad realimenta el crecimiento: quien crece algo mas rapido
     * no solo tiene mas individuos, sino que cada uno produce ademas mas semillas, y
     * cada semilla mas produce otro arbol que a su vez crece. Una ventaja de
     * crecimiento moderada se convierte asi en una diferencia de lluvia de semillas
     * de dos ordenes de magnitud. Y la misma formula ESTERILIZA a la especie
     * suprimida: un adulto al 5% de su MaxBiomass emitia el 5% de sus semillas y ya
     * no podia recuperarse aunque la mortalidad se relajase.
     *
     * Saturando, la madurez -y no el tamano- es lo que enciende la reproduccion.
     * 0 desactiva la saturacion (proporcionalidad lineal, como antes).
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float SeedBiomassHalfSaturation = 0.2f;

    /** Multiplicador de germinación: prob = VigorEnDestino * GerminationRate. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float GerminationRate = 0.5f;

    // LIMPIEZA: MinLightForGermination vivia aqui y era GLOBAL. Ahora es un campo
    // por especie (USpeciesData::MinLightForGermination), porque siendo global
    // obligaba a que TODO el reclutamiento pasara por claros y convertia la
    // ocupacion de huecos en una loteria que gana quien mas semillas manda. Si tu
    // DefaultGame.ini conserva la clave, queda huerfana y no hace nada.

    /**
     * Radio (cm) en el que una semilla cuenta los adultos de SU MISMA especie
     * para la inhibicion de Janzen-Connell. 0 = desactivado.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float ConspecificInhibitionRadiusCm = 1500.f;

    /**
     * Nº de adultos conespecificos dentro de ese radio que REDUCE A LA MITAD la
     * probabilidad de germinar (Janzen-Connell: los patogenos y herbivoros
     * especializados se acumulan bajo los adultos de su hospedador).
     *
     * Mas pequeno = inhibicion mas fuerte. 0 = desactivado. Es un estabilizador:
     * penaliza a la especie que domina LOCALMENTE, asi que empuja hacia la
     * mezcla. Ojo: por si solo no vence a una diferencia de mil a uno en lluvia
     * de semillas (la correccion maxima que puede dar es del orden de la razon de
     * densidades locales); su sitio es afinar el reparto, no romper un bloqueo.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float ConspecificHalfCount = 2.f;

    /**
     * true = el arbol MADRE no cuenta como conespecifico al evaluar la inhibicion
     * sobre sus propias semillas.
     *
     * Janzen-Connell solo estabiliza si la especie RARA recluta mejor que la comun.
     * Cuando el radio de dispersion no supera al de inhibicion, toda semilla cae
     * dentro del circulo de su madre, asi que hasta el ultimo adulto de una especie
     * al borde de la extincion paga la penalizacion por verse a si mismo. Eso pone
     * un techo al rescate de la especie rara justo donde mas falta hace.
     *
     * (El arreglo completo es ademas separar las dos escalas: SeedDispersalRadius
     * deberia ser varias veces ConspecificInhibitionRadiusCm para que exista una
     * fraccion real de semillas que escapan. Eso vive en el asset de especie.)
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    bool bExcludeMotherFromInhibition = true;

    /**
     * true = el radio de exclusion que impone un vecino a una plantula nueva
     * escala con el TAMANO de ese vecino (MinGerminationSpacingCm x su fraccion
     * de altura adulta), en vez de ser el mismo para un arbol de dosel que para
     * un plantón.
     *
     * Con el radio fijo, unos pocos miles de adultos cubren el mapa entero de
     * discos de exclusion -a 5 m y 16.000 arboles la cobertura pasa del 120%- y
     * el sotobosque deja de existir: no se puede germinar en ningun sitio salvo
     * en el hueco que acaba de dejar un muerto. Eso elimina el banco de plantulas
     * suprimidas, que es justo el mecanismo por el que una especie tolerante
     * hereda los huecos sin competir por semilla.
     *
     * Escalado, un adulto sigue apartando a 5 m pero una plantula del 1% de
     * biomasa solo aparta ~1 m, asi que el sotobosque vuelve a ser habitable. La
     * densidad la controlan entonces la luz y los recursos -que es donde debe
     * estar-, no una regla geometrica ciega a la especie.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    bool bSpacingScalesWithSize = true;

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

    /**
     * Radio MINIMO del kernel radicular, en celdas del campo de recursos.
     *
     * El kernel reparte con un peso lineal 1-d/R, que vale exactamente 0 en los
     * cuatro vecinos ortogonales cuando el radio no supera el tamano de celda. Con
     * el RootRadius por defecto (2 m) y celdas de 200 cm, el kernel de un adulto
     * escribia UNA sola celda: cada arbol se agotaba su propio pozo privado y no
     * tocaba el de nadie. La competencia subterranea -uno de los dos ejes que
     * deberian repartir el mapa- no existia como interaccion.
     *
     * Con un minimo de 2 celdas el disco alcanza a los vecinos y el recurso vuelve a
     * ser un bien comun por el que se compite. Ponlo a 0 para el comportamiento
     * anterior. (La solucion de fondo es calibrar RootRadius contra la separacion
     * media entre arboles, o bajar el tamano de celda del campo de recursos; esto es
     * la red de seguridad que impide que la geometria de la rejilla apague un eje
     * ecologico en silencio.)
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "8"))
    float MinRootRadiusCells = 2.f;

    /**
     * Fraccion maxima del recurso presente en una celda que se puede extraer en un
     * ano. Topa el consumo contra lo que realmente hay.
     *
     * Sin tope, un arbol podia "consumir" mas de lo disponible: la deuda negativa
     * resultante se difundia a las celdas vecinas -bajandoles recurso de verdad- y
     * despues se destruia al recortar a cero. O sea competencia por interferencia no
     * intencionada, cuya intensidad crecia con la demanda sin coste alguno para
     * quien la ejercia (otro eje monotono gratis), y ademas rompia la conservacion
     * de masa del campo, con lo que ninguna cuenta de balance cuadraba.
     *
     * 1.0 permite vaciar la celda entera en un ano; 0 desactiva el tope.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float MaxResourceUptakeFraction = 0.5f;

    // --- Ecología (Fase 2): grid de luz grueso (FLightFieldCoarse) ---
    /** Lado del voxel de luz, horizontal y vertical (cm). El nº de capas NO se
        configura: se deriva de la especie mas alta + LightCanopyHeadroomCm +
        LightGroundClearanceCm, porque la rejilla es relativa al terreno. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "50"))
    float LightCoarseCellSizeCm = 400.f;

    /**
     * Lado VERTICAL del voxel de luz (cm), independiente del horizontal.
     *
     * La rejilla siempre tuvo los dos tamanos separados, pero se le pasaba el mismo
     * valor a ambos. Con 400 cm en vertical, TODA la banda donde ocurre la
     * competencia de regeneracion -del suelo a los 4 m- cabe en una sola capa: las
     * plantulas y los arbolillos, que son la mayor parte de la poblacion, no existen
     * como estratos distintos en el campo de luz. 100-200 cm los devuelve al mapa
     * sin multiplicar el coste, porque la rejilla es relativa al terreno y solo
     * cubre la altura del arbol mas alto.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "25"))
    float LightCoarseCellSizeZCm = 200.f;

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

    // --- Forma de copa y germinacion (antes constantes en el .cpp del tick) ---
    // Entran en el bucle de luz y en la germinacion, o sea alteran el resultado
    // ecologico: deben ser parte de la configuracion reproducible del proyecto.
    // Si algun dia varian por especie, muevelas a USpeciesData.

    /** Radio de copa como fraccion de la altura del arbol (proxy hasta la geometria real). */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float CanopyRadiusFraction = 0.30f;

    // LIMPIEZA: aqui vivia CanopyShadowDensity ("opacidad de la copa"), que
    // alimentaba el deposito de sombra lineal. La sustituye CanopyLeafAreaIndex:
    // el grid de luz ya no acumula opacidad sino AREA FOLIAR, y la luz sale de
    // Beer-Lambert (ver FLightFieldCoarse). Si tu DefaultGame.ini conserva la clave
    // vieja, queda huerfana y no hace nada.

    /**
     * Espesor vertical de la copa como fraccion de la altura del arbol.
     *
     * ANTES ERA 1.0 IMPLICITO -el deposito de sombra recibia la altura entera del
     * arbol como profundidad de copa- y ese es el origen del bug que dejaba el
     * sotobosque a plena luz: la sombra se repartia por todo el fuste y se
     * desvanecia justo en la cota del suelo. Una copa real ocupa el tercio superior.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0.05", ClampMax = "1"))
    float CanopyDepthFraction = 0.30f;

    /** Indice de area foliar (LAI) de la copa de un adulto, medido en su eje.
        4.0 es un valor tipico de dosel cerrado de hoja ancha. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float CanopyLeafAreaIndex = 4.f;

    /** k de Beer-Lambert del dosel: Q = piso + (1-piso)*exp(-k*LAI). ~0.5 en hoja ancha. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float LightExtinctionK = 0.5f;

    /**
     * Luz difusa del cielo que llega al sotobosque aunque el dosel este cerrado
     * (medida real: 1-5% de la luz exterior).
     *
     * No es un detalle cosmetico: sin un suelo, bajo un dosel muy denso la luz
     * tenderia a 0 y con ella el factor de luz de TODAS las especies por igual, con
     * lo que la tolerante perderia su ventaja precisamente en la sombra profunda,
     * que es el unico sitio donde debe ganar.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "0.5"))
    float DiffuseLightFloor = 0.04f;

    /** Biomasa inicial de una plantula, como fraccion de MaxBiomass de su especie. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float GerminationBiomassFraction = 0.01f;

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

    // --- Perturbacion: claros (la dimension TEMPORAL del nicho) --------------
    //
    // Sin perturbacion cada arbol muere por su cuenta y su hueco es de un arbol:
    // NO HAY CLAROS. Y sin claros no hay sucesion, porque no existe la fase de
    // alta luz en la que la pionera es la mejor. El bucle que cierra la
    // coexistencia es: claro -> luz alta -> gana la pionera -> cierra el dosel ->
    // la pionera ya no recluta bajo su propia sombra pero la tolerante si ->
    // banco de plantulas -> cae el dominante -> la tolerante hereda -> claro.
    //
    // ARRANCA DESACTIVADO (tasa 0) a proposito: es el parametro mas sensible de
    // todo el modelo -demasiado frecuente y gana siempre la pionera, demasiado
    // raro y gana siempre la climacica-, asi que hay que activarlo y barrerlo
    // DESPUES de validar que el banco de plantulas funciona. Un claro sin banco
    // de plantulas solo beneficia a quien tiene mas semillas: amplifica la
    // exclusion en vez de corregirla.

    /** Fraccion del area del mapa perturbada al ano. 0.007 ~ rotacion de 140 anos. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "0", ClampMax = "0.2"))
    float DisturbanceRatePerYear = 0.f;

    /** Area minima de un claro (m2): la caida de un solo dominante. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "1"))
    float DisturbanceMinAreaM2 = 50.f;

    /** Area maxima de un claro (m2): el temporal raro de la cola de la distribucion. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "1"))
    float DisturbanceMaxAreaM2 = 5000.f;

    /** Exponente de la ley potencia de tamanos. Mas alto = mas claros pequenos. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "1", ClampMax = "5"))
    float DisturbanceAreaExponent = 2.f;

    /** Probabilidad de que un arbol dentro del claro caiga. <1 deja arboles residuales. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "0", ClampMax = "1"))
    float DisturbanceMortality = 0.9f;

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

    /** Margen de la caja envolvente de los componentes con viento. El WPO mueve
        vertices que el culling no ve; sin margen, un arbol al borde del encuadre
        desaparece de golpe con las ramas todavia dentro. UNICA fuente de este
        valor: lo leen los componentes de instancing Y los hero trees. */
    UPROPERTY(EditAnywhere, config, Category = "Fase6|Viento", meta = (ClampMin = "1", ClampMax = "3"))
    float WindBoundsScale = 1.15f;

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
