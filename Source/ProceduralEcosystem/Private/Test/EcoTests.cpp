/**
 * @file EcoTests.cpp
 * @author Juan Luque Roldán
 * @brief Batería de automatización que fija por escrito las invariantes del simulador:
 *        determinismo, fórmulas puras y propiedades ecológicas.
 *
 * Única unidad de traducción del módulo de pruebas y hoja del grafo de dependencias: consume
 * las cabeceras públicas del proyecto y no exporta nada. Los casos se auto-registran en el
 * Automation Framework bajo la jerarquía `Eco.*` y cubren las tres familias de riesgo que
 * ninguna otra parte vigila: determinismo bit a bit, corrección numérica de las fórmulas puras
 * con sus casos degenerados, y las propiedades ecológicas cualitativas que, de perderse, no
 * rompen la compilación pero vacían el modelo. Como un simulador procedural no tiene oráculo,
 * casi todo se verifica como relación entre ejecuciones -equivalencia entre dos
 * implementaciones, invariancia, monotonía, misma semilla misma salida- con semillas fijas
 * para que un fallo sea siempre reproducible. Ningún test monta un mundo ni un subsistema.
 *
 * @ingroup eco_test
 * @see @ref bib_testingmetamorfico
 */

#include "Misc/AutomationTest.h"
#include "Core/EcoCore.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TreePopulation.h"
#include "Ecology/TickScratch.h"
#include "Ecology/CarbonModel.h"
#include "Terrain/Field2D.h"
#include "Terrain/HeightField.h"        // relieve: ruido reparametrizado + erosión
#include "Terrain/LightFieldCoarse.h"
#include "Render/TreeArchetype.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeWindData.h"
#include "Geometry/SpaceColonization.h"
#include "Geometry/AttractorCloud.h"
#include "Geometry/TreeLightGridFine.h"
#include "Geometry/TreeMeshBuilder.h"
#include "Geometry/TrunkDeformer.h"     // deformación de tronco por árbol
#include "Render/TreeLibrary.h"         // VariantDeformSeed (identidad de curvatura)
#include "Species/SpeciesData.h"

#if WITH_DEV_AUTOMATION_TESTS
/** Flags comunes a los 31 casos: corren en el proceso del editor y aparecen bajo el filtro
    «Engine» del Session Frontend. */
static constexpr EAutomationTestFlags EcoTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

/**
 * Cimiento del determinismo: dos estados xorshift32 con la misma semilla avanzan en
 * lockstep durante 100.000 extracciones, con igualdad EXACTA sobre el float y con el
 * rango semiabierto [0,1) comprobado en cada paso.
 *
 * El caso patológico se prueba aparte: el 0 es punto absorbente del xorshift 13/17/5, así
 * que la guarda que lo sustituye por la razón áurea es lo único que impide que un flujo
 * mal sembrado devuelva ceros para siempre.
 *
 * @see @ref bib_marsaglia2003
 */
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


/**
 * Los dos puntos que caracterizan las fórmulas del vigor: la saturante de Monod vale
 * exactamente 0,5 cuando el recurso iguala la demanda de la especie, y la combinación es
 * literalmente el mínimo de Liebig, no un promedio.
 *
 * @see @ref bib_monod1949
 * @see @ref bib_liebig1840
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoVigor, "Eco.Vigor.Formulas", EcoTestFlags)
bool FEcoVigor::RunTest(const FString&) {
    TestTrue(TEXT("Monod=0.5"), FMath::IsNearlyEqual(EcologyRules::DemandFactor(1.f, 1.f), 0.5f, 1e-4f));
    TestEqual(TEXT("Liebig=min"), EcologyRules::Vigor(0.3f, 0.5f, 0.8f), 0.3f);
    return true;
}


/**
 * Denominadores degenerados: con biomasa máxima 0, el término logístico (1 - B/Bmax) y la
 * alometría de la altura darían 0/0. Solo se exige finitud, porque un único NaN se
 * propagaría por todos los arrays de la población en el tick siguiente.
 *
 * @see @ref bib_verhulst1838
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoNaN, "Eco.Robustez.NoNaN", EcoTestFlags)
bool FEcoNaN::RunTest(const FString&) {
    const float B = EcologyRules::GrowBiomassLogistic(0.f, 0.5f, 0.25f, 0.f, 1.f);
    TestTrue(TEXT("GrowBiomass finito"), FMath::IsFinite(B));
    TestTrue(TEXT("Height finito"), FMath::IsFinite(EcologyRules::HeightFromBiomass(1.f, 0.f, 2000.f)));
    return true;
}

/**
 * Contrato de la compactación de muertos: los once arrays paralelos siguen alineados y, sobre
 * todo, el orden relativo de los vivos SE PRESERVA. La aserción sobre `Position[1].X` es la
 * que prohíbe el intercambio con el último elemento, más rápido pero que reordenaría la
 * población y rompería la correspondencia estable entre índice y orden de nacimiento.
 *
 * @see @ref bib_acton2014
 */
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

