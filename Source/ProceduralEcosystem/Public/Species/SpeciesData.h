#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "SpeciesData.generated.h"
class UMaterialInterface;



/**
 * Forma de la envolvente de copa donde se siembran los atractores del SCA
 * (Fase 3, doc. §3.4 "envolvente: forma de la copa"). La misma nube de
 * atractores con distinta forma ya diferencia una conifera de un roble.
 */
UENUM(BlueprintType)
enum class ECrownShape : uint8
{
    Conical,    // conica, estrecha (conifera excurrente)
    Spherical,  // esferica, ancha (roble decurrente)
    Columnar    // alta y estrecha (cipres)
};

/**
 * Como se dobla el tronco de un arbol al que le ha tocado una capa de
 * deformacion (ver FTrunkDeformLayerSpec y el namespace TrunkDeformer).
 */
UENUM(BlueprintType)
enum class ETrunkDeformType : uint8
{
    /** Inclinacion RIGIDA: todo el arbol gira el mismo angulo alrededor de su
        pie. Se lee como "plantado torcido" o crecido en ladera. Con angulos
        grandes parece que se cae: mantenlo por debajo de ~10 grados. */
    Lean,

    /** Arqueado progresivo (angulo * t^ShapeParam): sale vertical del suelo y se
        va tumbando con la altura. ES el arbol arqueado, y el unico tipo cuya
        lectura mejora con angulos grandes. */
    Arc,

    /** Sinuoso: el tronco serpentea (ShapeParam = nº de ondas en toda la
        altura). Con 1.5-2 se lee como madera de ribera; con mas, como ruido. */
    SCurve
};

/**
 * UNA capa de deformacion de tronco declarada por la especie.
 *
 * Cada arbol tira los dados por CADA capa del array de forma independiente, asi
 * que la especie no describe "como es su tronco" sino la DISTRIBUCION de
 * troncos que produce: con Probability 0.4 sobre una capa Arc, cuatro de cada
 * diez individuos salen arqueados y el resto rectos, y los cuatro con angulos
 * y azimuts distintos.
 *
 * Componer varias capas es la forma de conseguir formas que ninguna da sola
 * (p.ej. Arc suave + SCurve pequena = tronco volcado y ademas ondulado).
 */
USTRUCT(BlueprintType)
struct FTrunkDeformLayerSpec
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion")
    ETrunkDeformType Type = ETrunkDeformType::Arc;

    /** Probabilidad de que a un arbol le toque ESTA capa [0..1]. 0 = desactivada
        (pero sigue consumiendo sus muestras: ver el contrato de TrunkDeformer). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0", ClampMax = "1"))
    float Probability = 0.25f;

    /** Angulo minimo de la capa, en grados. Se sortea uniforme en [Min, Max]. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0", ClampMax = "45"))
    float MinAngleDeg = 5.f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0", ClampMax = "45"))
    float MaxAngleDeg = 18.f;

    /** Exponente t^k del arqueo (Arc) o nº de ondas (SCurve). Ignorado en Lean.
        En Arc: < 1 arquea desde abajo, > 1 mantiene el pie recto y vuelca arriba. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deformacion", meta = (ClampMin = "0.25", ClampMax = "6"))
    float ShapeParam = 1.5f;
};

/**

/**
 * Parámetros de especie. Son compartidos y de solo lectura en runtime: la
 * población (Fase 2) los referencia por índice/id, no los copia por agente.
 * Se editan en el editor como assets. Nombres alineados con FSpeciesParams
 * del documento de diseño técnico.
 *
 * Nota sobre los ClampMin: varios de estos parámetros se usan como DIVISORES
 * en las fórmulas de la Fase 2 (MaxBiomass, Longevity, WaterDemand,
 * NutrientDemand). Por eso su mínimo es un positivo pequeño, no 0: un asset
 * con 0 produciría NaN/Inf en cuanto arranque la simulación.
 */
