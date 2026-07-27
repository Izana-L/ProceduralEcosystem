#include "Misc/AutomationTest.h"
#include "Core/EcoCore.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TreePopulation.h"
#include "Render/TreeArchetype.h"

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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLodBucket, "Eco.LOD.BucketEscala", EcoTestFlags)
bool FEcoLodBucket::RunTest(const FString&) {
    const int32 N = 5;
    // Invariante del doc 4.2: escala_en_bucket * borde_superior == fraccion de altura.
    for (int32 b = 0; b < N; ++b) {
        const float upper = TreeArchetype::BucketUpperRatio(b, N);
        const float r = upper - 0.01f; // dentro del bucket b, sin tocar el clamp inferior
        TestEqual(TEXT("BucketOf coherente"), TreeArchetype::BucketOf(r, N), b);
        const float scale = TreeArchetype::ScaleWithinBucket(r, b, N);
        TestTrue(TEXT("escala continua al cruzar bucket"), FMath::IsNearlyEqual(scale * upper, r, 1e-3f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLodHysteresis, "Eco.LOD.Histeresis", EcoTestFlags)
bool FEcoLodHysteresis::RunTest(const FString&) {
    const int32 N = 5; const float H = 0.15f;
    // Borde 3/5=0.6; banda = H/N = 0.03. En 0.61 NO cruza; en 0.64 (>0.63) si.
    TestEqual(TEXT("no cruza dentro de la banda"), TreeArchetype::BucketWithHysteresis(0.61f, 2, N, H), 2);
    TestEqual(TEXT("cruza superada la banda"), TreeArchetype::BucketWithHysteresis(0.64f, 2, N, H), 3);
    for (uint32 id = 1; id < 50; ++id)
        TestTrue(TEXT("variante estable en rango"), TreeArchetype::VariantOf(id, 4) < 4);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLodRemap, "Eco.LOD.RemapInstancias", EcoTestFlags)
bool FEcoLodRemap::RunTest(const FString&) {
    // Mapping instancia->id = [10,11,12,13,14]; borro instancias 1 y 3.
    // UE desplaza hacia abajo -> queda [10,12,14] y se reubican 2->1 y 4->2.
    TArray<uint32> Mapping = { 10, 11, 12, 13, 14 };
    TArray<int32>  Removed = { 1, 3 };
    TMap<uint32, int32> NewIndexOf;
    TreeInstancing::CompactMappingAfterRemoval(Mapping, Removed,
        [&](uint32 Id, int32 NewIndex) { NewIndexOf.Add(Id, NewIndex); });

    TestEqual(TEXT("num tras compactar"), Mapping.Num(), 3);
    TestEqual(TEXT("id en 1 == 12"), Mapping[1], 12u);
    TestEqual(TEXT("id en 2 == 14"), Mapping[2], 14u);
    TestEqual(TEXT("12 reubicado a 1"), NewIndexOf.FindRef(12u), 1);
    TestEqual(TEXT("14 reubicado a 2"), NewIndexOf.FindRef(14u), 2);
    return true;
}
#endif