/**
 * Barrido de dominio sobre la probabilidad de muerte: edades de 0 a 400 años -el doble de la
 * longevidad nominal- por tres pesos de estrés. El canal de edad crece como la cuarta potencia
 * de la edad relativa y a 400 años vale 16 antes del recorte, así que lo que se comprueba es
 * que los dos recortes y la composición como riesgos independientes dejan el resultado en [0,1].
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoMort, "Eco.Mortalidad.Rango", EcoTestFlags)
bool FEcoMort::RunTest(const FString&) {
    for (float age = 0.f; age <= 400.f; age += 25.f)
        for (float w : {0.2f, 2.f, 10.f}) {
            float p = EcologyRules::MortalityProbability(age, 200.f, 1.f, w, 1.f);
            TestTrue(TEXT("pDeath en [0,1]"), p >= 0.f && p <= 1.f);
        }
    return true;
}
/**
 * Continuidad de escala del bucket de LOD. La malla de un arquetipo se hornea al BORDE
 * SUPERIOR de su bucket, así que la altura en pantalla solo es continua al cambiar de bucket
 * si @f$Escala(r,b) \cdot BordeSuperior(b) = r@f$ para todo punto interior. Se comprueba esa
 * igualdad y la coherencia del bucket elegido en cada uno de los cinco intervalos.
 *
 * @see @ref bib_clarkjh1976
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLodBucket, "Eco.LOD.BucketEscala", EcoTestFlags)
bool FEcoLodBucket::RunTest(const FString&) {
    const int32 N = 5;
    // Invariante: escala_en_bucket * borde_superior == fracción de altura adulta.
    for (int32 b = 0; b < N; ++b) {
        const float upper = TreeArchetype::BucketUpperRatio(b, N);
        const float r = upper - 0.01f; // dentro del bucket b, sin tocar el clamp inferior
        TestEqual(TEXT("BucketOf coherente"), TreeArchetype::BucketOf(r, N), b);
        const float scale = TreeArchetype::ScaleWithinBucket(r, b, N);
        TestTrue(TEXT("escala continua al cruzar bucket"), FMath::IsNearlyEqual(scale * upper, r, 1e-3f));
    }
    return true;
}

/**
 * Banda de histéresis del cambio de bucket. Un árbol parado justo en una frontera oscilaría
 * de bucket tick a tick, y cada oscilación es una baja y un alta de instancia entre dos
 * componentes HISM. La banda efectiva es @f$H/N@f$ sobre el borde, y se comprueba a los dos
 * lados. De paso se barre la variante morfológica, que es un módulo de hash estable y no
 * puede salirse del número de variantes disponibles.
 *
 * @see @ref bib_schmitt1938
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLodHysteresis, "Eco.LOD.Histeresis", EcoTestFlags)
bool FEcoLodHysteresis::RunTest(const FString&) {
    const int32 N = 5; const float H = 0.15f;
    // Borde 3/5 = 0.6 y banda H/N = 0.03: en 0.61 no cruza; en 0.64 (> 0.63) sí.
    TestEqual(TEXT("no cruza dentro de la banda"), TreeArchetype::BucketWithHysteresis(0.61f, 2, N, H), 2);
    TestEqual(TEXT("cruza superada la banda"), TreeArchetype::BucketWithHysteresis(0.64f, 2, N, H), 3);
    for (uint32 id = 1; id < 50; ++id)
        TestTrue(TEXT("variante estable en rango"), TreeArchetype::VariantOf(id, 4) < 4);
    return true;
}

/**
 * Re-mapeo instancia -> árbol tras un borrado por lotes. `RemoveInstances` desplaza hacia
 * abajo los índices superiores, de modo que sin este bookkeeping, a partir de la primera
 * muerte cada árbol movería la instancia de otro. El test reproduce esa semántica con un
 * oráculo escrito a mano y verifica tanto el mapeo resultante como las notificaciones de
 * reubicación que recibe el llamador.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLodRemap, "Eco.LOD.RemapInstancias", EcoTestFlags)
bool FEcoLodRemap::RunTest(const FString&) {
    // Mapping instancia->id = [10,11,12,13,14]; se borran las instancias 1 y 3.
    // El desplazamiento hacia abajo deja [10,12,14], con 12 en 1 y 14 en 2.
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
//  Declive del individuo, eventos de muerte y depósito en los campos de recursos
// =============================================================================

/**
 * Los dos declives del árbol son mecanismos distintos y el test los separa.
 *
 * La senescencia por edad es IRREVERSIBLE y depende solo de la fracción de longevidad
 * alcanzada; la supresión por estrés es REVERSIBLE y tiene histéresis de dos umbrales, de
 * modo que se entra al superar el umbral de la especie y no se sale hasta bajar de una
 * fracción de él. Mezclarlas dejaría marcada de por vida a una plántula suprimida unos años
 * y haría imposible el banco de plántulas. Se cierran también los bordes de los dos
 * multiplicadores: el crecimiento escalado, la mortalidad saturada en 1 y el recorte que
 * impide que un multiplicador menor que 1 REDUZCA el riesgo.
 *
 * @see @ref bib_dinamicadeclaros
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoSenescence, "Eco.Mortalidad.Senescencia", EcoTestFlags)
bool FEcoSenescence::RunTest(const FString&) {
    const float Longevity = 200.f, AgeFrac = 0.75f, StressThr = 0.85f, ExitFrac = 0.4f;

    // --- Senescencia por EDAD: irreversible, y SOLO por edad ---------------
    TestFalse(TEXT("joven: no senescente"),
        EcologyRules::IsSenescentByAge(100.f, Longevity, AgeFrac));
    TestTrue(TEXT("pasada la fraccion de longevidad: senescente"),
        EcologyRules::IsSenescentByAge(150.f, Longevity, AgeFrac));

    // El estrés no entra en el criterio de edad: tiene su propio estado, reversible.
    TestFalse(TEXT("estres alto NO produce senescencia por edad"),
        EcologyRules::IsSenescentByAge(10.f, Longevity, AgeFrac));

    // --- Supresión por ESTRÉS: reversible, con histéresis ------------------
    TestTrue(TEXT("sano que cruza el umbral: entra en supresion"),
        EcologyRules::UpdateSuppression(false, 0.9f, StressThr, ExitFrac));
    TestFalse(TEXT("sano por debajo del umbral: no entra"),
        EcologyRules::UpdateSuppression(false, 0.5f, StressThr, ExitFrac));

    // Histéresis: con un solo umbral el estado parpadearía tick a tick. Un suprimido a
    // 0.5 sigue suprimido (0.5 > 0.85*0.4 = 0.34) aunque a 0.5 no habría entrado.
    TestTrue(TEXT("suprimido a estres medio: NO sale todavia"),
        EcologyRules::UpdateSuppression(true, 0.5f, StressThr, ExitFrac));
    TestFalse(TEXT("suprimido que se recupera del todo: sale"),
        EcologyRules::UpdateSuppression(true, 0.2f, StressThr, ExitFrac));

    // En declive el crecimiento se frena y la mortalidad se multiplica, ambos acotados.
    TestEqual(TEXT("sano: crecimiento x1"), EcologyRules::DeclineGrowthFactor(false, 0.1f), 1.f);
    TestEqual(TEXT("en declive: crecimiento x escala"), EcologyRules::DeclineGrowthFactor(true, 0.1f), 0.1f);
    TestEqual(TEXT("sano: pDeath intacta"), EcologyRules::ApplySenescentMortality(0.2f, false, 3.f), 0.2f);
    TestTrue(TEXT("senescente: pDeath x3"),
        FMath::IsNearlyEqual(EcologyRules::ApplySenescentMortality(0.2f, true, 3.f), 0.6f, 1e-5f));
    TestEqual(TEXT("pDeath nunca pasa de 1"), EcologyRules::ApplySenescentMortality(0.5f, true, 10.f), 1.f);
    TestEqual(TEXT("multiplicador < 1 no reduce la mortalidad"),
        EcologyRules::ApplySenescentMortality(0.2f, true, 0.5f), 0.2f);
    return true;
}

/**
 * Aritmética del anillo de eventos de muerte: buffer circular CON PÉRDIDA, de capacidad fija,
 * con contador global monótono en el escritor y cursor propio en el lector.
 *
 * Lo que se verifica es el wrap-around, que es donde escritor y lector pueden
 * desincronizarse: el rango legible arranca en @f$\max(Cursor,\,Contador-Capacidad)@f$, de
 * modo que un consumidor retrasado pierde los eventos antiguos por diseño pero NUNCA lee una
 * ranura ya pisada. Se cubren llenado parcial, consumo en vacío, sobreescritura y cursor al día.
 *
 * @note El test reimplementa el escritor y el lector con dos lambdas locales en vez de llamar
 *       al anillo de la simulación: valida el razonamiento modular, no la implementación.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeathRing, "Eco.Muertes.AnilloDeEventos", EcoTestFlags)
bool FEcoDeathRing::RunTest(const FString&) {
    const int32 Cap = 8;
    TArray<int64> Ring; Ring.SetNumZeroed(Cap); // ranura -> id global del evento
    int64 Counter = 0;

    auto Record = [&](int64 Id) { Ring[static_cast<int32>(Counter % Cap)] = Id; ++Counter; };
    auto Collect = [&](int64& Cursor, TArray<int64>& Out)
        {
            const int64 From = FMath::Max<int64>(Cursor, Counter - Cap);
            for (int64 g = From; g < Counter; ++g) { Out.Add(Ring[static_cast<int32>(g % Cap)]); }
            Cursor = Counter;
        };

    // 1) Llenado parcial: el consumidor ve exactamente lo que se escribió.
    int64 Cursor = 0;
    for (int64 i = 0; i < 5; ++i) { Record(i); }
    TArray<int64> Out;
    Collect(Cursor, Out);
    TestEqual(TEXT("llenado: 5 eventos"), Out.Num(), 5);
    for (int32 k = 0; k < Out.Num(); ++k) { TestEqual(TEXT("llenado: en orden"), Out[k], (int64)k); }

    // 2) Nada nuevo: nada que entregar, el cursor ya está al día.
    Out.Reset(); Collect(Cursor, Out);
    TestEqual(TEXT("sin muertes nuevas: 0 eventos"), Out.Num(), 0);

    // 3) Wrap-around: se escriben más de Cap eventos sin consumir. Solo deben
    //    entregarse los últimos Cap, y en orden.
    for (int64 i = 5; i < 30; ++i) { Record(i); }
    Out.Reset(); Collect(Cursor, Out);
    TestEqual(TEXT("wrap: solo caben Cap eventos"), Out.Num(), Cap);
    TestEqual(TEXT("wrap: el mas antiguo disponible es Counter-Cap"), Out[0], (int64)(30 - Cap));
    TestEqual(TEXT("wrap: el ultimo es el mas reciente"), Out.Last(), (int64)29);
    for (int32 k = 1; k < Out.Num(); ++k)
    {
        TestEqual(TEXT("wrap: consecutivos"), Out[k], Out[k - 1] + 1);
    }

    // 4) El cursor queda al día tras consumir.
    TestEqual(TEXT("cursor al dia"), Cursor, Counter);
    return true;
}

/**
 * Equivalencia entre los dos caminos del kernel de depósito: el denso, que escribe un campo
 * completo desde código serial, y el disperso, que emite pares (celda, cantidad) apendables
 * desde dentro de un bucle paralelo. Se comparan celda a celda tras re-aplicar los deltas.
 *
 * La segunda mitad verifica la conservación de masa: el peso radial se normaliza en dos
 * pasadas, así que la suma de lo depositado es exactamente la cantidad pedida pese al redondeo
 * a celdas. Radio o cantidad nulos no emiten ningún delta.
 *
 * @note La igualdad se comprueba con tolerancia, no bit a bit: los dos caminos suman en orden
 *       distinto y la suma en coma flotante no es asociativa.
 * @see @ref bib_goldberg1991
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoSparseKernel, "Eco.Recursos.KernelDisperso", EcoTestFlags)
bool FEcoSparseKernel::RunTest(const FString&) {
    FField2D Geometry;
    Geometry.Init(16, 16, /*CellSize*/ 100.0, FVector2D::ZeroVector, 0.f);

    const FVector Pos(750.0, 820.0, 0.0);
    const float Radius = 350.f;
    const float Amount = -12.5f;

    // Camino denso: escribe el campo entero.
    TArray<float> Dense; Dense.SetNumZeroed(Geometry.Num());
    EcologyRules::DepositKernel(Geometry, Dense, Pos, Radius, Amount);

    // Camino disperso: deltas re-aplicados sobre un campo equivalente, que es lo que
    // hace la reducción serial del tick.
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

    // Conservación de masa: el kernel está normalizado por la suma de pesos, así que lo
    // repartido suma exactamente la cantidad pedida.
    float Total = 0.f;
    for (const FCellDelta& D : Sparse) { Total += D.Amount; }
    TestTrue(TEXT("se deposita exactamente TotalAmount"), FMath::IsNearlyEqual(Total, Amount, 1e-3f));

    // Casos degenerados: radio o cantidad nulos no emiten ningún delta.
    Sparse.Reset();
    EcologyRules::DepositKernelSparse(Geometry, Sparse, Pos, 0.f, Amount);
    TestEqual(TEXT("radio 0 -> sin deltas"), Sparse.Num(), 0);
    EcologyRules::DepositKernelSparse(Geometry, Sparse, Pos, Radius, 0.f);
    TestEqual(TEXT("cantidad 0 -> sin deltas"), Sparse.Num(), 0);
    return true;
}

