/**
 * @file EcoNoise.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de la síntesis fractal de relieve del namespace EcoNoise.
 *
 * Define el envoltorio del ruido de gradiente de Unreal, la derivación de
 * offsets por semilla y sal, el recorte de octavas al límite de Nyquist y los
 * tres fractales que dan forma al terreno: fBm, hybrid multifractal y ridged
 * multifractal, más su composición con domain warp en TerrainSample. Las
 * constantes internas de los multifractales (offset y ganancia) viven aquí y no
 * se exponen: fijan el carácter del relieve y no son parámetros de usuario.
 *
 * @ingroup eco_terrain
 * @see @ref bib_musgrave1989
 * @see @ref bib_quilezdomainwarp
 */

#include "Terrain/EcoNoise.h"
#include "Core/EcoCore.h"

namespace
{
    /** PerlinNoise2D de UE llevado al rango [-1, 1] nominal; ver EcoNoise::PerlinAmplitude2D. */
    FORCEINLINE float Noise11(const FVector2D& PosCm, const FVector2D& Offset, double Freq)
    {
        const FVector2D P(PosCm.X * Freq + Offset.X, PosCm.Y * Freq + Offset.Y);
        return FMath::PerlinNoise2D(P) / EcoNoise::PerlinAmplitude2D;
    }
}

FVector2D EcoNoise::SeedOffset(uint32 Seed, uint32 Salt)
{
    // La segunda coordenada mezcla una sal secundaria fija para que X e Y no
    // queden correlacionadas al derivarse del mismo par (Seed, Salt).
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
    // Desplaza el ruido a mayormente positivo para que la señal acumulada, que
    // es la que realimenta el peso, funcione como un proxy de altitud.
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

        // El peso es la señal acumulada saturada a 1: en las zonas bajas tiende a
        // 0 y apaga el detalle fino (valles lisos). Se realimenta con el ruido
        // desplazado SIN el factor de amplitud, a diferencia de la formulación
        // canónica: con persistencias bajas (0.5) atenuar a la vez por amplitud y
        // por peso suprime el detalle de las octavas intermedias.
        Weight = FMath::Clamp(Weight, 0.0, 1.0);
        const double N = Noise11(PosCm, Offset, Freq) + kOffset;
        Signal = N * Amp;
        Result += Weight * Signal;
        Weight *= N;

        Norm += (1.0 + kOffset) * Amp;
    }

    // Normaliza por el máximo teórico de la suma, de modo que el resultado queda
    // aproximadamente en [0, 1]; puede asomar algo fuera y la normalización del
    // campo lo reajusta después.
    return static_cast<float>(Result / FMath::Max(Norm, (double)KINDA_SMALL_NUMBER));
}

float EcoNoise::RidgedMultifractal(const FVector2D& PosCm, const FVector2D& Offset,
    double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves)
{
    constexpr double kOffset = 1.0; // (1 - |n|)^2: cresta donde el ruido cruza cero
    constexpr double kGain = 2.0;   // realimentación: crestas nítidas, bajos lisos

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

    // 1) Domain warp: el fractal se evalúa en coordenadas desplazadas por OTRO
    //    fBm, con offsets propios y persistencia y lacunaridad fijas. Rompe la
    //    uniformidad de manchas del ruido puro y curva valles y laderas. La
    //    amplitud va en cm de mundo para que sea ajustable en unidades físicas.
    if (P.WarpStrengthCm > 0.0)
    {
        const float Wx = Fbm(Pos, P.WarpOffsetA, P.WarpWavelengthCm, 0.5, 2.0, P.WarpOctaves);
        const float Wy = Fbm(Pos, P.WarpOffsetB, P.WarpWavelengthCm, 0.5, 2.0, P.WarpOctaves);
        Pos.X += P.WarpStrengthCm * Wx;
        Pos.Y += P.WarpStrengthCm * Wy;
    }

    // 2) Base del relieve: hybrid multifractal (valles lisos, cumbres con detalle).
    const float Base = HybridMultifractal(Pos, P.BaseOffset,
        P.BaseWavelengthCm, P.Persistence, P.Lacunarity, P.Octaves);

    if (P.RidgeWeight <= KINDA_SMALL_NUMBER)
    {
        return Base;
    }

    // 3) Crestas: el ridged se mezcla solo en las zonas altas de la base, con una
    //    máscara suave de altitud; los valles conservan la forma redondeada y las
    //    cimas ganan aristas de cordillera. La banda [0.27, 0.42] corresponde a
    //    la mediana y el percentil 95 de la distribución del hybrid con los
    //    parámetros por defecto, de modo que las crestas entran en la mitad alta
    //    del relieve y dominan en las cumbres.
    const float Ridge = RidgedMultifractal(Pos, P.RidgeOffset,
        P.BaseWavelengthCm, P.Persistence, P.Lacunarity, P.Octaves);
    const float Mask = FMath::SmoothStep(0.27f, 0.42f, Base);
    return FMath::Lerp(Base, Ridge, P.RidgeWeight * Mask);
}
