/**
 * @file SpeciesAudit.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de la auditoría de especies: los cinco bloques del informe, la
 *        tabla de ejes y el comando de consola que la lanza.
 *
 * Contiene la lista de ejes monótonos del modelo con su orientación, el cálculo del soporte
 * real del kernel radicular y los cinco bloques que se vuelcan al log: estrés a pleno sol
 * con el despeje de la semisaturación que salvaría a todas las especies, balance de los
 * pozos de agua y nutrientes, solape entre raíz y copa, dominancia de Pareto por pares y
 * separación entre nichos. Evalúa las mismas funciones que el tick —@ref EcoVigor y
 * @ref EcologyRules— para que el informe no pueda divergir de la simulación.
 *
 * @ingroup eco_debug
 * @see @ref bib_pareto1896
 * @see @ref bib_tilman1982
 * @see @ref bib_macarthurlevins1967
 */

#include "Debug/SpeciesAudit.h"

#include "Config/EcosystemSettings.h"
#include "Species/SpeciesData.h"
#include "Ecology/Vigor.h"
#include "Ecology/EcologyRules.h"
#include "Core/GridMath.h"

#include "HAL/IConsoleManager.h"

// Categoría propia: LogEco es estático de la unidad de traducción del subsistema y no se
// puede compartir entre ficheros.
DEFINE_LOG_CATEGORY_STATIC(LogEcoAudit, Log, All);

namespace
{
    /**
     * Un eje monótono del modelo, con la orientación necesaria para normalizarlo a «más es
     * mejor». Es la unidad de la prueba de dominancia: si una especie gana o empata en todos
     * los ejes, gana la simulación.
     *
     * @note `bHigherIsBetter = false` marca los ejes donde la ventaja es el número pequeño
     *       —demandas de recurso, edad de madurez, mortalidades—, que se invierten al
     *       puntuar para que la comparación sea siempre «mayor gana».
     */
    struct FAxis
    {
        const TCHAR* Name;                   ///< Etiqueta con la que aparece en el informe.
        bool bHigherIsBetter;                ///< Sentido del rasgo antes de normalizar.
        float (*Get)(const USpeciesData&);   ///< Lector del rasgo en el asset de especie.
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

        // La anchura de la campana es un eje monótono, aunque parezca un rasgo de nicho:
        // d/dW exp(-((R-Opt)/W)^2) > 0 para todo R distinto del óptimo, luego ensancharla
        // sube la respuesta en todas las celdas menos en una. Es ventaja gratuita, y el
        // bloque de separación de nichos, que las mira como anchuras, no la detecta.
        { TEXT("WaterTolerance"),      true,  [](const USpeciesData& S) { return S.WaterTolerance; } },
        { TEXT("NutrientTolerance"),   true,  [](const USpeciesData& S) { return S.NutrientTolerance; } },

        // Un declive senescente que llega más tarde, frena menos, mata menos y da más
        // semilla es mejor en todos los casos.
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

    /** Valor del rasgo ya orientado a «más es mejor», listo para comparar entre especies. */
    float AxisScore(const FAxis& Axis, const USpeciesData& Species)
    {
        const float Raw = Axis.Get(Species);
        return Axis.bHigherIsBetter ? Raw : -Raw;
    }