/**
 * Invariancia por traslación vertical de la rejilla de luz relativa al terreno: dos copas
 * idénticas plantadas a la misma altura SOBRE EL SUELO, una en un valle y otra en una cresta
 * 100 m más alta, tienen que dar exactamente la misma luz bajo ellas.
 *
 * Se comprueba además que la conmutación absoluta -> relativa depende de que se hayan
 * entregado las cotas de suelo, y cuatro guardas ecológicas sobre el perfil resultante: el
 * suelo bajo una copa adulta queda claramente sombreado, la luz crece hacia la copa, el piso
 * difuso impide el cero absoluto y lejos de toda copa se recupera la luz plena. La última
 * aserción acota el número de capas, que es lo que hace barata esta rejilla.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLightTerrainRelative, "Eco.Luz.RelativaAlTerreno", EcoTestFlags)
bool FEcoLightTerrainRelative::RunTest(const FString&) {
    FLightFieldCoarse Light;
    const int32 W = 8, H = 8, L = 10;
    const double Cell = 400.0;
    Light.Init(W, H, L, Cell, Cell, FVector2D::ZeroVector, /*BaseZ*/ -400.0);
    TestFalse(TEXT("sin GroundZ es absoluta"), Light.IsTerrainRelative());

    // Mitad izquierda en un valle (z = 0), mitad derecha en una cresta (z = 10.000 cm).
    TArray<float> Ground; Ground.SetNumZeroed(W * H);
    for (int32 y = 0; y < H; ++y)
        for (int32 x = 0; x < W; ++x)
            Ground[y * W + x] = (x >= 4) ? 10000.f : 0.f;
    Light.SetGroundHeights(MoveTemp(Ground));
    TestTrue(TEXT("con GroundZ es relativa"), Light.IsTerrainRelative());

    // Sin sombra: luz plena en cualquier sitio.
    TestEqual(TEXT("sin sombra = luz plena"),
        Light.SampleLight(FVector(600.0, 600.0, 300.0)), FLightFieldCoarse::FullSunlight);

    // Dos copas idénticas, una en cada altiplano, a la MISMA altura sobre el suelo. La copa
    // es ancha a propósito: con un radio menor que el vóxel, el área foliar se reparte sobre
    // una huella mucho mayor que la copa y la sombra sale débil, que es lo correcto.
    const float CanopyR = 1200.f, CanopyDepth = 1200.f, CanopyLai = 4.f;
    Light.DepositCanopyLeafArea(FVector(600.0, 600.0, 0.0 + 2000.0), CanopyR, CanopyDepth, CanopyLai);
    Light.DepositCanopyLeafArea(FVector(2200.0, 600.0, 10000.0 + 2000.0), CanopyR, CanopyDepth, CanopyLai);
    Light.AccumulateExtinction();

    // A ras de suelo, bajo cada copa, la luz es la MISMA pese a los 100 m de diferencia de cota.
    const float ValleyQ = Light.SampleLight(FVector(600.0, 600.0, 0.0 + 50.0));
    const float RidgeQ = Light.SampleLight(FVector(2200.0, 600.0, 10000.0 + 50.0));
    TestTrue(TEXT("misma altura sobre el suelo -> misma luz"),
        FMath::IsNearlyEqual(ValleyQ, RidgeQ, 1e-4f));

    // Umbral ecológico, no un simple «< luz plena»: una sombra de tres centésimas dejaría el
    // sotobosque a plena luz y aun así pasaría la comparación laxa.
    TestTrue(TEXT("bajo una copa adulta el suelo esta claramente sombreado"), ValleyQ < 0.6f);

    // La sombra crece hacia abajo, que es la firma de un dosel real.
    const float CanopyQ = Light.SampleLight(FVector(600.0, 600.0, 0.0 + 1800.0));
    TestTrue(TEXT("perfil correcto: mas oscuro abajo que en la copa"), ValleyQ < CanopyQ);

    // El piso difuso impide el cero absoluto, que igualaría a todas las especies justo donde
    // la tolerancia a la sombra debe decidir.
    TestTrue(TEXT("nunca se llega a oscuridad total"), ValleyQ >= Light.DiffuseFloor);

    // Una copa no sombrea columnas alejadas en XY.
    const float FarQ = Light.SampleLight(FVector(3400.0, 2600.0, 10000.0 + 50.0));
    TestEqual(TEXT("lejos de toda copa = luz plena"), FarQ, FLightFieldCoarse::FullSunlight);

    // Medir las capas la reduce a métrica: es lo que abarata la rejilla relativa al terreno.
    TestTrue(TEXT("la rejilla cabe en pocas capas"), Light.Layers <= 16);
    return true;
}

/**
 * Estratificación vertical: bajo un único dominante, la luz decrece monótonamente hacia el
 * suelo. Es la propiedad que hace ASIMÉTRICA la competencia por luz -el que llega arriba
 * intercepta y el de abajo paga- y sin ella no hay dosel, ni banco de plántulas, ni sucesión.
 *
 * Las dos cotas extremas fijan los dos extremos de la extinción acumulada: el techo de la copa
 * queda casi a pleno sol porque solo se come media capa propia (el muestreo es en el centro
 * del vóxel, y de ahí la autoexclusión), y el suelo queda en penumbra.
 *
 * @see @ref bib_monsisaeki1953
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLightStratification, "Eco.Luz.EstratificacionVertical", EcoTestFlags)
bool FEcoLightStratification::RunTest(const FString&) {
    FLightFieldCoarse Light;
    const double Cell = 400.0;
    Light.Init(8, 8, 12, Cell, Cell, FVector2D::ZeroVector, /*BaseZ*/ -400.0);
    Light.SetExtinctionParams(0.5f, 0.04f);

    // Un dominante de 20 m con la copa en su tercio superior.
    const double ApexZ = 2000.0;
    Light.DepositCanopyLeafArea(FVector(600.0, 600.0, ApexZ), 1200.f, 600.f, 4.f);
    Light.AccumulateExtinction();

    const float QCanopy = Light.SampleLightSmooth(FVector(600.0, 600.0, ApexZ));
    const float QMid = Light.SampleLightSmooth(FVector(600.0, 600.0, 1000.0));
    const float QGround = Light.SampleLightSmooth(FVector(600.0, 600.0, 0.0));

    TestTrue(TEXT("la luz decrece de forma monotona hacia el suelo"),
        QCanopy > QMid && QMid > QGround);
    TestTrue(TEXT("el techo de la copa esta casi a pleno sol (autoexclusion)"), QCanopy > 0.8f);
    TestTrue(TEXT("el suelo esta en penumbra"), QGround < 0.5f);
    return true;
}

