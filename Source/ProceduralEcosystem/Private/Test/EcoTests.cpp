#include "Misc/AutomationTest.h"
#include "Core/EcoCore.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TreePopulation.h"

#if WITH_DEV_AUTOMATION_TESTS
static constexpr EAutomationTestFlags EcoTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoRng, "Eco.Rng.Determinismo", EcoTestFlags)
bool FEcoRng::RunTest(const FString&) {
    uint32 a = 12345, b = 12345;
    for (int i = 0; i < 100000; ++i) {
        float u = EcoRand::NextUnit(a);
        TestTrue(TEXT("[0,1)"), u >= 0.f && u < 1.f);
        TestEqual(TEXT("misma semilla misma secuencia"), u, EcoRand::NextUnit(b));
    }
    uint32 z = 0; TestNotEqual(TEXT("0 no absorbente"), EcoRand::NextU32(z), 0u);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoVigor, "Eco.Vigor.Formulas", EcoTestFlags)
bool FEcoVigor::RunTest(const FString&) {
    TestTrue(TEXT("Monod=0.5"), FMath::IsNearlyEqual(EcologyRules::DemandFactor(1.f, 1.f), 0.5f, 1e-4f));
    TestEqual(TEXT("Liebig=min"), EcologyRules::Vigor(0.3f, 0.5f, 0.8f), 0.3f);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoNaN, "Eco.Robustez.NoNaN", EcoTestFlags)
bool FEcoNaN::RunTest(const FString&) {
    const float B = EcologyRules::GrowBiomassLogistic(0.f, 0.5f, 0.25f, 0.f, 1.f); 
    TestTrue(TEXT("GrowBiomass finito"), FMath::IsFinite(B));
    TestTrue(TEXT("Height finito"), FMath::IsFinite(EcologyRules::HeightFromBiomass(1.f, 0.f, 2000.f)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoSoA, "Eco.Poblacion.CompactDead", EcoTestFlags)
bool FEcoSoA::RunTest(const FString&) {
    FTreePopulation P;
    for (int i = 0; i < 10; ++i) P.Add(FVector(i, 0, 0), 0, 1u + i, 0.f, 1.f);
    P.State[3] = ETreeState::Dead; P.State[7] = ETreeState::Dead;
    TestEqual(TEXT("2 eliminados"), P.CompactDead(), 2);
    TestEqual(TEXT("Num=8"), P.Num(), 8);
    TestEqual(TEXT("arrays alineados"), P.Biomass.Num(), P.Position.Num());
    TestEqual(TEXT("orden preservado (2 pos1)"), (double)P.Position[1].X, 2.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoMort, "Eco.Mortalidad.Rango", EcoTestFlags)
bool FEcoMort::RunTest(const FString&) {
    for (float age = 0.f; age <= 400.f; age += 25.f)
        for (float w : {0.2f, 2.f, 10.f}) {
            float p = EcologyRules::MortalityProbability(age, 200.f, 1.f, w, 1.f);
            TestTrue(TEXT("pDeath en [0,1]"), p >= 0.f && p <= 1.f);
        }
    return true;
}

#endif