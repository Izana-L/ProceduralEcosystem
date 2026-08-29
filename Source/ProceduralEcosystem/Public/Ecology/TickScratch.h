/**
 * @file TickScratch.h
 * @author Juan Luque Roldán
 * @brief Scratch privado por tarea paralela y productos diferidos de un tick.
 *
 * Reúne todo lo que una tarea del ParallelFor del tick produce sin poder aplicarlo en el
 * acto: los deltas dispersos de agua y nutrientes (FCellDelta), las semillas pendientes
 * de germinar (FPendingSeed), los pulsos de descomposición de los árboles muertos
 * (FPendingDeathPulse) y el embudo de reclutamiento y mortalidad por especie
 * (FEcoSpeciesFlow), agrupados en un único contexto por tarea (FTickScratch). Cada tarea
 * escribe solo en el suyo, de modo que dentro no hace falta ni lock ni atómica; el estado
 * común se reconstruye después en un paso serial de orden fijo,
 * EcologyRules::ReduceScratchInto, que es lo que hace el tick reproducible bit a bit.
 *
 * @ingroup eco_ecology
 * @see @ref bib_acton2014
 * @see @ref bib_goldberg1991
 */

#pragma once

#include "CoreMinimal.h"

/** Semilla pendiente de germinar, generada durante el paso paralelo del tick. */
struct FPendingSeed
{
    FVector Position = FVector::ZeroVector; ///< Punto de caída, en cm de mundo.
    uint16  SpeciesId = 0;                  ///< Especie de la madre.
    uint32  RngSeed = 1;                    ///< Stream RNG del hijo, ya derivado del de la madre.

    /**
     * StableId del árbol madre, para excluirlo del conteo de conespecíficos.
     *
     * Cuando el radio de dispersión no supera al de inhibición, toda semilla cae dentro
     * del círculo de su propia madre: sin esta exclusión, hasta el último adulto de una
     * especie al borde de la extinción pagaría la penalización por verse a sí mismo, lo
     * que pondría un techo al rescate de la especie rara justo donde el mecanismo debe
     * ser más fuerte.
     *
     * @see EcologyRules::ConspecificInhibitionFactor
     */
    uint32  ParentStableId = 0;
};

/**
 * Contadores por especie de un tick: el embudo de reclutamiento y el reparto de causas de
 * muerte.
 *
 * El reclutamiento atraviesa cuatro filtros multiplicativos —fuera de mapa, espaciado
 * mínimo, luz e inhibición conespecífica— y cualquiera de ellos puede estar aportando un
 * factor decisivo sin que el recuento de población lo delate: éste dice qué especie se
 * hunde, y estos contadores distinguen si es porque recluta poco, porque se le mueren las
 * plántulas o porque no llega a madurez. Es para el reclutamiento lo que
 * FTreePopulation::Limiter es para el crecimiento.
 *
 * Cada tarea acumula los suyos y se reducen sumándolos en orden fijo, nunca con atómicas:
 * un resultado dependiente del orden de los hilos rompería la reproducibilidad del tick.
 */
struct FEcoSpeciesFlow
{
    // ==== Embudo de reclutamiento ====
    int32 SeedsEmitted = 0;      ///< Semillas emitidas por los adultos de la especie.
    int32 RejectedOffMap = 0;    ///< Descartadas por caer fuera del mundo.
    int32 RejectedSpacing = 0;   ///< Descartadas por el espaciado mínimo con un vecino.
    int32 RejectedLight = 0;     ///< Descartadas por no ser el punto de caída un sitio seguro.
    int32 Germinated = 0;        ///< Semillas que acaban siendo un árbol nuevo.

    float JanzenConnellSum = 0.f;  ///< Suma de los factores de inhibición evaluados.
    int32 JanzenConnellCount = 0;  ///< Cuántos se evaluaron; el cociente da la media.

    // ==== Mortalidad ====
    // La muerte se atribuye al canal cuyo riesgo dominaba al morir. No es una causa en
    // sentido estricto -los dos canales actúan a la vez- pero responde a la pregunta que
    // importa: si el canal de edad no aparece nunca, la longevidad no compra nada y la
    // estrategia K no puede existir.
    int32 DeathsByAge = 0;         ///< Muertes con el riesgo por edad dominante.
    int32 DeathsByCondition = 0;   ///< Muertes con el riesgo por estrés o supresión dominante.

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

/**
 * Árbol muerto en este tick, pendiente de devolver sus nutrientes al suelo.
 *
 * Transporta además la identidad y el tamaño del muerto, que la capa de render necesita
 * para generar la caída, el tocón y la hojarasca.
 *
 * @see EcologyRules::DeathNutrientPulse
 * @see FTreeDeathEvent
 */
struct FPendingDeathPulse
{
    FVector Position = FVector::ZeroVector; ///< Base del tronco, en cm de mundo.
    float   RadiusCm = 0.f;                 ///< Mismo radio efectivo con el que consumía.
    float   Amount = 0.f;                   ///< Nutrientes a depositar, ya calculados.