/**
 * Coste de la tolerancia a la sombra: las curvas de respuesta a la luz de la pionera y de la
 * climácica tienen que CRUZARSE. La pionera rinde más a pleno sol y la tolerante rinde más
 * bajo el dosel, de modo que ninguna gana en todo el gradiente.
 *
 * El compromiso se impone bajando el techo de asimilación con la tolerancia,
 * @f$A_{max}(s) = 1 - c\,s@f$, mientras la tolerancia baja la semisaturación. El control
 * negativo -una climácica con el techo intacto- demuestra por qué el término de coste no es
 * opcional: sin él la tolerancia sería un eje monótono gratuito y habría exclusión competitiva
 * por construcción, no por ecología.
 *
 * @see @ref bib_toleranciasombra
 * @see @ref bib_exclusioncompetitiva
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoShadeToleranceTradeoff, "Eco.Luz.CosteDeLaTolerancia", EcoTestFlags)
bool FEcoShadeToleranceTradeoff::RunTest(const FString&) {
    const float KlMax = 0.12f, Cost = 0.40f;

    auto MakeResp = [KlMax, Cost](float ShadeTol)
        {
            EcoVigor::FLightResponse R;
            R.KlMax = KlMax;
            R.ShadeTolerance = ShadeTol;
            R.MaxAssimilation = 1.f - Cost * ShadeTol;
            return R;
        };

    const EcoVigor::FLightResponse Pioneer = MakeResp(0.05f);
    const EcoVigor::FLightResponse Climax = MakeResp(0.65f);

    const float SunPioneer = EcoVigor::LightFactor(1.f, Pioneer);
    const float SunClimax = EcoVigor::LightFactor(1.f, Climax);
    const float ShadePioneer = EcoVigor::LightFactor(0.10f, Pioneer);
    const float ShadeClimax = EcoVigor::LightFactor(0.10f, Climax);

    TestTrue(TEXT("a pleno sol gana la pionera"), SunPioneer > SunClimax);
    TestTrue(TEXT("bajo el dosel gana la tolerante"), ShadeClimax > ShadePioneer);

    // Control negativo: sin coste, la tolerante gana también a pleno sol y el cruce desaparece.
    const EcoVigor::FLightResponse FreeClimax = [KlMax]()
        {
            EcoVigor::FLightResponse R; R.KlMax = KlMax; R.ShadeTolerance = 0.65f; R.MaxAssimilation = 1.f; return R;
        }();
    TestTrue(TEXT("sin coste, la tolerante domina tambien a pleno sol"),
        EcoVigor::LightFactor(1.f, FreeClimax) > SunPioneer);
    return true;
}

/**
 * El acumulador de estrés tiene que ser una rampa continua, no un escalón. Es el término de
 * decaimiento el que le da un punto fijo interior,
 * @f$S^{*} = (Umbral - Vigor)\cdot Acumulacion / Decaimiento@f$, y el test lo alcanza por
 * iteración para tres vigores: dos por debajo del umbral, que deben dar valores distintos y
 * ninguno saturado, y uno por encima, que debe llevar el estrés a cero.
 *
 * Sin decaimiento el punto fijo sería binario y dos sitios de calidad muy distinta darían la
 * misma demografía. La segunda mitad comprueba el acoplamiento longevidad-estrés,
 * @f$w = w_{0}(L_{ref}/L)^{e}@f$: la especie longeva paga el mismo impuesto anual durante
 * muchos más años, así que la longevidad tiene que comprar resistencia al estrés crónico para
 * que la estrategia lenta sea viable. Con exponente 0 el acoplamiento se desactiva exactamente.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoStressRamp, "Eco.Estres.RampaContinua", EcoTestFlags)
bool FEcoStressRamp::RunTest(const FString&) {
    const float Thr = 0.45f, Acc = 1.f, Rec = 0.5f, Decay = 0.2f, Dt = 1.f;

    // Equilibrio alcanzado por iteración, con dt constante.
    auto FixedPoint = [&](float Vigor)
        {
            float S = 0.f;
            for (int32 i = 0; i < 500; ++i) { S = EcologyRules::UpdateStress(S, Vigor, Thr, Acc, Rec, Decay, Dt); }
            return S;
        };

    const float SBad = FixedPoint(0.05f);
    const float SMid = FixedPoint(0.35f);

    TestTrue(TEXT("peor sitio -> mas estres"), SBad > SMid);
    TestTrue(TEXT("el sitio intermedio NO satura en 1"), SMid < 0.95f);
    TestTrue(TEXT("el sitio intermedio tampoco queda en 0"), SMid > 0.01f);
    TestTrue(TEXT("por encima del umbral el estres se va a cero"),
        FixedPoint(0.9f) < KINDA_SMALL_NUMBER);

    // Acoplamiento longevidad-estrés: la especie longeva resiste mejor el estrés crónico.
    const float WLong = EcologyRules::EffectiveStressMortalityWeight(0.2f, 600.f, 300.f, 0.5f);
    const float WShort = EcologyRules::EffectiveStressMortalityWeight(0.2f, 150.f, 300.f, 0.5f);
    TestTrue(TEXT("la especie longeva resiste mejor el estres cronico"), WLong < WShort);
    TestEqual(TEXT("exponente 0 desactiva el acoplamiento"),
        EcologyRules::EffectiveStressMortalityWeight(0.2f, 600.f, 300.f, 0.f), 0.2f);
    return true;
}

// =============================================================================
//  Multiplicador de CO2 y datos de viento del árbol
// =============================================================================

/**
 * El multiplicador de CO2 tiene que ser una capa INOFENSIVA: acotada en
 * @f$[1-MaxReduction,\,1]@f$, monótona no decreciente en la luz, continua entre sus dos
 * extremos y finita ante entradas absurdas. Se verifican los cuatro puntos de esquina de
 * @f$f = 1 - MaxReduction \cdot Sombra \cdot (1 - Mezcla)@f$ y después un barrido completo en
 * luz.
 *
 * La parte crítica es la ablación: apagar la capa, o poner su reducción máxima a cero, devuelve
 * 1.0f EXACTO -comparado con igualdad, no con tolerancia-, que es lo que permite comparar bit a
 * bit una corrida con la capa y otra sin ella.
 *
 * @see @ref bib_co2dosel
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoCO2, "Eco.Vigor.CO2", EcoTestFlags)
bool FEcoCO2::RunTest(const FString&) {
    EcoCarbon::FCO2Params P;
    P.bEnabled = true;
    P.MaxReduction = 0.15f;
    P.FullMixingHeightCm = 2500.f;
    P.FullSunlight = 1.f;

    // A pleno sol no hay penalización, esté el árbol a la altura que esté.
    TestTrue(TEXT("pleno sol -> factor 1"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(1.f, 0.f, P), 1.f, 1e-5f));
    TestTrue(TEXT("pleno sol, arbol alto -> factor 1"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(1.f, 3000.f, P), 1.f, 1e-5f));

    // Peor caso: oscuridad total a ras de suelo -> exactamente 1 - MaxReduction.
    // Es el tope del efecto, y por diseño es leve.
    TestTrue(TEXT("dosel cerrado a ras de suelo -> 1 - MaxReduction"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(0.f, 0.f, P), 1.f - P.MaxReduction, 1e-5f));

    // La altura recupera el factor: por encima del dosel el aire está bien mezclado.
    TestTrue(TEXT("por encima del dosel no hay penalizacion"),
        FMath::IsNearlyEqual(EcoCarbon::CO2Factor(0.f, P.FullMixingHeightCm, P), 1.f, 1e-5f));
    TestTrue(TEXT("a media altura la penalizacion es intermedia"),
        EcoCarbon::CO2Factor(0.f, P.FullMixingHeightCm * 0.5f, P) > EcoCarbon::CO2Factor(0.f, 0.f, P));

    // Monótono en la luz y acotado en todo el dominio, incluidas entradas fuera de rango.
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

    // Ablación: apagada, la capa devuelve su elemento neutro EXACTO, no «casi 1».
    P.bEnabled = false;
    TestEqual(TEXT("desactivado -> 1.0 exacto"), EcoCarbon::CO2Factor(0.f, 0.f, P), 1.f);
    P.bEnabled = true; P.MaxReduction = 0.f;
    TestEqual(TEXT("MaxReduction 0 -> 1.0 exacto"), EcoCarbon::CO2Factor(0.f, 0.f, P), 1.f);
    return true;
}

/**
 * Esqueleto sintético con una sola bifurcación para validar los datos de viento por nodo.
 *
 * Los radios se rellenan a mano de modo que en la horquilla un hijo salga más grueso que el
 * otro, que es lo que decide cuál continúa la rama del padre. Lo comprobado:
 *
 * @li la base empotrada no se balancea, el balanceo crece hacia las puntas y tanto él como el
 *     desfase quedan dentro de rango;
 * @li los nodos de una misma rama comparten pivote y desfase, porque un desfase por nodo
 *     retorcería el tubo en lugar de balancearlo;
 * @li en una bifurcación el hijo más grueso hereda pivote y nivel del padre y solo el más fino
 *     abre rama nueva, cuyo pivote es la HORQUILLA y no su primer nodo;
 * @li misma semilla, mismos datos; y más rigidez de especie, menos balanceo.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoWindData, "Eco.Viento.Datos", EcoTestFlags)
bool FEcoWindData::RunTest(const FString&) {
    USpeciesData* Sp = NewObject<USpeciesData>(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->WindStiffness = 0.f;      // máxima flexibilidad: aísla la forma de la curva
    Sp->LeafFlutterScale = 1.f;

    // Tronco de cuatro nodos y bifurcación en el último:
    //
    //          5          <- A1
    //         /
    //        4   6        <- A0 (más grueso: continúa el tronco) y B0 (abre rama)
    //         \ /
    //  0-1-2-3            <- tronco; el nodo 3 es la horquilla
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

    // Radios como los dejaría el pipe model: gruesos abajo, finos arriba. A0 queda más
    // GRUESO que B0, así que A continúa el tronco y B es la lateral.
    for (int32 i = 0; i < Sk.Num(); ++i)
    {
        const float R = FMath::Lerp(10.f, 1.f, (float)i / FMath::Max(1, Sk.Num() - 1));
        Sk.Nodes[i].Radius = R;
        Sk.Nodes[i].PipeRadius = R;
    }

    FTreeWindData Wind;
    Wind.Build(Sk, *Sp, /*FineLight*/ nullptr, /*Seed*/ 12345u);

    TestTrue(TEXT("un dato por nodo"), Wind.IsValidFor(Sk));

    // 1) La base está empotrada en el suelo.
    TestTrue(TEXT("la base del tronco no se balancea"), Wind.Nodes[0].SwayWeight <= KINDA_SMALL_NUMBER);

    // 2) El balanceo crece hacia la punta y queda acotado.
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

    // 3) Nodos de la MISMA rama: mismo pivote y mismo desfase, o el tubo se retuerce.
    TestTrue(TEXT("misma rama -> mismo pivote"),
        Wind.Nodes[A0].PivotLocalCm.Equals(Wind.Nodes[A1].PivotLocalCm, 0.01));
    TestEqual(TEXT("misma rama -> mismo desfase"), Wind.Nodes[A0].Phase01, Wind.Nodes[A1].Phase01);

    // El tronco es la rama de nivel 0 y su pivote es la base del árbol.
    TestTrue(TEXT("el pivote del tronco es su base"),
        Wind.Nodes[2].PivotLocalCm.Equals(FVector::ZeroVector, 0.01));
    TestTrue(TEXT("el tronco es el nivel 0"), FMath::IsNearlyEqual(Wind.Nodes[2].BranchLevel01, 0.f, 1e-4f));

    // 4) En una bifurcación, el hijo MÁS GRUESO continúa la rama del padre y solo los más
    //    finos abren rama nueva. Con un eje que atraviesa la copa, el eje bifurca en CADA
    //    inserción lateral: con la regla ingenua -«el padre bifurcó, los dos hijos abren
    //    rama»- el propio tronco contaría como rama nueva a media altura, su pivote se
    //    reiniciaría ahí y el fuste se balancearía como una ramita colgada del punto
    //    equivocado.
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

    // 5) Determinismo: misma semilla, mismos datos; semilla distinta, desfases distintos.
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
// Geometría del árbol: perfil de tronco, inserción de ramas y mallado
// ---------------------------------------------------------------------------

/**
 * Fixture de especie para los tests de geometría: copa cónica pequeña con los tres parámetros
 * de la colonización del espacio en la relación que el algoritmo exige, @f$d_k < D < d_i@f$.
 *
 * Es deliberadamente modesta -250 atractores y 40 iteraciones- para que un árbol completo quepa
 * dentro del presupuesto de la batería, y crece con la autopoda por luz desactivada, de modo
 * que los tests midan geometría y no el efecto de la sombra.
 *
 * @param Outer Propietario del asset transitorio.
 * @return Especie recién creada, o `nullptr` si el motor no pudo construirla.
 * @see @ref bib_runions2007
 */
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
    Sp->LightEvery = 0;              // sin autopoda: aísla la geometría
    Sp->FineVoxelSizeCm = 35.f;

    Sp->TipRadiusCm = 1.5f;
    Sp->PipeExp = 2.2f;
    Sp->RingSegments = 12;
    return Sp;
}

