#include "Debug/SpeciesAudit.h"

#include "Config/EcosystemSettings.h"
#include "Species/SpeciesData.h"
#include "Ecology/Vigor.h"
#include "Ecology/EcologyRules.h"
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
        { TEXT("RootRadius"),          true,  [](const USpeciesData& S) { return S.RootRadius; } },

        // ANCHURA DE CAMPANA COMO EJE MONOTONO. Es contraintuitivo pero se demuestra
        // en una linea: d/dW exp(-((R-Opt)/W)^2) > 0 para todo R distinto del optimo,
        // luego ensanchar la campana SUBE la respuesta en todas las celdas menos en
        // una. Una anchura mayor es ventaja gratuita, y el bloque 5 -que las mira
        // como "separacion de nichos"- no la ve como tal.
        { TEXT("WaterTolerance"),      true,  [](const USpeciesData& S) { return S.WaterTolerance; } },
        { TEXT("NutrientTolerance"),   true,  [](const USpeciesData& S) { return S.NutrientTolerance; } },

        // Un declive que llega mas tarde, frena menos, mata menos y semilla mas es
        // mejor en todos los casos.
        { TEXT("SenescenceAgeFrac"),   true,  [](const USpeciesData& S) { return S.SenescenceAgeFraction; } },
        { TEXT("SenescentGrowth"),     true,  [](const USpeciesData& S) { return S.SenescentGrowthScale; } },
        { TEXT("SenescentSeed"),       true,  [](const USpeciesData& S) { return S.SenescentSeedScale; } },
        { TEXT("SenescMortalidad"),    false, [](const USpeciesData& S) { return S.SenescentMortalityMultiplier; } },
        { TEXT("MortSuprimido"),       false, [](const USpeciesData& S) { return S.SuppressedMortalityPerYear; } },

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

    /**
     * Celdas del campo de recursos que RECIBEN algo del disco radicular de un adulto.
     *
     * Antes esto devolvia (2*ceil(R/celda)+1)^2, o sea el area de la CAJA recorrida,
     * y con ella el bloque 2 declaraba nueve celdas donde el kernel escribe una: el
     * peso lineal 1-d/R vale exactamente 0 en los cuatro vecinos ortogonales cuando
     * el radio no supera el tamano de celda. La comprobacion de sostenibilidad del
     * pozo salia asi nueve veces optimista y daba luz verde a demandas que vacian la
     * celda. Ahora se cuentan las celdas con peso > 0, replicando el mismo peso que
     * usa EcologyRules.
     */
    int32 RootCellsWithWeight(float RootRadiusCm, float CellSizeCm)
    {
        if (RootRadiusCm <= 0.f || CellSizeCm <= 0.f) { return 1; }

        const int32 R = FMath::CeilToInt(RootRadiusCm / CellSizeCm);
        int32 Count = 0;
        for (int32 dy = -R; dy <= R; ++dy)
        {
            for (int32 dx = -R; dx <= R; ++dx)
            {
                const float DistCm = FMath::Sqrt(float(dx * dx + dy * dy)) * CellSizeCm;
                if (1.f - DistCm / RootRadiusCm > 0.f) { ++Count; }
            }
        }
        return FMath::Max(Count, 1);
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
    // Rango de f_L a pleno sol ENTRE especies, y su recorrido dentro de una misma
    // especie entre pleno sol y penumbra. La comparacion entre ambos es la que dice
    // si la luz puede repartir territorio o solo produce un ranking global.
    float MinSunAcrossSpecies = TNumericLimits<float>::Max();
    float MaxSunAcrossSpecies = 0.f;
    float MaxSpatialSwing = 0.f;

    for (const USpeciesData* Species : Sp)
    {
        const EcoVigor::FLightResponse LightResp = EcoVigor::MakeLightResponse(*Species, *S);
        const float fLSun = EcoVigor::LightFactor(FullSun, LightResp);
        const float fLShade = EcoVigor::LightFactor(kDeepShadeQ * FullSun, LightResp);
        const bool bCondemned = (fLSun < StressThreshold);
        bAnyCondemned |= bCondemned;

        MinSunAcrossSpecies = FMath::Min(MinSunAcrossSpecies, fLSun);
        MaxSunAcrossSpecies = FMath::Max(MaxSunAcrossSpecies, fLSun);
        MaxSpatialSwing = FMath::Max(MaxSpatialSwing, fLSun - fLShade);

        UE_LOG(LogEcoAudit, Log, TEXT("   %-18s %8.3f %8.3f %8.3f   %s"),
            *Species->SpeciesName.ToString(), Species->ShadeTolerance, fLSun, fLShade,
            bCondemned
            ? TEXT("*** CONDENADA: se estresa a pleno sol y sin vecinos")
            : (fLSun < StressThreshold * 1.3f ? TEXT("margen escaso") : TEXT("ok")));
    }
    // EL AVISO QUE HABRIA CAZADO SOLO EL PROBLEMA DE FONDO. Si la diferencia de f_L
    // ENTRE especies supera al recorrido de f_L a lo largo del gradiente de luz, el
    // eje de luz no puede invertir el orden en ninguna celda: deja de ser un reparto
    // espacial y se convierte en un ranking global, y como es el que suele imponer
    // el minimo de Liebig, arrastra con el a los otros dos.
    const float SpeciesSpread = MaxSunAcrossSpecies - MinSunAcrossSpecies;
    UE_LOG(LogEcoAudit, Log,
        TEXT("   recorrido de fL con la luz (sol -> sombra): %.3f | diferencia ENTRE especies a pleno sol: %.3f"),
        MaxSpatialSwing, SpeciesSpread);
    if (SpeciesSpread > MaxSpatialSwing)
    {
        UE_LOG(LogEcoAudit, Warning,
            TEXT("   *** La ventaja de RASGO en luz (%.3f) supera a toda la variacion que puede producir la SOMBRA "
                "(%.3f): la especie mas tolerante gana en todas las celdas a la vez. Baja LightHalfSaturationMax, "
                "sube ShadeToleranceAssimilationCost, o comprueba que el dosel genera sombra util (Eco.Luz.Perfil)."),
            SpeciesSpread, MaxSpatialSwing);
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
        // Radio EFECTIVO de un adulto, con el minimo en celdas que aplica el tick.
        const float AdultRadiusCm = EcologyRules::EffectiveRootRadiusCm(
            Species->RootRadius, 1.f, 1.f, S->MinRootRadiusCells * S->HeightfieldCellSizeCm);
        const int32 Cells = RootCellsWithWeight(AdultRadiusCm, S->HeightfieldCellSizeCm);
        if (Cells <= 1)
        {
            UE_LOG(LogEcoAudit, Warning,
                TEXT("   *** %s: el kernel radicular de un adulto escribe UNA sola celda (radio efectivo %.0f cm, "
                    "celda %.0f cm). No compite por suelo con nadie: son pozos privados, no un bien comun. "
                    "Sube RootRadius o MinRootRadiusCells."),
                *Species->SpeciesName.ToString(), AdultRadiusCm, S->HeightfieldCellSizeCm);
        }

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
        const float RootCm = EcologyRules::EffectiveRootRadiusCm(
            Species->RootRadius, 1.f, 1.f, S->MinRootRadiusCells * S->HeightfieldCellSizeCm);
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

            // SIN break: hay que contar los ejes, no salir al primero perdido.
            //
            // La prueba de todo-o-nada solo cazaba la dominancia PERFECTA, asi que
            // imprimia "ninguna especie domina a otra" sobre un conjunto donde una
            // gana en once de doce ejes -que en la practica es lo mismo-. Contando,
            // la dominancia casi-total tambien salta.
            int32 Wins = 0, Ties = 0, Losses = 0;
            for (int32 k = 0; k < kNumAxes; ++k)
            {
                const float ScoreA = AxisScore(kAxes[k], *Sp[a]);
                const float ScoreB = AxisScore(kAxes[k], *Sp[b]);
                if (ScoreA > ScoreB) { ++Wins; }
                else if (ScoreA < ScoreB) { ++Losses; }
                else { ++Ties; }
            }

            if (Losses == 0 && Wins > 0)
            {
                bAnyDominance = true;
                UE_LOG(LogEcoAudit, Warning,
                    TEXT("   *** %s DOMINA a %s: mejor o igual en los %d ejes. "
                        "%s se extinguira haga lo que haga el mapa."),
                    *Sp[a]->SpeciesName.ToString(), *Sp[b]->SpeciesName.ToString(),
                    kNumAxes, *Sp[b]->SpeciesName.ToString());
            }
            else if (Losses <= FMath::Max(1, kNumAxes / 10))
            {
                bAnyDominance = true;
                UE_LOG(LogEcoAudit, Warning,
                    TEXT("   *** %s domina CASI del todo a %s (%d ganados, %d empates, %d perdidos de %d ejes): "
                        "revisa si lo que pierde le compensa de verdad."),
                    *Sp[a]->SpeciesName.ToString(), *Sp[b]->SpeciesName.ToString(),
                    Wins, Ties, Losses, kNumAxes);
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

                if (!Sp[a]->bNutrientExcessPenalty && !Sp[b]->bNutrientExcessPenalty
                    && S->NicheExcessWidthScale <= 0.f)
                {
                    UE_LOG(LogEcoAudit, Warning,
                        TEXT("   *** Ninguna de las dos penaliza el exceso de nutrientes y NicheExcessWidthScale=0: "
                            "por encima del optimo fN vale 1 para las dos, la campana deja de ser unimodal y la "
                            "separacion que sigue no significa nada en la mitad rica del mapa."));
                }

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
