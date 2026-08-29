#pragma once

#include "CoreMinimal.h"

/** Semilla pendiente de germinar, generada durante el paso paralelo del tick. */
struct FPendingSeed
{
    FVector Position = FVector::ZeroVector; // mundo, cm
    uint16  SpeciesId = 0;
    uint32  RngSeed = 1; // stream del hijo, ya derivado

    /**
     * StableId del arbol MADRE, para poder excluirlo del conteo de Janzen-Connell.
     *
     * Sin el, cuando el radio de dispersion no supera al de inhibicion toda semilla
     * cae dentro del circulo de su propia madre, asi que hasta el ultimo adulto de
     * una especie al borde de la extincion paga la penalizacion por verse a si
     * mismo. Eso pone un techo al rescate de la especie rara justo donde el
     * mecanismo tendria que ser mas fuerte.
     */
    uint32  ParentStableId = 0;
};

/**
 * Contadores por especie de UN tick: el EMBUDO de reclutamiento y el reparto de
 * causas de muerte.
 *
 * POR QUE HACEN FALTA. El reclutamiento pasa por cuatro filtros multiplicativos
 * (fuera de mapa, espaciado, luz, Janzen-Connell) y cualquiera de ellos puede
 * estar aportando un factor 0 o un factor 100 sin que ningun log lo delate: con
 * solo el recuento de poblacion se ve QUE especie se hunde, no si es porque
 * recluta poco, porque se le mueren las plantulas o porque no llega a madurez
 * -que son tres arreglos distintos-. Es el equivalente para el reclutamiento de
 * lo que Agents.Limiter hizo por el crecimiento.
 *
 * Se acumulan por chunk y se reducen en orden fijo, NUNCA con atomicas: si el
 * resultado dependiera del orden de los hilos, el tick dejaria de ser
 * reproducible y con el todo el diseno experimental.
 */
struct FEcoSpeciesFlow
{
    // Embudo de reclutamiento
    int32 SeedsEmitted = 0;
    int32 RejectedOffMap = 0;
    int32 RejectedSpacing = 0;
    int32 RejectedLight = 0;
    int32 Germinated = 0;

    /** Suma de factores Janzen-Connell evaluados, y cuantos: su cociente es la media. */
    float JanzenConnellSum = 0.f;
    int32 JanzenConnellCount = 0;

    // Muertes, atribuidas al canal cuyo riesgo DOMINABA en el momento de morir.
    // No es una causa en sentido estricto -los dos canales actuan a la vez- pero
    // responde a la pregunta que importa: si el canal de edad no aparece nunca,
    // la longevidad no esta comprando nada y la estrategia K no puede existir.
    int32 DeathsByAge = 0;
    int32 DeathsByCondition = 0;

    void Reset() { *this = FEcoSpeciesFlow(); }

    FEcoSpeciesFlow& operator+=(const FEcoSpeciesFlow& O)
    {
        SeedsEmitted += O.SeedsEmitted;         RejectedOffMap += O.RejectedOffMap;
        RejectedSpacing += O.RejectedSpacing;   RejectedLight += O.RejectedLight;
        Germinated += O.Germinated;
        JanzenConnellSum += O.JanzenConnellSum; JanzenConnellCount += O.JanzenConnellCount;
        DeathsByAge += O.DeathsByAge;           DeathsByCondition += O.DeathsByCondition;
        return *this;
    }
};

/** Pulso de nutrientes pendiente de aplicar: un arbol que ha muerto este tick. */
struct FPendingDeathPulse
{
    FVector Position = FVector::ZeroVector; // mundo, cm
    float   RadiusCm = 0.f;                 // mismo radio efectivo que su consumo
    float   Amount = 0.f;                   // ya calculado con DeathNutrientPulse

    // --- Fase 5: datos para que el render genere caida/tocon/hojarasca ---
    uint16  SpeciesId = 0;
    uint32  StableId = 0;
    float   Biomass = 0.f;
    float   HeightCm = 0.f;
};

