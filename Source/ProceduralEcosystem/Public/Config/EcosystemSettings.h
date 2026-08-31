/**
 * @file EcosystemSettings.h
 * @author Juan Luque Roldán
 * @brief Ajustes de proyecto del simulador: el punto único donde vive todo valor que
 *        altera el resultado ecológico o el determinismo.
 *
 * Declara UEcosystemSettings, el objeto de ajustes que concentra semilla maestra, reloj
 * de simulación, síntesis y erosión del relieve, campos de recursos, ecuaciones de vigor
 * y demografía, perturbación, niveles de representación, capa de suelo, viento, CO2 y
 * presupuesto de frame. Un simulador reproducible no puede tener constantes escondidas
 * en las unidades de traducción: cada valor se declara como propiedad configurable, se
 * edita en Project Settings -> Game -> "Procedural Ecosystem", persiste en
 * Config/DefaultGame.ini y se lee por puntero constante. Los comentarios de cada campo
 * justifican el valor por defecto y la patología del modelo que evita.
 *
 * @ingroup eco_config
 * @see @ref bib_epicueconfig
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "EcosystemSettings.generated.h"

class USpeciesData;
class UMaterialInterface;
class UStaticMesh;                  // mallas de la capa de suelo (tocón y hojarasca)
class UMaterialParameterCollection; // colecciones de material de estación y de viento

/**
 * Regla con la que los tres factores de recurso (luz, agua, nutrientes) se colapsan en
 * el vigor.
 *
 * Vive aquí y no en Ecology/Vigor.h porque tiene que ser un UENUM para poder
 * configurarse; Vigor.h incluye esta cabecera solo por eso. El modo elegido debe ser el
 * mismo en el tick de crecimiento, en la germinación y en el mapa de idoneidad, o el
 * mapa dejaría de representar la función que hace crecer al bosque.
 *
 * @see EcoVigor::EvaluateVigor
 */
UENUM(BlueprintType)
enum class EEcoVigorCombine : uint8
{
    /**
     * Ley del mínimo de Liebig, @f$ vigor = \min(f_L, f_W, f_N) @f$: el recurso más
     * escaso manda y los otros dos no pesan nada.
     *
     * Su derivada respecto a los ejes NO limitantes es exactamente cero. Si el factor
     * de luz queda topado por debajo del pico de las campanas de nicho, el agua y los
     * nutrientes dejan de intervenir por completo y con ellos desaparece el reparto de
     * nicho: dentro de esa meseta, estar en el óptimo hídrico no vale más que estar a
     * media anchura de él. Es el modo por defecto porque es también el único que
     * conserva todo el peso de la luz (@ref VigorCombineMode).
     *
     * @see @ref bib_liebig1840
     */
    Minimum,

    /**
     * Media geométrica, @f$ vigor = (f_L f_W f_N)^{1/3} @f$: los tres ejes pesan
     * siempre, así que una desventaja de luz de factor r se compensa con una ventaja
     * de agua por nutrientes del mismo factor y cada especie vuelve a tener sitios
     * donde gana.
     *
     * El exponente 1/3 conserva la escala del vigor —tres factores de 0.6 dan 0.6, no
     * 0.216—, que es lo que permite cambiar de modo sin recalibrar StressVigorThreshold
     * ni los GrowthRate de los assets.
     *
     * @see @ref bib_bloom1985
     */
    GeometricMean,

    /**
     * Híbrido: min() entre los factores de SUMINISTRO y producto aparte de las
     * respuestas de nicho. Es la forma de los modelos de hueco clásicos, y descansa en
     * que una campana de nicho no mide disponibilidad de recurso sino tolerancia, y las
     * tolerancias se multiplican.
     *
     * Con bUseNicheResponse a false no hay tolerancias que separar y degenera
     * exactamente en Minimum.
     *
     * @see @ref bib_gapmodels
     */
    MinSupplyTimesNiche
};

