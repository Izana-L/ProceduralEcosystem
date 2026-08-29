/**
 * @file SpeciesData.h
 * @author Juan Luque Roldán
 * @brief Ficha de rasgos de una especie arbórea, editable como asset de datos.
 *
 * Declara USpeciesData, que reúne en una única ficha todo lo que distingue a una especie de
 * otra a lo largo de las cuatro capas del simulador: demografía y nicho de recurso,
 * morfología del generador de árboles (copa, tropismos, pipe model, tronco, mallado y
 * follaje), niveles de representación y materiales, y respuesta al viento. Declara también
 * las formas enumeradas que lo parametrizan y la capa de deformación de tronco. Los datos
 * viven aquí y las reglas fuera: la población referencia especies por un identificador de
 * 16 bits y ningún agente copia parámetros, lo que mantiene el tick paralelo y determinista.
 * Su único código ejecutable valida el asset en tiempo de editor.
 *
 * @ingroup eco_species
 * @see @ref bib_gapmodels
 * @see @ref bib_epicueconfig
 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeciesData.generated.h"
class UMaterialInterface;



/**
 * Forma de la envolvente de copa dentro de la cual se siembran los atractores del
 * algoritmo de colonización del espacio.
 *
 * Es el molde cerrado que fija la silueta antes de que crezca una sola rama: la misma
 * nube de atractores, con distinta forma de envolvente, ya diferencia una conífera de
 * un roble.
 *
 * @see @ref bib_weberpenn1995
 */
UENUM(BlueprintType)
enum class ECrownShape : uint8
{
    Conical,    ///< Cónica y estrecha: conífera excurrente.
    Spherical,  ///< Esférica y ancha: roble decurrente.
    Columnar    ///< Alta y estrecha: ciprés.
};

/**
 * Cómo se dobla el tronco de un árbol al que le ha tocado una capa de deformación.
 *
 * Cada tipo es un operador de doblado global parametrizado por la altura normalizada del
 * eje, aplicado sobre el esqueleto ya terminado.
 *
 * @see FTrunkDeformLayerSpec
 * @see @ref bib_barr1984
 */
UENUM(BlueprintType)
enum class ETrunkDeformType : uint8
{
    /** Inclinación rígida: el árbol entero gira el mismo ángulo alrededor de su pie, con
        la lectura de «plantado torcido» o crecido en ladera.
        @note Por encima de unos 10 grados se lee como que el árbol se cae. */
    Lean,

    /** Arqueado progresivo, @f$ \theta \cdot t^{k} @f$ con @f$ t @f$ la altura
        normalizada: sale vertical del suelo y se va tumbando con la altura. Es el único
        tipo cuya lectura mejora con ángulos grandes. */
    Arc,

    /** Sinuoso: el tronco serpentea con ShapeParam ondas repartidas en toda la altura.
        Con 1.5-2 se lee como madera de ribera; por encima, como ruido. */
    SCurve
};

/**
 * Una capa de deformación de tronco declarada por la especie.
 *
 * Cada árbol tira los dados por cada capa del array de forma independiente, así que la
 * especie no describe «cómo es su tronco» sino la distribución de troncos que produce:
 * con Probability 0.4 sobre una capa Arc, cuatro de cada diez individuos salen arqueados
 * y el resto rectos, y los cuatro con ángulos y azimuts distintos. Componer varias capas
 * da formas que ninguna alcanza sola (Arc suave más SCurve pequeña: tronco volcado y
 * además ondulado).
 *
 * @note El deformador consume un número fijo de muestras por capa, en orden de array y
 *       antes de aplicar la puerta de probabilidad. Añadir capas al final es seguro;
 *       reordenarlas o insertar en medio reparte otras formas a otros árboles.
 */
USTRUCT(BlueprintType)
struct FTrunkDeformLayerSpec
{
    GENERATED_BODY()

    /** Operador de doblado que aplica esta capa. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion")
    ETrunkDeformType Type = ETrunkDeformType::Arc;

    /** Probabilidad de que a un árbol le toque esta capa, en [0..1]. 0 la desactiva sin
        dejar de consumir sus muestras. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0", ClampMax = "1"))
    float Probability = 0.25f;

    /** Extremo inferior del ángulo de la capa, en grados; se sortea uniforme en
        [MinAngleDeg, MaxAngleDeg]. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0", ClampMax = "45"))
    float MinAngleDeg = 5.f;

    /** Extremo superior del ángulo de la capa, en grados. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0", ClampMax = "45"))
    float MaxAngleDeg = 18.f;

    /** Exponente @f$ t^{k} @f$ del arqueo en Arc, o número de ondas en SCurve; ignorado
        en Lean. En Arc, por debajo de 1 arquea ya desde abajo y por encima mantiene el
        pie recto y vuelca arriba. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0.25", ClampMax = "6"))
    float ShapeParam = 1.5f;
};

/**
 * Catálogo de rasgos de una especie arbórea.
 *
 * Asset compartido y de solo lectura en runtime: la población guarda por agente un
 * identificador de especie —el índice del asset en la lista resuelta del subsistema— y
 * nunca una copia de estos parámetros. Antes de cada tick, las respuestas de recurso se
 * aplanan a estructuras planas para que el bucle paralelo no lea ni un UObject.
 *
 * Los rasgos se agrupan en cuatro bloques independientes entre sí: demografía y nicho
 * (los consume la simulación de población), morfología del generador de árboles, niveles
 * de representación y materiales, y respuesta al viento.
 *
 * @note Varios rasgos actúan como divisores en las fórmulas de población y geometría
 *       (MaxBiomass, Longevity, WaterDemand, NutrientDemand, TrunkFlareHeightCm,
 *       LeafSpacingCm). Su ClampMin es un positivo pequeño y no cero: un asset a cero
 *       produciría NaN o infinitos en cuanto arrancase la simulación.
 * @see USpeciesData::IsDataValid
 */
UCLASS(BlueprintType)
class PROCEDURALECOSYSTEM_API USpeciesData : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    // ==== IDENTIDAD Y DIAGNÓSTICO ====