/**
 * El perfil de tronco convierte el cilindro del pipe model en un fuste.
 *
 * El esqueleto es una CADENA sin bifurcaciones a propósito: es el caso donde
 * @f$r_{padre}^{n} = \sum r_{hijo}^{n}@f$ degenera en @f$r_{padre} = r_{hijo}@f$ y sale un
 * cilindro perfecto. La primera aserción documenta ese cilindro como diagnóstico y el resto
 * verifica lo que el perfil añade encima: ensanche de pie, afilado con la altura y una pasada
 * de monotonía que evita el estrangulamiento en cono invertido bajo la copa.
 *
 * Las dos últimas comprobaciones protegen la separación entre los dos radios: el perfil vive
 * solo en el radio de mallado y no toca el estructural, que es la referencia de rigidez del
 * viento; y con el perfil desactivado los dos vuelven a coincidir.
 *
 * @see @ref bib_shinozaki1964
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

    // 1) Punto de partida: en una cadena, el pipe model da un CILINDRO.
    TestTrue(TEXT("el pipe model da radio constante en una cadena"),
        FMath::IsNearlyEqual(Sk.Nodes[0].PipeRadius, Sk.Nodes[N / 2].PipeRadius, 1e-4f));

    SpaceColonization::ApplyTrunkProfile(Sk, *Sp);

    // 2) El pie es claramente más ancho que el fuste a media altura.
    TestTrue(TEXT("la base es mas ancha que el fuste"),
        Sk.Nodes[0].Radius > Sk.Nodes[N / 2].Radius * 1.2f);

    // 3) ... y el fuste afila hacia arriba sobre la longitud de arco acumulada.
    TestTrue(TEXT("el eje afila con la altura"),
        Sk.Nodes[N / 2].Radius > Sk.Nodes[N - 1].Radius);

    // 4) Monotonía: ningún nodo más fino que su hijo. Sin esta pasada el afilado deja el eje
    //    más fino que el primer nodo de copa y aparece un cono invertido muy visible.
    for (int32 i = 1; i < N; ++i)
    {
        const int32 P = Sk.Nodes[i].Parent;
        TestTrue(TEXT("ningun nodo es mas fino que su hijo"),
            Sk.Nodes[P].Radius >= Sk.Nodes[i].Radius - KINDA_SMALL_NUMBER);
    }

    // 5) El radio estructural no se toca: el ensanche de pie puede duplicar el radio de
    //    mallado sin hacer al árbol un gramo más rígido.
    TestTrue(TEXT("el perfil no contamina PipeRadius"),
        FMath::IsNearlyEqual(Sk.Nodes[0].PipeRadius, Sk.Nodes[N / 2].PipeRadius, 1e-4f));
    TestTrue(TEXT("el ensanche solo esta en Radius"),
        Sk.Nodes[0].Radius > Sk.Nodes[0].PipeRadius);

    // 6) Control: con el perfil desactivado, el radio de mallado vuelve al del pipe model.
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

/**
 * El ángulo mínimo de inserción separa la rama lateral de su padre, y ni un grado más.
 *
 * Una dirección casi paralela se abre hasta el mínimo EXACTO y con el giro más corto posible,
 * comprobado exigiendo que la salida siga en el plano que formaban padre y dirección; una que
 * ya se separaba lo suficiente se devuelve intacta. Los dos casos frontera son una dirección
 * idéntica a la del padre -no hay plano que preservar, pero la salida tiene que ser unitaria y
 * separada, nunca NaN- y el ángulo cero como desactivación.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoBranchAngle, "Eco.Arbol.AnguloDeInsercion", EcoTestFlags)
bool FEcoBranchAngle::RunTest(const FString&) {
    const FVector Parent = FVector::UpVector;
    const float MinDeg = 45.f;
    const float CosMin = FMath::Cos(FMath::DegreesToRadians(MinDeg));

    // 1) Una dirección casi paralela al padre se abre hasta el mínimo exacto.
    {
        const FVector Almost = FVector(0.05f, 0.f, 1.f).GetSafeNormal();
        const FVector Out = SpaceColonization::ApplyBranchAngle(Almost, Parent, MinDeg);
        TestTrue(TEXT("se abre hasta el angulo pedido"),
            FMath::IsNearlyEqual((float)FVector::DotProduct(Out, Parent), CosMin, 1e-3f));
        TestTrue(TEXT("sigue siendo unitaria"), FMath::IsNearlyEqual((float)Out.Size(), 1.f, 1e-3f));

        // El giro es MÍNIMO: se queda en el plano que formaban padre y dirección.
        const FVector PlaneN = FVector::CrossProduct(Parent, Almost).GetSafeNormal();
        TestTrue(TEXT("el giro se queda en el plano padre-direccion"),
            FMath::Abs((float)FVector::DotProduct(Out, PlaneN)) < 1e-3f);
    }

    // 2) Una dirección que YA se separa lo suficiente no se toca.
    {
        const FVector Wide = FVector(1.f, 0.f, 0.2f).GetSafeNormal();
        const FVector Out = SpaceColonization::ApplyBranchAngle(Wide, Parent, MinDeg);
        TestTrue(TEXT("no toca lo que ya se separaba"), Out.Equals(Wide, 1e-4));
    }

    // 3) Caso degenerado: dirección IDÉNTICA a la del padre. No hay plano que preservar,
    //    pero la salida sigue siendo unitaria y separada, no un NaN.
    {
        const FVector Out = SpaceColonization::ApplyBranchAngle(Parent, Parent, MinDeg);
        TestTrue(TEXT("caso paralelo: unitaria"), FMath::IsNearlyEqual((float)Out.Size(), 1.f, 1e-3f));
        TestTrue(TEXT("caso paralelo: separada"),
            (float)FVector::DotProduct(Out, Parent) <= CosMin + 1e-3f);
    }

    // 4) Ángulo 0: desactivación, la identidad.
    {
        const FVector Almost = FVector(0.05f, 0.f, 1.f).GetSafeNormal();
        TestTrue(TEXT("0 grados = desactivado"),
            SpaceColonization::ApplyBranchAngle(Almost, Parent, 0.f).Equals(Almost, 1e-4));
    }

    return true;
}

/**
 * Sección de tronco no circular SIN abrir la costura del tubo.
 *
 * Los vértices @f$k = 0@f$ y @f$k = K@f$ de cada anillo son el MISMO punto, duplicado solo
 * para cerrar la UV en @f$u = 1@f$. La deformación angular de la sección únicamente cierra si
 * es exactamente periódica en el ángulo: cualquier término que no lo sea separa esos dos
 * vértices y abre una raja a lo largo de todo el tronco.
 *
 * Sobre un árbol crecido y mallado de verdad se comprueban cinco cosas: costura cerrada bit a
 * bit en posición y con la misma normal a los dos lados, sección que ya no es una
 * circunferencia, normales recalculadas sobre la superficie deformada -y no solo la silueta-,
 * ausencia de vértices degenerados y determinismo del par crecer-mallar.
 *
 * @note La costura se compara con igualdad exacta: una tolerancia la absorbería el
 *       desplazamiento que el material de viento aplica después sobre esos mismos vértices.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoSectionSeam, "Eco.Arbol.SeccionYCostura", EcoTestFlags)
bool FEcoSectionSeam::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->SectionLobeAmount = 0.15f;
    Sp->SectionLobeCount = 3;
    Sp->BarkReliefAmount = 0.06f;
    Sp->RingSegments = 12;           // >= 8: el mallador no lo eleva por su cuenta

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

    // 1) Costura cerrada, bit a bit, en posición y en normal.
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

    // 2) La sección del tronco ya no es una circunferencia: se mide sobre el anillo del pie.
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

    // 3) Las normales dejan de ser radiales puras: si lo fuesen, el sombreado seguiría
    //    leyéndose como un cilindro liso y la deformación viviría solo en la silueta.
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

    // 4) Ningún vértice degenerado: el recorte inferior del radio tiene que sostenerse.
    for (int32 v = 0; v < W.Vertices.Num(); ++v)
    {
        if (W.Vertices[v].ContainsNaN())
        {
            AddError(FString::Printf(TEXT("Vertice %d con NaN."), v));
            break;
        }
    }

    // 5) Determinismo: misma semilla, misma malla exacta.
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
 * Arquitectura excurrente: el eje atraviesa la copa y las inserciones de rama se reparten en
 * altura en vez de amontonarse a una sola cota.
 *
 * En una copa cónica el radio de la envolvente es MÁXIMO justo en la base de la copa. Si el eje
 * muriese ahí, su punta se llevaría de golpe los atractores de la banda más ancha y todas las
 * ramas nacerían a la misma altura: la silueta de paraguas. Con el líder recorriendo la copa,
 * el detector mide qué fracción de las inserciones cuelga de la mitad baja y falla con el
 * porcentaje medido si es demasiado pequeña.
 *
 * Las otras cuatro comprobaciones cierran la geometría resultante: el eje llega arriba y tiene
 * varios nodos, la falda de sub-copa siembra atractores por debajo de la base de copa, el eje
 * afila con la altura y ningún nodo se dispara muy por encima del ápice.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoLeaderSpread, "Eco.Arbol.EjeYRepartoDeRamas", EcoTestFlags)
bool FEcoLeaderSpread::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->CrownShape = ECrownShape::Conical;
    Sp->LeaderFraction = 1.f;        // conífera excurrente: el eje llega al ápice
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

    // Cotas de referencia derivadas de los rasgos de la especie: base de copa y ápice.
    const float CrownH = Sp->CrownHeightCm;
    const float TrunkH = CrownH * Sp->TrunkFraction / (1.f - Sp->TrunkFraction);
    const float CrownBaseZ = TrunkH;
    const float ApexZ = CrownBaseZ + CrownH;

    // 1) El eje llega arriba, no muere en la base de la copa.
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

    // 2) Las inserciones -nodos no-eje colgados del eje- se reparten en altura en vez de
    //    amontonarse en la punta del fuste.
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

    // 3) La falda de sub-copa siembra atractores por debajo de la base de copa.
    {
        int32 BelowCrown = 0;
        for (const FAttractor& A : Cloud.Attractors)
        {
            if (A.Pos.Z < CrownBaseZ - 1.f) { ++BelowCrown; }
        }
        TestTrue(TEXT("la falda siembra bajo la base de copa"), BelowCrown > 0);
    }

    // 4) El eje afila con la altura, que es lo que da el pipe model en cuanto hay ramas
    //    laterales repartidas a lo largo del fuste.
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

    // 5) La copa no se sale de la envolvente por arriba: ni el eje ni el SCA se disparan.
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
// Deformación de tronco por árbol (arqueado y torcido)
// ---------------------------------------------------------------------------

/**
 * Crece un árbol de prueba completo y devuelve su esqueleto. Concentra el andamiaje que
 * comparten los tests de deformación, que crecen el mismo árbol muchas veces.
 *
 * @param Seed               Semilla del flujo de CRECIMIENTO, distinta de la de curvatura.
 * @param DeformSeedOverride Identidad de curvatura impuesta desde fuera; -1 la deja derivarse
 *                           del propio árbol.
 * @param OutFinalRng        Si no es nulo, recibe el estado del flujo tras crecer, con el que
 *                           se comprueba que una capa de deformación no lo desplaza.
 */
