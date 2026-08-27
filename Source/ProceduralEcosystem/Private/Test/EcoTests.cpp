#include "Misc/AutomationTest.h"
#include "Core/EcoCore.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TreePopulation.h"
#include "Ecology/TickScratch.h"
#include "Ecology/CarbonModel.h"      // Fase 6
#include "Terrain/Field2D.h"
#include "Terrain/HeightField.h"   // relieve realista (ruido + erosion)
#include "Terrain/LightFieldCoarse.h"
#include "Render/TreeArchetype.h"
#include "Geometry/TreeSkeleton.h"    // Fase 6
#include "Geometry/TreeWindData.h"    // Fase 6
#include "Geometry/SpaceColonization.h"
#include "Geometry/AttractorCloud.h"
#include "Geometry/TreeLightGridFine.h"
#include "Geometry/TreeMeshBuilder.h"
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
//  FASE 6 � realismo y optimizacion final
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
    // A0 (indice 4) queda mas GRUESO que B0 (indice 6): eso hace de A la rama
    // que continua el tronco y de B la lateral, que es justo lo que se testea.
    for (int32 i = 0; i < Sk.Num(); ++i)
    {
        const float R = FMath::Lerp(10.f, 1.f, (float)i / FMath::Max(1, Sk.Num() - 1));
        Sk.Nodes[i].Radius = R;
        Sk.Nodes[i].PipeRadius = R;
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

    // 4) En una bifurcacion, el hijo MAS GRUESO continua la rama del padre y
    //    solo los mas finos abren rama nueva.
    //
    //    No es un detalle cosmetico: con un eje principal que atraviesa la copa,
    //    el eje bifurca en CADA insercion lateral. Con la regla ingenua ("el
    //    padre bifurco -> los dos hijos abren rama") el propio tronco se
    //    contaria como rama nueva a media altura, su pivote se reiniciaria ahi y
    //    el fuste se balancearia como una ramita colgada del punto equivocado.
    TestTrue(TEXT("el hijo mas grueso continua la rama del tronco"),
        Wind.Nodes[A0].PivotLocalCm.Equals(FVector::ZeroVector, 0.01));
    TestTrue(TEXT("el hijo mas grueso conserva el nivel del tronco"),
        FMath::IsNearlyEqual(Wind.Nodes[A0].BranchLevel01, Wind.Nodes[2].BranchLevel01, 1e-4f));
    TestTrue(TEXT("el hijo mas fino abre rama nueva (nivel mayor)"),
        Wind.Nodes[B0].BranchLevel01 > Wind.Nodes[A0].BranchLevel01);
    TestTrue(TEXT("el pivote de la rama nueva es la horquilla, no su primer nodo"),
        Wind.Nodes[B0].PivotLocalCm.Equals(Sk.Nodes[Fork].Pos, 0.01));
    TestTrue(TEXT("continuacion y rama nueva -> desfases distintos"),
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


// ---------------------------------------------------------------------------
// Troncos organicos y reparto de ramas
// ---------------------------------------------------------------------------

/** Especie de prueba coherente (cumple d_k < D < d_i) y pequena, para que el
    SCA termine rapido dentro de la bateria de tests. */
static USpeciesData* EcoTestSpecies(UObject* Outer)
{
    USpeciesData* Sp = NewObject<USpeciesData>(Outer);
    if (!Sp) { return nullptr; }

    Sp->CrownShape = ECrownShape::Conical;
    Sp->CrownRadiusCm = 250.f;
    Sp->CrownHeightCm = 600.f;
    Sp->TrunkFraction = 0.3f;
    Sp->NumAttractors = 250;

    Sp->StepLengthD = 40.f;
    Sp->InfluenceRadiusDi = 200.f;
    Sp->KillRadiusDk = 30.f;
    Sp->MaxIter = 40;
    Sp->LightEvery = 0;              // sin autopoda: aisla la geometria
    Sp->FineVoxelSizeCm = 35.f;

    Sp->TipRadiusCm = 1.5f;
    Sp->PipeExp = 2.2f;
    Sp->RingSegments = 12;
    return Sp;
}

/**
 * El perfil de tronco convierte el cilindro del pipe model en un fuste.
 *
 * El esqueleto es una CADENA sin bifurcaciones a proposito: es el caso donde el
 * pipe model da r_padre = r_hijo exactamente y de donde salia el cilindro
 * perfecto. Si el perfil no lo arregla aqui, no lo arregla en ningun sitio.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTrunkProfile, "Eco.Arbol.PerfilDeTronco", EcoTestFlags)
bool FEcoTrunkProfile::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->TrunkFlareStrength = 1.f;
    Sp->TrunkFlareHeightCm = 100.f;
    Sp->TrunkTopTaper = 0.7f;
    Sp->TrunkTaperExp = 1.5f;

    FTreeSkeleton Sk;
    Sk.InitRoot(FVector::ZeroVector, FVector::UpVector);
    int32 Prev = 0;
    for (int32 i = 1; i <= 12; ++i)
    {
        Prev = Sk.AddChild(Prev, FVector(0, 0, i * 50.0), FVector::UpVector, BNF_Axis);
    }
    const int32 N = Sk.Num();

    SpaceColonization::ComputeRadii(Sk, *Sp);

    // 1) Punto de partida: el pipe model, en una cadena, es un CILINDRO.
    //    (Es el diagnostico del problema, escrito como test.)
    TestTrue(TEXT("el pipe model da radio constante en una cadena"),
        FMath::IsNearlyEqual(Sk.Nodes[0].PipeRadius, Sk.Nodes[N / 2].PipeRadius, 1e-4f));

    SpaceColonization::ApplyTrunkProfile(Sk, *Sp);

    // 2) El pie es claramente mas ancho que el fuste a media altura.
    TestTrue(TEXT("la base es mas ancha que el fuste"),
        Sk.Nodes[0].Radius > Sk.Nodes[N / 2].Radius * 1.2f);

    // 3) ... y el fuste afila hacia arriba.
    TestTrue(TEXT("el eje afila con la altura"),
        Sk.Nodes[N / 2].Radius > Sk.Nodes[N - 1].Radius);

    // 4) MONOTONIA: ningun nodo mas fino que su hijo. Sin esta pasada el
    //    afilado deja el eje mas fino que el primer nodo de copa y aparece un
    //    estrangulamiento en cono invertido, que es muy visible.
    for (int32 i = 1; i < N; ++i)
    {
        const int32 P = Sk.Nodes[i].Parent;
        TestTrue(TEXT("ningun nodo es mas fino que su hijo"),
            Sk.Nodes[P].Radius >= Sk.Nodes[i].Radius - KINDA_SMALL_NUMBER);
    }

    // 5) El radio ESTRUCTURAL no se toca: es la referencia del viento y llevarle
    //    el ensanche de raiz haria que todo el arbol se balancease de mas.
    TestTrue(TEXT("el perfil no contamina PipeRadius"),
        FMath::IsNearlyEqual(Sk.Nodes[0].PipeRadius, Sk.Nodes[N / 2].PipeRadius, 1e-4f));
    TestTrue(TEXT("el ensanche solo esta en Radius"),
        Sk.Nodes[0].Radius > Sk.Nodes[0].PipeRadius);

    // 6) Con el perfil desactivado, Radius vuelve a ser exactamente el del pipe model.
    Sp->TrunkFlareStrength = 0.f;
    Sp->TrunkTopTaper = 1.f;
    FTreeSkeleton Plain;
    Plain.InitRoot(FVector::ZeroVector, FVector::UpVector);
    Prev = 0;
    for (int32 i = 1; i <= 12; ++i)
    {
        Prev = Plain.AddChild(Prev, FVector(0, 0, i * 50.0), FVector::UpVector, BNF_Axis);
    }
    SpaceColonization::ComputeRadii(Plain, *Sp);
    SpaceColonization::ApplyTrunkProfile(Plain, *Sp);
    TestTrue(TEXT("perfil desactivado -> Radius == PipeRadius"),
        FMath::IsNearlyEqual(Plain.Nodes[0].Radius, Plain.Nodes[0].PipeRadius, 1e-4f));

    return true;
}

/** El angulo de insercion separa la rama lateral de su padre, y ni un grado mas. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoBranchAngle, "Eco.Arbol.AnguloDeInsercion", EcoTestFlags)
bool FEcoBranchAngle::RunTest(const FString&) {
    const FVector Parent = FVector::UpVector;
    const float MinDeg = 45.f;
    const float CosMin = FMath::Cos(FMath::DegreesToRadians(MinDeg));

    // 1) Una direccion casi paralela al padre se abre hasta el minimo exacto.
    {
        const FVector Almost = FVector(0.05f, 0.f, 1.f).GetSafeNormal();
        const FVector Out = SpaceColonization::ApplyBranchAngle(Almost, Parent, MinDeg);
        TestTrue(TEXT("se abre hasta el angulo pedido"),
            FMath::IsNearlyEqual((float)FVector::DotProduct(Out, Parent), CosMin, 1e-3f));
        TestTrue(TEXT("sigue siendo unitaria"), FMath::IsNearlyEqual((float)Out.Size(), 1.f, 1e-3f));

        // El giro es MINIMO: se queda en el plano que formaban padre y direccion.
        const FVector PlaneN = FVector::CrossProduct(Parent, Almost).GetSafeNormal();
        TestTrue(TEXT("el giro se queda en el plano padre-direccion"),
            FMath::Abs((float)FVector::DotProduct(Out, PlaneN)) < 1e-3f);
    }

    // 2) Una direccion que YA se separa lo suficiente no se toca.
    {
        const FVector Wide = FVector(1.f, 0.f, 0.2f).GetSafeNormal();
        const FVector Out = SpaceColonization::ApplyBranchAngle(Wide, Parent, MinDeg);
        TestTrue(TEXT("no toca lo que ya se separaba"), Out.Equals(Wide, 1e-4));
    }

    // 3) Caso degenerado: direccion IDENTICA al padre. No hay plano que
    //    preservar, pero tiene que salir algo unitario y separado, no un NaN.
    {
        const FVector Out = SpaceColonization::ApplyBranchAngle(Parent, Parent, MinDeg);
        TestTrue(TEXT("caso paralelo: unitaria"), FMath::IsNearlyEqual((float)Out.Size(), 1.f, 1e-3f));
        TestTrue(TEXT("caso paralelo: separada"),
            (float)FVector::DotProduct(Out, Parent) <= CosMin + 1e-3f);
    }

    // 4) Angulo 0 = desactivado.
    {
        const FVector Almost = FVector(0.05f, 0.f, 1.f).GetSafeNormal();
        TestTrue(TEXT("0 grados = desactivado"),
            SpaceColonization::ApplyBranchAngle(Almost, Parent, 0.f).Equals(Almost, 1e-4));
    }

    return true;
}

/**
 * Seccion no circular SIN abrir la costura del tubo.
 *
 * Los vertices k = 0 y k = K de cada anillo son el MISMO punto, duplicado solo
 * para cerrar la UV en u = 1. Cualquier deformacion que no sea exactamente
 * periodica en el angulo los separa y abre una raja a lo largo de todo el
 * tronco. Es el fallo mas facil de introducir aqui y el mas dificil de
 * diagnosticar mirando la malla, asi que va como test y no como comentario.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoSectionSeam, "Eco.Arbol.SeccionYCostura", EcoTestFlags)
bool FEcoSectionSeam::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->SectionLobeAmount = 0.15f;
    Sp->SectionLobeCount = 3;
    Sp->BarkReliefAmount = 0.06f;
    Sp->RingSegments = 12;           // >= 8: el mallador no tiene que subirlo

    uint32 Rng = 4242u;
    FTreeSkeleton Sk;
    FTreeLightGridFine Light;
    FAttractorCloud Cloud;
    const FSpaceColonizationConfig Cfg;
    SpaceColonization::GrowTree(*Sp, Rng, FVector::ZeroVector, nullptr, Cfg, Sk, Light, Cloud);

    if (Sk.Num() < 2) { AddError(TEXT("El SCA no produjo esqueleto.")); return false; }

    FTreeMeshData Mesh;
    TreeMeshBuilder::BuildMesh(Sk, *Sp, /*Seed*/ 4242u, Mesh, &Light);

    const FTreeMeshBuffers& W = Mesh.Wood;
    const int32 K = 12;
    const int32 RingVerts = K + 1;
    const int32 N = Sk.Num();

    if (W.Vertices.Num() < N * RingVerts + 1)
    {
        AddError(TEXT("El mallador no produjo el bloque de anillos esperado."));
        return false;
    }

    // 1) COSTURA cerrada, bit a bit. No vale "casi igual": la tolerancia se la
    //    come el desplazamiento del material al aplicar el viento.
    for (int32 i = 0; i < N; ++i)
    {
        const int32 A = i * RingVerts;
        const int32 B = A + K;
        if (!(W.Vertices[A] == W.Vertices[B]))
        {
            AddError(FString::Printf(TEXT("Costura abierta en el anillo %d: %s vs %s"),
                i, *W.Vertices[A].ToString(), *W.Vertices[B].ToString()));
            break;
        }
        if (!W.Normals[A].Equals(W.Normals[B], 1e-4))
        {
            AddError(FString::Printf(TEXT("Normal distinta a los dos lados de la costura en el anillo %d."), i));
            break;
        }
    }

    // 2) La seccion del tronco YA NO es una circunferencia.
    {
        float MinR = TNumericLimits<float>::Max();
        float MaxR = 0.f;
        for (int32 k = 0; k < K; ++k)
        {
            const float R = (float)FVector::Dist(W.Vertices[k], Sk.Nodes[0].Pos);
            MinR = FMath::Min(MinR, R);
            MaxR = FMath::Max(MaxR, R);
        }
        TestTrue(TEXT("la seccion del tronco no es circular"), MaxR > MinR * 1.02f);
    }

    // 3) Las normales dejan de ser radiales puras: si no, el sombreado seguiria
    //    leyendose como un cilindro liso y la deformacion solo estaria en la
    //    silueta.
    {
        bool bAnyNonRadial = false;
        for (int32 k = 0; k < K && !bAnyNonRadial; ++k)
        {
            const FVector Radial = (W.Vertices[k] - Sk.Nodes[0].Pos).GetSafeNormal();
            if (FVector::DotProduct(Radial, W.Normals[k]) < 0.999f)
            {
                bAnyNonRadial = true;
            }
        }
        TestTrue(TEXT("las normales se recalculan sobre la superficie deformada"), bAnyNonRadial);
    }

    // 4) Ningun vertice degenerado (el clamp del radio tiene que sostenerse).
    for (int32 v = 0; v < W.Vertices.Num(); ++v)
    {
        if (W.Vertices[v].ContainsNaN())
        {
            AddError(FString::Printf(TEXT("Vertice %d con NaN."), v));
            break;
        }
    }

    // 5) DETERMINISMO: misma semilla, misma malla exacta.
    {
        uint32 Rng2 = 4242u;
        FTreeSkeleton Sk2; FTreeLightGridFine L2; FAttractorCloud C2;
        SpaceColonization::GrowTree(*Sp, Rng2, FVector::ZeroVector, nullptr, Cfg, Sk2, L2, C2);
        FTreeMeshData Mesh2;
        TreeMeshBuilder::BuildMesh(Sk2, *Sp, 4242u, Mesh2, &L2);

        TestEqual(TEXT("misma semilla -> mismo numero de vertices"),
            Mesh2.Wood.Vertices.Num(), W.Vertices.Num());
        bool bSame = (Mesh2.Wood.Vertices.Num() == W.Vertices.Num());
        for (int32 v = 0; bSame && v < W.Vertices.Num(); ++v)
        {
            bSame = (Mesh2.Wood.Vertices[v] == W.Vertices[v]);
        }
        TestTrue(TEXT("misma semilla -> misma geometria"), bSame);
    }

    return true;
}

