#include "Terrain/HeightField.h"
#include "Core/EcoCore.h"

void FHeightField::GenerateFractalNoise(int32 InWidth, int32 InHeight, double InCellSize,
                                        uint32 Seed, int32 Octaves,
                                        double BaseFrequency, double HeightScaleCm)
{
    Width    = FMath::Max(2, InWidth);
    Height   = FMath::Max(2, InHeight);
    CellSize = InCellSize;
    Data.SetNumUninitialized(Width * Height);

    // Desplazamiento por semilla: cada Seed produce un relieve distinto.
    const double OffX = (EcoRand::Hash32(Seed) & 0xFFFF) * 0.1;
    const double OffY = (EcoRand::Hash32(Seed ^ 0x9E3779B9u) & 0xFFFF) * 0.1;

    for (int32 y = 0; y < Height; ++y)
    {
        for (int32 x = 0; x < Width; ++x)
        {
            double freq = BaseFrequency;
            double amp  = 1.0;
            double sum  = 0.0;
            double norm = 0.0;

            for (int32 o = 0; o < Octaves; ++o)
            {
                const FVector2D P(OffX + x * CellSize * freq, OffY + y * CellSize * freq);
                sum  += amp * FMath::PerlinNoise2D(P); // devuelve [-1, 1]
                norm += amp;
                amp  *= 0.5;
                freq *= 2.0;
            }

            const double n01 = 0.5 * (sum / FMath::Max(norm, KINDA_SMALL_NUMBER)) + 0.5; // -> [0, 1]
            Data[y * Width + x] = static_cast<float>(n01 * HeightScaleCm);
        }
    }
}

float FHeightField::SampleHeight(double Xcm, double Ycm) const
{
    if (!IsValid()) return 0.f;

    double gx, gy;
    WorldToGrid(Xcm, Ycm, gx, gy);

    const int32 x0 = FMath::FloorToInt(gx);
    const int32 y0 = FMath::FloorToInt(gy);
    const float fx = static_cast<float>(gx - x0);
    const float fy = static_cast<float>(gy - y0);

    const float h00 = At(x0,     y0);
    const float h10 = At(x0 + 1, y0);
    const float h01 = At(x0,     y0 + 1);
    const float h11 = At(x0 + 1, y0 + 1);

    const float hx0 = FMath::Lerp(h00, h10, fx);
    const float hx1 = FMath::Lerp(h01, h11, fx);
    return FMath::Lerp(hx0, hx1, fy);
}

FVector FHeightField::SampleNormal(double Xcm, double Ycm) const
{
    const double e  = CellSize;
    const float  hL = SampleHeight(Xcm - e, Ycm);
    const float  hR = SampleHeight(Xcm + e, Ycm);
    const float  hD = SampleHeight(Xcm, Ycm - e);
    const float  hU = SampleHeight(Xcm, Ycm + e);

    const float dzdx = (hR - hL) / static_cast<float>(2.0 * e);
    const float dzdy = (hU - hD) / static_cast<float>(2.0 * e);
    return FVector(-dzdx, -dzdy, 1.f).GetSafeNormal();
}

float FHeightField::SampleSlope(double Xcm, double Ycm) const
{
    const double e  = CellSize;
    const float  hL = SampleHeight(Xcm - e, Ycm);
    const float  hR = SampleHeight(Xcm + e, Ycm);
    const float  hD = SampleHeight(Xcm, Ycm - e);
    const float  hU = SampleHeight(Xcm, Ycm + e);

    const float dzdx = (hR - hL) / static_cast<float>(2.0 * e);
    const float dzdy = (hU - hD) / static_cast<float>(2.0 * e);
    return FMath::Atan(FMath::Sqrt(dzdx * dzdx + dzdy * dzdy)); // radianes
}

FBox2D FHeightField::GetWorldBounds() const
{
    const FVector2D Min = Origin;
    const FVector2D Max = Origin + FVector2D((Width - 1) * CellSize, (Height - 1) * CellSize);
    return FBox2D(Min, Max);
}
