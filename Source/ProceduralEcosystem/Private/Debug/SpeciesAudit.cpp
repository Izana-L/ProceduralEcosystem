#include "Debug/SpeciesAudit.h"

#include "Config/EcosystemSettings.h"
#include "Species/SpeciesData.h"
#include "Ecology/Vigor.h"
#include "Core/GridMath.h"

#include "HAL/IConsoleManager.h"

// Categoria propia: LogEco es file-static de EcosystemSubsystem.cpp (ver la
// nota de EcoCore.h sobre por que no vive en una cabecera reflejada por UHT).
DEFINE_LOG_CATEGORY_STATIC(LogEcoAudit, Log, All);

namespace
{
    /** Luz relativa bajo un dosel cerrado, para leer la diferenciacion EN SOMBRA,
        que es donde la tolerancia deberia decidir algo. Es un valor de lectura,
        no entra en la simulacion. */
    constexpr float kDeepShadeQ = 0.20f;

    /**
     * Un eje monotono del modelo, ya normalizado a "mas es mejor". Es la lista
     * que hace falta para la prueba de dominancia: si una especie gana o empata
     * en todos ellos, gana la simulacion.
     *
     * bHigherIsBetter=false marca los ejes donde el numero pequeno es la ventaja
     * (demandas de recurso, edad de madurez): se invierten al puntuar para que
     * la comparacion sea siempre "mayor gana".
     */
    struct FAxis
    {
        const TCHAR* Name;
        bool bHigherIsBetter;
        float (*Get)(const USpeciesData&);
    };

    const FAxis kAxes[] =
    {
        { TEXT("ShadeTolerance"),      true,  [](const USpeciesData& S) { return S.ShadeTolerance; } },
        { TEXT("GrowthRate"),          true,  [](const USpeciesData& S) { return S.GrowthRate; } },
        { TEXT("Longevity"),           true,  [](const USpeciesData& S) { return S.Longevity; } },
        { TEXT("MaxBiomass"),          true,  [](const USpeciesData& S) { return S.MaxBiomass; } },
        { TEXT("MaxHeightCm"),         true,  [](const USpeciesData& S) { return S.MaxHeightCm; } },
        { TEXT("SeedDispersalRadius"), true,  [](const USpeciesData& S) { return S.SeedDispersalRadius; } },
        { TEXT("SeedRateScale"),       true,  [](const USpeciesData& S) { return S.SeedRateScale; } },
        { TEXT("GerminationRateScale"),true,  [](const USpeciesData& S) { return S.GerminationRateScale; } },
        { TEXT("WaterDemand"),         false, [](const USpeciesData& S) { return S.WaterDemand; } },
        { TEXT("NutrientDemand"),      false, [](const USpeciesData& S) { return S.NutrientDemand; } },
        { TEXT("MaturityAge"),         false, [](const USpeciesData& S) { return S.MaturityAge; } },
        { TEXT("MinLightGerminacion"), false, [](const USpeciesData& S) { return S.MinLightForGermination; } },
    };
    constexpr int32 kNumAxes = UE_ARRAY_COUNT(kAxes);

    /** Valor del eje ya orientado a "mas es mejor". */
    float AxisScore(const FAxis& Axis, const USpeciesData& Species)
    {
        const float Raw = Axis.Get(Species);
        return Axis.bHigherIsBetter ? Raw : -Raw;
    }

    /** Celdas del campo de recursos que toca el disco radicular de un adulto.
        Misma cuenta que EcologyRules::KernelCellCount, aqui sin necesitar el campo. */
    int32 RootCellCount(float RootRadiusM, float CellSizeCm)
    {
        if (RootRadiusM <= 0.f || CellSizeCm <= 0.f) { return 1; }
        const int32 R = FMath::CeilToInt(RootRadiusM * 100.f / CellSizeCm);
        return (2 * R + 1) * (2 * R + 1);
    }
}

