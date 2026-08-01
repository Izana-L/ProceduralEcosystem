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
    float VigorAtWorld(
        const FVector& WorldPos,
        const USpeciesData& Species,
        const FWaterField& Water,
        const FNutrientField& Nutrient,
        const FLightFieldCoarse& Light,
        float KlMax,
        EEcoLimiter* OutLimiter,
        const EcoCarbon::FCO2Params* CO2)
    {
        // Agua y nutrientes son campos 2D: se muestrean por XY (la Z no cuenta).
        const float W = Water.SampleWater(WorldPos.X, WorldPos.Y);
        const float N = Nutrient.SampleNutrient(WorldPos.X, WorldPos.Y);

        // La luz es 3D: usa la Z de WorldPos. SampleLightSmooth (trilineal) evita
        // los bloques del vecino-mas-cercano; el diseno lo recomienda justo para
        // la funcion de vigor. Si la rejilla de luz no es valida, asume cielo
        // despejado (Q=1): asi el heatmap funciona aunque aun no haya arboles.
        const float Q = Light.IsValid() ? Light.SampleLightSmooth(WorldPos) : FLightFieldCoarse::FullSunlight;

        const float fL = LightFactor(Q, Species.ShadeTolerance, KlMax);
        const float fW = WaterFactor(W, Species.WaterDemand);
        const float fN = NutrientFactor(N, Species.NutrientDemand);

        float V;
        if (OutLimiter)
        {
            V = CombineWithLimiter(fL, fW, fN, *OutLimiter);
        }
        else
        {
            V = Combine(fL, fW, fN);
        }

        // Fase 6 (doc. 6.3): CO2 como MULTIPLICADOR, no como cuarto termino del
        // min(). El limitante de Liebig que se devuelve en OutLimiter sigue siendo
        // el de los tres recursos de verdad -el CO2 aqui no se simula como recurso
        // consumible, solo modula la eficiencia-, asi que el mapa cualitativo de
        // limitantes no cambia de significado.
        if (CO2)
        {
            // Altura de copa 0: la idoneidad se evalua a ras de suelo, que es donde
            // cae una semilla.
            V *= EcoCarbon::CO2Factor(Q, /*CanopyHeightCm*/ 0.f, *CO2);
        }
        return V;
    }

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
                    const double Xcm = Ref.Origin.X + x * Ref.CellSize;
                    const double Ycm = Ref.Origin.Y + y * Ref.CellSize;
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