    uint16  SpeciesId = 0;                  ///< Índice en UEcosystemSettings::Species.
    uint32  StableId = 0;                   ///< Id estable: fija yaw y variante del tocón.
    float   Biomass = 0.f;                  ///< Biomasa al morir.
    float   HeightCm = 0.f;                 ///< Altura al morir.
};

/**
 * Depósito puntual sobre una celda de campo: «súmale Amount a la celda Cell».
 *
 * Cada tarea apila estos pares dispersos en lugar de escribir sobre un campo denso del
 * tamaño del mundo. Con 512x512 celdas y hasta 32 tareas, el scratch denso ocuparía
 * ~64 MB y su reducción serial costaría 32 x 262.144 x 2 = 16,8 M sumas por tick, de las
 * que el 99,9 % suman cero. El trabajo real es mucho menor: el radio efectivo de un adulto
 * es el mayor entre RootRadius metros y MinRootRadiusCells celdas —400 cm, o sea 2 celdas
 * a 200 cm por celda, con los valores por defecto—, así que el kernel de consumo toca un
 * bloque de 5x5 y 20.000 árboles sobre dos campos son ~1.000.000 de escrituras. En
 * disperso, la reducción cuesta lo que de verdad se depositó y la memoria baja a unos
 * pocos MB.
 *
 * El orden es igual de fijo que en denso —tareas por índice creciente y, dentro de cada
 * una, orden de inserción: árbol por índice creciente, celda por el orden del kernel—,
 * aunque no sea el mismo, así que la reducción sigue siendo reproducible.
 *
 * @see EcologyRules::DepositKernelSparse
 */
struct FCellDelta
{
    int32 Cell = 0;    ///< Índice plano de la celda en la rejilla del campo.
    float Amount = 0.f; ///< Cantidad a sumar; negativa si es consumo.
};

/**
 * Estado privado de una tarea paralela dentro de un tick.
 *
 * Cada tarea del ParallelFor escribe solo en su propia instancia, así que ningún campo de
 * esta estructura necesita lock ni atómica. Agrupa en un único contexto los cuatro
 * productos diferidos del tick —deltas de agua, deltas de nutrientes, semillas y pulsos
 * de muerte— más el embudo por especie: es más simple de pasar por tarea que cuatro
 * arrays paralelos, y da un único sitio donde reservar y resetear.
 *
 * @see EcologyRules::ReduceScratchInto
 */
struct FTickScratch
{
    TArray<FCellDelta> WaterDeltas;          ///< Consumo de agua, en pares (celda, cantidad).
    TArray<FCellDelta> NutrientDeltas;       ///< Consumo de nutrientes y pulsos de descomposición.
    TArray<FPendingSeed> Seeds;              ///< Semillas emitidas, pendientes de germinar.
    TArray<FPendingDeathPulse> DeathPulses;  ///< Muertos pendientes de devolver nutrientes.

    TArray<FEcoSpeciesFlow> SpeciesFlow;     ///< Embudo por especie; el índice es el SpeciesId.

    /** Vacía los buffers sin liberar su capacidad, listo para el siguiente tick. */
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
     * Reserva de una vez la capacidad esperada de los deltas, para que los Add() del paso
     * paralelo no realojen a mitad del tick.
     *
     * @param CellsPerTree Cota de celdas que toca el kernel de consumo de un árbol.
     * @see EcologyRules::KernelCellCount
     */
    void ReserveForTrees(int32 NumTrees, int32 CellsPerTree)
    {
        const int32 Expected = FMath::Max(64, NumTrees * FMath::Max(1, CellsPerTree));
        WaterDeltas.Reserve(Expected);
        NutrientDeltas.Reserve(Expected);
    }

    /** Memoria reservada por los deltas, en bytes; la reporta el perfilado Eco.Profile. */
    int32 DeltaBytes() const
    {
        return (WaterDeltas.Max() + NutrientDeltas.Max()) * sizeof(FCellDelta);
    }
};
