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
    Senescent,  // declive (Fase 5): casi deja de crecer, se reproduce menos y su
    // probabilidad de morir se multiplica. Es IRREVERSIBLE: una vez
    // dentro no se vuelve a Mature aunque baje el estres.
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
    
    TArray<FVector> Position;   // posicion de mundo, cm
    TArray<uint16>  SpeciesId;  // indice en UEcosystemSettings::Species (solo lectura)
    TArray<float>   Age;        // anios simulados
    TArray<float>   Biomass;    // proxy de tamano (unidades arbitrarias, ver SpeciesData::MaxBiomass)
    TArray<float>   Height;     // cm; cacheada desde Biomass (la lee mucho el grid de luz)
    TArray<float>   Stress;     // acumulador [0..1]
    TArray<ETreeState> State;
    TArray<uint32>  RngState;   // stream RNG propio del arbol -> determinismo bajo paralelismo
    TArray<uint32>  StableId;

    /** Siguiente id a repartir. Se copia en CopyFrom para que los ids de la
        germinacion sobre el buffer de escritura sean deterministas. */
    uint32 NextStableId = 1;

    /**
     * =====================================================================
     *  EL UNICO SITIO DONDE SE ENUMERAN LOS ARRAYS DEL SoA
     * =====================================================================
     * Invoca Visit(Array) sobre los NUEVE arrays paralelos, en orden fijo.
     *
     * POR QUE: la lista de campos estaba escrita a mano en SEIS sitios
     * distintos -Reserve, el bloque de copia de CompactDead, sus SetNum, su
     * checkSlow, CopyFrom y la serializacion del bake (mas la validacion de
     * longitudes al cargarlo)-. Anadir un campo nuevo al agente obligaba a
     * acordarse de todos; olvidar uno no da error de compilacion, da un array
     * descuadrado que revienta -o peor, mezcla datos de arboles distintos- a
     * mitad de una partida larga. Con el visitor, el campo se declara una vez
     * aqui, se anade a esta lista, y los seis sitios quedan correctos solos.
     *
     * Visit tiene que ser un lambda GENERICO (auto&), porque los arrays son de
     * tipos distintos. Se expande e inlinea entero: no cuesta nada en runtime.
     */
    template <typename FVisit>
    FORCEINLINE void ForEachArray(FVisit&& Visit)
    {
        Visit(Position);  Visit(SpeciesId); Visit(Age);   Visit(Biomass);
        Visit(Height);    Visit(Stress);    Visit(State); Visit(RngState);
        Visit(StableId);
    }

    template <typename FVisit>
    FORCEINLINE void ForEachArray(FVisit&& Visit) const
    {
        Visit(Position);  Visit(SpeciesId); Visit(Age);   Visit(Biomass);
        Visit(Height);    Visit(Stress);    Visit(State); Visit(RngState);
        Visit(StableId);
    }

    /** Igual, pero emparejando los arrays de DOS poblaciones (destino, origen).
        Lo usa CopyFrom; el orden es el mismo que el de ForEachArray. */
    template <typename FVisit>
    FORCEINLINE static void ForEachArrayPair(FTreePopulation& Dst, const FTreePopulation& Src, FVisit&& Visit)
    {
        Visit(Dst.Position,  Src.Position);  Visit(Dst.SpeciesId, Src.SpeciesId);
        Visit(Dst.Age,       Src.Age);       Visit(Dst.Biomass,   Src.Biomass);
        Visit(Dst.Height,    Src.Height);    Visit(Dst.Stress,    Src.Stress);
        Visit(Dst.State,     Src.State);     Visit(Dst.RngState,  Src.RngState);
        Visit(Dst.StableId,  Src.StableId);
    }
    /** Numero de arboles actualmente en el array (vivos + muertos sin compactar). */
    int32 Num() const { return Position.Num(); }

    /**
     * true si los NUEVE arrays paralelos tienen exactamente ExpectedNum
     * elementos. Es la invariante del SoA: la comprueba CompactDead y la usa
     * LoadState para rechazar un bake descuadrado ANTES de pisar el estado vivo
     * (antes esa validacion era otra lista de campos escrita a mano).
     */
    bool AllArraysHaveNum(int32 ExpectedNum) const
    {
        bool bOk = true;
        ForEachArray([ExpectedNum, &bOk](const auto& Array) { bOk = bOk && (Array.Num() == ExpectedNum); });
        return bOk;
    }

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
     * swap-with-last: esa reordena y rompe la correspondencia estable indice
     * <-> "posicion de nacimiento" que pide el documento para reproducibilidad.
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