UCLASS(BlueprintType)
class PROCEDURALECOSYSTEM_API USpeciesData : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    // --- Identidad / debug ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identidad")
    FName SpeciesName = TEXT("Especie");

    /** Color en el heatmap y en las esferas de debug. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identidad")
    FColor DebugColor = FColor::Green;

    // --- Ciclo vital (Fase 2) ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0"))
    float GrowthRate = 0.25f;

    /** Divisor en el crecimiento logístico (B/MaxBiomass): debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0.01"))
    float MaxBiomass = 100.f;

    /**
     * ESCALA de la mortalidad por edad, en años simulados. Divisor en la
     * fórmula, debe ser > 0.
     *
     * NO ES "LA EDAD A LA QUE MUERE EL ÁRBOL", y confundirlo es el error que
     * dejaba el bosque lleno de plantones. La mortalidad por edad es un hazard
     * acumulativo, pAge = dt·(Edad/Longevity)^4 por año (EcologyRules.h), así
     * que la EDAD MEDIANA DE MUERTE no es Longevity sino
     *
     *      T_mediana ≈ 1.282 · Longevity^0.8          (con YearsPerTick = 1)
     *
     * y para calibrarla al revés, si quieres que la mitad de los árboles pase
     * de T años:
     *
     *      Longevity = (T / 1.282)^1.25
     *
     * Con el viejo 200 la mediana caía en ~89 años: la mayor parte de la
     * población moría antes de terminar de crecer (a vigor medio, el logístico
     * tarda ~55 años en llegar al 95% de MaxBiomass). Con 600 la mediana sube a
     * ~214 años y un árbol pasa tres cuartas partes de su vida ya crecido, que
     * es lo que llena el bosque de adultos de distinta altura en vez de
     * plantones.
     *
     * Valores de referencia: 400 -> mediana ~155 años (pionera de vida corta);
     * 600 -> ~214; 800 -> ~269 (árbol de dosel longevo).
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0.01"))
    float Longevity = 600.f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "0"))
    float MaturityAge = 20.f;

    /** Altura (cm) de un arbol adulto (Biomass == MaxBiomass). Escala de HeightFromBiomass. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ciclo vital", meta = (ClampMin = "1"))
    float MaxHeightCm = 2000.f; // 20 m por defecto

    // --- Recursos (Fase 1/2) ---
    /** 0 = heliófila (necesita mucha luz), 1 = tolerante a la sombra. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0", ClampMax = "1"))
    float ShadeTolerance = 0.5f;

    /** Divisor en el factor de agua (W/WaterDemand): debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0.001"))
    float WaterDemand = 1.f;

    /** Divisor en el factor de nutrientes (N/NutrientDemand): debe ser > 0. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0.001"))
    float NutrientDemand = 1.f;

    /** Radio de raíz en metros; escala con la biomasa en el consumo (Fase 2). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recursos", meta = (ClampMin = "0"))
    float RootRadius = 2.f;

    // --- Dispersión (Fase 2) ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dispersión", meta = (ClampMin = "0"))
    float SeedDispersalRadius = 15.f; // m
    // ================================================================
    // --- Morfología SCA (Fase 3): geometría por-árbol ---
    // ================================================================
    // Estos parámetros NO intervienen en la Fase 2 (población). Los consume
    // la generación geométrica de los hero trees. La misma dirección de
    // crecimiento ponderada produce siluetas distintas cambiando los pesos
    // de tropismo: son la principal palanca de variedad entre especies
    // (doc. §3.4). Todas las longitudes en cm, coherente con el resto.

    // --- Envolvente de copa (siembra de atractores, doc. §3.1) ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa")
    ECrownShape CrownShape = ECrownShape::Spherical;

    /** Radio horizontal de la copa (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    float CrownRadiusCm = 400.f; // 4 m

    /** Altura vertical de la copa, de su base al ápice (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    float CrownHeightCm = 800.f; // 8 m

    /** Fracción de la altura total ocupada por tronco desnudo bajo la copa [0..1). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "0.95"))
    float TrunkFraction = 0.3f;

    /** Nº de atractores sembrados en la copa. Más = copa más tupida y más coste. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "1"))
    int32 NumAttractors = 400;

    /**
     * Hasta dónde sube el EJE PRINCIPAL dentro de la copa, como fracción de la
     * altura de copa. 0 = el tronco muere en la base de la copa (comportamiento
     * anterior); 1 = líder central que llega al ápice.
     *
     * Es la palanca excurrente/decurrente y arregla de raíz el artefacto de
     * "todas las ramas salen de la cima del tronco": si el eje termina en la
     * base de la copa, ese nodo es el ÚNICO que ve los atractores más gordos
     * (en una copa cónica el radio máximo cae justo ahí) y se los lleva todos,
     * de donde sale la silueta de paraguas. Con el eje atravesando la copa hay
     * nodos a todas las alturas y las ramas se reparten por el fuste.
     *
     * Y el afilado sale GRATIS del pipe model: cada rama lateral aporta su r^n
     * al eje por debajo de su inserción, así que el eje es grueso abajo y fino
     * arriba sin ningún truco. Conífera 0.9-1.0; roble/haya 0.3-0.5.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "1"))
    float LeaderFraction = 0.5f;

    /**
     * Amplitud del ruido que deforma el CONTORNO de la envolvente de copa.
     * 0 = molde exacto (cónico/esférico/columnar perfecto), 0.25 = lóbulos y
     * golfos naturales. El ruido es COHERENTE en azimut y altura, no blanco:
     * con ruido blanco la silueta no cambiaría, porque el máximo estadístico
     * de cientos de muestras reconstruye la envolvente original.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "0.6"))
    float EnvelopeNoise = 0.25f;

    /**
     * Sesgo vertical de la densidad de atractores dentro de la copa (exponente
     * de la altura normalizada). 1 = uniforme; < 1 lleva masa hacia el ápice;
     * > 1 la baja hacia la base de la copa. Controla "dónde está el grueso de
     * la copa" sin cambiar su forma.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0.25", ClampMax = "4"))
    float CrownVerticalBias = 1.f;

    /**
     * Fracción de atractores sembrados POR DEBAJO de la base de copa, con la
     * densidad cayendo hacia el suelo ("falda" de sub-copa).
     *
     * Sin esto el tronco desnudo es una zona prohibida para las ramas y la copa
     * arranca de golpe en un plano, que es lo que se lee como artificial. Un
     * 10-15% basta para las ramas bajas dispersas de un árbol real.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Copa", meta = (ClampMin = "0", ClampMax = "0.4"))
    float SubCrownFraction = 0.12f;

    // --- Tropismos: dirección de crecimiento por iteración (doc. §3.4) ---
    /** Peso del llenado de espacio (dirección promedio hacia los atractores). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wSCA = 1.0f;

    /** Gravitropismo: sesgo hacia arriba (alto en conífera = líder recto dominante). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wGrav = 0.3f;

    /** Fototropismo: inclinación al gradiente de luz de la rejilla fina (opcional). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wPhot = 0.1f;

    /** Inercia / rigidez: mantiene la dirección previa y quita el zigzag. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0"))
    float wPrev = 0.4f;

    /** Jitter de dirección por-árbol (0..1): variación reproducible desde el RngState. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0", ClampMax = "1"))
    float DirNoise = 0.1f;

    /**
     * SEMIángulo del cono de percepción del paso "asociar" del SCA: un nodo
     * solo compite por los atractores que caen dentro de ese cono alrededor de
     * su propia dirección. 180 = esfera completa (desactivado).
     *
     * Es el clásico "ángulo de percepción" del SCA (Runions et al., 2007) y el
     * arreglo más barato del abanico de ramas: sin él, un nodo reclama incluso
     * los atractores que tiene DETRÁS, y la punta del eje -que está centrada-
     * resulta ser la más cercana a casi todo y se lo lleva todo.
     *
     * Ojo al bajarlo: un cono demasiado estrecho deja atractores huérfanos y el
     * árbol se queda corto. Baja desde 95 en pasos pequeños.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "20", ClampMax = "180"))
    float PerceptionAngleDeg = 95.f;

    /**
     * Ángulo MÍNIMO de inserción de una rama lateral respecto a la dirección de
     * su padre. 0 = desactivado (la rama sale con la dirección que le den los
     * tropismos, que puede ser casi paralela al padre y se lee como si el eje
     * se hubiera deshilachado). 45-80 en coníferas da la lectura de verticilo.
     *
     * Solo se aplica al PRIMER nodo de la rama: si se aplicara a toda la cadena,
     * el SCA dejaría de poder dirigirla hacia los atractores.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tropismos", meta = (ClampMin = "0", ClampMax = "85"))
    float BranchAngleDeg = 45.f;

    // --- Radios del SCA: debe cumplirse d_k < D < d_i (doc. §3.1, Apéndice A) ---
    /** D: longitud del internodo (paso por iteración), cm. Fija la resolución del esqueleto. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float StepLengthD = 40.f;

    /** d_i: radio de influencia. Un nodo "ve" atractores hasta esta distancia. Debe ser > D. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float InfluenceRadiusDi = 200.f;

    /** d_k: radio de muerte. Un atractor a esta distancia de un nodo nuevo se da por alcanzado. Debe ser < D. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "0.1"))
    float KillRadiusDk = 30.f;

    /** Iteraciones máximas de crecimiento (doc: 30-100). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    int32 MaxIter = 60;

    /** Cada cuántas iteraciones se refresca la luz interna (autopoda emergente). 0 = nunca. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "0"))
    int32 LightEvery = 8;

    /** Tamaño del vóxel de la rejilla de luz fina local (cm; doc: 25-50 cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Radios", meta = (ClampMin = "1"))
    float FineVoxelSizeCm = 35.f;

    // --- Pipe model: radios de rama (doc. §3.6) ---
    /** Exponente del pipe model: r_padre^n = Σ r_hijo^n. n≈2 (da Vinci) a ~2.5 (empírico). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "1.0", ClampMax = "4.0"))
    float PipeExp = 2.2f;

    /** Radio de las ramillas terminales (cm); el pipe model engrosa hacia la base. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "0.05"))
    float TipRadiusCm = 1.5f;

    /** Radio del último nodo de una ramilla como fracción de TipRadiusCm. < 1
        afila la punta en vez de dejarla como un cilindro cortado a plano. No
        propaga hacia la base: el pipe model se calcula con el radio sin afilar. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|PipeModel", meta = (ClampMin = "0.05", ClampMax = "1.0"))
    float TipTaper = 0.35f;

    // ================================================================
    // --- Perfil de tronco: lo que el pipe model NO describe ---
    // ================================================================
    // El pipe model (r_padre^n = Σ r_hijo^n) modela la MADERA FUNCIONAL, o sea
    // la conservación del área de xilema, y es correcto como tal. Pero en una
    // cadena sin bifurcaciones da r_padre = r_hijo EXACTAMENTE, así que el
    // tronco desnudo salía como un cilindro matemáticamente perfecto.
    //
    // Un tronco real no es eso: acumula albura, duramen y corteza durante
    // décadas, y en el pie añade el ensanche de raíz (root flare / butt swell)
    // que reparte el momento de vuelco al suelo. Eso es geometría externa, no
    // hidráulica, y por tanto va aquí encima y no dentro del pipe model.

    /** Cuánto se ensancha el pie del tronco sobre su radio estructural.
        0.8 = el pie es un 80% más ancho que el fuste. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0", ClampMax = "3"))
    float TrunkFlareStrength = 0.8f;

    /** Altura característica del ensanche (cm): decae como exp(-h/esto), así
        que a 3x esta altura ya no queda nada. Típico: 8-12% de la altura total. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "1"))
    float TrunkFlareHeightCm = 120.f;

    /** Radio del eje en su punta como fracción del que le da el pipe model.
        < 1 afila el fuste con la altura. 1 = solo el pipe model. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0.2", ClampMax = "1"))
    float TrunkTopTaper = 0.82f;

    /** Curvatura del afilado a lo largo del eje. > 1 concentra el adelgazamiento
        arriba (fuste recto y luego afila); < 1 lo reparte desde abajo. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0.25", ClampMax = "4"))
    float TrunkTaperExp = 1.5f;

    /** Desviación máxima de la vertical del eje, en grados: la curvatura suave
        de todo el fuste (un árbol se inclina como un todo). 0 = poste recto. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0", ClampMax = "20"))
    float TrunkSweepDeg = 4.f;

    /** Serpenteo de alta frecuencia, nodo a nodo, en grados. Pequeño: es el
        detalle que quita la lectura de "extrusión perfecta". */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco", meta = (ClampMin = "0", ClampMax = "8"))
    float TrunkWobbleDeg = 1.2f;

    // ================================================================
    // --- Deformación de tronco: arqueado / torcido POR ÁRBOL ---
    // ================================================================
    // TrunkSweepDeg y TrunkWobbleDeg de arriba son sinuosidad SIEMPRE ACTIVA y
    // sutil: todos los árboles de la especie la llevan, y su tope duro (~20°,
    // MaxAxisTiltRad) existe porque viven dentro del bucle que encadena el eje,
    // que avanza en Z y pararía de avanzar con ángulos grandes.
    //
    // Esto es otra cosa: una TIRADA por árbol que decide si ESE individuo sale
    // arqueado, y cuánto. Se aplica como un doblado isométrico del esqueleto
    // YA TERMINADO (namespace TrunkDeformer), así que puede llegar a ángulos que
    // el eje del SCA no puede alcanzar, y dobla también la copa.

    /**
     * Capas de deformación de tronco de esta especie. Array VACÍO = ningún árbol
     * se deforma, y la geometría sale bit a bit idéntica a la de antes de que
     * existiera este sistema (el deformador ni siquiera se ejecuta).
     *
     * OJO AL ORDEN: cada capa consume un nº fijo de muestras del sub-stream del
     * árbol, en orden de array. Añadir capas AL FINAL no altera las que ya
     * había; reordenarlas o insertar en medio reparte otras formas a otros
     * árboles (no rompe nada, pero tu bosque calibrado cambia).
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Tronco|Deformacion")
    TArray<FTrunkDeformLayerSpec> TrunkDeformLayers;

    // --- Mallado: de esqueleto a malla (doc. §3.7) ---
    /** K: nº de vértices del anillo de sección de cada rama (tubos).
        OJO: por debajo de 8 no hay resolución angular para el relieve de
        sección; el mallador sube el mínimo efectivo si hay deformación activa. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "3", ClampMax = "16"))
    int32 RingSegments = 10;

    /**
     * Amplitud de los LÓBULOS de la sección, como fracción del radio. La sección
     * deja de ser una circunferencia y pasa a ser un polígono redondeado que
     * además GIRA lentamente con la altura, que es lo que se lee como "tronco
     * retorcido" en vez de "cilindro".
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "0", ClampMax = "0.4"))
    float SectionLobeAmount = 0.10f;

    /** Nº de lóbulos de la sección. 2 = sección elíptica, 3-4 = tronco acostillado. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "2", ClampMax = "6"))
    int32 SectionLobeCount = 3;

    /**
     * Relieve grueso de la superficie (bultos y hendiduras), como fracción del
     * radio. Deliberadamente PEQUEÑO: las grietas finas de la corteza son
     * trabajo del material -las texturas de Materials_TreeBark ya traen normal
     * y height- y meterlas como geometría solo gasta triángulos.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Malla", meta = (ClampMin = "0", ClampMax = "0.2"))
    float BarkReliefAmount = 0.05f;

    // --- Follaje: filotaxis (ver Geometry/TreeFoliage.h) ---
    // Las hojas se reparten a lo largo de las ramillas, una por ranura cada
    // LeafSpacingCm de longitud de rama, girando PhyllotaxisAngleDeg entre
    // ranuras consecutivas. LeafSpacingCm es, por tanto, la palanca de DENSIDAD
    // (y de coste): la mitad de separación es el doble de hojas.

    /** Largo de la hoja, del pecíolo a la punta (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "1"))
    float LeafSizeCm = 20.f;

    /** Ancho de la hoja como fracción de su largo. 1 = cuadrada, <1 = lanceolada. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0.05", ClampMax = "2"))
    float LeafWidthRatio = 0.45f;

    /** Distancia entre hojas consecutivas a lo largo de la ramilla (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0.5"))
    float LeafSpacingCm = 8.f;

    /** Ángulo de divergencia entre hojas consecutivas. 137.5 = espiral áurea
        (el caso general), 180 = dística (haya), 90 = decusada. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0", ClampMax = "180"))
    float PhyllotaxisAngleDeg = 137.5f;

    /** Una rama lleva hoja mientras su radio no supere TipRadiusCm × esto: el
        follaje sale de la madera joven, no del tronco. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "1"))
    float LeafBearingRadiusScale = 2.f;

    /** Ángulo de inserción sobre la perpendicular a la ramilla. 0 = la hoja sale
        en ángulo recto; positivo = inclinada hacia la punta. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "-80", ClampMax = "80"))
    float LeafInsertionAngleDeg = 35.f;

    /** Separación de la hoja respecto a la corteza (cm): sin ella la card queda
        clavada dentro del tubo de la rama. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0"))
    float PetioleLengthCm = 1.5f;

    /** Cuánto orienta la hoja su cara al gradiente de luz de la rejilla fina.
        0 = siempre al cielo, 1 = totalmente heliotrópica. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0", ClampMax = "1"))
    float LeafHeliotropism = 0.6f;

    /** Fracción de ranuras que llegan a producir hoja [0..1]. Aclara el follaje
        sin cambiar su reparto; para MÁS follaje, baja LeafSpacingCm. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "SCA|Follaje", meta = (ClampMin = "0", ClampMax = "1"))
    float LeafDensity = 0.8f;

    // ================================================================
    // --- LOD y librería de arquetipos (Fase 4) ---
    // ================================================================
    // 3 especies × 5 buckets × 4 variantes = 60 mallas horneadas UNA vez que
    // representan a los 20.000 árboles (doc. §4.2). El nº de buckets es global
    // (Project Settings): define la escala común de tamaños.

    /** Variantes morfológicas por especie: más = menos repetición, más memoria. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD", meta = (ClampMin = "1", ClampMax = "16"))
    int32 NumLodVariants = 4;

    /** Material de la corteza (sección 0 de la malla horneada y del hero tree). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD")
    TObjectPtr<UMaterialInterface> BarkMaterial;

    /** Material del follaje (sección 1): two-sided + masked + subsurface. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD")
    TObjectPtr<UMaterialInterface> LeafMaterial;

    /** Material del impostor de campo lejano. Si es null se reutiliza
        LeafMaterial (funciona, se ve mal): el atlas se hornea con el plugin
        Impostor Baker cuando llegues al pulido. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "LOD")
    TObjectPtr<UMaterialInterface> ImpostorMaterial;
    // ================================================================
// --- Senescencia (Fase 5): el declive antes de la muerte ---
// ================================================================
// El estado ETreeState::Senescent deja de ser un placeholder. Un arbol
// entra en declive por VEJEZ (fraccion de su longevidad) o por ESTRES
// sostenido; entonces casi deja de crecer, no se reproduce y su
// probabilidad de morir se multiplica. De aqui salen los tocones/snags
// y el auto-aclareo visible del bosque.

/**
 * Fraccion de la longevidad a partir de la cual el arbol entra en senescencia.
 *
 * Ojo a como se compone con la mediana de muerte (ver Longevity): a 0.75 la
 * senescencia entraba MUY por detras de la mediana (0.75·L frente a 1.282·L^0.8)
 * y encima multiplicaba una pAge que en esa zona ya era enorme -a 0.75·L con
 * L=200, pAge ≈ 0.32/año, y el x3 la subia a ~0.95/año-. O sea que no era una
 * fase de declive: era la ejecucion, y por eso "los longevos morian rapido".
 *
 * A 0.35 la senescencia entra JUSTO ANTES de la mediana, cuando pAge todavia
 * ronda 0.015/año: el multiplicador es entonces un empujon y no una sentencia, y
 * la senescencia vuelve a leerse como lo que debia ser (crecimiento parado,
 * menos semilla, snags que aparecen poco a poco).
 *
 * El 0.35 no es arbitrario, es el optimo medido (L = 600, multiplicador 2):
 *
 *   fraccion   entra a   % de la cohorte    coste en vida
 *              (años)    que la alcanza     mediana (años)
 *     0.25       150          89 %              -22
 *     0.35       210          54 %               -2      <- aqui
 *     0.45       270          11 %                0
 *
 * Por debajo la senescencia empieza a acortar la vida de verdad; por encima
 * casi ningun arbol llega a estar senescente y la fase deja de existir.
 */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fase5|Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescenceAgeFraction = 0.35f;

    /** Estres sostenido (>=) que fuerza la senescencia aunque el arbol sea joven. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fase5|Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescenceStressThreshold = 0.85f;

    /** Multiplicador del crecimiento en senescencia (~0 = deja de crecer). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fase5|Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescentGrowthScale = 0.1f;

    /**
     * Multiplicador de la probabilidad de morir cuando esta senescente.
     *
     * Baja de 3 a 2, pero por una razon distinta a la que parece: con
     * SenescenceAgeFraction en 0.35 este multiplicador ya casi no decide cuanto
     * vive un arbol viejo (medido con L = 600: mediana 215 años con
     * multiplicador 1, y 213 con multiplicador 3). Lo que si decide es la suerte
     * de los PLANTONES SUPRIMIDOS, que entran en senescencia por ESTRES y no por
     * vejez, porque multiplica la pDeath TOTAL y no solo la parte de edad.
     *
     * O sea que este parametro se calibra mirando el sotobosque, no el dosel: a 2
     * un suprimido con estres maximo muere a ~0.4/año, que es la criba que hace
     * falta para que no se acumulen arbolitos condenados. A 1.5 se quedaria en
     * 0.3 y durarian demasiado; a 3 (el valor viejo) se llevaba por delante
     * tambien a los viejos sanos.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fase5|Senescencia", meta = (ClampMin = "1"))
    float SenescentMortalityMultiplier = 2.f;
    /** Fraccion de la tasa de semillas que CONSERVA un arbol senescente.
       0 = deja de reproducirse (comportamiento anterior). Cortarlo a cero
       silenciaba justo la ventana de maxima fecundidad -la biomasa, y por tanto
       SeedRate*Biomass, es maxima en los ultimos años- cuando en un bosque real
       los dominantes viejos son la PRINCIPAL fuente de semilla. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fase5|Senescencia", meta = (ClampMin = "0", ClampMax = "1"))
    float SenescentSeedScale = 0.3f; // baja de 0.4: con la longevidad recalibrada
    // hay MUCHOS mas senescentes vivos a la vez, y a 0.4 dominarian la lluvia de
    // semillas ellos solos.

    // ================================================================
    // --- Fase 6: viento (doc. 6.1) ---
    // ================================================================
    // Estos dos parametros NO los lee ningun material: los "dobla" el mallador
    // dentro del canal UV3 de cada vertice (ver Geometry/TreeWindData.h). Es
    // deliberado y tiene una ventaja concreta: el material de corteza y el de
    // hoja son UNO para todo el bosque -un solo shader, un solo draw call por
    // arquetipo- y aun asi cada especie se mueve distinto, porque la diferencia
    // viaja en la geometria y no en parametros por instancia.

    /** Rigidez frente al viento. 0 = rama larga y flexible (sauce, abedul),
        1 = practicamente rigido (conifera de tronco recto, arbol joven grueso).
        Escala a la baja TODO el balanceo del arbol. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fase6|Viento", meta = (ClampMin = "0", ClampMax = "1"))
    float WindStiffness = 0.4f;

    /** Amplitud del aleteo de las hojas, relativa al balanceo de su rama. 0 = la
        hoja se mueve solo con la rama (conifera: aciculas cortas y rigidas);
        1 = aleteo normal; >1 = hoja grande y suelta (chopo, alamo temblon). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Fase6|Viento", meta = (ClampMin = "0", ClampMax = "2"))
    float LeafFlutterScale = 1.0f;

#if WITH_EDITOR
    /** Validación de datos: avisa de configuraciones incoherentes al editar el asset. */
    virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif
};
