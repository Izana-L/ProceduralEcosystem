#include "Terrain/NutrientField.h"
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
                double freq = PatchFrequency;
                double amp = 1.0;
                double sum = 0.0;
                double norm = 0.0;

                for (int32 o = 0; o < Octaves; ++o)
                {
                    const FVector2D P(OffX + x * CellSize * freq, OffY + y * CellSize * freq);
                    sum += amp * FMath::PerlinNoise2D(P); // [-0.707, 0.707]
                    norm += amp;
                    amp *= 0.5;
                    freq *= 2.0;
                }

                Raw[y * W + x] = static_cast<float>(sum / FMath::Max(norm, KINDA_SMALL_NUMBER));
            }
        });

    // 2) min/max real (barrido serial O(N)).
    float RawMin = TNumericLimits<float>::Max();
    float RawMax = -TNumericLimits<float>::Max();
    for (const float V : Raw)
    {
        RawMin = FMath::Min(RawMin, V);
        RawMax = FMath::Max(RawMax, V);
    }

    // 3) Normalizacion a [0, OutputMax]: mismo criterio que el agua, para que
    //    ambos campos entren en la formula de vigor (Monod, Fase 2) con rangos
    //    comparables sin reescalar ahi.
    const float Range = FMath::Max(RawMax - RawMin, KINDA_SMALL_NUMBER);
    ParallelFor(Ht, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                const int32 i = y * W + x;
                const float t = (Raw[i] - RawMin) / Range; // [0, 1]
                Field.Data[i] = t * OutputMax;
            }
        });
}