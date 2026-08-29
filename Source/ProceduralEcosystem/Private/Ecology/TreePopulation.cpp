/**
 * @file TreePopulation.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de las operaciones de gestión del SoA de árboles.
 *
 * Contiene las cuatro operaciones de FTreePopulation que no son inline: la reserva y la
 * copia, que se reducen a recorrer el visitor ForEachArray; el alta de un árbol, único
 * punto que enumera los campos a mano porque cada uno recibe un valor distinto; y la
 * compactación estable de los muertos, escrita con punteros de lectura y escritura para
 * conservar el orden relativo de los vivos.
 *
 * @ingroup eco_ecology
 */

#include "Ecology/TreePopulation.h"

void FTreePopulation::Reserve(int32 ExpectedNum)
{
    ForEachArray([ExpectedNum](auto& Array) { Array.Reserve(ExpectedNum); });
}

int32 FTreePopulation::Add(const FVector& InPosition, uint16 InSpeciesId, uint32 InRngState,
    float InAge, float InBiomass)
{
    // Aquí sí se enumeran los campos: cada uno recibe un valor distinto, así que no hay
    // nada que factorizar con el visitor ForEachArray.
    const int32 Index = Position.Add(InPosition);
    SpeciesId.Add(InSpeciesId);
    Age.Add(InAge);
    Biomass.Add(InBiomass);
    Height.Add(0.f); // EcologyRules la recalcula desde Biomass en el primer tick
    Stress.Add(0.f);
    State.Add(ETreeState::Sapling);
    RngState.Add((InRngState != 0u) ? InRngState : 1u); // 0 es absorbente para xorshift32
    StableId.Add(NextStableId++);
    Vigor.Add(0.f);   // instrumentación: el primer tick la sobrescribe
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
            // El visitor copia los once campos a la vez: es lo que garantiza que ninguno
            // se quede atrás y acabe emparejado con los datos de otro árbol.
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
