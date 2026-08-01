#include "Misc/AutomationTest.h"
#include "Core/EcoCore.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TreePopulation.h"
#include "Ecology/TickScratch.h"
#include "Ecology/CarbonModel.h"      // Fase 6
#include "Terrain/Field2D.h"
#include "Terrain/LightFieldCoarse.h"
#include "Render/TreeArchetype.h"
#include "Geometry/TreeSkeleton.h"    // Fase 6
#include "Geometry/TreeWindData.h"    // Fase 6
#include "Species/SpeciesData.h"      // Fase 6

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

// =============================================================================
//  Fase 5 (correccion B14): la fase se subio sin un solo test. Estos cubren las
//  tres piezas que de verdad tenian logica sutil -y donde estaban los bugs-:
//  la senescencia, el anillo de muertes y el kernel disperso de la Fase C1.
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoSenescence, "Eco.Fase5.Senescencia", EcoTestFlags)
bool FEcoSenescence::RunTest(const FString&) {
    const float Longevity = 200.f, AgeFrac = 0.75f, StressThr = 0.85f;

    // Entrada por VEJEZ: 0.75*200 = 150 anios.
    TestFalse(TEXT("joven y sin estres: no senescente"),
        EcologyRules::IsSenescent(100.f, Longevity, AgeFrac, 0.f, StressThr));
    TestTrue(TEXT("pasada la fraccion de longevidad: senescente"),
        EcologyRules::IsSenescent(150.f, Longevity, AgeFrac, 0.f, StressThr));

    // Entrada por ESTRES, aunque sea joven.
    TestTrue(TEXT("estres por encima del umbral: senescente"),
        EcologyRules::IsSenescent(10.f, Longevity, AgeFrac, 0.9f, StressThr));

    // IsSenescent es pura sobre el estres ACTUAL: por si sola NO recuerda. La
    // irreversibilidad la impone SimulateTick con un OR contra el estado previo;
    // este test documenta el contrato para que nadie la use suelta.
    TestFalse(TEXT("IsSenescent no recuerda: al recuperarse devuelve false"),
        EcologyRules::IsSenescent(10.f, Longevity, AgeFrac, 0.1f, StressThr));

    // El crecimiento se frena y la mortalidad se multiplica, ambos acotados.
    TestEqual(TEXT("sano: crecimiento x1"), EcologyRules::SenescentGrowthFactor(false, 0.1f), 1.f);
    TestEqual(TEXT("senescente: crecimiento x escala"), EcologyRules::SenescentGrowthFactor(true, 0.1f), 0.1f);
    TestEqual(TEXT("sano: pDeath intacta"), EcologyRules::ApplySenescentMortality(0.2f, false, 3.f), 0.2f);
    TestTrue(TEXT("senescente: pDeath x3"),
        FMath::IsNearlyEqual(EcologyRules::ApplySenescentMortality(0.2f, true, 3.f), 0.6f, 1e-5f));
    TestEqual(TEXT("pDeath nunca pasa de 1"), EcologyRules::ApplySenescentMortality(0.5f, true, 10.f), 1.f);
    TestEqual(TEXT("multiplicador < 1 no reduce la mortalidad"),
        EcologyRules::ApplySenescentMortality(0.2f, true, 0.5f), 0.2f);
    return true;
}