/**
 * El eje atraviesa la copa y las ramas se reparten por el fuste.
 *
 * Es el test de la queja original: en copa conica el radio de la envolvente es
 * MAXIMO justo en la base de la copa, que era donde moria el tronco, asi que su
 * punta veia todos los atractores gordos y se los llevaba -> silueta de
 * paraguas. Con el lider recorriendo la copa, las inserciones tienen que
 * repartirse en altura.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLeaderSpread, "Eco.Arbol.EjeYRepartoDeRamas", EcoTestFlags)
bool FEcoLeaderSpread::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->CrownShape = ECrownShape::Conical;
    Sp->LeaderFraction = 1.f;        // conifera excurrente: el eje llega al apice
    Sp->SubCrownFraction = 0.15f;
    Sp->EnvelopeNoise = 0.25f;
    Sp->BranchAngleDeg = 60.f;

    uint32 Rng = 7u;
    FTreeSkeleton Sk;
    FTreeLightGridFine Light;
    FAttractorCloud Cloud;
    const FSpaceColonizationConfig Cfg;
    SpaceColonization::GrowTree(*Sp, Rng, FVector::ZeroVector, nullptr, Cfg, Sk, Light, Cloud);

    const int32 N = Sk.Num();
    if (N < 8) { AddError(TEXT("El SCA no produjo suficiente esqueleto.")); return false; }

    const float CrownH = Sp->CrownHeightCm;
    const float TrunkH = CrownH * Sp->TrunkFraction / (1.f - Sp->TrunkFraction);
    const float CrownBaseZ = TrunkH;
    const float ApexZ = CrownBaseZ + CrownH;

    // 1) El eje llega ARRIBA, no muere en la base de la copa.
    float AxisTopZ = 0.f;
    int32 AxisNodes = 0;
    for (int32 i = 0; i < N; ++i)
    {
        if (Sk.Nodes[i].IsAxis())
        {
            AxisTopZ = FMath::Max(AxisTopZ, (float)Sk.Nodes[i].Pos.Z);
            ++AxisNodes;
        }
    }
    TestTrue(TEXT("el eje atraviesa la copa"), AxisTopZ > CrownBaseZ + CrownH * 0.6f);
    TestTrue(TEXT("el eje tiene varios nodos"), AxisNodes >= 4);

    // 2) Las INSERCIONES de rama (nodos no-eje colgados del eje) se reparten en
    //    altura en vez de amontonarse en la punta del fuste.
    int32 TotalInsertions = 0;
    int32 LowInsertions = 0;              // por debajo de la mitad de la copa
    const float MidZ = CrownBaseZ + CrownH * 0.5f;
    for (int32 i = 1; i < N; ++i)
    {
        const int32 P = Sk.Nodes[i].Parent;
        if (P < 0 || !Sk.Nodes[P].IsAxis() || Sk.Nodes[i].IsAxis()) { continue; }

        ++TotalInsertions;
        if (Sk.Nodes[P].Pos.Z < MidZ) { ++LowInsertions; }
    }

    TestTrue(TEXT("hay ramas laterales colgando del eje"), TotalInsertions >= 4);
    if (TotalInsertions > 0)
    {
        const float LowFrac = (float)LowInsertions / (float)TotalInsertions;
        if (LowFrac < 0.25f)
        {
            AddError(FString::Printf(
                TEXT("Solo el %.0f%% de las inserciones esta en la mitad baja de la copa: las ramas siguen concentradas arriba (silueta de paraguas)."),
                LowFrac * 100.f));
        }
    }

    // 3) La falda de sub-copa siembra atractores POR DEBAJO de la base de copa.
    {
        int32 BelowCrown = 0;
        for (const FAttractor& A : Cloud.Attractors)
        {
            if (A.Pos.Z < CrownBaseZ - 1.f) { ++BelowCrown; }
        }
        TestTrue(TEXT("la falda siembra bajo la base de copa"), BelowCrown > 0);
    }

    // 4) El eje afila con la altura: es el "cuanto mas alto, menos grueso" que
    //    sale del pipe model en cuanto hay ramas laterales repartidas.
    {
        int32 LowAxis = INDEX_NONE, HighAxis = INDEX_NONE;
        for (int32 i = 0; i < N; ++i)
        {
            if (!Sk.Nodes[i].IsAxis()) { continue; }
            if (LowAxis == INDEX_NONE) { LowAxis = i; }
            HighAxis = i;
        }
        if (LowAxis != INDEX_NONE && HighAxis != LowAxis)
        {
            TestTrue(TEXT("el eje es mas fino arriba que abajo"),
                Sk.Nodes[HighAxis].Radius < Sk.Nodes[LowAxis].Radius);
        }
    }

    // 5) La copa no se sale de la envolvente por arriba (el eje no se dispara).
    for (int32 i = 0; i < N; ++i)
    {
        if (Sk.Nodes[i].Pos.Z > ApexZ + CrownH * 0.5f)
        {
            AddError(TEXT("Hay nodos muy por encima del apice: el eje o el SCA se ha disparado."));
            break;
        }
    }

    return true;
}


// ---------------------------------------------------------------------------
// Relieve realista (ruido reparametrizado + erosion)
// ---------------------------------------------------------------------------

/** Parametros compactos para los tests: la MISMA extension de ~1 km del juego
    (misma fisica de pendientes) pero a media resolucion, y erosion abreviada,
    para que la bateria siga siendo rapida. Ojo: encoger el mapa sin encoger
    HeightScaleCm cambiaria la fisica (300 m de desnivel en 256 m de mapa es
    empinado por construccion). */