static void EcoGrowTestTree(const USpeciesData& Sp, uint32 Seed, FTreeSkeleton& OutSk,
    int64 DeformSeedOverride = -1, uint32* OutFinalRng = nullptr)
{
    uint32 Rng = Seed;
    FTreeLightGridFine Light;
    FAttractorCloud Cloud;
    FSpaceColonizationConfig Cfg;
    Cfg.DeformSeedOverride = DeformSeedOverride;
    SpaceColonization::GrowTree(Sp, Rng, FVector::ZeroVector, nullptr, Cfg, OutSk, Light, Cloud);
    if (OutFinalRng) { *OutFinalRng = Rng; }
}

/** Añade una capa de deformación al asset de especie de prueba, con su puerta de probabilidad
    y su rango de ángulo en grados. */
static void EcoAddDeformLayer(USpeciesData& Sp, ETrunkDeformType Type, float Probability,
    float MinDeg, float MaxDeg, float ShapeParam)
{
    FTrunkDeformLayerSpec L;
    L.Type = Type;
    L.Probability = Probability;
    L.MinAngleDeg = MinDeg;
    L.MaxAngleDeg = MaxDeg;
    L.ShapeParam = ShapeParam;
    Sp.TrunkDeformLayers.Add(L);
}

/**
 * Desvío de punta: desplazamiento horizontal del nodo más alto respecto a la base, en fracción
 * de la altura alcanzada. Es la única magnitud escalar con la que los cuatro tests de
 * deformación comparan formas de árbol.
 *
 * @return Razón adimensional, o 0 si el árbol es degenerado y no levanta del suelo.
 */
static float EcoTipLeanRatio(const FTreeSkeleton& Sk)
{
    int32 Top = INDEX_NONE;
    double TopZ = -TNumericLimits<double>::Max();
    for (int32 i = 0; i < Sk.Num(); ++i)
    {
        if (Sk.Nodes[i].Pos.Z > TopZ) { TopZ = Sk.Nodes[i].Pos.Z; Top = i; }
    }
    if (Top == INDEX_NONE || TopZ <= 0.0) { return 0.f; }

    const FVector Tip = Sk.Nodes[Top].Pos - Sk.Nodes[0].Pos;
    return static_cast<float>(FVector2D(Tip.X, Tip.Y).Size() / TopZ);
}

/**
 * Aislamiento del flujo de deformación. Una especie con una capa presente pero IMPOSIBLE
 * -probabilidad 0- tiene que dar el mismo árbol, bit a bit, que la especie sin capas, y dejar
 * el flujo principal exactamente donde lo dejaría ésta.
 *
 * Lo segundo es lo que de verdad se prueba: el deformador extrae siempre sus muestras de un
 * sub-flujo derivado por hash y no del estado vivo del árbol. Si tirase del flujo principal,
 * añadir una capa a UNA especie desplazaría la secuencia y cambiaría la copa de todos los
 * árboles del bosque.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeformNoOp, "Eco.Arbol.DeformNoOp", EcoTestFlags)
bool FEcoDeformNoOp::RunTest(const FString&) {
    USpeciesData* Base = EcoTestSpecies(GetTransientPackage());
    USpeciesData* WithLayers = EcoTestSpecies(GetTransientPackage());
    if (!Base || !WithLayers) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }

    // La capa sigue consumiendo sus cuatro muestras del sub-flujo aunque nunca se active:
    // ese gasto fijo es parte del contrato y no debe notarse desde fuera.
    EcoAddDeformLayer(*WithLayers, ETrunkDeformType::Arc, /*Probability*/ 0.f, 10.f, 30.f, 1.5f);

    FTreeSkeleton A, B;
    uint32 RngA = 0, RngB = 0;
    EcoGrowTestTree(*Base, 909u, A, -1, &RngA);
    EcoGrowTestTree(*WithLayers, 909u, B, -1, &RngB);

    TestEqual(TEXT("el deformador no desplaza el stream RNG principal"), RngB, RngA);
    TestEqual(TEXT("mismo numero de nodos"), B.Num(), A.Num());

    bool bSame = (A.Num() == B.Num());
    for (int32 i = 0; bSame && i < A.Num(); ++i)
    {
        bSame = (A.Nodes[i].Pos == B.Nodes[i].Pos) && (A.Nodes[i].Radius == B.Nodes[i].Radius);
    }
    TestTrue(TEXT("sin capas activas la geometria es identica bit a bit"), bSame);
    return true;
}

/**
 * La deformación es una ISOMETRÍA: dobla el árbol sin estirarlo. El re-encadenado rota el
 * vector al padre nodo a nodo, de modo que la curvatura se acumula a lo largo del fuste y cada
 * longitud de internodo se conserva.
 *
 * Es la propiedad de la que cuelga el resto de la geometría. El perfil de tronco trabaja sobre
 * la longitud de arco acumulada y las UV de la corteza también: si las longitudes cambiasen al
 * doblar, un árbol arqueado tendría el pie de otro tamaño que su gemelo recto, cuando debe ser
 * el MISMO árbol doblado. De ahí que el doblado se aplique antes que el perfil, y que el test
 * cierre comprobando que el radio del pie es el mismo en los dos.
 *
 * Se exige además que la topología no cambie -la deformación solo mueve nodos-, que la raíz
 * siga plantada donde estaba y que ningún nodo salga con NaN ni con dirección no unitaria.
 *
 * @see @ref bib_barr1984
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeformIsometry, "Eco.Arbol.DeformIsometria", EcoTestFlags)
bool FEcoDeformIsometry::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->LeaderFraction = 1.f;

    // Árbol de referencia sin deformar.
    FTreeSkeleton Straight;
    EcoGrowTestTree(*Sp, 31337u, Straight);
    if (Straight.Num() < 8) { AddError(TEXT("El SCA no produjo esqueleto.")); return false; }

    // El MISMO árbol -misma semilla de crecimiento- con una capa determinista de 25 grados.
    EcoAddDeformLayer(*Sp, ETrunkDeformType::Arc, /*Probability*/ 1.f, 25.f, 25.f, 1.f);
    FTreeSkeleton Bent;
    EcoGrowTestTree(*Sp, 31337u, Bent);

    if (Bent.Num() != Straight.Num())
    {
        AddError(TEXT("La deformacion cambio la TOPOLOGIA del esqueleto: solo debe mover nodos."));
        return false;
    }

    float WorstError = 0.f;
    for (int32 i = 1; i < Bent.Num(); ++i)
    {
        const int32 P = Bent.Nodes[i].Parent;
        if (P < 0) { continue; }

        const float LenBefore = (float)FVector::Dist(Straight.Nodes[i].Pos, Straight.Nodes[P].Pos);
        const float LenAfter = (float)FVector::Dist(Bent.Nodes[i].Pos, Bent.Nodes[P].Pos);
        WorstError = FMath::Max(WorstError, FMath::Abs(LenAfter - LenBefore));

        if (Bent.Nodes[i].Pos.ContainsNaN())
        {
            AddError(FString::Printf(TEXT("Nodo %d con NaN tras deformar."), i));
            return false;
        }
        if (!FMath::IsNearlyEqual(Bent.Nodes[i].Dir.Size(), 1.f, 1e-3f))
        {
            AddError(FString::Printf(TEXT("Nodo %d con Dir no unitaria (%.4f) tras deformar."),
                i, Bent.Nodes[i].Dir.Size()));
            return false;
        }
    }
    TestTrue(FString::Printf(TEXT("las longitudes de internodo se conservan (peor error %.4f cm)"), WorstError),
        WorstError < 0.05f);

    // La raíz no se mueve: el árbol sigue plantado donde estaba.
    TestTrue(TEXT("la base del tronco no se mueve"),
        Bent.Nodes[0].Pos.Equals(Straight.Nodes[0].Pos, 1e-3));

    // Corolario de la isometría: el perfil de tronco sale igual porque la longitud de arco
    // sobre la que se calcula es invariante.
    TestTrue(TEXT("el radio del pie es el mismo doblado que recto"),
        FMath::IsNearlyEqual(Bent.Nodes[0].Radius, Straight.Nodes[0].Radius, 0.05f));

    return true;
}

