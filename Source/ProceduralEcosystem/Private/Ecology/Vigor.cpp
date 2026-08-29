/**
 * @file Vigor.cpp
 * @author Juan Luque Roldán
 * @brief Construcción de las curvas de respuesta por especie y bake del campo de idoneidad.
 *
 * Contiene lo único de EcoVigor que no es inline. Los Make*Response traducen el asset
 * de especie a curvas evaluables: resuelven las fracciones de [0,1] que guarda el
 * USpeciesData contra los máximos de salida de los campos y aplican el coste de la
 * tolerancia a la sombra al techo de asimilación. BakeSuitabilityField recorre en
 * paralelo la rejilla del relieve —una fila por tarea, celdas de salida disjuntas, sin
 * locks y determinista— evaluando el vigor a ras de suelo. Aloja también
 * ResolveExcessWidth, copia única de la decisión sobre la rama de exceso de la campana
 * de nicho, que agua y nutrientes tienen que tomar igual.
 *
 * @ingroup eco_ecology
 * @see @ref bib_nichounimodal
 * @see @ref bib_toleranciasombra
 */

#include "Ecology/Vigor.h"

#include "Terrain/Field2D.h"
#include "Terrain/HeightField.h"
#include "Terrain/WaterField.h"
#include "Terrain/NutrientField.h"
#include "Terrain/LightFieldCoarse.h"
#include "Species/SpeciesData.h"
#include "Config/EcosystemSettings.h"

#include "Async/ParallelFor.h"

namespace EcoVigor
{
    namespace
    {
        /**
         * Anchura de la rama de exceso a partir de la de déficit y del flag de la
         * especie. Copia única: agua y nutrientes tienen que decidirlo igual.
         *
         * Con penalización del exceso, campana simétrica. Sin ella, rama derecha ancha
         * (UEcosystemSettings::NicheExcessWidthScale) para que la respuesta siga siendo
         * unimodal; con escala 0 la rama satura en 1 y la curva se vuelve monótona.
         */
        float ResolveExcessWidth(float DeficitWidthAbs, bool bPenalizeExcess, float ExcessScale)
        {
            if (bPenalizeExcess) { return DeficitWidthAbs; }
            return (ExcessScale > 0.f) ? DeficitWidthAbs * ExcessScale : 0.f;
        }
    }

    FResourceResponse MakeWaterResponse(const USpeciesData& Species, const UEcosystemSettings& Settings)
    {
        FResourceResponse R;
        R.Demand = Species.WaterDemand;
        R.OptimumAbs = Species.WaterOptimum * Settings.WaterOutputMax;
        R.WidthAbs = Species.WaterTolerance * Settings.WaterOutputMax;
        R.ExcessWidthAbs = ResolveExcessWidth(R.WidthAbs, Species.bWaterloggingPenalty, Settings.NicheExcessWidthScale);
        R.bUseNiche = Settings.bUseNicheResponse;
        return R;
    }

    FResourceResponse MakeNutrientResponse(const USpeciesData& Species, const UEcosystemSettings& Settings)
    {
        FResourceResponse R;
        R.Demand = Species.NutrientDemand;
        R.OptimumAbs = Species.NutrientOptimum * Settings.NutrientOutputMax;
        R.WidthAbs = Species.NutrientTolerance * Settings.NutrientOutputMax;
        R.ExcessWidthAbs = ResolveExcessWidth(R.WidthAbs, Species.bNutrientExcessPenalty, Settings.NicheExcessWidthScale);
        R.bUseNiche = Settings.bUseNicheResponse;
        return R;
    }

    FLightResponse MakeLightResponse(const USpeciesData& Species, const UEcosystemSettings& Settings)
    {
        FLightResponse R;
        R.KlMax = Settings.LightHalfSaturationMax;
        R.ShadeTolerance = Species.ShadeTolerance;
        R.MaxAssimilation = FMath::Max(
            1.f - Settings.ShadeToleranceAssimilationCost * Species.ShadeTolerance, KINDA_SMALL_NUMBER);
        return R;
    }

    FSpeciesResponses MakeSpeciesResponses(const USpeciesData& Species, const UEcosystemSettings& Settings)
    {
        FSpeciesResponses R;
        R.Light = MakeLightResponse(Species, Settings);
        R.Water = MakeWaterResponse(Species, Settings);
        R.Nutrient = MakeNutrientResponse(Species, Settings);
        return R;
    }

    void BakeSuitabilityField(
        const FHeightField& Height,
        const FField2D& Water,
        const FField2D& Nutrient,
        const FLightFieldCoarse& Light,
        const FSpeciesResponses& Responses,
        EEcoVigorCombine CombineMode,
        FField2D& OutSuitability,
        TArray<uint8>* OutLimiter,
        const EcoCarbon::FCO2Params* CO2)
    {
        const FField2D& Ref = Height.Field;
        if (!Ref.IsValid())
        {
            OutSuitability = FField2D();
            if (OutLimiter) { OutLimiter->Reset(); }
            return;
        }

        // Misma geometría que el relieve: el TArray resultante encaja tal cual en el
        // UFieldVisualizer y junto al resto de campos.
        OutSuitability.Init(Ref.Width, Ref.Height, Ref.CellSize, Ref.Origin, 0.f);
        if (OutLimiter)
        {
            OutLimiter->SetNumUninitialized(Ref.Width * Ref.Height);
        }

        const int32 W = Ref.Width;
        const int32 H = Ref.Height;

        // Las respuestas llegan ya construidas (POD por valor): ni una lectura de UObject
        // dentro del ParallelFor, y el heatmap evalúa la misma curva que el tick.
        const FSpeciesResponses Resp = Responses;

        // Copia local de los parámetros de CO2, o desactivado si no se pasan: el puntero
        // se resuelve fuera del ParallelFor por el mismo motivo.
        EcoCarbon::FCO2Params CO2Local;
        CO2Local.bEnabled = false;
        if (CO2) { CO2Local = *CO2; }

        // Una fila por tarea: cada fila escribe celdas disjuntas, así que es seguro sin
        // locks y determinista (mismo patrón que FNutrientField y FWaterField).
        ParallelFor(H, [&](int32 y)
            {
                for (int32 x = 0; x < W; ++x)
                {
                    const int32 i = y * W + x;

                    // Nodo a mundo (convención de FField2D: el valor vive en el nodo).
                    const double Xcm = Ref.NodeWorldX(x);
                    const double Ycm = Ref.NodeWorldY(y);
                    const double Zcm = Height.SampleHeight(Xcm, Ycm); // luz a ras de suelo

                    const float Wv = Water.SampleBilinear(Xcm, Ycm);
                    const float Nv = Nutrient.SampleBilinear(Xcm, Ycm);
                    const float Q = Light.IsValid()
                        ? Light.SampleLightSmooth(FVector(Xcm, Ycm, Zcm))
                        : FLightFieldCoarse::FullSunlight;

                    EEcoLimiter Lim;
                    float V = EvaluateVigor(Q, Wv, Nv, Resp, CombineMode, Lim);

                    // El heatmap tiene que representar el mismo número que consume el
                    // tick, o deja de explicar por qué el bosque crece donde crece.
                    V *= EcoCarbon::CO2Factor(Q, /*CanopyHeightCm*/ 0.f, CO2Local);

                    OutSuitability.Data[i] = V;
                    if (OutLimiter)
                    {
                        (*OutLimiter)[i] = static_cast<uint8>(Lim);
                    }
                }
            });
    }
}