/**
 * Anillo de muertes: reproduce la logica de RecordDeathEvent /
 * CollectNewDeathEvents con la capacidad FIJA de la correccion A9. Lo importante
 * es el WRAP-AROUND: es la parte con indices modulares y era donde el escritor y
 * el lector podian desincronizarse.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeathRing, "Eco.Fase5.AnilloDeMuertes", EcoTestFlags)
bool FEcoDeathRing::RunTest(const FString&) {
    const int32 Cap = 8;
    TArray<int64> Ring; Ring.SetNumZeroed(Cap); // guardamos el id global de cada evento
    int64 Counter = 0;

    auto Record = [&](int64 Id) { Ring[static_cast<int32>(Counter % Cap)] = Id; ++Counter; };
    auto Collect = [&](int64& Cursor, TArray<int64>& Out)
        {
            const int64 From = FMath::Max<int64>(Cursor, Counter - Cap);
            for (int64 g = From; g < Counter; ++g) { Out.Add(Ring[static_cast<int32>(g % Cap)]); }
            Cursor = Counter;
        };

    // 1) Fase de llenado: el consumidor ve exactamente lo que se escribio.
    int64 Cursor = 0;
    for (int64 i = 0; i < 5; ++i) { Record(i); }
    TArray<int64> Out;
    Collect(Cursor, Out);
    TestEqual(TEXT("llenado: 5 eventos"), Out.Num(), 5);
    for (int32 k = 0; k < Out.Num(); ++k) { TestEqual(TEXT("llenado: en orden"), Out[k], (int64)k); }

    // 2) Nada nuevo -> nada que entregar (el cursor ya esta al dia).
    Out.Reset(); Collect(Cursor, Out);
    TestEqual(TEXT("sin muertes nuevas: 0 eventos"), Out.Num(), 0);

    // 3) WRAP-AROUND: se escriben mas de Cap eventos sin consumir. Solo deben
    //    entregarse los ultimos Cap, y en orden.
    for (int64 i = 5; i < 30; ++i) { Record(i); }
    Out.Reset(); Collect(Cursor, Out);
    TestEqual(TEXT("wrap: solo caben Cap eventos"), Out.Num(), Cap);
    TestEqual(TEXT("wrap: el mas antiguo disponible es Counter-Cap"), Out[0], (int64)(30 - Cap));
    TestEqual(TEXT("wrap: el ultimo es el mas reciente"), Out.Last(), (int64)29);
    for (int32 k = 1; k < Out.Num(); ++k)
    {
        TestEqual(TEXT("wrap: consecutivos"), Out[k], Out[k - 1] + 1);
    }

    // 4) El cursor queda al dia tras consumir.
    TestEqual(TEXT("cursor al dia"), Cursor, Counter);
    return true;
}

/**
 * Kernel disperso (optimizacion C1): debe repartir EXACTAMENTE la misma cantidad
 * que la version densa, celda a celda. Si esto pasa, la reduccion nueva produce
 * el mismo campo que la vieja salvo por el orden de suma en punto flotante.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoSparseKernel, "Eco.Fase5.KernelDisperso", EcoTestFlags)
bool FEcoSparseKernel::RunTest(const FString&) {
    FField2D Geometry;
    Geometry.Init(16, 16, /*CellSize*/ 100.0, FVector2D::ZeroVector, 0.f);

    const FVector Pos(750.0, 820.0, 0.0);
    const float Radius = 350.f;
    const float Amount = -12.5f;

    // Denso
    TArray<float> Dense; Dense.SetNumZeroed(Geometry.Num());
    EcologyRules::DepositKernel(Geometry, Dense, Pos, Radius, Amount);

    // Disperso -> aplicado sobre un campo equivalente
    TArray<FCellDelta> Sparse;
    EcologyRules::DepositKernelSparse(Geometry, Sparse, Pos, Radius, Amount);
    TArray<float> FromSparse; FromSparse.SetNumZeroed(Geometry.Num());
    for (const FCellDelta& D : Sparse)
    {
        TestTrue(TEXT("celda dentro de rango"), FromSparse.IsValidIndex(D.Cell));
        FromSparse[D.Cell] += D.Amount;
    }

    TestTrue(TEXT("el kernel deposita en alguna celda"), Sparse.Num() > 0);
    for (int32 c = 0; c < Dense.Num(); ++c)
    {
        TestTrue(TEXT("denso == disperso celda a celda"),
            FMath::IsNearlyEqual(Dense[c], FromSparse[c], 1e-4f));
    }

    // Conservacion de masa: el kernel esta normalizado, asi que la suma de lo
    // depositado es exactamente TotalAmount (esa es la razon de las dos pasadas).
    float Total = 0.f;
    for (const FCellDelta& D : Sparse) { Total += D.Amount; }
    TestTrue(TEXT("se deposita exactamente TotalAmount"), FMath::IsNearlyEqual(Total, Amount, 1e-3f));

    // Radio 0 o cantidad 0: no se emite nada (y no se peta).
    Sparse.Reset();
    EcologyRules::DepositKernelSparse(Geometry, Sparse, Pos, 0.f, Amount);
    TestEqual(TEXT("radio 0 -> sin deltas"), Sparse.Num(), 0);
    EcologyRules::DepositKernelSparse(Geometry, Sparse, Pos, Radius, 0.f);
    TestEqual(TEXT("cantidad 0 -> sin deltas"), Sparse.Num(), 0);
    return true;
}