void EcoSpeciesAudit::RunAndLog()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S)
    {
        UE_LOG(LogEcoAudit, Warning, TEXT("[Auditoria] No hay UEcosystemSettings."));
        return;
    }

    // Resolucion de los assets. Se hace aqui y no a traves del subsistema para
    // que la auditoria funcione sin mundo ni PIE: es una herramienta de datos.
    TArray<const USpeciesData*> Sp;
    for (const TSoftObjectPtr<USpeciesData>& Soft : S->Species)
    {
        if (const USpeciesData* Loaded = Soft.LoadSynchronous())
        {
            Sp.Add(Loaded);
        }
    }

    if (Sp.Num() == 0)
    {
        UE_LOG(LogEcoAudit, Warning,
            TEXT("[Auditoria] No hay especies en Project Settings -> Procedural Ecosystem."));
        return;
    }

    const float KlMax = S->LightHalfSaturationMax;
    const float StressThreshold = S->StressVigorThreshold;
    const float FullSun = EcoGrid::FullSunlight;

    UE_LOG(LogEcoAudit, Log, TEXT("========================================================================"));
    UE_LOG(LogEcoAudit, Log, TEXT("[Auditoria] %d especies | KlMax=%.3f | umbral de estres=%.3f | celda=%.0f cm"),
        Sp.Num(), KlMax, StressThreshold, S->HeightfieldCellSizeCm);

    // ------------------------------------------------------------------
    // 1) Estres a pleno sol
    // ------------------------------------------------------------------
    UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
    UE_LOG(LogEcoAudit, Log, TEXT("1) LUZ  (fL a pleno sol debe superar el umbral de estres con margen)"));
    UE_LOG(LogEcoAudit, Log, TEXT("   %-18s %8s %8s %8s   %s"),
        TEXT("especie"), TEXT("ShadeTol"), TEXT("fL sol"), TEXT("fL sombra"), TEXT("veredicto"));

    bool bAnyCondemned = false;
    for (const USpeciesData* Species : Sp)
    {
        const float fLSun = EcoVigor::LightFactor(FullSun, Species->ShadeTolerance, KlMax);
        const float fLShade = EcoVigor::LightFactor(kDeepShadeQ * FullSun, Species->ShadeTolerance, KlMax);
        const bool bCondemned = (fLSun < StressThreshold);
        bAnyCondemned |= bCondemned;

        UE_LOG(LogEcoAudit, Log, TEXT("   %-18s %8.3f %8.3f %8.3f   %s"),
            *Species->SpeciesName.ToString(), Species->ShadeTolerance, fLSun, fLShade,
            bCondemned
            ? TEXT("*** CONDENADA: se estresa a pleno sol y sin vecinos")
            : (fLSun < StressThreshold * 1.3f ? TEXT("margen escaso") : TEXT("ok")));
    }
    if (bAnyCondemned)
    {
        // Kl < Q*(1-U)/U con Q = FullSunlight es la condicion para que fL supere
        // el umbral; despejando el KlMax que la cumple para TODAS las especies.
        float NeededKlMax = TNumericLimits<float>::Max();
        for (const USpeciesData* Species : Sp)
        {
            const float OneMinusTol = FMath::Max(1.f - Species->ShadeTolerance, KINDA_SMALL_NUMBER);
            const float MaxKl = FullSun * (1.f - StressThreshold) / FMath::Max(StressThreshold, KINDA_SMALL_NUMBER);
            NeededKlMax = FMath::Min(NeededKlMax, MaxKl / OneMinusTol);
        }
        UE_LOG(LogEcoAudit, Warning,
            TEXT("   -> Con estas tolerancias, LightHalfSaturationMax tendria que bajar de %.2f "
                "para que ninguna especie se estrese a pleno sol (ahora %.2f)."),
            NeededKlMax, KlMax);
    }

    // ------------------------------------------------------------------
    // 2) Sostenibilidad del pozo de agua y de nutrientes
    // ------------------------------------------------------------------
    UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
    UE_LOG(LogEcoAudit, Log, TEXT("2) POZOS  (demanda anual de un adulto frente a la recarga DONDE VIVE)"));
    UE_LOG(LogEcoAudit, Log, TEXT("   %-18s %6s %10s %10s %7s   %10s %10s %7s"),
        TEXT("especie"), TEXT("celdas"),
        TEXT("dem.agua"), TEXT("rec.agua"), TEXT("ratio"),
        TEXT("dem.nutr"), TEXT("rec.nutr"), TEXT("ratio"));

    for (const USpeciesData* Species : Sp)
    {
        const int32 Cells = RootCellCount(Species->RootRadius, S->HeightfieldCellSizeCm);

        // BASE LOCAL REPRESENTATIVA, no el maximo del campo.
        //
        // Esta cuenta usaba WaterOutputMax, o sea el valor de la celda MAS
        // humeda del mapa, y eso es un suministro que casi ningun arbol ve. Con
        // un campo muy sesgado -y el TWI lo esta siempre: su mediana suele caer
        // por debajo del 20% del rango- el error es de un orden de magnitud, y
        // hacia el lado peligroso: daba por sostenibles demandas que en realidad
        // vacian el pozo bajo cada adulto.
        //
        // Con la respuesta de nicho activa hay un valor mucho mejor y ya
        // disponible: el OPTIMO de la especie, que es justo la humedad del sitio
        // donde esa especie vive. Sin nicho no hay tal referencia y se cae al
        // comportamiento anterior.
        const float WaterBaseLocal = S->bUseNicheResponse
            ? Species->WaterOptimum * S->WaterOutputMax
            : S->WaterOutputMax;
        const float NutrientBaseLocal = S->bUseNicheResponse
            ? Species->NutrientOptimum * S->NutrientOutputMax
            : S->NutrientOutputMax;

        const float WaterDemandYear = Species->MaxBiomass * Species->WaterDemand;
        const float WaterSupplyYear = Cells * S->WaterRechargeRate * WaterBaseLocal;
        const float WaterRatio = WaterDemandYear / FMath::Max(WaterSupplyYear, KINDA_SMALL_NUMBER);

        const float NutrientDemandYear = Species->MaxBiomass * Species->NutrientDemand;
        const float NutrientSupplyYear = Cells * S->NutrientRechargeRate * NutrientBaseLocal;
        const float NutrientRatio = NutrientDemandYear / FMath::Max(NutrientSupplyYear, KINDA_SMALL_NUMBER);

        UE_LOG(LogEcoAudit, Log, TEXT("   %-18s %6d %10.1f %10.1f %6.2fx   %10.1f %10.1f %6.2fx  %s"),
            *Species->SpeciesName.ToString(), Cells,
            WaterDemandYear, WaterSupplyYear, WaterRatio,
            NutrientDemandYear, NutrientSupplyYear, NutrientRatio,
            (WaterRatio > 1.f || NutrientRatio > 1.f) ? TEXT("*** DEFICITARIA") : TEXT("ok"));
    }
    UE_LOG(LogEcoAudit, Log,
        TEXT("   (ratio > 1 = el adulto agota su propio pozo mas rapido de lo que se recarga."));
    UE_LOG(LogEcoAudit, Log,
        TEXT("    Apunta a < 0.4: el kernel de consumo carga mas la celda central que el borde,"));
    UE_LOG(LogEcoAudit, Log,
        TEXT("    asi que el reparto uniforme que supone esta cuenta es optimista.)"));

    // ------------------------------------------------------------------
    // 3) Solape raiz / copa
    // ------------------------------------------------------------------
    UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
    UE_LOG(LogEcoAudit, Log, TEXT("3) GEOMETRIA  (la raiz deberia competir en un radio comparable al de la copa)"));
    for (const USpeciesData* Species : Sp)
    {
        const float RootCm = Species->RootRadius * 100.f;
        const float CrownCm = S->CanopyRadiusFraction * Species->MaxHeightCm;
        UE_LOG(LogEcoAudit, Log, TEXT("   %-18s raiz %6.0f cm | copa %6.0f cm | raiz/copa %.2f  %s"),
            *Species->SpeciesName.ToString(), RootCm, CrownCm,
            CrownCm > 0.f ? RootCm / CrownCm : 0.f,
            (CrownCm > 0.f && RootCm < 0.5f * CrownCm)
            ? TEXT("<- la sombra se solapa y la raiz no: la luz manda")
            : TEXT(""));
    }

    // ------------------------------------------------------------------
    // 4) Dominancia entre especies
    // ------------------------------------------------------------------
    UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
    UE_LOG(LogEcoAudit, Log, TEXT("4) DOMINANCIA  (si A gana o empata a B en TODOS los ejes, B esta condenada)"));

    bool bAnyDominance = false;
    for (int32 a = 0; a < Sp.Num(); ++a)
    {
        for (int32 b = 0; b < Sp.Num(); ++b)
        {
            if (a == b) { continue; }

            bool bNeverWorse = true;
            bool bStrictlyBetterSomewhere = false;
            for (int32 k = 0; k < kNumAxes; ++k)
            {
                const float ScoreA = AxisScore(kAxes[k], *Sp[a]);
                const float ScoreB = AxisScore(kAxes[k], *Sp[b]);
                if (ScoreA < ScoreB) { bNeverWorse = false; break; }
                if (ScoreA > ScoreB) { bStrictlyBetterSomewhere = true; }
            }

            if (bNeverWorse && bStrictlyBetterSomewhere)
            {
                bAnyDominance = true;
                UE_LOG(LogEcoAudit, Warning,
                    TEXT("   *** %s DOMINA a %s: mejor o igual en los %d ejes. "
                        "%s se extinguira haga lo que haga el mapa."),
                    *Sp[a]->SpeciesName.ToString(), *Sp[b]->SpeciesName.ToString(),
                    kNumAxes, *Sp[b]->SpeciesName.ToString());
            }
        }
    }

    if (!bAnyDominance)
    {
        UE_LOG(LogEcoAudit, Log, TEXT("   Ninguna especie domina a otra en todos los ejes. Bien."));
    }

    // ------------------------------------------------------------------
    // 5) Separacion de nichos (solo si la respuesta unimodal esta activa)
    // ------------------------------------------------------------------
    // WaterOptimum y NutrientOptimum NO son ejes monotonos -no hay un "mejor
    // optimo"-, asi que quedan deliberadamente fuera de la prueba de dominancia.
    // Lo que hay que medir en ellos es otra cosa: si las campanas de dos especies
    // estan lo bastante SEPARADAS como para que cada una tenga su zona del mapa.
    if (S->bUseNicheResponse)
    {
        UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
        UE_LOG(LogEcoAudit, Log, TEXT("5) NICHOS  (separacion entre campanas; >1.0 = zonas ganadoras distintas)"));

        auto Separation = [](float OptA, float WidA, float OptB, float WidB)
            {
                // Distancia entre optimos medida en anchuras: es la forma estandar
                // de decir si dos campanas se distinguen o son la misma.
                return FMath::Abs(OptA - OptB) / FMath::Max(0.5f * (WidA + WidB), KINDA_SMALL_NUMBER);
            };

        for (int32 a = 0; a < Sp.Num(); ++a)
        {
            for (int32 b = a + 1; b < Sp.Num(); ++b)
            {
                const float SepWater = Separation(Sp[a]->WaterOptimum, Sp[a]->WaterTolerance,
                    Sp[b]->WaterOptimum, Sp[b]->WaterTolerance);
                const float SepNutrient = Separation(Sp[a]->NutrientOptimum, Sp[a]->NutrientTolerance,
                    Sp[b]->NutrientOptimum, Sp[b]->NutrientTolerance);
                const float Best = FMath::Max(SepWater, SepNutrient);

                UE_LOG(LogEcoAudit, Log, TEXT("   %-14s vs %-14s  agua %.2f | nutrientes %.2f  %s"),
                    *Sp[a]->SpeciesName.ToString(), *Sp[b]->SpeciesName.ToString(),
                    SepWater, SepNutrient,
                    (Best < 0.5f)
                    ? TEXT("*** practicamente el mismo nicho: separa los optimos o estrecha las campanas")
                    : (Best < 1.f ? TEXT("solape alto") : TEXT("ok")));
            }
        }
    }
    else
    {
        UE_LOG(LogEcoAudit, Log,
            TEXT("   (bUseNicheResponse=false: agua y nutrientes usan Monod monotona, "
                "asi que NO pueden repartir nicho por mucho que ajustes las demandas)"));
    }

    // Tabla completa de ejes, para ver de un vistazo donde estan los empates.
    UE_LOG(LogEcoAudit, Log, TEXT("   --- valores por eje (orientados a 'mas es mejor') ---"));
    {
        FString Header = FString::Printf(TEXT("   %-20s"), TEXT(""));
        for (const USpeciesData* Species : Sp)
        {
            Header += FString::Printf(TEXT(" %10s"), *Species->SpeciesName.ToString().Left(10));
        }
        UE_LOG(LogEcoAudit, Log, TEXT("%s"), *Header);
    }
    for (int32 k = 0; k < kNumAxes; ++k)
    {
        FString Row = FString::Printf(TEXT("   %-20s"), kAxes[k].Name);
        for (const USpeciesData* Species : Sp)
        {
            Row += FString::Printf(TEXT(" %10.3f"), kAxes[k].Get(*Species));
        }
        Row += kAxes[k].bHigherIsBetter ? TEXT("   (mas = mejor)") : TEXT("   (menos = mejor)");
        UE_LOG(LogEcoAudit, Log, TEXT("%s"), *Row);
    }

    UE_LOG(LogEcoAudit, Log, TEXT("========================================================================"));
}

static FAutoConsoleCommand GEcoAuditSpecies(
    TEXT("Eco.AuditarEspecies"),
    TEXT("Audita los assets de especie: estres a pleno sol, sostenibilidad de los pozos, "
        "solape raiz/copa y dominancia entre especies. No corre la simulacion."),
    FConsoleCommandDelegate::CreateStatic(&EcoSpeciesAudit::RunAndLog));
