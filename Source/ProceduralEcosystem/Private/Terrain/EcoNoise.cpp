#include "Terrain/EcoNoise.h"
#include "Core/EcoCore.h"

namespace
{
    /** PerlinNoise2D de UE reescalado a [-1, 1] (su rango real es ~+-0.707). */
    FORCEINLINE float Noise11(const FVector2D& PosCm, const FVector2D& Offset, double Freq)
    {
        const FVector2D P(PosCm.X * Freq + Offset.X, PosCm.Y * Freq + Offset.Y);
        return FMath::PerlinNoise2D(P) / EcoNoise::PerlinAmplitude2D;
    }
}

FVector2D EcoNoise::SeedOffset(uint32 Seed, uint32 Salt)
{
    // La conversion hash -> offset esta en OffsetFromHash (copia unica: la
    // comparte el campo de nutrientes, que deriva sus dos sales por su cuenta).
    return FVector2D(
        OffsetFromHash(EcoRand::Hash32(Seed ^ Salt)),
        OffsetFromHash(EcoRand::Hash32(Seed ^ Salt ^ 0x9E3779B9u)));
}

int32 EcoNoise::ClampOctavesToNyquist(int32 Octaves,
    double BaseWavelengthCm, double Lacunarity, double CellSizeCm)
{
    const double MinWavelength = 2.0 * FMath::Max(CellSizeCm, 1.0);
    const double Lac = FMath::Max(Lacunarity, 1.01);

    int32 Usable = 0;
    double Wavelength = FMath::Max(BaseWavelengthCm, 1.0);
    while (Usable < Octaves && Wavelength >= MinWavelength)
    {
        ++Usable;
        Wavelength /= Lac;
    }
    return FMath::Max(Usable, 1);
}

float EcoNoise::Fbm(const FVector2D& PosCm, const FVector2D& Offset,
    double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves)
{
    double Freq = 1.0 / FMath::Max(BaseWavelengthCm, 1.0);
    double Amp = 1.0;
    double Sum = 0.0;
    double Norm = 0.0;

    for (int32 o = 0; o < Octaves; ++o)
    {
        Sum += Amp * Noise11(PosCm, Offset, Freq);
        Norm += Amp;
        Amp *= Persistence;
        Freq *= Lacunarity;
    }
    return static_cast<float>(Sum / FMath::Max(Norm, (double)KINDA_SMALL_NUMBER));
}

float EcoNoise::HybridMultifractal(const FVector2D& PosCm, const FVector2D& Offset,
    double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves)
{
    // "Offset" de Musgrave: desplaza el ruido a mayormente-positivo para que
    // la realimentacion (weight *= signal) tenga sentido como "altitud".
    constexpr double kOffset = 0.7;

    double Freq = 1.0 / FMath::Max(BaseWavelengthCm, 1.0);
    double Amp = 1.0;

    // Octava base.
    double Signal = (Noise11(PosCm, Offset, Freq) + kOffset) * Amp;
    double Result = Signal;
    double Weight = Signal;
    double Norm = (1.0 + kOffset) * Amp;

    for (int32 o = 1; o < Octaves; ++o)
    {
        Amp *= Persistence;
        Freq *= Lacunarity;

        // El peso es la senal acumulada saturada a 1: en zonas bajas tiende a 0
        // y apaga el detalle fino (valles lisos). A diferencia del Musgrave
        // literal, el peso se realimenta con el ruido SIN el factor de
        // amplitud: con persistencias bajas (0.5) el original atenua doble
        // (amplitud y peso) y mata todo el detalle medio.
        Weight = FMath::Clamp(Weight, 0.0, 1.0);
        const double N = Noise11(PosCm, Offset, Freq) + kOffset;
        Signal = N * Amp;
        Result += Weight * Signal;
        Weight *= N;

        Norm += (1.0 + kOffset) * Amp;
    }

    // Normaliza por el maximo teorico: el resultado queda aprox en [0, 1]
    // (puede asomar un poco fuera; FillNormalizedFrom lo reajusta despues).
    return static_cast<float>(Result / FMath::Max(Norm, (double)KINDA_SMALL_NUMBER));
}

float EcoNoise::RidgedMultifractal(const FVector2D& PosCm, const FVector2D& Offset,
    double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves)
{
    constexpr double kOffset = 1.0; // (1 - |n|)^2: crestas donde el ruido cruza 0
    constexpr double kGain = 2.0;   // realimentacion: crestas nitidas, bajos lisos

    double Freq = 1.0 / FMath::Max(BaseWavelengthCm, 1.0);
    double Amp = 1.0;

    double Signal = FMath::Square(kOffset - FMath::Abs(Noise11(PosCm, Offset, Freq)));
    double Result = Signal * Amp;
    double Norm = Amp;

    for (int32 o = 1; o < Octaves; ++o)
    {
        Amp *= Persistence;
        Freq *= Lacunarity;

        const double Weight = FMath::Clamp(Signal * kGain, 0.0, 1.0);
        Signal = FMath::Square(kOffset - FMath::Abs(Noise11(PosCm, Offset, Freq))) * Weight;
        Result += Signal * Amp;
        Norm += Amp;
    }
    return static_cast<float>(Result / FMath::Max(Norm, (double)KINDA_SMALL_NUMBER));
}

float EcoNoise::TerrainSample(double Xcm, double Ycm, const FTerrainNoiseParams& P)
{
    FVector2D Pos(Xcm, Ycm);

    // 1) Domain warp (Quilez): evalua el fractal en coordenadas distorsionadas
    //    por OTRO fBm. Rompe la uniformidad "de manchas" del Perlin puro y
    //    curva valles y laderas de forma organica.
    if (P.WarpStrengthCm > 0.0)
    {
        const float Wx = Fbm(Pos, P.WarpOffsetA, P.WarpWavelengthCm, 0.5, 2.0, P.WarpOctaves);
        const float Wy = Fbm(Pos, P.WarpOffsetB, P.WarpWavelengthCm, 0.5, 2.0, P.WarpOctaves);
        Pos.X += P.WarpStrengthCm * Wx;
        Pos.Y += P.WarpStrengthCm * Wy;
    }

    // 2) Base: hybrid multifractal (valles lisos, cumbres con detalle).
    const float Base = HybridMultifractal(Pos, P.BaseOffset,
        P.BaseWavelengthCm, P.Persistence, P.Lacunarity, P.Octaves);

    if (P.RidgeWeight <= KINDA_SMALL_NUMBER)
    {
        return Base;
    }

    // 3) Crestas: ridged multifractal mezclado SOLO en las zonas altas de la
    //    base (mascara suave por altitud): los valles conservan la forma
    //    suave y las cimas ganan aristas de cordillera.
    // La banda de la mascara esta calibrada sobre la distribucion REAL del
    // hybrid con los parametros por defecto (mediana ~0.27, p95 ~0.42): las
    // crestas entran desde la mitad alta del relieve y dominan en las cimas.
    const float Ridge = RidgedMultifractal(Pos, P.RidgeOffset,
        P.BaseWavelengthCm, P.Persistence, P.Lacunarity, P.Octaves);
    const float Mask = FMath::SmoothStep(0.27f, 0.42f, Base);
    return FMath::Lerp(Base, Ridge, P.RidgeWeight * Mask);
}
