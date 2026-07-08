#include "Ecology/TreePopulation.h"

void FTreePopulation::Reset()
{
    Position.Reset();
    SpeciesId.Reset();
    Age.Reset();
    Biomass.Reset();
    Height.Reset();
    Stress.Reset();
    State.Reset();
    RngState.Reset();
}

void FTreePopulation::Reserve(int32 ExpectedNum)
{
    Position.Reserve(ExpectedNum);
    SpeciesId.Reserve(ExpectedNum);
    Age.Reserve(ExpectedNum);
    Biomass.Reserve(ExpectedNum);
    Height.Reserve(ExpectedNum);
    Stress.Reserve(ExpectedNum);
    State.Reserve(ExpectedNum);
    RngState.Reserve(ExpectedNum);
}

int32 FTreePopulation::Add(const FVector& InPosition, uint16 InSpeciesId, uint32 InRngState,
    float InAge, float InBiomass)
{
    const int32 Index = Position.Add(InPosition);
    SpeciesId.Add(InSpeciesId);
    Age.Add(InAge);
    Biomass.Add(InBiomass);
    Height.Add(0.f); // se recalcula desde Biomass en el primer tick (clase 3: EcologyRules)
    Stress.Add(0.f);
    State.Add(ETreeState::Sapling);
    RngState.Add((InRngState != 0u) ? InRngState : 1u); // 0 es absorbente para xorshift32 (ver EcoCore)
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
            Position[Write] = Position[Read];
            SpeciesId[Write] = SpeciesId[Read];
            Age[Write] = Age[Read];
            Biomass[Write] = Biomass[Read];
            Height[Write] = Height[Read];
            Stress[Write] = Stress[Read];
            State[Write] = State[Read];
            RngState[Write] = RngState[Read];
        }
        ++Write;
    }

    Position.SetNum(Write, EAllowShrinking::No);
    SpeciesId.SetNum(Write, EAllowShrinking::No);
    Age.SetNum(Write, EAllowShrinking::No);
    Biomass.SetNum(Write, EAllowShrinking::No);
    Height.SetNum(Write, EAllowShrinking::No);
    Stress.SetNum(Write, EAllowShrinking::No);
    State.SetNum(Write, EAllowShrinking::No);
    RngState.SetNum(Write, EAllowShrinking::No);

    return OldNum - Write;
}

void FTreePopulation::CopyFrom(const FTreePopulation& Src)
{
    Position = Src.Position;
    SpeciesId = Src.SpeciesId;
    Age = Src.Age;
    Biomass = Src.Biomass;
    Height = Src.Height;
    Stress = Src.Stress;
    State = Src.State;
    RngState = Src.RngState;
}