/**
 * Rejilla de luz relativa al terreno (optimizacion C2): dos columnas con cotas
 * muy distintas deben comportarse IGUAL para la misma altura sobre el suelo. Es
 * justo lo que la version absoluta no hacia.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLightTerrainRelative, "Eco.Fase5.LuzRelativaAlTerreno", EcoTestFlags)
bool FEcoLightTerrainRelative::RunTest(const FString&) {
    FLightFieldCoarse Light;
    const int32 W = 8, H = 8, L = 10;
    const double Cell = 400.0;
    Light.Init(W, H, L, Cell, Cell, FVector2D::ZeroVector, /*BaseZ*/ -400.0);
    TestFalse(TEXT("sin GroundZ es absoluta"), Light.IsTerrainRelative());

    // Columna 1 en un valle (z=0), columna 6 en una cresta (z=10.000 cm).
    TArray<float> Ground; Ground.SetNumZeroed(W * H);
    for (int32 y = 0; y < H; ++y)
        for (int32 x = 0; x < W; ++x)
            Ground[y * W + x] = (x >= 4) ? 10000.f : 0.f;
    Light.SetGroundHeights(MoveTemp(Ground));
    TestTrue(TEXT("con GroundZ es relativa"), Light.IsTerrainRelative());

    // Sin sombra: luz plena en cualquier sitio.
    TestEqual(TEXT("sin sombra = luz plena"),
        Light.SampleLight(FVector(600.0, 600.0, 300.0)), FLightFieldCoarse::FullSunlight);

    // Dos copas identicas, una en cada altiplano, a la MISMA altura sobre el suelo.
    const float CanopyR = 500.f, CanopyDepth = 1200.f;
    Light.DepositCanopyShadow(FVector(600.0, 600.0, 0.0 + 1500.0), CanopyR, CanopyDepth, 0.8f);
    Light.DepositCanopyShadow(FVector(2200.0, 600.0, 10000.0 + 1500.0), CanopyR, CanopyDepth, 0.8f);

    // A ras de suelo, bajo cada copa, la luz debe ser la MISMA pese a los 100 m
    // de diferencia de cota.
    const float ValleyQ = Light.SampleLight(FVector(600.0, 600.0, 0.0 + 50.0));
    const float RidgeQ = Light.SampleLight(FVector(2200.0, 600.0, 10000.0 + 50.0));
    TestTrue(TEXT("misma altura sobre el suelo -> misma luz"),
        FMath::IsNearlyEqual(ValleyQ, RidgeQ, 1e-4f));
    TestTrue(TEXT("bajo la copa hay sombra"), ValleyQ < FLightFieldCoarse::FullSunlight);

    // Y una copa NO puede sombrear la otra columna (estan lejos en XY).
    const float FarQ = Light.SampleLight(FVector(3400.0, 2600.0, 10000.0 + 50.0));
    TestEqual(TEXT("lejos de toda copa = luz plena"), FarQ, FLightFieldCoarse::FullSunlight);

    // La rejilla es pequena: es el objetivo de la optimizacion.
    TestTrue(TEXT("la rejilla cabe en pocas capas"), Light.Layers <= 16);
    return true;
}

// =============================================================================
//  FASE 6 — realismo y optimizacion final
// =============================================================================

