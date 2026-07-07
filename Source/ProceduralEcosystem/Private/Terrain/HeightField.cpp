#include "Terrain/HeightField.h"
#include "Core/EcoCore.h"
#include "Async/ParallelFor.h"

void FHeightField::GenerateFractalNoise(int32 InWidth, int32 InHeight, double InCellSize,
    uint32 Seed, int32 Octaves,
    double BaseFrequency, double HeightScaleCm)
{
    // Prepara la rejilla base (geometria + almacenamiento); el valor inicial
    // da igual, se sobrescribe entero a continuacion.
    Field.Init(InWidth, InHeight, InCellSize, FVector2D::ZeroVector, 0.f);

    // Desplazamiento por semilla: cada Seed produce un relieve distinto.
    const double OffX = (EcoRand::Hash32(Seed) & 0xFFFF) * 0.1;
    const double OffY = (EcoRand::Hash32(Seed ^ 0x9E3779B9u) & 0xFFFF) * 0.1;

    const int32 W = Field.Width;
    const int32 Ht = Field.Height;

    // -----------------------------------------------------------------
    // 1) fBm crudo, en paralelo por filas. Cada fila escribe celdas
    //    distintas y PerlinNoise2D es puro -> el resultado NO depende del
    //    orden de los hilos (determinista).
    // -----------------------------------------------------------------
    TArray<float> Raw;
    Raw.SetNumUninitialized(W * Ht);

    ParallelFor(Ht, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                double freq = BaseFrequency;
                double amp = 1.0;
                double sum = 0.0;
                double norm = 0.0;

                for (int32 o = 0; o < Octaves; ++o)
                {
                    const FVector2D P(OffX + x * InCellSize * freq, OffY + y * InCellSize * freq);
                    sum += amp * FMath::PerlinNoise2D(P); // devuelve ~[-0.707, 0.707]
                    norm += amp;
                    amp *= 0.5;
                    freq *= 2.0;
                }

                Raw[y * W + x] = static_cast<float>(sum / FMath::Max(norm, KINDA_SMALL_NUMBER));
            }
        });

    // -----------------------------------------------------------------
    // 2) min/max real. Antes se usaba 0.5*perlin+0.5 asumiendo rango
    //    [-1,1], pero el Perlin 2D de UE llega solo a ~+-0.707, asi que el
    //    relieve se quedaba comprimido en ~[0.15, 0.85] y nunca alcanzaba
    //    0 ni el maximo. Normalizando por el rango REAL el terreno aprovecha
    //    toda la amplitud y HeightScaleCm pasa a ser la amplitud pico-valle.
    //    (min/max es exacto en paralelo, pero un barrido serial O(N) basta.)
    // -----------------------------------------------------------------
    float RawMin = TNumericLimits<float>::Max();
    float RawMax = -TNumericLimits<float>::Max();
    for (const float V : Raw)
    {
        RawMin = FMath::Min(RawMin, V);
        RawMax = FMath::Max(RawMax, V);
    }
    const float Range = FMath::Max(RawMax - RawMin, KINDA_SMALL_NUMBER);
    const float ScaleCm = static_cast<float>(HeightScaleCm);

    // 3) Normaliza a [0, HeightScaleCm], de nuevo en paralelo.
    ParallelFor(Ht, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                const int32 i = y * W + x;
                const float t = (Raw[i] - RawMin) / Range; // [0, 1]
                Field.Data[i] = t * ScaleCm;
            }
        });
}

float FHeightField::SampleSlope(double Xcm, double Ycm) const
{
    const double e = Field.CellSize;
    const float  hL = SampleHeight(Xcm - e, Ycm);
    const float  hR = SampleHeight(Xcm + e, Ycm);
    const float  hD = SampleHeight(Xcm, Ycm - e);
    const float  hU = SampleHeight(Xcm, Ycm + e);

    const float dzdx = (hR - hL) / static_cast<float>(2.0 * e);
    const float dzdy = (hU - hD) / static_cast<float>(2.0 * e);
    return FMath::Atan(FMath::Sqrt(dzdx * dzdx + dzdy * dzdy)); // radianes
}

FVector FHeightField::SampleNormal(double Xcm, double Ycm) const
{
    const double e = Field.CellSize;
    const float  hL = SampleHeight(Xcm - e, Ycm);
    const float  hR = SampleHeight(Xcm + e, Ycm);
    const float  hD = SampleHeight(Xcm, Ycm - e);
    const float  hU = SampleHeight(Xcm, Ycm + e);

    const float dzdx = (hR - hL) / static_cast<float>(2.0 * e);
    const float dzdy = (hU - hD) / static_cast<float>(2.0 * e);
    return FVector(-dzdx, -dzdy, 1.f).GetSafeNormal();
}