/**
 * La capa de arco arquea de verdad, y más ángulo da más arqueo. Se comparan tres árboles con
 * la misma semilla de crecimiento -sin capa, 15 grados y 30 grados- por su desvío de punta.
 *
 * Los umbrales se derivan de la fórmula del deformador y no se ajustan a ojo: con
 * @f$\alpha(t) = \theta t^{k}@f$, el desplazamiento de la punta es del orden de
 * @f$H\theta/(k+1)@f$, o sea @f$\approx 0{,}26H@f$ con 30 grados y @f$k = 1@f$ si el eje
 * llegase al ápice. Se pide bastante menos, porque el nodo más alto puede ser una rama de copa
 * y no el propio eje.
 *
 * @note La sinuosidad base del tronco se anula en la especie de prueba para que lo medido sea
 *       solo la capa.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeformArcs, "Eco.Arbol.DeformArquea", EcoTestFlags)
bool FEcoDeformArcs::RunTest(const FString&) {
    auto GrowWithArc = [this](float AngleDeg, float& OutLean) -> bool
        {
            USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
            if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
            Sp->LeaderFraction = 1.f;   // el eje llega al ápice: el arqueo se lee entero
            Sp->TrunkSweepDeg = 0.f;    // aísla la deformación de la sinuosidad base
            Sp->TrunkWobbleDeg = 0.f;
            if (AngleDeg > 0.f)
            {
                EcoAddDeformLayer(*Sp, ETrunkDeformType::Arc, 1.f, AngleDeg, AngleDeg, 1.f);
            }

            FTreeSkeleton Sk;
            EcoGrowTestTree(*Sp, 20250828u, Sk);
            if (Sk.Num() < 8) { AddError(TEXT("El SCA no produjo esqueleto.")); return false; }
            OutLean = EcoTipLeanRatio(Sk);
            return true;
        };

    float LeanNone = 0.f, LeanMild = 0.f, LeanStrong = 0.f;
    if (!GrowWithArc(0.f, LeanNone)) { return false; }
    if (!GrowWithArc(15.f, LeanMild)) { return false; }
    if (!GrowWithArc(30.f, LeanStrong)) { return false; }

    TestTrue(FString::Printf(TEXT("sin deformacion el arbol es casi vertical (desvio %.3f)"), LeanNone),
        LeanNone < 0.10f);
    TestTrue(FString::Printf(TEXT("Arc 30 grados arquea el arbol (desvio %.3f)"), LeanStrong),
        LeanStrong > 0.12f);
    TestTrue(FString::Printf(TEXT("mas angulo, mas arqueo (%.3f < %.3f)"), LeanMild, LeanStrong),
        LeanMild < LeanStrong);
    return true;
}

/**
 * Validación estadística de la puerta de probabilidad: con @f$p = 0{,}5@f$ se dobla
 * aproximadamente la mitad de los árboles, cada ángulo cae dentro del rango declarado en el
 * asset y los ángulos sorteados son mayoritariamente distintos entre sí, no una única forma
 * repetida.
 *
 * La segunda mitad prueba el contrato de muestreo por capas. Dos especies que solo difieren en
 * la probabilidad de su PRIMERA capa tienen que producir exactamente el mismo azimut en la
 * última: cada capa extrae siempre el mismo número de valores y la puerta se aplica después, de
 * modo que recalibrar una capa no re-reparte las formas de todo el bosque.
 *
 * @note Se mide sobre el muestreo y no creciendo doscientos árboles: la puerta de probabilidad
 *       vive entera ahí y un SCA completo por muestra no cabe en el presupuesto de la batería.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeformProbability, "Eco.Arbol.DeformProbabilidad", EcoTestFlags)
bool FEcoDeformProbability::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    EcoAddDeformLayer(*Sp, ETrunkDeformType::Arc, /*Probability*/ 0.5f, 10.f, 30.f, 1.5f);

    const int32 Samples = 2000;
    int32 Bent = 0;
    TSet<uint32> DistinctAngles;
    for (int32 i = 0; i < Samples; ++i)
    {
        const TrunkDeformer::FTrunkDeformState S =
            TrunkDeformer::Sample(*Sp, EcoRand::Hash32(static_cast<uint32>(i) ^ 0xABCDu));
        if (!S.IsIdentity())
        {
            ++Bent;
            DistinctAngles.Add(static_cast<uint32>(FMath::RoundToInt(S.Layers[0].AngleRad * 10000.f)));

            const float Deg = FMath::RadiansToDegrees(S.Layers[0].AngleRad);
            if (Deg < 9.9f || Deg > 30.1f)
            {
                AddError(FString::Printf(TEXT("Angulo %.2f fuera del rango [10, 30] del asset."), Deg));
                return false;
            }
        }
    }

    const float Frac = static_cast<float>(Bent) / static_cast<float>(Samples);
    TestTrue(FString::Printf(TEXT("con p=0.5 se dobla ~la mitad (%.2f)"), Frac),
        Frac > 0.45f && Frac < 0.55f);
    TestTrue(TEXT("cada arbol doblado recibe su propio angulo"), DistinctAngles.Num() > Bent / 2);

    // Editar la probabilidad de una capa no puede desplazar las muestras de las siguientes.
    {
        USpeciesData* Two = EcoTestSpecies(GetTransientPackage());
        USpeciesData* TwoEdited = EcoTestSpecies(GetTransientPackage());
        if (!Two || !TwoEdited) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }

        EcoAddDeformLayer(*Two, ETrunkDeformType::Lean, 0.5f, 5.f, 8.f, 1.f);
        EcoAddDeformLayer(*Two, ETrunkDeformType::Arc, 1.f, 20.f, 20.f, 1.f);

        EcoAddDeformLayer(*TwoEdited, ETrunkDeformType::Lean, 0.9f, 5.f, 8.f, 1.f); // única diferencia
        EcoAddDeformLayer(*TwoEdited, ETrunkDeformType::Arc, 1.f, 20.f, 20.f, 1.f);

        bool bStable = true;
        for (int32 i = 0; i < 64 && bStable; ++i)
        {
            const uint32 Seed = EcoRand::Hash32(static_cast<uint32>(i));
            const TrunkDeformer::FTrunkDeformState A = TrunkDeformer::Sample(*Two, Seed);
            const TrunkDeformer::FTrunkDeformState B = TrunkDeformer::Sample(*TwoEdited, Seed);

            // La capa de arco tiene probabilidad 1: está siempre y es la ÚLTIMA de cada estado.
            bStable = !A.IsIdentity() && !B.IsIdentity()
                && FMath::IsNearlyEqual(A.Layers.Last().AzimuthRad, B.Layers.Last().AzimuthRad, 1e-6f);
        }
        TestTrue(TEXT("cambiar la probabilidad de una capa no mueve las muestras de las siguientes"), bStable);
    }

    return true;
}

/**
 * La identidad de curvatura viaja aparte de la semilla de crecimiento. Es el mecanismo del que
 * dependen las dos cosas que no pueden fallar en el render instanciado: que un árbol no se
 * enderece al cruzar de bucket de edad, y que no se enderece al promocionar a hero tree delante
 * del jugador.
 *
 * El primer bloque comprueba que la semilla de deformación de una variante ignora el bucket de
 * edad -y sí distingue variantes y especies-, pasando por las claves de arquetipo reales para
 * atrapar el fallo de que el bucket acabe entrando en la fórmula. El segundo hace crecer dos
 * árboles con semillas de crecimiento distintas pero la misma identidad impuesta y exige que se
 * doblen igual, que es el puente entre hero tree e instancia. El tercero cierra con determinismo
 * puro.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeformIdentity, "Eco.Arbol.DeformIdentidad", EcoTestFlags)
bool FEcoDeformIdentity::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->LeaderFraction = 1.f;
    Sp->TrunkSweepDeg = 0.f;
    Sp->TrunkWobbleDeg = 0.f;
    EcoAddDeformLayer(*Sp, ETrunkDeformType::Arc, 1.f, 25.f, 25.f, 1.f);

    // 1) Dos claves de arquetipo que solo difieren en el bucket de edad dan la misma semilla
    //    de deformación: un árbol no se endereza al crecer.
    {
        const FArchetypeKey Young(/*Species*/ 1, /*AgeBucket*/ 0, /*Variant*/ 2);
        const FArchetypeKey Old(/*Species*/ 1, /*AgeBucket*/ 4, /*Variant*/ 2);
        TestEqual(TEXT("la curvatura de una variante ignora el bucket de edad"),
            UTreeLibrary::VariantDeformSeed(Young.Species, Young.Variant),
            UTreeLibrary::VariantDeformSeed(Old.Species, Old.Variant));
    }
    TestNotEqual(TEXT("variantes distintas reciben curvaturas distintas"),
        UTreeLibrary::VariantDeformSeed(1, 0), UTreeLibrary::VariantDeformSeed(1, 3));
    TestNotEqual(TEXT("especies distintas reciben curvaturas distintas"),
        UTreeLibrary::VariantDeformSeed(0, 1), UTreeLibrary::VariantDeformSeed(2, 1));

    // 2) Dos árboles con semillas de crecimiento distintas pero la misma identidad de
    //    curvatura impuesta se doblan igual: es el puente hero tree <-> instancia.
    const int64 Override = static_cast<int64>(UTreeLibrary::VariantDeformSeed(0, 1));
    FTreeSkeleton HeroLike, InstanceLike;
    EcoGrowTestTree(*Sp, 111u, HeroLike, Override);
    EcoGrowTestTree(*Sp, 222u, InstanceLike, Override);

    const float LeanA = EcoTipLeanRatio(HeroLike);
    const float LeanB = EcoTipLeanRatio(InstanceLike);
    TestTrue(FString::Printf(TEXT("mismo override -> misma curvatura (%.3f vs %.3f)"), LeanA, LeanB),
        FMath::Abs(LeanA - LeanB) < 0.05f);

    // 3) Determinismo puro: misma semilla, misma geometría exacta.
    {
        FTreeSkeleton R1, R2;
        EcoGrowTestTree(*Sp, 4444u, R1);
        EcoGrowTestTree(*Sp, 4444u, R2);
        bool bSame = (R1.Num() == R2.Num());
        for (int32 i = 0; bSame && i < R1.Num(); ++i)
        {
            bSame = (R1.Nodes[i].Pos == R2.Nodes[i].Pos);
        }
        TestTrue(TEXT("misma semilla -> mismo arbol doblado"), bSame);
    }

    return true;
}