/**
 * CO2 (doc. 6.3). Lo importante que hay que garantizar es que la capa sea
 * INOFENSIVA: acotada, sin discontinuidades, y con un "off" que devuelve 1.0
 * EXACTO -sin ese exacto, la ablacion de la Fase 7 no seria comparable bit a
 * bit con las corridas anteriores-.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoCO2, "Eco.Fase6.CO2", EcoTestFlags)
bool FEcoCO2::RunTest(const FString&) {
    EcoCarbon::FCO2Params P;
    P.bEnabled = true;
    P.MaxReduction = 0.15f;
    P.FullMixingHeightCm = 2500.f;
    P.FullSunlight = 1.f;

    // A pleno sol no hay penalizacion, este el arbol a la altura que este.
    TestTrue(TEXT("pleno sol -> factor 1"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(1.f, 0.f, P), 1.f, 1e-5f));
    TestTrue(TEXT("pleno sol, arbol alto -> factor 1"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(1.f, 3000.f, P), 1.f, 1e-5f));

    // Peor caso: oscuridad total a ras de suelo -> exactamente 1 - MaxReduction.
    TestTrue(TEXT("dosel cerrado a ras de suelo -> 1 - MaxReduction"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(0.f, 0.f, P), 1.f - P.MaxReduction, 1e-5f));

    // La altura recupera el factor: por encima de FullMixingHeightCm no penaliza.
    TestTrue(TEXT("por encima del dosel no hay penalizacion"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(0.f, P.FullMixingHeightCm, P), 1.f, 1e-5f));
    TestTrue(TEXT("a media altura la penalizacion es intermedia"),
        EcoCarbon::CO2Factor(0.f, P.FullMixingHeightCm * 0.5f, P) > EcoCarbon::CO2Factor(0.f, 0.f, P));

    // Monotono en la luz y acotado en TODO el dominio (incluidos valores absurdos).
    float Prev = -1.f;
    for (float Q = 0.f; Q <= 1.001f; Q += 0.05f)
    {
        const float F = EcoCarbon::CO2Factor(Q, 0.f, P);
        TestTrue(TEXT("factor en [1-MaxReduction, 1]"), F >= 1.f - P.MaxReduction - 1e-4f && F <= 1.f + 1e-4f);
        TestTrue(TEXT("mas luz nunca da menos CO2"), F >= Prev - 1e-4f);
        Prev = F;
    }
    TestTrue(TEXT("Q negativo no rompe"), FMath::IsFinite(EcoCarbon::CO2Factor(-5.f, 0.f, P)));
    TestTrue(TEXT("altura negativa no rompe"), FMath::IsFinite(EcoCarbon::CO2Factor(0.5f, -100.f, P)));

    // ABLACION: apagado devuelve 1.0 EXACTO (no "casi 1"), que es lo que permite
    // reproducir bit a bit los resultados anteriores a la Fase 6.
    P.bEnabled = false;
    TestEqual(TEXT("desactivado -> 1.0 exacto"), EcoCarbon::CO2Factor(0.f, 0.f, P), 1.f);
    P.bEnabled = true; P.MaxReduction = 0.f;
    TestEqual(TEXT("MaxReduction 0 -> 1.0 exacto"), EcoCarbon::CO2Factor(0.f, 0.f, P), 1.f);
    return true;
}

/**
 * Datos de viento (doc. 6.1). Se construye un esqueleto minimo con UNA
 * bifurcacion y se comprueban las cuatro propiedades de las que depende que el
 * balanceo se vea bien:
 *   1. La base del tronco NO se mueve (esta empotrada en el suelo).
 *   2. El movimiento crece hacia las puntas.
 *   3. Los nodos de una MISMA rama comparten pivote y desfase (si no, el tubo se
 *      retuerce en vez de balancearse).
 *   4. Ramas distintas tienen desfases distintos (si no, el arbol entero se mueve
 *      como una sola pieza).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoWindData, "Eco.Fase6.DatosDeViento", EcoTestFlags)
bool FEcoWindData::RunTest(const FString&) {
    USpeciesData* Sp = NewObject<USpeciesData>(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->WindStiffness = 0.f;      // maxima flexibilidad: aisla la forma de la curva
    Sp->LeafFlutterScale = 1.f;

    // Tronco de 4 nodos y, en el ultimo, una bifurcacion en dos ramas de 2 nodos.
    //        5   6      <- rama B
    //         \ /
    //  0-1-2-3-4        <- tronco (rama A) ... 4 abre las dos hijas
    FTreeSkeleton Sk;
    Sk.InitRoot(FVector::ZeroVector, FVector::UpVector);
    int32 Prev = 0;
    for (int32 i = 1; i <= 3; ++i)
    {
        Prev = Sk.AddChild(Prev, FVector(0, 0, i * 100.0), FVector::UpVector);
    }
    const int32 Fork = Prev;                                   // nodo 3: aqui bifurca
    const int32 A0 = Sk.AddChild(Fork, FVector(80, 0, 380.0), FVector(1, 0, 0.5).GetSafeNormal());
    const int32 A1 = Sk.AddChild(A0, FVector(160, 0, 440.0), FVector(1, 0, 0.4).GetSafeNormal());
    const int32 B0 = Sk.AddChild(Fork, FVector(-80, 0, 380.0), FVector(-1, 0, 0.5).GetSafeNormal());

    // Radios como los dejaria el pipe model: gruesos abajo, finos arriba.
    for (int32 i = 0; i < Sk.Num(); ++i)
    {
        Sk.Nodes[i].Radius = FMath::Lerp(10.f, 1.f, (float)i / FMath::Max(1, Sk.Num() - 1));
    }

    FTreeWindData Wind;
    Wind.Build(Sk, *Sp, /*FineLight*/ nullptr, /*Seed*/ 12345u);

    TestTrue(TEXT("un dato por nodo"), Wind.IsValidFor(Sk));

    // 1) La base no se mueve.
    TestTrue(TEXT("la base del tronco no se balancea"), Wind.Nodes[0].SwayWeight <= KINDA_SMALL_NUMBER);

    // 2) El balanceo crece hacia la punta.
    TestTrue(TEXT("la punta se balancea mas que el tronco"),
        Wind.Nodes[A1].SwayWeight > Wind.Nodes[0].SwayWeight);
    TestTrue(TEXT("el balanceo crece a lo largo de la rama"),
        Wind.Nodes[A1].SwayWeight >= Wind.Nodes[A0].SwayWeight);
    for (const FTreeWindNode& N : Wind.Nodes)
    {
        TestTrue(TEXT("balanceo acotado a [0,1]"), N.SwayWeight >= 0.f && N.SwayWeight <= 1.f);
        TestTrue(TEXT("desfase acotado a [0,1)"), N.Phase01 >= 0.f && N.Phase01 < 1.f);
        TestTrue(TEXT("AO neutro sin rejilla de luz"), FMath::IsNearlyEqual(N.CanopyAO, 1.f, 1e-4f));
    }

    // 3) Nodos de la MISMA rama: mismo pivote y mismo desfase.
    TestTrue(TEXT("misma rama -> mismo pivote"),
        Wind.Nodes[A0].PivotLocalCm.Equals(Wind.Nodes[A1].PivotLocalCm, 0.01));
    TestEqual(TEXT("misma rama -> mismo desfase"), Wind.Nodes[A0].Phase01, Wind.Nodes[A1].Phase01);

    // El tronco es la rama 0 y su pivote es la base.
    TestTrue(TEXT("el pivote del tronco es su base"),
        Wind.Nodes[2].PivotLocalCm.Equals(FVector::ZeroVector, 0.01));
    TestTrue(TEXT("el tronco es el nivel 0"), FMath::IsNearlyEqual(Wind.Nodes[2].BranchLevel01, 0.f, 1e-4f));

    // 4) Ramas hermanas: nacen de la MISMA horquilla, asi que comparten pivote
    //    (es el punto de insercion, no el primer nodo), pero se mueven con
    //    desfases distintos y su nivel es mayor que el del tronco.
    TestTrue(TEXT("las hijas son de nivel mayor que el tronco"),
        Wind.Nodes[A0].BranchLevel01 > Wind.Nodes[2].BranchLevel01);
    TestTrue(TEXT("ramas hermanas -> mismo pivote (la horquilla)"),
        Wind.Nodes[A0].PivotLocalCm.Equals(Wind.Nodes[B0].PivotLocalCm, 0.01));
    TestTrue(TEXT("el pivote es la horquilla, no el primer nodo de la rama"),
        Wind.Nodes[A0].PivotLocalCm.Equals(Sk.Nodes[Fork].Pos, 0.01));
    TestTrue(TEXT("ramas hermanas -> desfases distintos"),
        !FMath::IsNearlyEqual(Wind.Nodes[A0].Phase01, Wind.Nodes[B0].Phase01, 1e-5f));

    // 5) DETERMINISMO: misma semilla, mismos datos; semilla distinta, desfases distintos.
    FTreeWindData Again;
    Again.Build(Sk, *Sp, nullptr, 12345u);
    for (int32 i = 0; i < Wind.Nodes.Num(); ++i)
    {
        TestEqual(TEXT("misma semilla -> mismo desfase"), Again.Nodes[i].Phase01, Wind.Nodes[i].Phase01);
        TestEqual(TEXT("misma semilla -> mismo balanceo"), Again.Nodes[i].SwayWeight, Wind.Nodes[i].SwayWeight);
    }
    FTreeWindData Other;
    Other.Build(Sk, *Sp, nullptr, 999u);
    TestTrue(TEXT("otra semilla -> otro desfase"),
        !FMath::IsNearlyEqual(Other.Nodes[A0].Phase01, Wind.Nodes[A0].Phase01, 1e-5f));

    // 6) La rigidez de la especie escala el balanceo a la baja.
    Sp->WindStiffness = 1.f;
    FTreeWindData Rigid;
    Rigid.Build(Sk, *Sp, nullptr, 12345u);
    TestTrue(TEXT("mas rigidez -> menos balanceo"),
        Rigid.Nodes[A1].SwayWeight < Wind.Nodes[A1].SwayWeight);

    return true;
}
#endif
