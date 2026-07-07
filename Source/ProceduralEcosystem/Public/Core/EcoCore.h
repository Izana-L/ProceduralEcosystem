#pragma once

#include "CoreMinimal.h"
#include "EcoCore.generated.h"

// NOTA: la categoría de log LogEco NO se declara aquí. Meter
// DECLARE_LOG_CATEGORY_EXTERN en una cabecera reflejada por UHT (con UENUM/
// USTRUCT) hace que UHT deje de registrar los tipos siguientes. Como solo la
// usa EcosystemSubsystem.cpp, allí se declara y define con
// DEFINE_LOG_CATEGORY_STATIC.

/**
 * RNG determinista y ligero (xorshift32). El estado cabe en un uint32, de modo
 * que en la Fase 2 cada árbol podrá llevar su propio stream sin coste extra.
 *
 * REGLA DE ORO de reproducibilidad: NUNCA uses FMath::Rand()/rand() globales
 * en la simulación. Todo lo estocástico sale de estos streams. Así una misma
 * semilla da SIEMPRE el mismo bosque, y el resultado no depende del hilo que
 * procese cada agente (clave para paralelizar en Fase 2).
 */
namespace EcoRand
{
    FORCEINLINE uint32 NextU32(uint32& State)
    {
        uint32 x = (State != 0u) ? State : 0x9E3779B9u; // el estado 0 es absorbente
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        State = x;
        return x;
    }

    /** Devuelve un float en [0, 1). */
    FORCEINLINE float NextUnit(uint32& State)
    {
        return static_cast<float>(NextU32(State) >> 8) * (1.0f / 16777216.0f);
    }

    /** Float en [Min, Max). */
    FORCEINLINE float NextRange(uint32& State, float Min, float Max)
    {
        return Min + (Max - Min) * NextUnit(State);
    }

    /** Entero en [Min, Max] (ambos inclusive). El sesgo por módulo es despreciable aquí. */
    FORCEINLINE int32 NextRangeInt(uint32& State, int32 Min, int32 Max)
    {
        if (Max <= Min) { return Min; }
        const uint32 Span = static_cast<uint32>(Max - Min) + 1u;
        return Min + static_cast<int32>(NextU32(State) % Span);
    }

    // NOTA Fase 2: aquí vivirán los helpers de dominio (PoissonInt para el nº de
    // semillas, kernel de dispersión, etc.), todos derivados de un uint32& State.

    /** Mezcla de bits (finalizer estilo Murmur3): deriva semillas hijas estables. */
    FORCEINLINE uint32 Hash32(uint32 x)
    {
        x ^= x >> 16; x *= 0x7feb352du;
        x ^= x >> 15; x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    /** Semilla estable por-índice: no depende del orden de proceso. */
    FORCEINLINE uint32 SeedForIndex(uint32 MasterSeed, int32 Index)
    {
        return Hash32(MasterSeed ^ Hash32(static_cast<uint32>(Index) * 2654435761u));
    }
}

/** Subsistemas con su propio stream de RNG (§Fase 0: reproducibilidad). */
UENUM(BlueprintType)
enum class EEcoRngStream : uint8
{
    Colonization,
    Dispersal,
    Mortality,
    Morphology,
    Debug,   // herramientas de depuración: NO perturba los streams de la simulación
    Count UMETA(Hidden)
};

/**
 * Contenedor de un stream por subsistema, derivados de una única semilla maestra.
 * Struct plano (no reflejado): se usa dentro del subsistema de simulación.
 */
struct FEcosystemRng
{
    uint32 MasterSeed = 1u;
    uint32 State[static_cast<int32>(EEcoRngStream::Count)] = {}; // cero-inicializado por seguridad

    void Init(uint32 InMasterSeed)
    {
        MasterSeed = (InMasterSeed != 0u) ? InMasterSeed : 1u;
        for (int32 i = 0; i < static_cast<int32>(EEcoRngStream::Count); ++i)
        {
            State[i] = EcoRand::Hash32(MasterSeed ^ (0x01000193u * static_cast<uint32>(i + 1)));
        }
    }

    FORCEINLINE float  Unit(EEcoRngStream S) { return EcoRand::NextUnit(State[static_cast<int32>(S)]); }
    FORCEINLINE uint32 U32(EEcoRngStream S) { return EcoRand::NextU32(State[static_cast<int32>(S)]); }

    /** Float en [Min, Max). */
    FORCEINLINE float  RangeF(EEcoRngStream S, float Min, float Max)
    {
        return EcoRand::NextRange(State[static_cast<int32>(S)], Min, Max);
    }

    /** Entero en [Min, Max] (inclusive). */
    FORCEINLINE int32  RangeI(EEcoRngStream S, int32 Min, int32 Max)
    {
        return EcoRand::NextRangeInt(State[static_cast<int32>(S)], Min, Max);
    }
};

/** Agente de depuración de la Fase 0 (proto de la población de la Fase 2). */
USTRUCT()
struct FEcoDebugAgent
{
    GENERATED_BODY()

    UPROPERTY() FVector Position = FVector::ZeroVector;
    UPROPERTY() FColor  Color = FColor::Green;
    UPROPERTY() float   Radius = 100.f; // cm
};