/**
 * El tope de doblado acumulado se sostiene aunque el asset pida un disparate. Cuatro capas de
 * inclinación de 45 grados suman 180 nominales: es un error de edición, no un caso de uso, pero
 * la salida tiene que ser un árbol raro y no NaN, geometría invertida o un tronco tumbado.
 *
 * El recorte actúa sobre la MAGNITUD del vector de doblado ya sumado y preserva su dirección,
 * no capa a capa. El umbral del desvío de punta se deriva de ese tope -1 rad, unos 57 grados,
 * cuya tangente vale ~1,55- más margen para las ramas de copa que salen hacia el lado del vuelco.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoDeformClamp, "Eco.Arbol.DeformTope", EcoTestFlags)
bool FEcoDeformClamp::RunTest(const FString&) {
    USpeciesData* Sp = EcoTestSpecies(GetTransientPackage());
    if (!Sp) { AddError(TEXT("No se pudo crear la especie de prueba.")); return false; }
    Sp->LeaderFraction = 1.f;
    for (int32 i = 0; i < 4; ++i)
    {
        EcoAddDeformLayer(*Sp, ETrunkDeformType::Lean, 1.f, 45.f, 45.f, 1.f);
    }

    FTreeSkeleton Sk;
    EcoGrowTestTree(*Sp, 5150u, Sk);
    if (Sk.Num() < 8) { AddError(TEXT("El SCA no produjo esqueleto.")); return false; }

    for (int32 i = 0; i < Sk.Num(); ++i)
    {
        if (Sk.Nodes[i].Pos.ContainsNaN() || Sk.Nodes[i].Dir.ContainsNaN())
        {
            AddError(FString::Printf(TEXT("Nodo %d con NaN con las capas al maximo."), i));
            return false;
        }
    }

    // Umbral derivado del tope de doblado, con margen para las ramas de copa.
    const float Lean = EcoTipLeanRatio(Sk);
    TestTrue(FString::Printf(TEXT("el tope acota el vuelco (%.2f)"), Lean), Lean < 2.5f);
    TestTrue(TEXT("el arbol sigue creciendo hacia arriba"), Sk.Nodes[Sk.Num() - 1].Pos.Z > 0.0);
    return true;
}


// ---------------------------------------------------------------------------
// Relieve: síntesis por ruido reparametrizado y erosión
// ---------------------------------------------------------------------------

/**
 * Parámetros de relieve reducidos para la batería: la MISMA extensión de ~1 km y la misma
 * amplitud vertical que el mapa real, pero a media resolución y con la erosión abreviada.
 *
 * Lo que se baja es la resolución, nunca la extensión ni la escala de altura: encoger el mapa
 * dejando el desnivel intacto cambiaría la física de pendientes -300 m de caída en 256 m de
 * mapa son empinados por construcción- y los umbrales de los tests de pendiente dejarían de
 * significar nada.
 */
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

/**
 * Pendiente máxima y media del campo, medidas como @f$|\Delta h| / CellSize@f$ entre vecinos
 * 4-conexos. Solo se miran las diferencias hacia delante, para no contar cada arista dos veces,
 * y la suma se acumula en `double` porque son cientos de miles de aristas.
 *
 * @note La pendiente resultante es una TANGENTE adimensional, no un ángulo: 0,65 son unos 33
 *       grados y 4,2 unos 76. Los umbrales de los tests de relieve están escritos en esa unidad.
 */
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

/**
 * Determinismo bit a bit del relieve completo: dos mapas generados con la misma semilla se
 * comparan elemento a elemento, sin tolerancia, y una semilla distinta debe dar un mapa
 * distinto.
 *
 * Cubre la cadena entera -ruido, 8.000 gotas hidráulicas y ocho iteraciones térmicas-, y la
 * parte hidráulica es serial por construcción: cada gota ve el resultado de las anteriores.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTerrainDeterminism, "Eco.Relieve.Determinismo", EcoTestFlags)
bool FEcoTerrainDeterminism::RunTest(const FString&)
{
    // Cadena completa: ruido + gotas + térmica.
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

/**
 * Recorte de octavas por el límite de muestreo de la rejilla: una octava cuya longitud de onda
 * cae por debajo del doble del tamaño de celda no es representable y solo aporta aliasing, así
 * que se descarta antes de sumarla.
 *
 * Los cuatro casos están calculados a mano sobre una octava base de 700 m con lacunaridad 2, e
 * incluyen los dos bordes del contrato: nunca se devuelve menos de una octava y nunca se
 * añaden más de las pedidas.
 *
 * @see @ref bib_nyquistshannon
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTerrainNyquist, "Eco.Relieve.Nyquist", EcoTestFlags)
bool FEcoTerrainNyquist::RunTest(const FString&)
{
    // Octava base de 700 m. Con celda de 2 m el límite es 4 m y caben 8 de las 12 octavas;
    // con celda de 30 m el límite es 60 m y solo caben las cuatro más largas.
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

/**
 * Estadística de pendientes del ruido puro, con la erosión desactivada para medir solo la
 * síntesis: la pendiente media debe quedar en valores de relieve y no de agujas, y no debe
 * haber paredes verticales.
 *
 * La última aserción detecta aliasing con independencia de la amplitud del mapa: el salto entre
 * dos celdas vecinas no puede pasar de una cuarta parte de la amplitud total, cota que un
 * campo con picos de un vértice de ancho no cumple.
 *
 * @note Los umbrales llevan margen deliberado sobre los valores medidos, porque el resultado
 *       varía entre semillas y la implementación de Perlin puede diferir entre plataformas.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEcoTerrainSlopes, "Eco.Relieve.PendientesRealistas", EcoTestFlags)
bool FEcoTerrainSlopes::RunTest(const FString&)
{
    // Solo el ruido: formas de cientos de metros y octavas recortadas a Nyquist.
    // Valores medidos con estos parámetros: media 0.314, máximo 2.62.
    FTerrainGenParams P = EcoTestTerrainParams(12345u);
    P.bErosion = false;
    FHeightField HF;
    HF.Generate(P);

    float MaxSlope, MeanSlope;
    EcoTestSlopeStats(HF.Field, MaxSlope, MeanSlope);
    TestTrue(TEXT("pendiente media < 33 grados"), MeanSlope < 0.65f);
    TestTrue(TEXT("sin paredes verticales (max < 76 grados)"), MaxSlope < 4.2f);

    // El salto entre celdas vecinas es una fracción pequeña de la amplitud total del mapa.
    float Mn, Mx;
    FField2D::MinMax(HF.Field.Data, Mn, Mx);
    const float Amplitude = Mx - Mn;
    TestTrue(TEXT("salto maximo entre vecinos < 25% de la amplitud"),
        MaxSlope * static_cast<float>(HF.Field.CellSize) < 0.25f * Amplitude);
    return true;
}

/**
 * Estabilidad de las dos erosiones, medidas por separado.
 *
 * La cadena completa -hidráulica más térmica- debe dejar todas las alturas finitas y el rango
 * acotado respecto al original con un margen del 5 % de la amplitud: la erosión no crea
 * material de la nada ni cava por debajo del mínimo previo. Deliberadamente NO se exige
 * suavizado, porque la hidráulica talla barrancos y puede subir la pendiente media, y eso es
 * relieve, no un defecto.
 *
 * La térmica sola, con parámetros agresivos, sí tiene que recortar la pendiente máxima hacia el
 * ángulo de talud sin empinar el terreno en media, y sobre todo CONSERVAR LA MASA: la suma de
 * alturas antes y después difiere menos de una diezmilésima relativa. Es la firma de un esquema
 * gather de dos pasadas con doble buffer, que solo redistribuye material entre vecinos.
 */
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

    // 1) Cadena completa (hidráulica + térmica): finita y acotada. No se comprueba suavizado,
    //    porque la hidráulica talla barrancos y puede subir la pendiente media.
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

    // 2) Térmica sola y agresiva sobre un mapa fresco: recorta la pendiente máxima hacia el
    //    ángulo de talud sin ganar masa (medido: máximo 2.55 -> 1.57).
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