    /** Nombre legible de la especie; aparece en los volcados y en la auditoría. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identidad")
    FName SpeciesName = TEXT("Especie");

    /** Color con el que la especie se dibuja en el heatmap y en las esferas de depuración. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identidad")
    FColor DebugColor = FColor::Green;

    // ==== CICLO VITAL ====

    /** Tasa intrínseca de crecimiento del logístico, por año simulado y a vigor 1. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0"))
    float GrowthRate = 0.25f;

    /** Capacidad de carga del árbol adulto: divisor del término @f$ 1 - B/B_{max} @f$ del
        crecimiento logístico, y referencia con la que se normalizan altura, fecundidad y
        biomasa inicial de una plántula. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0.01"))
    float MaxBiomass = 100.f;

    /**
     * Escala del riesgo de mortalidad por edad, en años simulados.
     *
     * No es la edad a la que muere el árbol: la mortalidad por edad es un riesgo
     * acumulativo @f$ p_{edad} = \Delta t\,(Edad/Longevity)^4 @f$ por año, de modo que la
     * edad mediana de muerte de una cohorte vale @f$ T \approx 1.282\,Longevity^{0.8} @f$
     * con un año por tick, y se calibra a la inversa con
     * @f$ Longevity = (T/1.282)^{1.25} @f$. Esa mediana ha de quedar holgadamente por
     * encima del tiempo que tarda el logístico en llenar el árbol —a vigor medio, unos 55
     * años hasta el 95 % de MaxBiomass—, o la población muere antes de terminar de crecer
     * y el bosque se queda en plántulas. Referencias: 400 da una mediana de ~155 años
     * (pionera de vida corta), 600 de ~214 y 800 de ~269 (árbol de dosel longevo).
     *
     * @warning Divisor de la fórmula de mortalidad: debe ser > 0.
     * @see EcologyRules::AgeMortalityProbability
     * @see @ref bib_weibull1951
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0.01"))
    float Longevity = 600.f;

    /** Edad, en años simulados, a partir de la cual el árbol es adulto y produce semilla. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0"))
    float MaturityAge = 20.f;

    /** Altura en cm de un árbol con Biomass igual a MaxBiomass: es la escala de la
        alometría que traduce biomasa a altura. @see EcologyRules::HeightFromBiomass */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "1"))
    float MaxHeightCm = 2000.f; // 20 m por defecto

    // ==== RECURSOS: CUÁNTO CONSUME LA ESPECIE ====

    /** Posición de la especie en el eje sol-sombra: 0 es heliófila —rinde mucho a pleno sol
        y se apaga en penumbra— y 1 tolerante a la sombra. El coste de la tolerancia (menor
        asimilación máxima) lo aplica el vigor, no este campo.
        @see EcoVigor::MakeSpeciesResponses */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0", ClampMax = "1"))
    float ShadeTolerance = 0.5f;

    /**
     * Consumo de agua por unidad de biomasa y año.
     *
     * Con la respuesta de nicho activa —el modo por defecto— este rasgo no entra en el
     * vigor: solo fija cuánta agua retira el árbol del pozo de su celda, y la idoneidad la
     * deciden WaterOptimum y WaterTolerance. Mantener separadas respuesta y consumo es lo
     * que hace posible la coexistencia: mientras un mismo número era divisor de la
     * respuesta y multiplicador del gasto, bajarlo daba dos ventajas a la vez y sin coste.
     * Con la respuesta de nicho desactivada vuelve a ser el divisor de la función de
     * saturación de Monod.
     *
     * @warning Divisor en ese segundo modo: debe ser > 0.
     * @see @ref bib_monod1949
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0.001"))
    float WaterDemand = 1.f;

    /** Consumo de nutrientes por unidad de biomasa y año, con el mismo doble papel que
        WaterDemand. @see WaterDemand */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0.001"))
    float NutrientDemand = 1.f;

    // ==== NICHO DE RECURSO: DÓNDE ESTÁ MEJOR LA ESPECIE ====
    //
    // Óptimo y anchura de una respuesta unimodal, expresados como fracciones [0..1] del
    // máximo del campo (WaterOutputMax / NutrientOutputMax de los ajustes) y no en unidades
    // absolutas: así, cambiar el rango de salida de un campo no invalida en silencio la
    // calibración de todas las especies. Son la palanca del reparto de nicho: dos especies
    // con óptimos separados se quedan cada una con una parte del mapa —una la vaguada, otra
    // la cresta— y coexisten sin necesidad de tener los números empatados a mano.
    // Las fracciones las resuelve a absolutos EcoVigor::MakeSpeciesResponses.

    /**
     * Humedad óptima, como fracción de WaterOutputMax. 0.2 es ladera seca; 0.8, fondo de
     * barranco.
     *
     * @see @ref bib_nichounimodal
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos|Nicho", meta = (ClampMin = "0", ClampMax = "1"))
    float WaterOptimum = 0.55f;

    /** Anchura de la campana de humedad, como fracción de WaterOutputMax: a una anchura del
        óptimo el factor de agua ha caído a 0.37. Estrecha describe a un especialista; ancha,
        a un generalista, que es una ventaja y hay que compensar en otro eje. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos|Nicho", meta = (ClampMin = "0.02", ClampMax = "2"))
    float WaterTolerance = 0.35f;

    /** Si está activo, el exceso de agua también penaliza (encharcamiento, anoxia) y la
        campana es simétrica. Mantenerlo activo es lo que impide que la especie de vaguada
        colonice además la cresta y vuelva a barrer el mapa entero. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos|Nicho")
    bool bWaterloggingPenalty = true;

    /** Fertilidad óptima, como fracción de NutrientOutputMax. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos|Nicho", meta = (ClampMin = "0", ClampMax = "1"))
    float NutrientOptimum = 0.55f;

    /** Anchura de la campana de fertilidad, como fracción de NutrientOutputMax. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos|Nicho", meta = (ClampMin = "0.02", ClampMax = "2"))
    float NutrientTolerance = 0.40f;

    /** Desactivado, la respuesta satura en 1 por encima del óptimo: un suelo más rico de lo
        que la especie necesita no le hace daño, a diferencia del encharcamiento. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos|Nicho")
    bool bNutrientExcessPenalty = false;

    /** Radio de la zona de influencia radicular, en metros: el área alrededor del árbol de
        la que retira agua y nutrientes. El radio efectivo crece con la biomasa del
        individuo, así que este valor es el del adulto. @see @ref bib_zonadeinfluencia */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0"))
    float RootRadius = 2.f;

    // ==== DISPERSIÓN Y RECLUTAMIENTO ====

    /** Radio, en metros, del disco alrededor del árbol madre donde caen sus semillas. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dispersión", meta = (ClampMin = "0"))
    float SeedDispersalRadius = 15.f; // m

    // ---- Fecundidad frente a establecimiento: el compromiso r/K ----
    //
    // Los dos multiplicadores siguientes van en sentidos opuestos por diseño. Son el tercer
    // compromiso clásico de la dinámica forestal, tras tolerancia frente a crecimiento y
    // crecimiento frente a longevidad, y el modelo lo necesita explícito porque nada se lo
    // impone: una especie con SeedRateScale 3 y GerminationRateScale 1.5 a la vez es una
    // estrategia dominante y se lleva el bosque. Pionera —mucha semilla pequeña que arraiga
    // mal— 2.5 / 0.6; climácica —poca semilla grande que arraiga bien— 0.5 / 1.6.

    /**
     * Multiplicador de fecundidad de la especie sobre la tasa global
     * SeedsPerAdultPerYear, aplicado sobre la biomasa relativa del árbol.
     *
     * @see @ref bib_estrategiark
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dispersión", meta = (ClampMin = "0.01", ClampMax = "10"))
    float SeedRateScale = 1.f;

    /** Multiplicador de la probabilidad de arraigar, sobre la tasa global GerminationRate:
        la reserva de una semilla grande le da más margen para instalarse. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dispersión", meta = (ClampMin = "0.01", ClampMax = "10"))
    float GerminationRateScale = 1.f;

    /**
     * Nicho de regeneración: luz mínima, como fracción de sol pleno, que necesita una
     * semilla de esta especie para germinar.
     *
     * Es un rasgo de especie y no un umbral global, y en esa diferencia se juega la
     * coexistencia, por una razón aritmética antes que ecológica. Con un umbral común, todo
     * el reclutamiento ocurre en claros y el claro se lo lleva quien mande más semillas: una
     * especie abundante siembra órdenes de magnitud más que una rara, se queda todos los
     * huecos y la rareza se vuelve una trampa sin salida. Es un efecto de prioridad, y
     * ningún ajuste de vigor, demanda o mortalidad lo corrige, porque todos actúan después
     * de decidir quién ocupa el sitio. Con umbral por especie, el sotobosque en penumbra
     * queda vetado a la pionera —allí sus semillas valen cero— y la tolerante acumula debajo
     * un banco de plántulas suprimidas que hereda el hueco cuando el árbol de dosel muere,
     * sin pasar por ninguna lotería de semillas: la regeneración avanzada del bosque real.
     *
     * Valores guía: pionera heliófila 0.5-0.6, intermedia 0.3-0.4, climácica de sotobosque
     * 0.05-0.15.
     *
     * @see EcologyRules::IsSafeGerminationSite
     * @see @ref bib_dinamicadeclaros
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dispersión", meta = (ClampMin = "0", ClampMax = "1"))
    float MinLightForGermination = 0.5f;
    // ================================================================
    // ==== MORFOLOGÍA: GEOMETRÍA DEL ÁRBOL INDIVIDUAL ====
    // ================================================================
    // Ninguno de los rasgos que siguen interviene en la simulación de población: los
    // consume la generación geométrica, por colonización del espacio, de la malla de un
    // árbol. La misma dirección de crecimiento ponderada produce siluetas muy distintas
    // cambiando los pesos de tropismo, y ésa es la principal palanca de variedad entre
    // especies. Todas las longitudes van en cm, como en el resto del proyecto, y quien los
    // ejecuta es el namespace SpaceColonization.

    // ---- Envolvente de copa: dónde se siembran los atractores ----

    /** Molde geométrico dentro del cual se siembra la nube de atractores. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa")
    ECrownShape CrownShape = ECrownShape::Spherical;

    /** Radio horizontal de la copa, en cm. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    float CrownRadiusCm = 400.f; // 4 m

    /** Altura vertical de la copa, de su base al ápice, en cm. La altura total del árbol se
        deriva de ella y de TrunkFraction. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    float CrownHeightCm = 800.f; // 8 m

    /** Fracción de la altura total ocupada por tronco desnudo bajo la copa, en [0..1). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "0.95"))
    float TrunkFraction = 0.3f;

    /** Número de atractores sembrados en la copa: más atractores dan una copa más tupida y
        cuestan más tiempo de generación. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    int32 NumAttractors = 400;

    /**
     * Hasta dónde sube el eje principal dentro de la copa, como fracción de su altura: 0
     * deja morir el tronco en la base de la copa y 1 es un líder central que llega al ápice.
     *
     * Es la palanca de la arquitectura excurrente frente a la decurrente, y lo que evita el
     * artefacto de que todas las ramas salgan de la cima del tronco: con el eje terminado en
     * la base de la copa, ese nodo es el único que ve los atractores del anillo más ancho
     * —en una copa cónica el radio máximo cae justo ahí— y se los lleva todos, de donde sale
     * la silueta de paraguas. Con el eje atravesando la copa hay nodos a todas las alturas y
     * las ramas se reparten por el fuste. El afilado del eje no necesita entonces ningún
     * truco: cada rama lateral aporta su @f$ r^n @f$ al eje por debajo de su inserción, y el
     * pipe model lo deja grueso abajo y fino arriba. Conífera 0.9-1.0; roble o haya 0.3-0.5.
     *
     * @see @ref bib_halle1978
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "1"))
    float LeaderFraction = 0.5f;

    /**
     * Amplitud del ruido que deforma el contorno de la envolvente de copa: 0 respeta el
     * molde exacto y 0.25 da lóbulos y golfos de aspecto natural.
     *
     * @note El ruido es coherente en azimut y altura, no blanco. Con ruido blanco la
     *       silueta no cambiaría, porque el máximo estadístico de cientos de muestras
     *       reconstruye la envolvente original.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "0.6"))
    float EnvelopeNoise = 0.25f;

    /**
     * Sesgo vertical de la densidad de atractores dentro de la copa, como exponente de la
     * altura normalizada: 1 la reparte uniforme, por debajo de 1 lleva masa hacia el ápice
     * y por encima la baja hacia la base. Controla dónde está el grueso de la copa sin
     * cambiar su forma.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0.25", ClampMax = "4"))
    float CrownVerticalBias = 1.f;

    /**
     * Fracción de atractores sembrados por debajo de la base de copa, con la densidad
     * cayendo hacia el suelo: la falda de sub-copa.
     *
     * Sin ella el tronco desnudo es zona prohibida para las ramas y la copa arranca de golpe
     * en un plano, que es lo que se lee como artificial. Un 10-15 % basta para las ramas
     * bajas dispersas de un árbol real, y además garantiza que el nodo base tenga atractores
     * en rango aunque el radio de influencia no llegue a la base de la copa.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "0.4"))
    float SubCrownFraction = 0.12f;

    // ---- Tropismos: la dirección de crecimiento de cada iteración ----
    //
    // La dirección de un nodo nuevo es la suma normalizada de cuatro términos con estos
    // pesos, más un jitter. Solo importan sus proporciones relativas, no su escala.

    /**
     * Peso del llenado de espacio: dirección promedio hacia los atractores asociados.
     *
     * @see @ref bib_prusinkiewicz1990
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wSCA = 1.0f;

    /** Peso del gravitropismo, el sesgo hacia arriba. Alto en conífera: líder recto y
        dominante. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wGrav = 0.3f;

    /** Peso del fototropismo: inclinación hacia el gradiente de la rejilla de luz fina
        local. Sin rejilla fina el término no aporta nada. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wPhot = 0.1f;

    /** Peso de la inercia o rigidez de la rama: conserva la dirección previa y quita el
        zigzag entre iteraciones. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wPrev = 0.4f;

    /** Amplitud del jitter de dirección, en [0..1]. Sale del estado de aleatoriedad del
        árbol, así que es variación reproducible y no ruido de ejecución. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0", ClampMax = "1"))
    float DirNoise = 0.1f;

    /**
     * Semiángulo del cono de percepción del paso de asociación: un nodo solo compite por
     * los atractores que caen dentro de ese cono alrededor de su propia dirección. 180 es
     * la esfera completa, es decir, el cono desactivado.
     *
     * Es lo que evita el abanico de ramas: sin cono, un nodo reclama también los atractores
     * que tiene detrás, y la punta del eje —que está centrada— resulta la más cercana a casi
     * todo y se los lleva.
     *
     * @warning Un cono demasiado estrecho deja atractores huérfanos y el árbol se queda
     *          corto. Conviene bajarlo desde 95 en pasos pequeños.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "20", ClampMax = "180"))
    float PerceptionAngleDeg = 95.f;

    /**
     * Ángulo mínimo de inserción de una rama lateral respecto a la dirección de su padre.
     * 0 lo desactiva y deja que la rama salga con la dirección que le den los tropismos, que
     * puede ser casi paralela al padre y se lee como un eje deshilachado; entre 45 y 80 da
     * la lectura de verticilo de las coníferas.
     *
     * @note Se aplica solo al primer nodo de la rama: forzado en toda la cadena, la
     *       colonización del espacio dejaría de poder dirigirla hacia los atractores.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0", ClampMax = "85"))
    float BranchAngleDeg = 45.f;

    // ---- Radios de la colonización del espacio ----

    /**
     * Longitud del internodo, @f$ D @f$, en cm: lo que avanza un nodo por iteración. Fija la
     * resolución del esqueleto.
     *
     * @warning Los tres radios deben cumplir @f$ d_k < D < d_i @f$. Con
     *          @f$ d_k \ge D @f$ el nodo nuevo consume su propio atractor sin haber tirado
     *          de la rama y el árbol no ramifica; con @f$ d_i \le D @f$ ningún nodo ve
     *          atractores fuera de su propio paso y el crecimiento se para.
     * @see @ref bib_runions2007
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float StepLengthD = 40.f;

    /** Radio de influencia @f$ d_i @f$, en cm: hasta dónde ve un nodo los atractores por los
        que compite. Ha de ser mayor que StepLengthD. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float InfluenceRadiusDi = 200.f;

    /** Radio de muerte @f$ d_k @f$, en cm: un atractor a esa distancia de un nodo recién
        creado se da por alcanzado y se retira de la nube. Ha de ser menor que StepLengthD. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "0.1"))
    float KillRadiusDk = 30.f;

    /** Tope de iteraciones de crecimiento; el rango útil está entre 30 y 100. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    int32 MaxIter = 60;

    /** Cada cuántas iteraciones se refresca la rejilla de luz fina interna del árbol; 0 la
        desactiva. De ese refresco sale la autopoda emergente de las ramas interiores, sin
        ninguna regla de poda explícita. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "0"))
    int32 LightEvery = 8;

    /** Arista del vóxel de esa rejilla de luz fina, en cm; el rango útil está entre 25 y 50. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float FineVoxelSizeCm = 35.f;

    // ---- Pipe model: radio de cada rama ----

    /**
     * Exponente @f$ n @f$ de la conservación del área de conducción en cada bifurcación,
     * @f$ r_{padre}^{\,n} = \sum r_{hijo}^{\,n} @f$. Vale 2 en la regla clásica de da Vinci
     * y ronda 2.5 en los ajustes empíricos.
     *
     * @see @ref bib_shinozaki1964
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "1.0", ClampMax = "4.0"))
    float PipeExp = 2.2f;

    /** Radio de las ramillas terminales, en cm: es la condición de contorno desde la que el
        pipe model engrosa hacia la base. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "0.05"))
    float TipRadiusCm = 1.5f;

    /** Radio del último nodo de una ramilla como fracción de TipRadiusCm: por debajo de 1
        afila la punta en vez de dejarla como un cilindro cortado a plano. No se propaga
        hacia la base, porque el pipe model se calcula con el radio sin afilar. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "0.05", ClampMax = "1.0"))
    float TipTaper = 0.35f;

    // ================================================================
    // ==== PERFIL DE TRONCO: LO QUE EL PIPE MODEL NO DESCRIBE ====
    // ================================================================
    // El pipe model describe la madera funcional —la conservación del área de xilema— y es
    // correcto como tal, pero en una cadena sin bifurcaciones da r_padre = r_hijo exacto, o
    // sea un cilindro matemáticamente perfecto. Un tronco real acumula albura, duramen y
    // corteza durante décadas y añade en el pie el ensanche de raíz que reparte el momento
    // de vuelco al suelo. Eso es geometría externa y no hidráulica, y por eso se aplica como
    // una capa por encima del pipe model y no dentro de él.

    /**
     * Ensanche del pie del tronco sobre su radio estructural: 0.8 deja el pie un 80 % más
     * ancho que el fuste. El engrosamiento decae con la altura como
     * @f$ 1 + s\,e^{-h/H} @f$.
     *
     * @see @ref bib_metzger1893
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0", ClampMax = "3"))
    float TrunkFlareStrength = 0.8f;

    /** Altura característica @f$ H @f$ del ensanche, en cm: a tres veces esta altura ya no
        queda nada de él. Un valor típico es del 8 al 12 % de la altura total.
        @warning Divisor de la exponencial: debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "1"))
    float TrunkFlareHeightCm = 120.f;

    /** Radio del eje en su punta como fracción del que le da el pipe model: por debajo de 1
        afila el fuste con la altura, y 1 deja el pipe model tal cual. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0.2", ClampMax = "1"))
    float TrunkTopTaper = 0.82f;

    /** Exponente con el que se reparte ese afilado a lo largo del eje: por encima de 1 lo
        concentra arriba —fuste recto y luego afilado— y por debajo lo reparte desde abajo. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0.25", ClampMax = "4"))
    float TrunkTaperExp = 1.5f;

    /** Curvatura suave del eje, en grados de desviación máxima de la vertical: el árbol se
        inclina como un todo. 0 da un poste recto. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0", ClampMax = "20"))
    float TrunkSweepDeg = 4.f;

    /** Serpenteo de alta frecuencia del eje, nodo a nodo, en grados. Pequeño por definición:
        es el detalle que quita la lectura de extrusión perfecta. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0", ClampMax = "8"))
    float TrunkWobbleDeg = 1.2f;

    // ================================================================
    // ==== DEFORMACIÓN DE TRONCO: ARQUEADO O TORCIDO POR ÁRBOL ====
    // ================================================================
    // TrunkSweepDeg y TrunkWobbleDeg son sinuosidad siempre activa y sutil: la llevan todos
    // los árboles de la especie, y su tope duro de unos 20 grados existe porque viven dentro
    // del bucle que encadena el eje, que avanza en Z y dejaría de avanzar con ángulos
    // grandes. Lo que sigue es otra cosa: una tirada por árbol que decide si ese individuo
    // concreto sale arqueado y cuánto. Se aplica como un doblado isométrico del esqueleto ya
    // terminado, con lo que alcanza ángulos vedados al eje y dobla también la copa.

    /**
     * Capas de deformación de tronco que la especie sortea por árbol. Un array vacío deja
     * la geometría intacta: el deformador ni siquiera se ejecuta.
     *
     * @warning Cada capa consume un número fijo de muestras del flujo aleatorio del árbol,
     *          en orden de array. Añadir capas al final no altera las que ya había;
     *          reordenarlas o insertar en medio reparte otras formas a otros árboles, lo que
     *          no rompe nada pero cambia un bosque ya calibrado.
     * @see FTrunkDeformLayerSpec
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco|Deformacion")
    TArray<FTrunkDeformLayerSpec> TrunkDeformLayers;

    // ---- Mallado: del esqueleto a la malla ----

    /** Número de vértices del anillo de sección con que se tubula cada rama.
        @note Por debajo de 8 no hay resolución angular para el relieve de sección, y el
              mallador sube el mínimo efectivo si hay deformación activa. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "3", ClampMax = "16"))
    int32 RingSegments = 10;

    /**
     * Amplitud de los lóbulos de la sección, como fracción del radio. La sección deja de ser
     * una circunferencia y pasa a ser un polígono redondeado que además gira lentamente con
     * la altura, y eso es lo que se lee como tronco retorcido en vez de como cilindro.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "0", ClampMax = "0.4"))
    float SectionLobeAmount = 0.10f;

    /** Número de lóbulos de la sección: 2 da una sección elíptica y 3 o 4 un tronco
        acostillado. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "2", ClampMax = "6"))
    int32 SectionLobeCount = 3;

    /**
     * Relieve grueso de la superficie —bultos y hendiduras— como fracción del radio.
     * Deliberadamente pequeño: las grietas finas de la corteza son trabajo del material, que
     * ya trae mapas de normal y de altura, y modelarlas como geometría solo gasta
     * triángulos.
     *
     * @warning La suma de SectionLobeAmount y BarkReliefAmount ha de quedar por debajo de
     *          0.95: por encima, la deformación puede anular el radio y colapsar el tubo
     *          sobre su eje.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "0", ClampMax = "0.2"))
    float BarkReliefAmount = 0.05f;

    // ---- Follaje y filotaxis ----
    //
    // Las hojas se reparten a lo largo de las ramillas, una por ranura cada LeafSpacingCm de
    // longitud de rama, girando PhyllotaxisAngleDeg entre ranuras consecutivas.
    // LeafSpacingCm es por tanto la palanca de densidad y de coste: la mitad de separación
    // es el doble de hojas.

    /** Largo de la hoja, del pecíolo a la punta, en cm. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "1"))
    float LeafSizeCm = 20.f;

    /** Ancho de la hoja como fracción de su largo: 1 la deja cuadrada y por debajo de 1,
        lanceolada. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0.05", ClampMax = "2"))
    float LeafWidthRatio = 0.45f;

    /** Paso longitudinal entre ranuras de hoja consecutivas a lo largo de la ramilla, en cm.
        @warning Divisor de la espiral filotáctica: debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0.5"))
    float LeafSpacingCm = 8.f;

    /**
     * Ángulo de divergencia entre hojas consecutivas: 137.5 es la espiral áurea del caso
     * general, 180 la disposición dística del haya y 90 la decusada.
     *
     * @see @ref bib_vogel1979
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0", ClampMax = "180"))
    float PhyllotaxisAngleDeg = 137.5f;

    /** Umbral de radio portador de hoja, como múltiplo de TipRadiusCm: una rama lleva
        follaje mientras su radio no lo supere, de modo que el follaje sale de la madera
        joven y no del tronco. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "1"))
    float LeafBearingRadiusScale = 2.f;

    /** Ángulo de inserción de la hoja sobre la perpendicular a la ramilla: 0 la saca en
        ángulo recto y un valor positivo la inclina hacia la punta. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "-80", ClampMax = "80"))
    float LeafInsertionAngleDeg = 35.f;

    /** Longitud del pecíolo, en cm: separa la hoja de la corteza para que su plano no quede
        clavado dentro del tubo de la rama. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0"))
    float PetioleLengthCm = 1.5f;

    /**
     * Cuánto orienta la hoja su lámina al gradiente de la rejilla de luz fina: 0 la deja
     * siempre mirando al cielo y 1 la hace totalmente heliotrópica.
     *
     * @see @ref bib_ehleringer1980
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0", ClampMax = "1"))
    float LeafHeliotropism = 0.6f;

    /** Fracción de las ranuras que llegan a producir hoja, en [0..1]. Aclara el follaje sin
        cambiar su reparto; para más follaje se baja LeafSpacingCm. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0", ClampMax = "1"))
    float LeafDensity = 0.8f;

    // ================================================================
    // ==== NIVELES DE REPRESENTACIÓN Y MATERIALES ====
    // ================================================================
    // La librería de arquetipos no hornea este asset tal cual: lo duplica por cada terna de
    // especie, bucket de tamaño y variante, y escala la morfología del duplicado. Una ficha
    // describe por tanto una familia de mallas y no una sola. Con tres especies, cinco
    // buckets y cuatro variantes salen sesenta mallas horneadas una única vez que
    // representan a decenas de miles de árboles. El número de buckets es un ajuste global y
    // no un rasgo: fija la escala común de tamaños para todas las especies. Los materiales
    // se toman siempre del asset base, nunca del duplicado. Los aplica
    // UTreeLibrary::GetArchetypeSpecies.

    /** Variantes morfológicas horneadas por especie y bucket: más variantes rompen la
        repetición visual y cuestan memoria de malla. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "1", ClampMax = "16"))
    int32 NumLodVariants = 4;

    /** Material de la corteza: sección 0 de la malla, tanto en la instancia horneada como en
        el hero tree. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD")
    TObjectPtr<UMaterialInterface> BarkMaterial;

    /** Material del follaje: sección 1 de la malla. La tarjeta de hoja pide un material de
        dos caras, con máscara de opacidad y dispersión subsuperficial. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD")
    TObjectPtr<UMaterialInterface> LeafMaterial;

    /** Material del impostor de campo lejano, el que lleva el atlas del árbol visto desde
        fuera. Nulo reutiliza LeafMaterial: las dos tarjetas cruzadas se dibujan igual, pero
        sin atlas se leen como dos planos de hojas.
        @see TreeMeshBaker::BuildImpostorMesh */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD")
    TObjectPtr<UMaterialInterface> ImpostorMaterial;

    // ================================================================
    // ==== DECLIVE: SENESCENCIA POR EDAD Y SUPRESIÓN POR ESTRÉS ====
    // ================================================================
    // Dos canales de declive separados a propósito. La senescencia por edad es irreversible
    // y entra al rebasar una fracción de la longevidad; la supresión por estrés es
    // reversible, con histéresis, y entra al alcanzar el umbral de estrés de la especie. En
    // los dos casos el árbol casi deja de crecer y cambia su riesgo de morir, pero solo la
    // senescencia recorta la fecundidad. De la senescencia salen los tocones y el
    // auto-aclareo visible del dosel; de la supresión, el banco de plántulas del sotobosque,
    // que se perdería si un año malo dejase marcada de por vida a una plántula.
    // Los evalúa EcologyRules dentro del paso paralelo del tick.

    /**
     * Fracción de la longevidad a partir de la cual el árbol entra en senescencia.
     *
     * Lo que decide su efecto es cómo se compone con la edad mediana de muerte
     * @f$ 1{,}282\,L^{0{,}8} @f$ (@ref Longevity), porque el multiplicador de mortalidad
     * actúa sobre un riesgo por edad que crece como la cuarta potencia. Entrando muy por
     * detrás de la mediana el riesgo por edad ya es enorme —a @f$ 0{,}75\,L @f$ con
     * @f$ L = 200 @f$ ronda 0.32 al año— y multiplicarlo no abre ninguna etapa de declive,
     * solo adelanta la muerte. Entrando justo antes de la mediana, con el riesgo aún en
     * torno a 0.015 al año, el multiplicador es un empujón: crecimiento parado, menos
     * semilla y tocones que aparecen poco a poco.
     *
     * Óptimo medido con @f$ L = 600 @f$ y multiplicador 2:
     *
     * @verbatim
     * fracción   entra a   % de la cohorte   coste en vida
     *            (años)    que la alcanza    mediana (años)
     *   0.25       150           89 %             -22
     *   0.35       210           54 %              -2   <- valor por defecto
     *   0.45       270           11 %               0
     * @endverbatim
     *
     * Por debajo, la senescencia acorta la vida de verdad; por encima, casi ningún árbol
     * llega a estar senescente y la etapa deja de existir.
     *
     * @see EcologyRules::IsSenescentByAge
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescenceAgeFraction = 0.35f;

    /** Estrés al que el árbol entra en supresión, a cualquier edad. Se sale por debajo de
        una fracción de este umbral, fijada como ajuste global: la banda de histéresis es lo
        que evita que el estado parpadee tick a tick.
        @note Pese al nombre, no interviene en la senescencia, que solo depende de la edad.
        @see EcologyRules::UpdateSuppression */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescenceStressThreshold = 0.85f;

    /** Multiplicador del crecimiento mientras el árbol está senescente; cerca de 0 lo
        detiene. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescentGrowthScale = 0.1f;

    /**
     * Multiplicador de la probabilidad de morir de un árbol senescente.
     *
     * Se aplica sobre la probabilidad ya combinada —edad y condición— y no solo sobre la
     * parte de edad, y se acota por abajo a 1: la senescencia nunca rebaja el riesgo. Con
     * la senescencia entrando justo antes de la mediana, apenas decide cuánto vive un árbol
     * de dosel sano: medido con @f$ L = 600 @f$, la mediana de la cohorte es de 215 años con
     * multiplicador 1 y de 213 con multiplicador 3. Donde sí se nota es en los árboles que
     * llegan viejos y estresados a la vez, porque amplifica los dos canales.
     *
     * @see EcologyRules::ApplySenescentMortality
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Senescencia", meta = (ClampMin = "1"))
    float SenescentMortalityMultiplier = 2.f;

    /**
     * Fracción de la tasa de semillas que conserva un árbol senescente.
     *
     * A cero el árbol deja de reproducirse, lo que silencia justo su ventana de máxima
     * fecundidad: la lluvia de semillas es proporcional a la biomasa y ésa es máxima en los
     * últimos años, cuando el dominante viejo es la principal fuente de semilla. Al alza
     * pasa lo contrario, porque con la senescencia entrando pronto hay muchos senescentes
     * vivos a la vez y acaparan la lluvia de semillas ellos solos.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescentSeedScale = 0.3f;

    // ---- Supresión por estrés: el aguante bajo el dosel ----

    /**
     * Probabilidad anual de morir de un árbol suprimido, que sustituye al canal de estrés
     * general mientras dura la supresión.
     *
     * Es el único rasgo de especie que puede reducir el riesgo de un árbol en apuros en vez
     * de amplificarlo, y con él se cierra el compromiso entre crecimiento y supervivencia:
     * sin este rasgo, una especie lenta paga el mismo impuesto anual que una rápida durante
     * muchos más años y no tiene con qué compensarlo. Aquí la tolerante paga en velocidad y
     * cobra en aguante: con 0.03 sobrevive unos 33 años en penumbra, de sobra para esperar a
     * que se abra un claro; una pionera con 0.30 aguanta unos 3 y desaparece del sotobosque.
     *
     * Valores de referencia: pionera 0.30, intermedia 0.10, climácica 0.03.
     *
     * @see EcologyRules::SuppressedMortalityProbability
     * @see @ref bib_toleranciasombra
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Supresion", meta = (ClampMin = "0", ClampMax = "1"))
    float SuppressedMortalityPerYear = 0.15f;

    /** Multiplicador del crecimiento mientras el árbol está suprimido; cerca de 0 lo
        detiene. Bajo pero no nulo: un suprimido que no crece nada tampoco puede aprovechar
        una mejora parcial de luz mientras el claro no acaba de abrirse. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Supresion", meta = (ClampMin = "0", ClampMax = "1"))
    float SuppressedGrowthScale = 0.1f;

    // ================================================================
    // ==== RESPUESTA AL VIENTO ====
    // ================================================================
    // Ningún material lee estos dos rasgos: el mallador los hornea por vértice en el canal
    // UV3 de la malla (Geometry/TreeWindData.h). Es lo que permite que el material de
    // corteza y el de hoja sean uno solo para todo el bosque —un shader, una llamada de
    // dibujo por arquetipo— y que aun así cada especie se mueva distinto, porque la
    // diferencia viaja en la geometría y no en parámetros por instancia.

    /** Rigidez frente al viento, en [0..1]: 0 es rama larga y flexible —sauce, abedul— y 1
        es prácticamente rígido, como una conífera de tronco recto. Escala a la baja todo el
        balanceo del árbol, tanto el del tronco como el jerárquico de las ramas.
        @see @ref bib_vientovegetacion */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Viento", meta = (ClampMin = "0", ClampMax = "1"))
    float WindStiffness = 0.4f;

    /** Amplitud del aleteo de alta frecuencia de la hoja, relativa al balanceo de su rama: 0
        deja la hoja moviéndose solo con la rama —acículas cortas y rígidas—, 1 es el aleteo
        normal y por encima de 1 describe una hoja grande y suelta, como la del álamo. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Viento", meta = (ClampMin = "0", ClampMax = "2"))
    float LeafFlutterScale = 1.0f;

#if WITH_EDITOR
    /**
     * Validación del asset en el editor: bloquea como error los valores que producirían
     * NaN o infinitos por ser divisores y los que rompen las invariantes geométricas del
     * generador de árboles, y avisa de las combinaciones de rasgos sin compromiso.
     *
     * @return Invalid si algún error duro impide usar la especie; en caso contrario, el
     *         resultado que devuelva la clase base.
     */
    virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
