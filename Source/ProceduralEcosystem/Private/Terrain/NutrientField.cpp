#include "Terrain/NutrientField.h"
#include "Terrain/EcoNoise.h"
#include "Core/EcoCore.h"
#include "Async/ParallelFor.h"

void FNutrientField::GeneratePatchyBase(int32 Width, int32 Height, double CellSize,
    const FVector2D& Origin, uint32 Seed,
    float OutputMax, double PatchFrequency, int32 Octaves)
{
    Field.Init(Width, Height, CellSize, Origin, 0.f);

    // Hash DISTINTO al que usa FHeightField::GenerateFractalNoise: aunque
    // compartan la misma MasterSeed del proyecto, el desplazamiento debe
    // salir distinto para que el patron de nutrientes no quede pegado al
    // relieve (no son la misma causa fisica).
    const double OffX = (EcoRand::Hash32(Seed ^ 0xA24BAED4u) & 0xFFFF) * 0.1;
    const double OffY = (EcoRand::Hash32(Seed ^ 0x5BD1E995u) & 0xFFFF) * 0.1;

    const int32 W = Field.Width;
    const int32 Ht = Field.Height;

    // 1) fBm crudo en paralelo (cada fila escribe celdas distintas -> determinista).
    TArray<float> Raw;
    Raw.SetNumUninitialized(W * Ht);

    ParallelFor(Ht, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                Raw[y * W + x] = EcoNoise::FractalPerlin(x, y, CellSize, OffX, OffY, PatchFrequency, Octaves);
            }
        });

    // 2) Normalizacion a [0, OutputMax]: mismo criterio que el agua, para que
    //    ambos campos entren en la formula de vigor (Monod, Fase 2) con rangos
    //    comparables sin reescalar ahi.
    Field.FillNormalizedFrom(Raw, OutputMax);
}