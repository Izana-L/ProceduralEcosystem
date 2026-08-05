#include "Terrain/HeightField.h"
#include "Terrain/EcoNoise.h"
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
                Raw[y * W + x] = EcoNoise::FractalPerlin(x, y, InCellSize, OffX, OffY, BaseFrequency, Octaves);
            }
        });

    // -----------------------------------------------------------------
    // 2) Normaliza al rango REAL del ruido. Antes se usaba 0.5*perlin+0.5
    //    asumiendo rango [-1,1], pero el Perlin 2D de UE llega solo a ~+-0.707,
    //    asi que el relieve se quedaba comprimido en ~[0.15, 0.85]. Con el
    //    min/max real el terreno aprovecha toda la amplitud y HeightScaleCm
    //    pasa a ser la amplitud pico-valle.
    // -----------------------------------------------------------------
    Field.FillNormalizedFrom(Raw, static_cast<float>(HeightScaleCm));
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