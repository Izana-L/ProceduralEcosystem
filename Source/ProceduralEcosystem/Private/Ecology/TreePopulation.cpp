#include "Ecology/TreePopulation.h"

void FTreePopulation::Reserve(int32 ExpectedNum)
{
    ForEachArray([ExpectedNum](auto& Array) { Array.Reserve(ExpectedNum); });
}

int32 FTreePopulation::Add(const FVector& InPosition, uint16 InSpeciesId, uint32 InRngState,
    float InAge, float InBiomass)
{
    // Este SI enumera los campos: cada uno recibe un valor DISTINTO, asi que no
    // hay nada que factorizar (a diferencia de Reserve/CompactDead/CopyFrom, que
    // hacen lo mismo con los once y usan el visitor ForEachArray).
    const int32 Index = Position.Add(InPosition);
    SpeciesId.Add(InSpeciesId);
    Age.Add(InAge);
    Biomass.Add(InBiomass);
    Height.Add(0.f); // se recalcula desde Biomass en el primer tick (clase 3: EcologyRules)
    Stress.Add(0.f);
    State.Add(ETreeState::Sapling);
    RngState.Add((InRngState != 0u) ? InRngState : 1u); // 0 es absorbente para xorshift32 (ver EcoCore)
    StableId.Add(NextStableId++);
    Vigor.Add(0.f);   // instrumentacion: el primer tick lo sobrescribe
    Limiter.Add(0);
    return Index;
}

int32 FTreePopulation::CompactDead()
{
    const int32 OldNum = Num();
    int32 Write = 0;

    for (int32 Read = 0; Read < OldNum; ++Read)
    {
        if (State[Read] == ETreeState::Dead)
        {
            continue; // se descarta
        }
        if (Write != Read)
        {
            // Antes eran las asignaciones escritas a mano; si se anadia un
            // campo al agente y se olvidaba esta linea, el array nuevo se
            // quedaba con los datos de OTRO arbol tras la primera muerte.
            ForEachArray([Write, Read](auto& Array) { Array[Write] = Array[Read]; });
        }
        ++Write;
    }

    ForEachArray([Write](auto& Array) { Array.SetNum(Write, EAllowShrinking::No); });
    checkSlow(AllArraysHaveNum(Write));
    return OldNum - Write;
}

void FTreePopulation::CopyFrom(const FTreePopulation& Src)
{
    ForEachArrayPair(*this, Src, [](auto& Dst, const auto& From) { Dst = From; });
    NextStableId = Src.NextStableId;
}
