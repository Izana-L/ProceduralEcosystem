#pragma once

#include "CoreMinimal.h"

/** Semilla pendiente de germinar, generada durante el paso paralelo del tick. */
struct FPendingSeed
{
    FVector Position = FVector::ZeroVector; // mundo, cm
    uint16  SpeciesId = 0;
    uint32  RngSeed = 1; // stream del hijo, ya derivado (ver clase 5)
};

/** Pulso de nutrientes pendiente de aplicar: un arbol que ha muerto este tick. */
struct FPendingDeathPulse
{
    FVector Position = FVector::ZeroVector; // mundo, cm
    float   RadiusCm = 0.f;                 // mismo radio efectivo que su consumo
    float   Amount = 0.f;                   // ya calculado con DeathNutrientPulse (clase 3)
};

/**
 * Estado privado de UNA tarea paralela dentro de un tick (doc. 2.4:
 * "scratch privado por hilo"). Se usa con ParallelForWithTaskContext (ver
 * clase 5): UE5 crea tantas instancias como decida repartir el trabajo, y
 * cada tarea escribe SOLO en la suya — nunca hay dos tareas escribiendo en
 * el mismo FTickScratch a la vez, así que dentro de esta clase no hace
 * falta ningún lock ni operación atómica.
 *
 * Agrupa las CUATRO cosas que el documento reparte en "Scratch_Water",
 * "Scratch_Nutrient", "Scratch_Seeds" y "Scratch_DeathPulses": en vez de
 * cuatro arrays paralelos de contextos, un solo contexto con cuatro campos
 * — más simple de pasar a ParallelForWithTaskContext, que solo admite un
 * tipo de contexto por llamada.
 */
struct FTickScratch
{
    TArray<float> WaterDeltas;    // mismo tamaño que FWaterField::Field.Data
    TArray<float> NutrientDeltas; // mismo tamaño que FNutrientField::Field.Data
    TArray<FPendingSeed> Seeds;
    TArray<FPendingDeathPulse> DeathPulses;

    /** Reserva los deltas al tamaño del campo (numero de celdas) y los pone a 0. */
    void InitFromFieldSize(int32 NumCells)
    {
        WaterDeltas.SetNumZeroed(NumCells);
        NutrientDeltas.SetNumZeroed(NumCells);
        Seeds.Reset();
        DeathPulses.Reset();
    }

    /** Vuelve a poner los deltas a 0 sin liberar memoria; listo para el siguiente tick. */
    void ResetForNextTick()
    {
        for (float& V : WaterDeltas) { V = 0.f; }
        for (float& V : NutrientDeltas) { V = 0.f; }
        Seeds.Reset();
        DeathPulses.Reset();
    }
};