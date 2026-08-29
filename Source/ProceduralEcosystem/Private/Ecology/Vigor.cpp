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
         * Anchura de la rama de EXCESO a partir de la de deficit y del flag de la
         * especie. Copia unica: agua y nutrientes tienen que decidirlo igual, y
         * hasta ahora cada Make*Response se limitaba a copiar su booleano.
         *
         * Con penalizacion: campana simetrica. Sin ella: rama derecha ANCHA en vez
         * de recortada, para que la respuesta siga siendo unimodal (ver
         * UEcosystemSettings::NicheExcessWidthScale). Escala 0 = saturar en 1, el
         * comportamiento anterior.
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

        // Misma geometria que el relieve: el TArray resultante encaja tal cual en
        // el UFieldVisualizer y en el resto de campos.
        OutSuitability.Init(Ref.Width, Ref.Height, Ref.CellSize, Ref.Origin, 0.f);
        if (OutLimiter)
        {
            OutLimiter->SetNumUninitialized(Ref.Width * Ref.Height);
        }

        const int32 W = Ref.Width;
        const int32 H = Ref.Height;

        // Las respuestas llegan ya construidas (POD por valor): ni una lectura del
        // UObject dentro del ParallelFor, y ademas garantiza que el heatmap evalua
        // literalmente la misma curva que el tick.
        const FSpeciesResponses Resp = Responses;

        // Fase 6: copia local de los parametros de CO2 (o desactivado). Se saca
        // del puntero fuera del ParallelFor por el mismo motivo.
        EcoCarbon::FCO2Params CO2Local;
        CO2Local.bEnabled = false;
        if (CO2) { CO2Local = *CO2; }

        // Una fila por tarea: cada fila escribe celdas disjuntas -> determinista y
        // seguro sin locks (mismo patron que FNutrientField / FWaterField).
        ParallelFor(H, [&](int32 y)
            {
                for (int32 x = 0; x < W; ++x)
                {
                    const int32 i = y * W + x;

                    // Nodo -> mundo (convencion de FField2D: el valor vive en el nodo).
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

                    // Fase 6: el heatmap tiene que representar EL MISMO numero que
                    // consume el tick, o dejaria de servir para explicar por que el
                    // bosque crece donde crece.
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