static FTerrainGenParams EcoTestTerrainParams(uint32 Seed)
{
    FTerrainGenParams P;
    P.Width = 256;
    P.Height = 256;
    P.CellSizeCm = 400.0;
    P.Seed = Seed;
    P.HeightScaleCm = 30000.0;
    P.Hydraulic.Droplets = 8000;
    P.Thermal.Iterations = 8;
    return P;
}

/** Pendiente |dh|/dist maxima y media entre vecinos 4-conexos. */
static void EcoTestSlopeStats(const FField2D& F, float& OutMax, float& OutMean)
{
    OutMax = 0.f;
    double Sum = 0.0;
    int64 Count = 0;
    const float Cell = static_cast<float>(F.CellSize);
    for (int32 y = 0; y < F.Height; ++y)
    {
        for (int32 x = 0; x < F.Width; ++x)
        {
            const float H = F.GetAt(x, y);
            if (x + 1 < F.Width)
            {
                const float S = FMath::Abs(F.GetAt(x + 1, y) - H) / Cell;
                OutMax = FMath::Max(OutMax, S); Sum += S; ++Count;
            }
            if (y + 1 < F.Height)
            {
                const float S = FMath::Abs(F.GetAt(x, y + 1) - H) / Cell;
                OutMax = FMath::Max(OutMax, S); Sum += S; ++Count;
            }
        }
    }
    OutMean = (Count > 0) ? static_cast<float>(Sum / Count) : 0.f;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTerrainDeterminism, "Eco.Relieve.Determinismo", EcoTestFlags)