    /**
     * Celdas del campo de recursos que reciben una cantidad no nula del disco radicular de
     * un adulto: el soporte real del kernel de depósito.
     *
     * Recorre la caja envolvente pero cuenta solo donde el peso lineal @f$1 - d/R@f$ es
     * positivo, replicando el que aplica `EcologyRules`. El área de la caja sobreestima el
     * soporte hasta nueve veces cuando el radio no supera el tamaño de celda —el peso vale
     * cero en los cuatro vecinos ortogonales—, y con ella el balance del pozo daría por
     * sostenibles demandas que en realidad lo vacían.
     *
     * @return Número de celdas con peso, nunca menor que 1.
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

    // Los assets se resuelven aquí y no a través del subsistema: así la auditoría corre
    // sin mundo, como la herramienta de datos que es.
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

    // Luz relativa bajo dosel cerrado, punto de lectura de la diferenciación en sombra,
    // que es donde la tolerancia debería decidir algo. Solo se usa para informar: no entra
    // en la simulación. Es local a la función y no de ámbito de fichero: con la compilación
    // unity varios .cpp del módulo comparten unidad de traducción, y un nombre de ámbito de
    // fichero ocultaría a los homónimos locales de los demás (C4459, que UBT trata como error).
    constexpr float kDeepShadeQ = 0.20f;

    UE_LOG(LogEcoAudit, Log, TEXT("========================================================================"));
    UE_LOG(LogEcoAudit, Log, TEXT("[Auditoria] %d especies | KlMax=%.3f | umbral de estres=%.3f | celda=%.0f cm"),
        Sp.Num(), KlMax, StressThreshold, S->HeightfieldCellSizeCm);

    // ------------------------------------------------------------------
    // 1) Estrés a pleno sol y señal espacial del eje de luz
    // ------------------------------------------------------------------
    UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
    UE_LOG(LogEcoAudit, Log, TEXT("1) LUZ  (fL a pleno sol debe superar el umbral de estres con margen)"));
    UE_LOG(LogEcoAudit, Log, TEXT("   %-18s %8s %8s %8s   %s"),
        TEXT("especie"), TEXT("ShadeTol"), TEXT("fL sol"), TEXT("fL sombra"), TEXT("veredicto"));

    bool bAnyCondemned = false;
    // Dos magnitudes que luego se comparan: cuánto separa el rasgo a las especies a pleno
    // sol, y cuánto puede moverse una misma especie entre pleno sol y penumbra. De su
    // comparación depende que la luz reparta territorio o solo produzca un ranking global.
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
    // Criterio de cruce de curvas expresado como comparación de rangos: si la diferencia
    // de fL entre especies supera al recorrido de fL a lo largo del gradiente de luz, el
    // orden entre especies no se invierte en ninguna celda. La luz deja de repartir
    // territorio y, al ser normalmente la limitante, arrastra con ella a los otros dos ejes.
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
        // Kl < Q*(1-U)/U, con Q la luz plena y U el umbral, es la condición para que fL lo
        // supere; se despeja el KlMax que la cumple para todas las especies a la vez.
        // El despeje ignora el techo de asimilación, así que el valor sugerido es optimista
        // cuando la tolerancia a la sombra tiene coste.
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
    // 2) Sostenibilidad de los pozos de agua y de nutrientes
    // ------------------------------------------------------------------
    UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
    UE_LOG(LogEcoAudit, Log, TEXT("2) POZOS  (demanda anual de un adulto frente a la recarga DONDE VIVE)"));
    UE_LOG(LogEcoAudit, Log, TEXT("   %-18s %6s %10s %10s %7s   %10s %10s %7s"),
        TEXT("especie"), TEXT("celdas"),
        TEXT("dem.agua"), TEXT("rec.agua"), TEXT("ratio"),
        TEXT("dem.nutr"), TEXT("rec.nutr"), TEXT("ratio"));

    for (const USpeciesData* Species : Sp)
    {
        // Radio radicular efectivo de un adulto, con el mínimo en celdas que aplica el tick.
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

        // La referencia es la base local representativa, no el máximo del campo: el máximo
        // es el valor de la celda más húmeda o más fértil del mapa, un suministro que casi
        // ningún árbol ve. Con un campo tan sesgado como el TWI -su mediana suele quedar por
        // debajo del 20% del rango- el error llega a un orden de magnitud, y hacia el lado
        // peligroso. Con la respuesta de nicho activa, el óptimo de la especie es justo la
        // humedad o la fertilidad del sitio donde esa especie vive; sin ella no existe tal
        // referencia y solo queda el máximo.
        const float WaterBaseLocal = S->bUseNicheResponse
            ? Species->WaterOptimum * S->WaterOutputMax
            : S->WaterOutputMax;
        const float NutrientBaseLocal = S->bUseNicheResponse
            ? Species->NutrientOptimum * S->NutrientOutputMax
            : S->NutrientOutputMax;

        // El suministro es la cota superior de la recarga, tasa por base, que solo se
        // alcanza con la celda a cero; la demanda tiene la misma forma que en el tick.
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
    // 3) Solape raíz / copa
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

            // Se cuentan todos los ejes, sin salir al primero perdido: la prueba de
            // todo-o-nada solo vería la dominancia estricta y daría por bueno un conjunto
            // donde una especie gana en todos menos uno, que en la práctica es lo mismo.
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
    // 5) Separación de nichos (solo con la respuesta unimodal activa)
    // ------------------------------------------------------------------
    // Los óptimos de agua y de nutrientes quedan deliberadamente fuera de la prueba de
    // dominancia porque no son ejes monótonos: no existe un «mejor óptimo». Lo que hay que
    // medir en ellos es otra cosa, si las campanas de dos especies están lo bastante
    // separadas como para que cada una tenga su zona ganadora del mapa.
    if (S->bUseNicheResponse)
    {
        UE_LOG(LogEcoAudit, Log, TEXT("------------------------------------------------------------------------"));
        UE_LOG(LogEcoAudit, Log, TEXT("5) NICHOS  (separacion entre campanas; >1.0 = zonas ganadoras distintas)"));

        auto Separation = [](float OptA, float WidA, float OptB, float WidB)
            {
                // Distancia entre óptimos medida en anchuras de campana: la medida canónica
                // de diferenciación de nicho. Se evalúa sobre las fracciones del asset, y
                // el cociente es invariante de escala, así que da igual la unidad de campo.
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

                // Fallo de forma previo a la separación: sin penalización por exceso la
                // campana deja de ser unimodal y la mitad rica del gradiente se aplana, con
                // lo que la separación no significa nada ahí. La comprobación cubre el eje
                // de nutrientes; el mismo razonamiento vale para el de agua.
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

    // Tabla completa de ejes, para ver de un vistazo dónde están los empates y qué rasgo
    // sostiene cada dominancia de las anteriores.
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

// ---------------------------------------------------------------------------
//  Comando de consola
// ---------------------------------------------------------------------------
// Sin mundo asociado: la auditoría no lo necesita y así también corre fuera del juego.
static FAutoConsoleCommand GEcoAuditSpecies(
    TEXT("Eco.AuditarEspecies"),
    TEXT("Audita los assets de especie: estres a pleno sol, sostenibilidad de los pozos, "
        "solape raiz/copa y dominancia entre especies. No corre la simulacion."),
    FConsoleCommandDelegate::CreateStatic(&EcoSpeciesAudit::RunAndLog));