/**
 * Un deposito puntual sobre una celda de campo: "sumale Amount a la celda Cell".
 *
 * POR QUE UNA LISTA DISPERSA Y NO UN CAMPO DENSO POR TAREA (optimizacion C1):
 * la version anterior daba a cada FTickScratch dos TArray<float> del tamano
 * COMPLETO del campo (512x512 = 262.144 celdas). Con hasta 32 tareas eso son
 * ~64 MB de scratch y, sobre todo, una reduccion SERIAL de
 * 32 x 262.144 x 2 = 16,8 M sumas por tick... de las cuales el 99,9% suman cero.
 *
 * El trabajo real es minusculo: EffectiveRootRadiusCm nunca pasa de RootRadius
 * metros (2 m por defecto), o sea ~1 celda a 200 cm/celda, asi que DepositKernel
 * toca un bloque de 3x3. A 20.000 arboles y 2 campos son ~360.000 escrituras
 * reales. Guardando (celda, cantidad) la reduccion pasa a ser proporcional a lo
 * que de verdad se deposito, y la memoria cae de ~64 MB a unos pocos MB.
 *
 * El determinismo NO cambia: la reduccion sigue recorriendo las tareas en orden
 * de indice creciente y, dentro de cada tarea, las entradas en su orden de
 * insercion (arbol por indice creciente, celda por el orden fijo del kernel).
 * Es un orden distinto del de la version densa -y por tanto el fingerprint
 * cambia respecto a ella- pero igual de fijo y reproducible.
 */
struct FCellDelta
{
    int32 Cell = 0;
    float Amount = 0.f;
};

/**
 * Estado privado de UNA tarea paralela dentro de un tick (doc. 2.4:
 * "scratch privado por hilo"). Cada tarea del ParallelFor escribe SOLO en la
 * suya, asi que dentro de esta clase no hace falta ningun lock ni atomica.
 *
 * Agrupa las CUATRO cosas que el documento reparte en "Scratch_Water",
 * "Scratch_Nutrient", "Scratch_Seeds" y "Scratch_DeathPulses": un solo contexto
 * con cuatro campos es mas simple de pasar por tarea que cuatro arrays paralelos.
 */
struct FTickScratch
{
    TArray<FCellDelta> WaterDeltas;
    TArray<FCellDelta> NutrientDeltas;
    TArray<FPendingSeed> Seeds;
    TArray<FPendingDeathPulse> DeathPulses;

    /** Embudo por especie de esta tarea (indice = SpeciesId). */
    TArray<FEcoSpeciesFlow> SpeciesFlow;

    /** Vacia los buffers SIN liberar memoria: listo para el siguiente tick. */
    void ResetForNextTick(int32 NumSpecies)
    {
        WaterDeltas.Reset();
        NutrientDeltas.Reset();
        Seeds.Reset();
        DeathPulses.Reset();

        SpeciesFlow.SetNum(NumSpecies, EAllowShrinking::No);
        for (FEcoSpeciesFlow& F : SpeciesFlow) { F.Reset(); }
    }

    /**
     * Reserva de una vez la capacidad esperada de los deltas para que los Add()
     * del paso paralelo no realojen a mitad del tick. CellsPerTree es la cota
     * de celdas que toca el kernel de consumo de un arbol.
     */
    void ReserveForTrees(int32 NumTrees, int32 CellsPerTree)
    {
        const int32 Expected = FMath::Max(64, NumTrees * FMath::Max(1, CellsPerTree));
        WaterDeltas.Reserve(Expected);
        NutrientDeltas.Reserve(Expected);
    }

    /** Memoria realmente ocupada por los deltas (para Eco.Profile). */
    int32 DeltaBytes() const
    {
        return (WaterDeltas.Max() + NutrientDeltas.Max()) * sizeof(FCellDelta);
    }
};
