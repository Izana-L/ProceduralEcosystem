#include "Ecology/Vigor.h"

#include "Terrain/Field2D.h"
#include "Terrain/HeightField.h"
#include "Terrain/WaterField.h"
#include "Terrain/NutrientField.h"
#include "Terrain/LightFieldCoarse.h"
#include "Species/SpeciesData.h"

#include "Async/ParallelFor.h"

namespace EcoVigor
{
    void BakeSuitabilityField(
        const FHeightField& Height,
        const FWaterField& Water,
        const FNutrientField& Nutrient,
        const FLightFieldCoarse& Light,
        const USpeciesData& Species,
        float KlMax,
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

        // Copia de valores por especie fuera del bucle (evita tocar el UObject
        // dentro de ParallelFor y ahorra indirecciones).
        const float ShadeTol = Species.ShadeTolerance;
        const float WaterDem = Species.WaterDemand;
        const float NutriDem = Species.NutrientDemand;

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

                    const float Wv = Water.SampleWater(Xcm, Ycm);
                    const float Nv = Nutrient.SampleNutrient(Xcm, Ycm);
                    const float Q = Light.IsValid()
                        ? Light.SampleLightSmooth(FVector(Xcm, Ycm, Zcm))
                        : FLightFieldCoarse::FullSunlight;

                    const float fL = LightFactor(Q, ShadeTol, KlMax);
                    const float fW = WaterFactor(Wv, WaterDem);
                    const float fN = NutrientFactor(Nv, NutriDem);

                    EEcoLimiter Lim;
                    float V = CombineWithLimiter(fL, fW, fN, Lim);

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