bool FEcoTerrainDeterminism::RunTest(const FString&)
{
    // Pipeline COMPLETO (ruido + gotas + termica): misma semilla, mismo mapa.
    FHeightField A, B;
    A.Generate(EcoTestTerrainParams(777u));
    B.Generate(EcoTestTerrainParams(777u));
    TestTrue(TEXT("mapa valido"), A.IsValid());
    TestTrue(TEXT("misma semilla -> mismo relieve (bit a bit)"),
        A.Field.Data == B.Field.Data);

    FHeightField C;
    C.Generate(EcoTestTerrainParams(778u));
    TestFalse(TEXT("otra semilla -> otro relieve"), A.Field.Data == C.Field.Data);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTerrainNyquist, "Eco.Relieve.Nyquist", EcoTestFlags)
bool FEcoTerrainNyquist::RunTest(const FString&)
{
    // 700 m de octava base, celda de 2 m (limite 4 m): caben las octavas
    // 700, 350, ..., 5.47 m -> 8 de 12. Con celda de 30 m (limite 60 m)
    // caben 700, 350, 175, 87.5 m -> 4.
    TestEqual(TEXT("celda 2 m -> 8 octavas"),
        EcoNoise::ClampOctavesToNyquist(12, 70000.0, 2.0, 200.0), 8);
    TestEqual(TEXT("celda 30 m -> 4 octavas"),
        EcoNoise::ClampOctavesToNyquist(12, 70000.0, 2.0, 3000.0), 4);
    TestEqual(TEXT("nunca menos de 1"),
        EcoNoise::ClampOctavesToNyquist(12, 100.0, 2.0, 3000.0), 1);
    TestEqual(TEXT("no anade octavas de mas"),
        EcoNoise::ClampOctavesToNyquist(3, 70000.0, 2.0, 200.0), 3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTerrainSlopes, "Eco.Relieve.PendientesRealistas", EcoTestFlags)
bool FEcoTerrainSlopes::RunTest(const FString&)
{
    // Solo el RUIDO (sin erosion): con la reparametrizacion (formas de cientos
    // de metros, recorte de Nyquist) las pendientes ya deben ser de relieve,
    // no de agujas. El generador antiguo daba pendientes medias de ~83 grados;
    // el nuevo, ~17 (medido: media 0.314, max 2.62; umbrales con margen para
    // la diferencia entre semillas y el Perlin de cada plataforma).
    FTerrainGenParams P = EcoTestTerrainParams(12345u);
    P.bErosion = false;
    FHeightField HF;
    HF.Generate(P);

    float MaxSlope, MeanSlope;
    EcoTestSlopeStats(HF.Field, MaxSlope, MeanSlope);
    TestTrue(TEXT("pendiente media < 33 grados"), MeanSlope < 0.65f);
    TestTrue(TEXT("sin paredes verticales (max < 76 grados)"), MaxSlope < 4.2f);

    // Sin aliasing: el salto entre celdas vecinas es una fraccion pequena de
    // la amplitud total (con pinchos por vertice llegaba a ~la amplitud).
    float Mn, Mx;
    FField2D::MinMax(HF.Field.Data, Mn, Mx);
    const float Amplitude = Mx - Mn;
    TestTrue(TEXT("salto maximo entre vecinos < 25% de la amplitud"),
        MaxSlope * static_cast<float>(HF.Field.CellSize) < 0.25f * Amplitude);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTerrainErosion, "Eco.Relieve.ErosionEstable", EcoTestFlags)
bool FEcoTerrainErosion::RunTest(const FString&)
{
    FTerrainGenParams P = EcoTestTerrainParams(4321u);
    P.bErosion = false;
    FHeightField HF;
    HF.Generate(P);

    float MaxBefore, MeanBefore;
    EcoTestSlopeStats(HF.Field, MaxBefore, MeanBefore);
    float MnB, MxB;
    FField2D::MinMax(HF.Field.Data, MnB, MxB);
    const float Amplitude = MxB - MnB;

    // 1) Pipeline completo (hidraulica + termica): acotado y finito. OJO: la
    //    pendiente MEDIA puede subir un poco (la hidraulica talla barrancos:
    //    eso es relieve, no un bug), asi que aqui no se asserta suavizado.
    TerrainErosion::FHydraulicParams Hyd; Hyd.Droplets = 8000;
    TerrainErosion::FThermalParams Th;   Th.Iterations = 8;
    TerrainErosion::HydraulicErode(HF.Field, 99u, Hyd);
    TerrainErosion::ThermalErode(HF.Field, Th);

    float Mn, Mx;
    FField2D::MinMax(HF.Field.Data, Mn, Mx);
    for (const float V : HF.Field.Data)
    {
        if (!FMath::IsFinite(V)) { AddError(TEXT("altura no finita tras la erosion")); return false; }
    }
    TestTrue(TEXT("la erosion no crea material de la nada"), Mx <= MxB + 0.05f * Amplitude);
    TestTrue(TEXT("la erosion no cava bajo el minimo original"), Mn >= MnB - 0.05f * Amplitude);

    // 2) Termica SOLA y agresiva (talud 20, 30 iters): SI debe recortar las
    //    pendientes maximas hacia el talud sin ganar masa (medido: max
    //    2.55 -> 1.57 con estos parametros).
    FTerrainGenParams P2 = EcoTestTerrainParams(4321u);
    P2.bErosion = false;
    FHeightField HT;
    HT.Generate(P2);
    double MassBefore = 0.0;
    for (const float V : HT.Field.Data) { MassBefore += V; }

    TerrainErosion::FThermalParams Th2;
    Th2.Iterations = 30;
    Th2.TalusAngleDeg = 20.f;
    Th2.Strength = 0.8f;
    TerrainErosion::ThermalErode(HT.Field, Th2);

    double MassAfter = 0.0;
    for (const float V : HT.Field.Data) { MassAfter += V; }
    float MaxTh, MeanTh;
    EcoTestSlopeStats(HT.Field, MaxTh, MeanTh);
    TestTrue(TEXT("la termica recorta la pendiente maxima"), MaxTh < MaxBefore * 0.9f);
    TestTrue(TEXT("la termica no empina el terreno en media"), MeanTh <= MeanBefore * 1.01f);
    TestTrue(TEXT("la termica conserva la masa"),
        FMath::Abs(static_cast<float>(MassAfter - MassBefore)) < 1e-4f * static_cast<float>(MassBefore));
    return true;
}
#endif
