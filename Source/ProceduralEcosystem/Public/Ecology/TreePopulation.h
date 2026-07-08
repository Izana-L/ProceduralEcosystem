#pragma once

#include "CoreMinimal.h"

/**
 * Estado biologico de un arbol dentro de su ciclo de vida (doc. Fase 2, 2.1).
 * Plano (no UENUM): se usa en un array de uint8 dentro de una estructura
 * caliente que se recorre miles de veces por tick, y no necesita exponerse
 * a Blueprint ni al editor.
 */
enum class ETreeState : uint8
{
    Sapling,    // plantula: recien germinado, biomasa baja
    Mature,     // adulto: puede reproducirse (Age > SpeciesData->MaturityAge)
    Senescent,  // declive: aun vivo pero con alta probabilidad de morir (placeholder Fase 5)
    Dead        // marcado para compactar; no se dibuja ni se procesa
};

/**
 * Poblacion de arboles en structure-of-arrays (doc. Fase 2, 2.1).
 *
 * POR QUE SoA Y NO UN TArray<FTreeAgent>: a 20k agentes, cada pasada del tick
 * (crecimiento, sombreado, mortalidad...) toca 2-3 campos como mucho. Con SoA
 * esos campos van contiguos en memoria y la cache los prefetchea bien; con
 * array-of-structs cargarias los ~35 bytes de CADA arbol aunque solo
 * necesites 8. La diferencia es real a este tamano y este proyecto la paga
 * en cada tick, no una vez.
 *
 * El indice i identifica a un arbol de forma ESTABLE mientras esta vivo: los
 * demas sistemas (spatial hash, scratch por hilo) referencian arboles por
 * este indice dentro del mismo tick. Solo cambia al compactar en CompactDead().
 *
 * Esta clase es un contenedor PASIVO: no sabe de campos de recursos, RNG de
 * subsistema ni reglas de ecologia. Eso vive en EcologyRules (clase 3) y en
 * el bucle de tick de EcosystemSubsystem (clase 5). Aqui solo hay datos y las
 * operaciones basicas de gestion del array: anadir, compactar, copiar.
 */
struct PROCEDURALECOSYSTEM_API FTreePopulation
{
    // --- Estado por-agente (arrays paralelos, mismo indice = mismo arbol) ---
    TArray<FVector> Position;   // posicion de mundo, cm
    TArray<uint16>  SpeciesId;  // indice en UEcosystemSettings::Species (solo lectura)
    TArray<float>   Age;        // anios simulados
    TArray<float>   Biomass;    // proxy de tamano (unidades arbitrarias, ver SpeciesData::MaxBiomass)
    TArray<float>   Height;     // cm; cacheada desde Biomass (la lee mucho el grid de luz)
    TArray<float>   Stress;     // acumulador [0..1]
    TArray<ETreeState> State;
    TArray<uint32>  RngState;   // stream RNG propio del arbol -> determinismo bajo paralelismo

    /** Numero de arboles actualmente en el array (vivos + muertos sin compactar). */
    int32 Num() const { return Position.Num(); }

    bool IsValidIndex(int32 Index) const { return Position.IsValidIndex(Index); }

    /** Vacia todos los arrays sin liberar la capacidad reservada. */
    void Reset();

    /** Reserva espacio en todos los arrays a la vez (evita realojos durante germinacion masiva). */
    void Reserve(int32 ExpectedNum);

    /**
     * Anade un arbol nuevo (germinacion) y devuelve su indice.
     * InRngState debe salir de EcoRand::SeedForIndex/Hash32 sobre datos ya
     * deterministas (posicion del padre, tick, contador de semilla) para que
     * el nuevo stream no dependa del orden de procesamiento de los hilos.
     */
    int32 Add(const FVector& InPosition, uint16 InSpeciesId, uint32 InRngState,
        float InAge = 0.f, float InBiomass = 0.f);

    /**
     * Elimina todos los arboles marcados State == Dead, preservando el orden
     * relativo de los que quedan vivos (compactacion estable, doc. 2.7: "la
     * germinacion y compactacion en orden fijo"). No es la tecnica clasica de
     * swap-with-last: esa reordena y rompe la correspondencia estable índice
     * <-> "posición de nacimiento" que pide el documento para reproducibilidad.
     * Devuelve cuantos arboles se eliminaron.
     */
    int32 CompactDead();

    /**
     * Copia el contenido de Src sobre este objeto, redimensionando si hace
     * falta. Se usa al inicio de cada tick para preparar el buffer de
     * escritura a partir del snapshot de lectura (doc. 2.4, paso 1).
     */
    void CopyFrom(const FTreePopulation& Src);
};