/**
 * Ajustes del ecosistema, en Project Settings -> Game -> "Procedural Ecosystem".
 *
 * Reúne semilla maestra, reloj de simulación, relieve, campos de recursos, ecuaciones
 * de vigor y demografía, perturbación, presentación, capa de suelo, viento, CO2 y
 * presupuesto de frame. Las propiedades son de configuración, así que la tabla de
 * calibración persiste en Config/DefaultGame.ini y admite edición en vivo desde el
 * editor. La única instancia que se consulta es el objeto por defecto de la clase.
 *
 * @note Acceso canónico: `const UEcosystemSettings* S = UEcosystemSettings::Get();`.
 *       Es O(1) y no reserva memoria, así que puede llamarse en cualquier sitio salvo
 *       dentro de un lambda paralelo, donde los valores se capturan por valor.
 * @warning Una clave de Config/DefaultGame.ini que ya no corresponde a ninguna
 *          propiedad queda huérfana: no hace nada y conviene borrarla. El comando de
 *          consola de auditoría del .ini las localiza.
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Procedural Ecosystem"))
class PROCEDURALECOSYSTEM_API UEcosystemSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    /** Sección del árbol de Project Settings bajo la que aparecen estos ajustes. */
    virtual FName GetCategoryName() const override { return TEXT("Game"); }

    // ==== Reproducibilidad ====

    /** Semilla maestra del mundo: de ella derivan los flujos de la simulación y los
        hashes del terreno, del ruido y de los arquetipos. Fijarla fija el bosque. */
    UPROPERTY(EditAnywhere, config, Category = "Reproducibilidad")
    int32 MasterSeed = 12345;

    // ==== Tiempo de simulación (tick desacoplado del frame) ====

    /** Segundos reales por tick en modo vivo. */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "0.01"))
    float SecondsPerSimTick = 0.5f;

    /** Años simulados que avanza cada tick: la segunda unidad del paso fijo, la del
        reloj ecológico. La primera es SecondsPerSimTick, la del reloj de pared. */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "0.01"))
    float YearsPerTick = 1.0f;

    /** Tope al NÚMERO de ticks de recuperación que se ejecutan en un frame, que es lo
        que evita la espiral de la muerte cuando cae el framerate. El tope al TIEMPO es
        aparte: @ref TickBudgetMsPerFrame.
        @see @ref bib_fiedler2004 */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo", meta = (ClampMin = "1"))
    int32 MaxStepsPerFrame = 4;

    /** El mundo arranca con la simulación pausada (bosque congelado para inspección). */
    UPROPERTY(EditAnywhere, config, Category = "Tiempo")
    bool bStartPaused = true;

    // ==== Relieve: síntesis fractal (ver FHeightField::Generate) ====

    /** Muestras por lado del heightfield. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "2"))
    int32 HeightfieldResolution = 512;

    /** Centímetros por muestra: 512 muestras a 200 cm dan un mapa de ~1 km de lado. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1"))
    float HeightfieldCellSizeCm = 200.f;

    /** Amplitud vertical del relieve, de valle a pico. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0"))
    float HeightScaleCm = 30000.f;

    /** Octavas del fractal: más octavas = más detalle fino. El generador descarta las
        que caen bajo el límite de Nyquist de la rejilla (longitud de onda menor que
        2 * HeightfieldCellSizeCm), porque solo aportarían aliasing.
        @see @ref bib_nyquistshannon */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1", ClampMax = "12"))
    int32 HeightfieldOctaves = 8;

    /** Longitud de onda (m) de la octava base: el ancho de las formas GRANDES del
        relieve. Para un mapa de ~1 km, 600-1000 m da uno o dos macizos. La octava base
        se parametriza en longitud de onda, y no en frecuencia, para que el número se
        pueda comparar de un vistazo con el lado del mapa y con el desnivel.
        @see @ref bib_perlin1985
        @see @ref bib_mandelbrot1968 */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "10.0"))
    float TerrainBaseWavelengthM = 700.f;

    /** Amplitud conservada por octava, en (0,1). El criterio del valor por defecto es
        geomorfológico, no estético: 0.5 reproduce el espectro @f$\sim 1/f^2@f$ de los
        modelos digitales de elevación reales. Más alto = más rugoso. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0.1", ClampMax = "0.9"))
    float TerrainPersistence = 0.5f;

    /** Multiplicador de frecuencia entre octavas (2 = estándar). */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "1.5", ClampMax = "4.0"))
    float TerrainLacunarity = 2.f;

    /** Amplitud (m) del domain warp: evalúa el fractal en coordenadas desplazadas por
        otro fBm para curvar valles y laderas y romper la isotropía del fBm puro.
        0 = desactivado.
        @see @ref bib_quilezdomainwarp */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0.0"))
    float TerrainWarpStrengthM = 150.f;

    /** Longitud de onda (m) de las formas del warp. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "10.0"))
    float TerrainWarpWavelengthM = 400.f;

    /** Peso con el que el ridged multifractal se mezcla con el fBm en las zonas altas:
        0 = solo colinas suaves, 1 = cordillera afilada. La mezcla se interpola por
        altitud, de modo que el mapa da colinas abajo y crestas arriba.
        @see @ref bib_musgrave1989 */
    UPROPERTY(EditAnywhere, config, Category = "Relieve", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TerrainRidgeWeight = 0.45f;

    // ==== Relieve: erosión (bake único al generar; ver TerrainErosion) ====

    /** Interruptor general de la erosión (hidráulica + térmica). */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion")
    bool bTerrainErosion = true;

    /** Número de gotas de la erosión hidráulica (0 = desactivada). Más gotas = red de
        drenaje más marcada. Unas 120.000 sobre 512x512 tardan menos de 1 s al generar. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "0"))
    int32 TerrainHydraulicDroplets = 120000;

    /** Intensidad de la erosión hidráulica, en [0,1]: escala la tasa de arranque de
        material de cada gota. Es el único mando de un método que en su formulación
        original tiene coeficientes separados de capacidad, erosión y deposición.
        @see @ref bib_beyer2015 */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float TerrainErosionStrength = 0.5f;

    /** Iteraciones de erosión térmica (0 = desactivada): relajan las pendientes que
        superan el ángulo de talud, formando laderas de derrubios. */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "0"))
    int32 TerrainThermalIterations = 25;

    /** Ángulo de talud (grados): pendiente máxima estable del material suelto. En la
        naturaleza cae entre 30 y 37.
        @see @ref bib_olsen2004 */
    UPROPERTY(EditAnywhere, config, Category = "Relieve|Erosion", meta = (ClampMin = "10.0", ClampMax = "80.0"))
    float TerrainTalusAngleDeg = 34.f;

    // ==== Recursos: campo base de agua ====

    /** Cota superior a la que se remapea el índice topográfico de humedad. Debe casar
        con NutrientOutputMax: el vigor recibe agua y nutrientes como dos entradas de la
        misma ecuación y tienen que llegar en escalas comparables.
        @see @ref bib_beven1979 */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Agua", meta = (ClampMin = "0.001"))
    float WaterOutputMax = 10.f;

    /** Rellena las depresiones del heightfield (priority-flood) antes de calcular las
        direcciones de flujo D8, de modo que ninguna celda sea un callejón sin salida.
        Se conserva como interruptor para poder ablacionarlo.
        @see @ref bib_barnes2014
        @see @ref bib_ocallaghan1984 */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Agua")
    bool bFillWaterSinks = true;

    /**
     * Normaliza el TWI por RANGO (percentil espacial) en lugar de linealmente.
     *
     * Es la corrección del vacío de árboles en las llanuras. El TWI vale
     * ln(acumulación/tan(pendiente)) con la pendiente acotada por debajo, así que toda
     * celda llana cobra ln(1/tan(mín)) ≈ 6,9 unidades solo por ser llana: llanuras y
     * fondos forman una isla de valores extremos, y la salida de la cuenca mayor estira
     * el máximo aún más. Normalizado linealmente, el 96% del mapa queda por debajo de
     * 0,3·WaterOutputMax y las llanuras aisladas en 0,5-0,7: a más anchuras de campana de
     * cualquier óptimo de especie de las que la respuesta de nicho puede salvar, con lo
     * que allí el factor de agua es ~0, no germina nada y las llanuras quedan sin bosque
     * por construcción —el heatmap de idoneidad las muestra en negro para todas las
     * especies a la vez—.
     *
     * Con el rango, el campo es uniforme por área y WaterOptimum pasa a leerse como
     * percentil: 0,25 es «más húmedo que el cuarto más seco del mapa», la anchura
     * sugerida por Eco.PercentilesCampos pasa a ser 0,25 en vez de ~0,05, y las llanuras
     * ocupan el tramo alto (≈0,85-0,95) CONTIGUO al resto, alcanzable por cualquier
     * especie de óptimo húmedo. La ordenación espacial —lo único con significado en un
     * índice reescalado— se conserva exacta.
     *
     * @warning Cambia la distribución del campo, no su ordenación: tras activarlo o
     *          desactivarlo conviene revisar los óptimos de nicho de las especies con
     *          Eco.PercentilesCampos. A false se conserva la normalización lineal
     *          antigua, para reproducir corridas ya calibradas con ella.
     * @see FField2D::FillRankNormalizedFrom
     * @see @ref bib_beven1979
     */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Agua")
    bool bWaterRankNormalization = true;

    // ==== Recursos: campo base de nutrientes ====

    /** Cota superior a la que se remapea el ruido de nutrientes; debe casar con
        @ref WaterOutputMax por el mismo motivo. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "0.001"))
    float NutrientOutputMax = 10.f;

    /** Frecuencia base (cm^-1) del ruido de parches: más baja = parches más grandes.
        Es double porque el valor útil está en el orden de 1e-4. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "0.0"))
    double NutrientPatchFrequency = 0.00015;

    /** Octavas del ruido de nutrientes. Bastan pocas: interesa el parcheado a escala de
        paisaje, no el detalle fino. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Nutrientes", meta = (ClampMin = "1"))
    int32 NutrientOctaves = 3;

    // ==== Recursos: extensión vertical de la rejilla de luz ====

    /** Margen de altura (cm) por encima del árbol MÁS ALTO que cubre la rejilla de luz
        gruesa. El número de capas no se configura: se deriva de la MaxHeightCm mayor de
        las especies más este margen, porque la rejilla es relativa al terreno y no tiene
        que cubrir el desnivel del relieve (ver FLightFieldCoarse). */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Luz", meta = (ClampMin = "0"))
    float LightCanopyHeadroomCm = 1500.f;

    /** Margen (cm) POR DEBAJO del terreno que cubre la rejilla de luz. Da holgura en
        pendientes fuertes, donde la copa de un vecino cuesta abajo cae por debajo de la
        cota de esta columna. */
    UPROPERTY(EditAnywhere, config, Category = "Recursos|Luz", meta = (ClampMin = "0"))
    float LightGroundClearanceCm = 800.f;

    // ==== Vigor: curvas de respuesta y regla de combinación ====

    /** Especie por defecto del mapa de idoneidad (índice dentro de Species). */
    UPROPERTY(EditAnywhere, config, Category = "Vigor", meta = (ClampMin = "0"))
    int32 HeatmapSpeciesIndex = 0;

    /**
     * Semisaturación de la curva de luz para la especie menos tolerante a la sombra:
     * @f$ f_L = A_{max}\,Q/(Q + K_{lMax}(1-s)) @f$ (@ref EcoVigor::LightFactor).
     *
     * Decide si la luz limita por SOMBRA o por calibración, así que es el parámetro más
     * delicado del vigor. Con Q normalizada a [0,1], un valor de 1.0 topa fL en 0.50
     * para una heliófila incluso a pleno sol y en campo abierto: la luz pasa a ser el
     * mínimo de Liebig en casi todo el mapa aunque no haya una sola copa y, como fL
     * apenas varía en el espacio pero sí mucho entre especies, el modelo se reduce a un
     * ranking global de tolerancia y la exclusión competitiva queda fabricada.
     *
     * El valor correcto es el que hace que fL SATURE a pleno sol —de modo que en el
     * claro limiten agua y nutrientes, que es donde el modelo sabe repartir nicho— y
     * caiga en picado bajo dosel, que es donde la tolerancia debe decidir. Con Q en
     * [0,1] el rango sano es 0.10-0.25.
     *
     * @note El comando de consola Eco.AuditarEspecies avisa si la varianza espacial de
     *       fL es despreciable frente a la diferencia entre especies.
     * @see @ref bib_monod1949
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0.01"))
    float LightHalfSaturationMax = 0.12f;

    /**
     * Regla con la que se combinan los tres factores de recurso (@ref EEcoVigorCombine).
     *
     * El valor por defecto es la ley del mínimo, y no la media geométrica que a primera
     * vista parece preferible, porque la raíz cúbica DILUYE un factor catastrófico. Con
     * KlMax en 0.12 y el umbral de estrés en 0.43, para que la falta de luz llegase a
     * estresar a un árbol situado en su óptimo hídrico haría falta Q < 0.010, y el piso
     * de luz difusa es 0.04: la luz no podría matar nunca, que es justo el mecanismo que
     * sostiene la sucesión.
     *
     * Con la ley del mínimo y este KlMax se obtienen las dos cosas a la vez: la luz
     * conserva todo su peso —la pionera entra en estrés por debajo de Q=0.089 y la
     * climácica aguanta hasta Q=0.048, o sea que existe una banda de penumbra donde una
     * sufre y la otra no— y el reparto de nicho sigue activo, porque fL a pleno sol ya
     * no topa por debajo del pico de las campanas y el agua pasa a limitar a solo
     * 0.36-0.62 anchuras del óptimo.
     *
     * @note GeometricMean se justifica si el limitante sigue saliendo "luz" en más del
     *       40% de los árboles A PLENO SOL.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    EEcoVigorCombine VigorCombineMode = EEcoVigorCombine::Minimum;

    /**
     * Coste de la tolerancia a la sombra: capacidad fotosintética máxima
     * @f$ A_{max}(s) = 1 - c\,s @f$, con c este valor y s la ShadeTolerance de la
     * especie (@ref EcoVigor::FLightResponse).
     *
     * Sin este término, la tolerancia a la sombra es una ventaja estrictamente monótona
     * y gratuita: sube fL a cualquier nivel de luz —también a pleno sol— y no cuesta
     * nada en ninguna otra ecuación. Un rasgo así hace que la especie más tolerante gane
     * en todas las celdas a la vez, que es exclusión competitiva por construcción.
     *
     * Con el coste, las curvas de la pionera y de la climácica SE CRUZAN: la pionera
     * rinde más en el claro y la tolerante en la penumbra, y ninguna gana en todas
     * partes. Es la primera diferencia estabilizadora del modelo. El cruce es único y no
     * depende de las tolerancias de las dos especies: cae siempre en
     * @f$ Q^{*} = K_{lMax}(1-c)/c @f$, o sea Q = 0.18 con KlMax 0.12 y c 0.40. 0 devuelve
     * la tolerancia sin coste.
     *
     * @see @ref bib_toleranciasombra
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "0.9"))
    float ShadeToleranceAssimilationCost = 0.40f;

    /**
     * Elige la forma de la respuesta al agua y a los nutrientes: campana de nicho
     * unimodal con óptimo y anchura por especie (`true`, ver USpeciesData::WaterOptimum)
     * o saturante de Monod monótona (`false`, más recurso es siempre mejor).
     *
     * Es un interruptor A/B deliberado. La curva monótona hace IMPOSIBLE que dos
     * especies se repartan un gradiente de recurso —solo cambia cuánto les gusta el
     * sitio bueno, no cuál es su sitio—, así que con ella la exclusión competitiva queda
     * garantizada por la FORMA de la función y no por los números.
     *
     * @warning Con la respuesta de nicho activa, WaterDemand y NutrientDemand dejan de
     *          intervenir en el vigor y quedan solo como tasa de consumo. La separación
     *          es intencionada: que el mismo número dividiese la respuesta y multiplicase
     *          el consumo daba a la especie poco exigente una ventaja doble sin coste.
     * @see @ref bib_nichounimodal
     * @see @ref bib_exclusioncompetitiva
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    bool bUseNicheResponse = true;

    /**
     * Anchura de la rama DERECHA de la campana de nicho (por encima del óptimo), como
     * múltiplo de la anchura declarada por la especie, cuando esa especie no penaliza el
     * exceso (bWaterloggingPenalty / bNutrientExcessPenalty a false).
     *
     * No basta con saturar la respuesta en 1 por encima del óptimo: así la curva deja de
     * ser unimodal y se vuelve monótona no decreciente, con lo que en toda la mitad rica
     * del mapa el factor vale 1.0000 exacto para cualquier especie y un óptimo más bajo
     * resulta mejor o igual en el 100% de las celdas. Es exactamente el eje monótono
     * gratuito que la respuesta de nicho viene a eliminar, reintroducido por la puerta de
     * atrás; y en la mitad pobre el reparto se decide solo por quién tiene la campana más
     * ancha.
     *
     * Con una rama derecha ancha (2-3x) se conserva la idea biológica —un suelo más rico
     * no hace tanto daño como uno pobre— sin perder la unimodalidad, que es lo único que
     * reparte territorio. 0 devuelve la saturación exacta.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "10"))
    float NicheExcessWidthScale = 2.5f;

    /** Vigor por debajo del cual un árbol acumula estrés en vez de recuperarse. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float StressVigorThreshold = 0.3f;

    /** Estrés acumulado por año y por unidad de vigor que falta hasta el umbral. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressAccumulationRate = 1.f;

    /** Estrés que se descuenta por año mientras el vigor está en el umbral o por encima.
        Es una tasa fija: no depende de cuánto vigor sobre. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressRecoveryRate = 0.5f;

    /**
     * Decaimiento PROPORCIONAL del estrés, @f$ -k\,S @f$ por año. Es el término que
     * convierte el estrés en una variable de estado graduada.
     *
     * Sin él, la actualización del estrés es un integrador puro con tope y su punto fijo
     * es un escalón: por encima del umbral de vigor el estrés cae a 0 exacto y el árbol
     * es inmortal por ese canal, y por debajo sube hasta 1 y muere al 20% anual, sin nada
     * en medio. Dos sitios con vigor 0.44 y 0.90 darían idéntica demografía, y dos
     * especies separadas por 0.04 de vigor en el mismo píxel quedarían separadas por una
     * diferencia de mortalidad infinita: exclusión competitiva fabricada por la forma de
     * la función.
     *
     * Con @f$ k > 0 @f$ el punto fijo pasa a ser
     * @f$ S^{*} = (\text{umbral} - vigor)\cdot\text{acumulación}/k @f$, una rampa continua
     * en [0,1] que mapea vigor a mortalidad de forma suave y monótona.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressDecayRate = 0.2f;

    /** Peso del estrés acumulado en la probabilidad de morir por tick, antes de la
        corrección por longevidad (@ref LongevityStressExponent). */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float StressMortalityWeight = 0.2f;

    /**
     * Exponente con el que la longevidad de una especie rebaja su peso de mortalidad
     * por estrés:
     *
     * @code
     * peso efectivo = StressMortalityWeight * (LongevityStressRefYears / Longevity)^exponente
     * @endcode
     *
     * Los cuatro parámetros de estrés son GLOBALES e idénticos para todas las especies,
     * mientras que GrowthRate, MaturityAge y MaxBiomass son rasgos por especie. Un
     * impuesto de mortalidad en fracción por año igual para todos penaliza linealmente a
     * quien necesita más años para crecer, y la única compensación que le queda a la
     * estrategia lenta —la longevidad— actúa sobre la mortalidad por edad, que aporta una
     * fracción despreciable del riesgo total. Sin este acoplamiento, «lento» equivale a
     * «peor» y la estrategia K resulta matemáticamente irrepresentable.
     *
     * Con él, una especie longeva paga en velocidad y cobra en resiliencia, que es el
     * compromiso real. Exponente 0 lo desactiva.
     *
     * @see @ref bib_estrategiark
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "2"))
    float LongevityStressExponent = 0.5f;

    /** Longevidad (años) de la especie «media», la que paga el StressMortalityWeight sin
        corregir en el acoplamiento anterior. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1"))
    float LongevityStressRefYears = 300.f;

    /**
     * Histéresis de la supresión: un árbol suprimido no sale del estado hasta que su
     * estrés baja de esta FRACCIÓN del umbral de entrada de su especie
     * (USpeciesData::SenescenceStressThreshold).
     *
     * La supresión por estrés es REVERSIBLE, a diferencia de la senescencia por edad: un
     * árbol suprimido casi deja de crecer pero se recupera si mejoran sus condiciones. Es
     * lo que hace posible el banco de plántulas —plántulas tolerantes que esperan décadas
     * en penumbra y heredan el hueco cuando cae el dominante—, que es el mecanismo de
     * coexistencia de un bosque climácico.
     *
     * @pre Los umbrales de entrada y salida tienen que ser distintos: con uno solo el
     *      estado parpadearía tick a tick alrededor del corte.
     * @see @ref bib_dinamicadeclaros
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float SuppressionExitStressFraction = 0.4f;

    /**
     * Semillas que produce al año un adulto ya crecido: la media de la Poisson de la
     * que se saca el conteo (@ref EcologyRules::ComputeSeedCount).
     *
     * La lluvia real de un árbol escala con su biomasa RELATIVA, no con la absoluta, de
     * modo que MaxBiomass no actúe como multiplicador accidental de fecundidad: un árbol
     * a media biomasa relativa produce la mitad.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float SeedsPerAdultPerYear = 10.f;

    /**
     * Biomasa relativa (fracción de MaxBiomass) a la que un adulto produce la MITAD de
     * su lluvia de semillas: @f$ \lambda = S\,r/(r + k) @f$, con r la biomasa relativa,
     * S las SeedsPerAdultPerYear y k este valor.
     *
     * Con proporcionalidad lineal a la biomasa alcanzada, la fecundidad realimenta el
     * crecimiento: quien crece algo más rápido no solo tiene más individuos, sino que
     * cada uno produce además más semillas, y cada semilla de más produce otro árbol que
     * a su vez crece; una ventaja de crecimiento moderada se convierte así en dos órdenes
     * de magnitud de diferencia en lluvia de semillas. La misma proporcionalidad
     * esteriliza al suprimido, que al 5% de su MaxBiomass emite el 5% de sus semillas y
     * no puede recuperarse aunque la mortalidad afloje.
     *
     * Saturando, es la MADUREZ y no el tamaño lo que enciende la reproducción. 0
     * desactiva la saturación y vuelve a la proporcionalidad lineal.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float SeedBiomassHalfSaturation = 0.2f;

    /** Multiplicador de germinación: la probabilidad es el vigor en el destino por este
        factor, antes de la inhibición conespecífica. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float GerminationRate = 0.5f;

    /**
     * Radio (cm) dentro del cual una semilla cuenta los adultos de SU MISMA especie para
     * la inhibición de Janzen-Connell. 0 la desactiva.
     *
     * @see @ref bib_janzenconnell
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float ConspecificInhibitionRadiusCm = 1500.f;

    /**
     * Número de adultos conespecíficos dentro de ese radio que REDUCE A LA MITAD la
     * probabilidad de germinar: los patógenos y herbívoros especializados se acumulan
     * bajo los adultos de su hospedador. Más pequeño = inhibición más fuerte; 0 la
     * desactiva.
     *
     * Es un estabilizador: penaliza a la especie que domina LOCALMENTE y empuja hacia la
     * mezcla.
     *
     * @note Su alcance es acotado. La corrección máxima que puede dar es del orden de la
     *       razón de densidades locales, así que no vence una diferencia de mil a uno en
     *       lluvia de semillas: afina el reparto, no rompe un bloqueo.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float ConspecificHalfCount = 2.f;

    /**
     * El árbol MADRE no cuenta como conespecífico al evaluar la inhibición sobre sus
     * propias semillas.
     *
     * La inhibición solo estabiliza si la especie RARA recluta mejor que la común. Cuando
     * el radio de dispersión no supera al de inhibición, toda semilla cae dentro del
     * círculo de su madre y hasta el último adulto de una especie al borde de la
     * extinción paga la penalización por verse a sí mismo, lo que pone un techo al
     * rescate de la especie rara justo donde más falta hace.
     *
     * @note La otra mitad del problema vive en el asset de especie: para que exista una
     *       fracción real de semillas que escapan, SeedDispersalRadius debe ser varias
     *       veces ConspecificInhibitionRadiusCm.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    bool bExcludeMotherFromInhibition = true;

    /**
     * El radio de exclusión que un vecino impone a una plántula nueva escala con el
     * TAMAÑO de ese vecino (MinGerminationSpacingCm por su fracción de altura adulta),
     * en vez de ser el mismo para un árbol de dosel que para una plántula.
     *
     * Con radio fijo, unos pocos miles de adultos cubren el mapa entero de discos de
     * exclusión —a 5 m y 16.000 árboles la cobertura pasa del 120%— y el sotobosque deja
     * de existir: no se puede germinar en ningún sitio salvo en el hueco que acaba de
     * dejar un muerto, lo que elimina el banco de plántulas suprimidas.
     *
     * Escalado, un adulto sigue apartando a 5 m pero una plántula del 1% de biomasa solo
     * aparta ~1 m, así que la densidad la controlan la luz y los recursos y no una regla
     * geométrica ciega al tamaño.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia")
    bool bSpacingScalesWithSize = true;

    /** Fracción de la biomasa que vuelve al campo de nutrientes como pulso al morir el
        árbol. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientDecompositionFactor = 0.3f;

    // ==== Regeneración de los pools de recursos ====
    //
    // Al final de cada tick el pool relaja hacia su campo base a la tasa de recarga y
    // difunde lateralmente a la tasa de difusión, ambas en unidades por año. Es lo que
    // convierte agua y nutrientes en bienes comunes agotables por los que se compite.

    /** Velocidad (por año) a la que el pool de agua relaja hacia el campo base. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float WaterRechargeRate = 0.3f;

    /** Difusión lateral (por año) del pool de agua entre celdas vecinas. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float WaterDiffusionRate = 0.1f;

    /** Velocidad (por año) a la que el pool de nutrientes relaja hacia el campo base. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientRechargeRate = 0.15f;

    /** Difusión lateral (por año) del pool de nutrientes entre celdas vecinas. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float NutrientDiffusionRate = 0.2f;

    /**
     * Radio MÍNIMO del kernel radicular, en celdas del campo de recursos.
     *
     * El kernel reparte el consumo con un peso lineal @f$ 1 - d/R @f$, que vale
     * exactamente 0 en los cuatro vecinos ortogonales cuando el radio no supera el
     * tamaño de celda. Con un RootRadius de 2 m y celdas de 200 cm, el kernel de un
     * adulto escribe UNA sola celda: cada árbol se agota su propio pozo privado y no
     * toca el de nadie, con lo que la competencia subterránea deja de existir como
     * interacción.
     *
     * Con un mínimo de 2 celdas el disco alcanza a los vecinos y el recurso vuelve a ser
     * un bien común. Es una red de seguridad frente a un artefacto de rejilla, no un
     * mecanismo ecológico: la solución de fondo es calibrar RootRadius contra la
     * separación media entre árboles o bajar el tamaño de celda del campo.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "8"))
    float MinRootRadiusCells = 2.f;

    /**
     * Fracción máxima del recurso presente en una celda que un árbol puede extraer en un
     * año: topa el consumo contra lo que realmente hay. 1.0 permite vaciar la celda
     * entera en un año; 0 desactiva el tope.
     *
     * Sin tope, un árbol puede «consumir» más de lo disponible. La deuda negativa
     * resultante se difunde a las celdas vecinas —bajándoles recurso de verdad— y
     * después se destruye al recortar a cero: aparece una competencia por interferencia
     * no intencionada cuya intensidad crece con la demanda sin coste alguno para quien la
     * ejerce, y se rompe la conservación de masa del campo.
     *
     * @see @ref bib_tilman1982
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float MaxResourceUptakeFraction = 0.5f;

    // ==== Rejilla de luz gruesa (FLightFieldCoarse) ====

    /** Lado horizontal del vóxel de luz (cm). El número de capas no se configura: se
        deriva de la especie más alta más LightCanopyHeadroomCm y LightGroundClearanceCm,
        porque la rejilla es relativa al terreno. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "50"))
    float LightCoarseCellSizeCm = 400.f;

    /**
     * Lado VERTICAL del vóxel de luz (cm), independiente del horizontal.
     *
     * Es el parámetro que fija la resolución de la estratificación vertical, y por tanto
     * cuántos estratos distingue la competencia por luz. Con 400 cm, toda la banda donde
     * ocurre la regeneración —del suelo a los 4 m— cabe en una sola capa y las plántulas
     * y arbolillos, que son la mayor parte de la población, dejan de existir como
     * estratos separados. Entre 100 y 200 cm los devuelve al mapa sin multiplicar el
     * coste, porque la rejilla solo cubre la altura del árbol más alto.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "25"))
    float LightCoarseCellSizeZCm = 200.f;

    /**
     * Cada cuántos ticks se reconstruye la rejilla de luz gruesa. 1 = cada tick.
     *
     * Las copas cambian de tamaño despacio, así que subirlo a 2-4 apenas altera el
     * resultado y ahorra la pasada serial de borrado y depósito de área foliar.
     *
     * @warning No es una optimización neutra: cambia el resultado de la simulación, de
     *          modo que dos corridas con distinto valor no son comparables bit a bit.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1", ClampMax = "16"))
    int32 LightRebuildEveryNTicks = 1;

    /** Radio (cm) de exclusión que un vecino impone a una plántula nueva; escalado por
        el tamaño del vecino si bSpacingScalesWithSize está activo. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float MinGerminationSpacingCm = 100.f;

    // ==== Forma de la copa y atenuación de la luz ====
    //
    // Entran en el bucle de luz y en la germinación, o sea que alteran el resultado
    // ecológico: son parte de la configuración reproducible y no constantes de una
    // unidad de traducción. Si algún día varían por especie, su sitio es USpeciesData.

    /** Radio de copa como fracción de la altura del árbol; proxy geométrico con el que la
        rejilla de luz deposita el área foliar. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float CanopyRadiusFraction = 0.30f;

    /**
     * Espesor vertical de la copa como fracción de la altura del árbol, es decir la
     * profundidad de la capa de área foliar contada desde el ápice hacia abajo.
     *
     * Debe ser bastante menor que 1: una copa real ocupa el tercio superior. Si el
     * depósito recibe la altura entera del árbol como profundidad de copa, la sombra se
     * reparte por todo el fuste, se desvanece en la cota del suelo y el sotobosque queda
     * a plena luz.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0.05", ClampMax = "1"))
    float CanopyDepthFraction = 0.30f;

    /** Índice de área foliar de la copa de un adulto, medido en su eje. La rejilla
        acumula ÁREA FOLIAR, no opacidad, que es lo que permite aplicar Beer-Lambert.
        4.0 es típico de un dosel cerrado de hoja ancha. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float CanopyLeafAreaIndex = 4.f;

    /** Coeficiente de extinción del dosel:
        @f$ Q = f + (1-f)\,e^{-k\,\mathrm{LAI}} @f$, con f el suelo difuso. En torno a
        0.5 en hoja ancha.
        @see @ref bib_monsisaeki1953 */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0"))
    float LightExtinctionK = 0.5f;

    /**
     * Suelo difuso: fracción mínima de luz del cielo que llega al sotobosque aunque el
     * dosel esté cerrado. Medida en campo, está entre el 1 y el 5% de la luz exterior.
     *
     * Su papel no es cosmético sino funcional: sin ese suelo, bajo un dosel muy denso la
     * luz tiende a 0 y con ella el factor de luz de todas las especies por igual, con lo
     * que la tolerante pierde su ventaja precisamente en la sombra profunda, que es el
     * único sitio donde debe ganar.
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "0.5"))
    float DiffuseLightFloor = 0.04f;

    /** Biomasa inicial de una plántula, como fracción de la MaxBiomass de su especie. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "0", ClampMax = "1"))
    float GerminationBiomassFraction = 0.01f;

    /** Lado (cm) de la celda del hash espacial, que se reconstruye cada tick desde el
        snapshot de lectura. Lo consumen el espaciado mínimo de germinación y las
        consultas por rango de los hero trees; la competencia por recursos NO pasa por
        él, sino por los campos compartidos. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "50"))
    float SpatialHashCellSizeCm = 500.f;

    /**
     * Número de árboles por tarea del ParallelFor del tick.
     *
     * El número de chunks es @f$\min(\lceil P/G \rceil,\ 32)@f$ —el tope de tareas del
     * tick también es constante— y NUNCA depende del número de hilos de la máquina. Eso
     * fija el orden de la reducción de deltas y es lo que garantiza que el resultado sea
     * bit a bit idéntico en cualquier CPU, porque la suma en coma flotante no es
     * asociativa. Más pequeño = más paralelismo pero más coste de reducción; 512 es el
     * punto medio para poblaciones de miles a decenas de miles, aunque a partir de 16.384
     * árboles se toca el tope de 32 chunks y bajar el grano ya no añade tareas.
     *
     * @see @ref bib_goldberg1991
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia", meta = (ClampMin = "1"))
    int32 TickChunkGrainSize = 512;

    // ==== Perturbación: claros, la dimensión TEMPORAL del nicho ====
    //
    // Sin perturbación cada árbol muere por su cuenta y su hueco es de un árbol: no hay
    // claros, y sin claros no hay sucesión, porque falta el tramo de alta luz en el que
    // la pionera es la mejor. El bucle que cierra la coexistencia es: claro -> luz alta
    // -> gana la pionera -> cierra el dosel -> la pionera ya no recluta bajo su propia
    // sombra pero la tolerante sí -> banco de plántulas -> cae el dominante -> la
    // tolerante hereda -> claro.

    /**
     * Fracción del área del mapa perturbada al año; su inverso es la rotación
     * (0.007/año equivale a una rotación de unos 140 años).
     *
     * @warning Arranca desactivado a propósito. Es el parámetro más sensible del modelo
     *          —demasiado frecuente y gana siempre la pionera, demasiado raro y gana
     *          siempre la climácica— y un claro sin banco de plántulas funcional solo
     *          beneficia a quien tiene más semillas, con lo que amplifica la exclusión
     *          en vez de corregirla.
     * @see @ref bib_dinamicadeclaros
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "0", ClampMax = "0.2"))
    float DisturbanceRatePerYear = 0.f;

    /** Área mínima de un claro (m2): la caída de un solo dominante. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "1"))
    float DisturbanceMinAreaM2 = 50.f;

    /** Área máxima de un claro (m2): el temporal raro de la cola de la distribución. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "1"))
    float DisturbanceMaxAreaM2 = 5000.f;

    /** Exponente de la ley potencia de la que se muestrea el área del claro, truncada
        entre el mínimo y el máximo anteriores. Más alto = más claros pequeños.
        @see @ref bib_leypotenciaclaros */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "1", ClampMax = "5"))
    float DisturbanceAreaExponent = 2.f;

    /** Probabilidad de que un árbol dentro del claro caiga. Por debajo de 1 quedan pies
        residuales dentro del hueco. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|Perturbacion", meta = (ClampMin = "0", ClampMax = "1"))
    float DisturbanceMortality = 0.9f;

    // ================================================================
    // ==== Presentación: el puente de escala entre simulación y render ====
    // ================================================================
    //
    // Tres bandas de distancia: hero con geometría completa, malla instanciada por
    // bucket de tamaño e impostor de campo lejano. Dentro de cada especie el tamaño se
    // discretiza en buckets, porque a diferencia de un LOD clásico aquí los árboles
    // crecen y cambian de representación por sí solos.

    /** Interruptor maestro de la capa de render instanciada. Apagado deja solo la
        simulación y las esferas de diagnóstico, que es el estado de referencia para
        ablacionar toda la capa de presentación. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bEnableTreeRendering = true;

    /** Buckets de tamaño en los que se discretiza cada especie. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1", ClampMax = "32"))
    int32 NumAgeBuckets = 5;

    /** Histéresis del cambio de bucket, en fracción de bucket: evita que un árbol parado
        en el borde dé de alta y de baja su instancia en ticks alternos.
        @see @ref bib_clarkjh1976 */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "0.49"))
    float BucketHysteresis = 0.15f;

    /** Radio (cm) dentro del cual un árbol puede ser hero, es decir generarse con
        geometría completa por colonización del espacio. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float HeroRadiusCm = 6000.f;   // 60 m

    /** Máximo de hero trees simultáneos: un working set de decenas. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    int32 HeroBudget = 24;

    /** Hero trees generados por frame. Amortiza el coste de la colonización del espacio
        para que no se traduzca en tirones. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 MaxHeroPerFrame = 1;

    /** Distancia (cm) a partir de la cual se dibuja el impostor en vez de la malla.
        @see @ref bib_impostores */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float ImpostorRadiusCm = 25000.f;   // 250 m

    /** Más allá de esta distancia (cm) el árbol no se dibuja: el campo lejano lo cubre
        el HLOD de World Partition. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float CullRadiusCm = 120000.f;      // 1.2 km

    /** Cadencia (en frames) del re-nivelado completo, la reasignación de todos los
        árboles a banda de LOD y bucket. Los árboles se mueven despacio respecto a la
        cámara, así que no hace falta cada frame. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 RelevelEveryNFrames = 5;

    /** Variación aleatoria de escala por instancia, en fracción: 0.1 da el rango
        0.9-1.1. Rompe la repetición visual dentro de un mismo bucket. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "0.5"))
    float InstanceScaleJitter = 0.1f;

    /** Cambio mínimo de escala que justifica reescribir la transformada de una
        instancia; por debajo, el cambio es invisible y no compensa el coste. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0"))
    float ScaleUpdateThreshold = 0.02f;

    /** Arquetipos horneados por frame cuando se piden bajo demanda. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "1"))
    int32 MaxBakesPerFrame = 2;

    /** Hornea la librería entera al arrancar: un tirón inicial de ~1 s a cambio de coste
        cero después. Es lo indicado para medir framerate sin ruido de horneado. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bPrebakeLibraryOnStart = false;

    /** Las instancias cercanas proyectan sombra. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bInstancesCastShadow = true;

    /** Los impostores proyectan sombra. Por defecto no: la sombra del campo lejano la
        aporta el proxy HLOD, y un crossboard proyectaría una silueta cruzada. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD")
    bool bImpostorsCastShadow = false;

    /** Floats de PerInstanceCustomData que se reservan por instancia:
        [0] fase estacional del árbol, [1] sequedad (0 sano, 1 seco-senescente) y
        [2] apertura de copa para la oclusión ambiental. Bajarlo a 2 no rompe nada: el
        material se queda sin el canal de oclusión y lo lee como 0. */
    UPROPERTY(EditAnywhere, config, Category = "Render|LOD", meta = (ClampMin = "0", ClampMax = "4"))
    int32 NumInstanceCustomDataFloats = 3;

    // ==== Assets de especie y de diagnóstico ====

    /** Catálogo de especies del mundo. El índice en este array es el identificador de
        especie que usan la población y el resto de capas. */
    UPROPERTY(EditAnywhere, config, Category = "Especies")
    TArray<TSoftObjectPtr<USpeciesData>> Species;

    /** Material de decal (dominio Deferred Decal) con un parámetro de textura "FieldTex",
        con el que se pintan sobre el terreno los campos escalares. */
    UPROPERTY(EditAnywhere, config, Category = "Debug")
    TSoftObjectPtr<UMaterialInterface> HeatmapDecalMaterial;

    // ================================================================
    // ==== Bosque vivo: continuidad visual del crecimiento y de la muerte ====
    // ================================================================

    /** Interpola la escala de los hero trees entre ticks para que el crecimiento se vea
        continuo y no a saltos discretos de tick. */
    UPROPERTY(EditAnywhere, config, Category = "BosqueVivo")
    bool bSmoothHeroGrowth = true;

    /** Constante de tiempo (s) del suavizado exponencial de la escala del hero. */
    UPROPERTY(EditAnywhere, config, Category = "BosqueVivo", meta = (ClampMin = "0.01"))
    float HeroGrowthSmoothingSeconds = 0.6f;

    /** Muertes recientes que la simulación conserva en un anillo para que la capa de
        suelo levante tocones y hojarasca a partir de ellas. */
    UPROPERTY(EditAnywhere, config, Category = "BosqueVivo", meta = (ClampMin = "0"))
    int32 DeathEventBufferSize = 256;

    // ==== Ciclo estacional del follaje ====

    /** Colección de parámetros de material con los escalares "Season", en [0,1), y
        "Snow": el material de follaje los lee para tintar y secar la hoja y para mezclar
        la nieve. Si es nulo, el ciclo estacional no se aplica. */
    UPROPERTY(EditAnywhere, config, Category = "Estaciones")
    TSoftObjectPtr<UMaterialParameterCollection> SeasonMPC;

    /** La estación avanza sola con el tiempo, sin intervención. */
    UPROPERTY(EditAnywhere, config, Category = "Estaciones")
    bool bAutoAdvanceSeason = true;

    /**
     * La estación sigue el AÑO SIMULADO en vez del reloj de pared.
     *
     * Es lo coherente mientras la simulación corre: con los valores por defecto (0.5 s
     * por tick, 1 año por tick y un año visual de 24 s) el reloj de pared daría una sola
     * primavera mientras pasan 48 años simulados, o sea que follaje y ecología contarían
     * calendarios distintos. Con la simulación pausada se cae automáticamente al reloj de
     * VisualYearSeconds, para poder animar la estación sobre un bosque congelado.
     */
    UPROPERTY(EditAnywhere, config, Category = "Estaciones")
    bool bSeasonFollowsSimClock = true;

    /** Segundos reales que dura un ciclo estacional completo cuando la estación va con
        el reloj de pared. */
    UPROPERTY(EditAnywhere, config, Category = "Estaciones", meta = (ClampMin = "0.1"))
    float VisualYearSeconds = 24.f;

    // ==== Capa de suelo: tocones, madera muerta y hojarasca ====
    //
    // Cada muerte deja un tocón que aguanta en pie, cae y permanece como tronco tumbado
    // antes de retirarse, más unas tarjetas de hojarasca alrededor. Tocones y hojarasca
    // viven en anillos de tamaño fijo: al llenarse se reutiliza el más viejo.

    /** Interruptor maestro de la capa de suelo. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo")
    bool bEnableSoilLayer = true;

    /** Malla del tocón y del tronco caído; un cilindro basta. Si es nula, no se generan
        tocones. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo")
    TSoftObjectPtr<UStaticMesh> SnagMesh;

    /** Malla de la hojarasca, una tarjeta plana. Si es nula, no se genera hojarasca. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo")
    TSoftObjectPtr<UStaticMesh> LitterMesh;

    /** Material de la madera muerta. Opcional: si es nulo se usa el de la malla. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo")
    TSoftObjectPtr<UMaterialInterface> SnagMaterial;

    /** Material de la hojarasca. Opcional; encaja el mismo material de hoja otoñal. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo")
    TSoftObjectPtr<UMaterialInterface> LitterMaterial;

    /** Tamaño del anillo de tocones y troncos simultáneos. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0"))
    int32 MaxSnags = 512;

    /** Tamaño del anillo de tarjetas de hojarasca simultáneas. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0"))
    int32 MaxLitter = 4096;

    /** Altura del tocón como fracción de la altura que tenía el árbol al morir. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0.05", ClampMax = "1"))
    float SnagHeightFraction = 0.45f;

    /** Segundos reales que tarda el tocón en caer y quedar como tronco tumbado. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0.1"))
    float SnagFallSeconds = 4.f;

    /** Segundos que el tocón aguanta EN PIE antes de empezar a caer: un árbol muerto no
        se desploma en el instante en que muere, y esa permanencia es ecológicamente
        relevante. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0"))
    float SnagStandingSeconds = 6.f;

    /**
     * Segundos que el tronco tumbado permanece como madera muerta antes de retirarse.
     * 0 = no se retira nunca.
     *
     * @note Conviene cuadrarlo con la mancha de descomposición del terreno, que decae
     *       como @f$ e^{-\lambda t} @f$ en años simulados: con 0.5 s por año simulado y
     *       @f$ \lambda = 0.5 @f$ la mancha se apaga en unos 4 años, o sea unos 2 s
     *       reales. Para que desaparezcan a la vez hay que subir
     *       DecompositionDecayPerYear o bajar este valor.
     */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0"))
    float SnagLogSeconds = 20.f;

    /** Tarjetas de hojarasca esparcidas por cada muerte. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0"))
    int32 LitterPerDeath = 6;

    /** Lado (cm) en mundo de una tarjeta de hojarasca. La escala del componente se deriva de
        los bounds reales de LitterMesh, así que este valor es el tamaño que se ve sea
        cual sea la malla asignada. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "1"))
    float LitterCardCm = 70.f;

    /** Altura (cm) a la que se levanta la hojarasca sobre el terreno, contra el
        z-fighting con el material del suelo. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0"))
    float LitterGroundOffsetCm = 3.f;

    /** La capa de suelo se apaga también al apagar la capa de árboles
        (@ref bEnableTreeRendering). Es lo coherente cuando se ablaciona la presentación
        entera, porque tocones y hojarasca forman parte de ella; a false se estudian por
        separado. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo")
    bool bSoilFollowsTreeRendering = true;

    /** Radio (cm) en el que se dispersa la hojarasca alrededor del árbol muerto. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo", meta = (ClampMin = "0"))
    float LitterRadiusCm = 300.f;

    /** Los tocones y troncos proyectan sombra. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo")
    bool bSnagsCastShadow = true;

    // ==== Descomposición visible en el terreno ====

    /** Tasa de decaimiento exponencial por año de la mancha de descomposición. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo|Descomposicion", meta = (ClampMin = "0"))
    float DecompositionDecayPerYear = 0.5f;

    /** Escala del pulso de descomposición depositado al morir un árbol. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo|Descomposicion", meta = (ClampMin = "0"))
    float DecompositionPulseScale = 1.f;

    /** Valor que se pinta como máximo del campo de descomposición. El rango es FIJO, y no
        el máximo instantáneo del campo, para que la intensidad de una mancha no cambie de
        un tick a otro por lo que ocurra en el resto del mapa. */
    UPROPERTY(EditAnywhere, config, Category = "Suelo|Descomposicion", meta = (ClampMin = "0.001"))
    float DecompositionPaintMax = 20.f;

    // ================================================================
    // ==== Viento ====
    // ================================================================
    //
    // El movimiento lo hace el MATERIAL en el vertex shader, por World Position Offset.
    // Desde C++ solo se empuja, una vez por frame y para todo el bosque, el estado global
    // del viento a una colección de parámetros de material: coste O(1) y cero trabajo por
    // árbol. La variedad por rama y por árbol viaja horneada en los canales UV de la
    // malla (ver Geometry/TreeWindData.h).

    /**
     * Colección de parámetros de material del viento. El subsistema de render escribe en
     * ella los escalares WindStrength (fuerza ya modulada por las ráfagas), WindGust (el
     * valor de ráfaga crudo en [0,1]), WindTime (reloj propio del viento, en segundos) y
     * WindWpoCutoff (distancia en cm a partir de la cual no debe haber balanceo), más el
     * vector WindDirection, unitario y en el plano horizontal.
     *
     * Si es nula, el viento no se aplica. Puede ser el mismo asset que SeasonMPC: los
     * nombres de parámetro no chocan.
     *
     * @see @ref bib_vientovegetacion
     */
    UPROPERTY(EditAnywhere, config, Category = "Viento")
    TSoftObjectPtr<UMaterialParameterCollection> WindMPC;

    /** Interruptor maestro del viento. Apagado fuerza WindStrength a 0, con lo que el
        material deja de desplazar vértices y desaparece su coste. */
    UPROPERTY(EditAnywhere, config, Category = "Viento")
    bool bEnableWind = true;

    /** Dirección base del viento, en grados de yaw de mundo. */
    UPROPERTY(EditAnywhere, config, Category = "Viento", meta = (ClampMin = "-360", ClampMax = "360"))
    float WindDirectionDeg = 45.f;

    /** Oscilación lenta de la dirección, en grados a cada lado. Una dirección
        perfectamente fija se lee como artificial enseguida. */
    UPROPERTY(EditAnywhere, config, Category = "Viento", meta = (ClampMin = "0", ClampMax = "90"))
    float WindDirectionWanderDeg = 12.f;

    /** Fuerza base del viento; el material la escala a su amplitud en centímetros. */
    UPROPERTY(EditAnywhere, config, Category = "Viento", meta = (ClampMin = "0", ClampMax = "4"))
    float WindStrength = 0.35f;

    /** Amplitud de las ráfagas como fracción de la fuerza base: 0 da viento constante,
        que se nota falso, y 1 va de la calma total al doble de fuerza. */
    UPROPERTY(EditAnywhere, config, Category = "Viento", meta = (ClampMin = "0", ClampMax = "1"))
    float WindGustAmplitude = 0.5f;

    /** Periodo (s) de la ráfaga principal. Se compone con un segundo seno de periodo
        inconmensurable con éste para que no se perciba el bucle. */
    UPROPERTY(EditAnywhere, config, Category = "Viento", meta = (ClampMin = "0.1"))
    float WindGustPeriodSeconds = 7.f;

    /**
     * Distancia (cm) a partir de la cual los componentes de instancing dejan de evaluar
     * el World Position Offset del viento. 0 = sin corte.
     *
     * El WPO sobre geometría masiva tiene un coste de vertex shader que no compensa a
     * distancia: a 120 m un balanceo de 20 cm es sub-píxel, así que no se pierde nada
     * visible y se recorta el coste en la mayor parte del bosque.
     */
    UPROPERTY(EditAnywhere, config, Category = "Viento", meta = (ClampMin = "0"))
    float WindWpoCutoffCm = 12000.f;   // 120 m

    /** Los impostores se mueven con el viento. Por defecto no: son el campo lejano y su
        geometría es un crossboard, sobre el que el balanceo se leería como un
        cizallamiento. */
    UPROPERTY(EditAnywhere, config, Category = "Viento")
    bool bWindOnImpostors = false;

    /** Factor de agrandado de la caja envolvente de los componentes con viento. El WPO
        mueve vértices que el culling no ve; sin margen, un árbol al borde del encuadre
        desaparece de golpe con las ramas todavía dentro.
        @note Es la única fuente de este valor: lo leen tanto los componentes de
              instancing como los hero trees. */
    UPROPERTY(EditAnywhere, config, Category = "Viento", meta = (ClampMin = "1", ClampMax = "3"))
    float WindBoundsScale = 1.15f;

    // ================================================================
    // ==== Materiales ====
    // ================================================================

    /**
     * Escribe en PerInstanceCustomData[2] la apertura de copa de cada árbol: la luz de la
     * rejilla gruesa muestreada a media altura de su copa, 1 a pleno sol y 0 bajo dosel
     * cerrado. El material la usa como término de oclusión ambiental, de modo que un
     * árbol del sotobosque se vea más apagado que uno emergente sin necesidad de
     * iluminación global.
     *
     * @note Cuesta un muestreo trilineal por árbol instanciado y por re-nivelado, no por
     *       frame ni por impostor.
     */
    UPROPERTY(EditAnywhere, config, Category = "Materiales")
    bool bCanopyAOInstanceData = true;

    /** Nieve máxima en pleno invierno. Se escribe como escalar "Snow" en SeasonMPC y el
        material la mezcla según la normal hacia arriba. 0 = sin nieve, que es lo propio
        de un bosque templado o de laurisilva. */
    UPROPERTY(EditAnywhere, config, Category = "Materiales", meta = (ClampMin = "0", ClampMax = "1"))
    float MaxSnowAmount = 0.f;

    // ================================================================
    // ==== Perfil vertical de CO2 ====
    // ================================================================
    //
    // Multiplicador analítico del vigor dependiente solo de la altura: sin simulación
    // volumétrica ni campo nuevo. La fórmula y su justificación viven en
    // Ecology/CarbonModel.h.

    /**
     * Aplica el multiplicador de CO2 al vigor.
     *
     * @warning Cambia el resultado de la simulación: hay que apagarlo aquí o con el CVar
     *          Eco.CO2.Enable para reproducir bit a bit una corrida hecha sin él.
     * @see @ref bib_co2dosel
     */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|CO2")
    bool bEnableCO2Factor = true;

    /** Reducción máxima del vigor bajo dosel cerrado, en fracción. El efecto es leve por
        diseño: entre 0.10 y 0.20 es el rango sensato. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|CO2", meta = (ClampMin = "0", ClampMax = "0.9"))
    float CO2MaxReduction = 0.15f;

    /** Altura (cm) por encima de la cual el aire se considera bien mezclado y la
        penalización desaparece. Su valor es la altura del dosel dominante. */
    UPROPERTY(EditAnywhere, config, Category = "Ecologia|CO2", meta = (ClampMin = "1"))
    float CO2FullMixingHeightCm = 2500.f;

    // ================================================================
    // ==== Presupuesto de frame e instrumentación ====
    // ================================================================

    /**
     * Presupuesto de tiempo (ms) que el TICK de simulación puede consumir dentro de un
     * frame. Al agotarlo, los ticks pendientes se dejan para el frame siguiente aunque no
     * se haya llegado a MaxStepsPerFrame. 0 = sin límite.
     *
     * Acotar el número de ticks no es acotar el coste: con 20.000 árboles un solo tick
     * puede pasarse de presupuesto y con 200 caben veinte. Por eso las dos cotas, la de
     * número y la de tiempo, actúan juntas.
     *
     * @see @ref bib_fiedler2004
     */
    UPROPERTY(EditAnywhere, config, Category = "Rendimiento", meta = (ClampMin = "0"))
    float TickBudgetMsPerFrame = 4.f;

    /** Objetivo de frame completo (ms) contra el que se compara el reparto medido.
        16.6 ms son 60 fps. */
    UPROPERTY(EditAnywhere, config, Category = "Rendimiento", meta = (ClampMin = "1"))
    float FrameBudgetMs = 16.6f;

    /** Muestra en pantalla el reparto del frame y la población. */
    UPROPERTY(EditAnywhere, config, Category = "Rendimiento")
    bool bShowFrameBudgetHUD = false;

    /** Acceso canónico a los ajustes: devuelve el objeto por defecto de la clase, ya
        deserializado desde el .ini. */
    static const UEcosystemSettings* Get() { return GetDefault<UEcosystemSettings>(); }
};
