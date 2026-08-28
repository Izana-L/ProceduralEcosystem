#pragma once

#include "CoreMinimal.h"

/**
 * Ruido procedural compartido.
 *
 * - FractalPerlin: el bucle de fBm por octavas original. Lo sigue usando
 *   FNutrientField::GeneratePatchyBase (parches de nutrientes).
 * - El resto es la sintesis de RELIEVE realista que usa FHeightField::Generate:
 *   fBm reparametrizado (longitud de onda / persistencia / lacunaridad),
 *   multifractales de Musgrave (hybrid y ridged) y domain warp de Quilez.
 *
 *   Referencias:
 *   - Musgrave, Kolb & Mace, "The Synthesis and Rendering of Eroded Fractal
 *     Terrains" (SIGGRAPH 1989) y Musgrave, "Methods for Realistic Landscape
 *     Imaging" (1993): hybrid/ridged multifractal.
 *   - I. Quilez, "Domain warping" (iquilezles.org): distorsion del dominio.
 */
namespace EcoNoise
{
    /** Amplitud REAL del PerlinNoise2D de UE: no llega a +-1, se queda en
        ~+-sqrt(2)/2. Se divide por esto para trabajar en [-1, 1]. */
    inline constexpr float PerlinAmplitude2D = 0.70710678f;

    /**
     * Una muestra de fBm (fractal Brownian motion) sobre PerlinNoise2D.
     *
     * @param X, Y           Indices de celda de la rejilla.
     * @param CellSize       cm por celda (convierte indice -> distancia de mundo).
     * @param OffX, OffY     Desplazamiento por semilla (descorrelaciona campos).
     * @param BaseFrequency  Frecuencia de la primera octava.
     * @param Octaves        Nº de octavas (amp *= 0.5, freq *= 2 por octava).
     * @return               Valor normalizado por la suma de amplitudes
     *                       (~[-0.707, 0.707]: el Perlin 2D de UE no llega a +-1).
     */
    FORCEINLINE float FractalPerlin(int32 X, int32 Y, double CellSize,
        double OffX, double OffY, double BaseFrequency, int32 Octaves)
    {
        double freq = BaseFrequency;
        double amp = 1.0;
        double sum = 0.0;
        double norm = 0.0;

        for (int32 o = 0; o < Octaves; ++o)
        {
            const FVector2D P(OffX + X * CellSize * freq, OffY + Y * CellSize * freq);
            sum += amp * FMath::PerlinNoise2D(P);
            norm += amp;
            amp *= 0.5;
            freq *= 2.0;
        }

        return static_cast<float>(sum / FMath::Max(norm, KINDA_SMALL_NUMBER));
    }

    // ------------------------------------------------------------------
    // Sintesis de relieve (Fase "relieve realista")
    // ------------------------------------------------------------------

    /**
     * Parametros de la forma del relieve. Se expresan en LONGITUD DE ONDA (cm),
     * no en frecuencia: "el ancho de las formas" es tuneable a ojo; una
     * frecuencia en cm^-1 no. Los offsets son desplazamientos en el espacio del
     * ruido derivados de la semilla (streams independientes por componente:
     * base, crestas y las dos coordenadas del warp).
     */
    struct FTerrainNoiseParams
    {
        double BaseWavelengthCm = 70000.0; // ancho de las formas grandes (700 m)
        double Persistence = 0.5;          // amplitud conservada por octava
        double Lacunarity = 2.0;           // multiplicador de frecuencia por octava
        int32  Octaves = 8;

        double WarpStrengthCm = 15000.0;   // amplitud del domain warp (0 = off)
        double WarpWavelengthCm = 40000.0; // ancho de las formas del warp
        int32  WarpOctaves = 3;

        float  RidgeWeight = 0.45f;        // 0 = colinas suaves, 1 = crestas

        FVector2D BaseOffset = FVector2D::ZeroVector;
        FVector2D RidgeOffset = FVector2D::ZeroVector;
        FVector2D WarpOffsetA = FVector2D::ZeroVector;
        FVector2D WarpOffsetB = FVector2D::ZeroVector;
    };

    /**
     * Hash de 32 bits -> desplazamiento en el espacio del ruido.
     *
     * 16 bits * 0.1 mantiene el offset en [0, 6553.6]: lejos del origen (para
     * descorrelacionar campos) pero sin degradar la precision del double dentro
     * del PerlinNoise2D. La constante estaba escrita dos veces -aqui y a mano en
     * FNutrientField::GeneratePatchyBase-, asi que tocar la escala en un sitio
     * dejaba los dos campos en espacios de ruido distintos sin avisar.
     */
    FORCEINLINE double OffsetFromHash(uint32 Hashed)
    {
        return (Hashed & 0xFFFF) * 0.1;
    }

    /** Desplazamiento en el espacio del ruido derivado de (Seed, Salt): cada
        Salt abre un stream descorrelacionado. Misma convencion que usaba
        FHeightField (Hash32 truncado a 16 bits, escala 0.1). */
    PROCEDURALECOSYSTEM_API FVector2D SeedOffset(uint32 Seed, uint32 Salt);

    /**
     * Recorta las octavas que caen bajo el limite de Nyquist de la rejilla:
     * una octava con longitud de onda < 2*CellSize no se puede representar y
     * solo mete aliasing (los "pinchos" por vertice del relieve antiguo).
     * Devuelve al menos 1.
     */
    PROCEDURALECOSYSTEM_API int32 ClampOctavesToNyquist(int32 Octaves,
        double BaseWavelengthCm, double Lacunarity, double CellSizeCm);

    /** fBm clasico en espacio de mundo (cm), normalizado a ~[-1, 1]. */
    PROCEDURALECOSYSTEM_API float Fbm(const FVector2D& PosCm, const FVector2D& Offset,
        double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves);

    /**
     * Hybrid multifractal de Musgrave: el peso de cada octava se realimenta con
     * la senal acumulada, asi que las zonas BAJAS pierden el detalle fino
     * (valles lisos) y las altas lo conservan (cumbres rugosas), que es la
     * estadistica del relieve erosionado real. Resultado aprox en [0, 1].
     */
    PROCEDURALECOSYSTEM_API float HybridMultifractal(const FVector2D& PosCm, const FVector2D& Offset,
        double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves);

    /**
     * Ridged multifractal de Musgrave: (1 - |ruido|)^2 crea crestas afiladas
     * conectadas (cordilleras); el peso realimentado suaviza las zonas bajas.
     * Resultado aprox en [0, 1].
     */
    PROCEDURALECOSYSTEM_API float RidgedMultifractal(const FVector2D& PosCm, const FVector2D& Offset,
        double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves);

    /**
     * Muestra completa del relieve en (Xcm, Ycm): domain warp -> hybrid como
     * base -> mezcla de ridged en las zonas altas (mascara suave por altitud,
     * modulada por RidgeWeight). Resultado aprox en [0, 1]; FHeightField lo
     * normaliza despues al rango real, asi que solo importa la FORMA.
     */
    PROCEDURALECOSYSTEM_API float TerrainSample(double Xcm, double Ycm, const FTerrainNoiseParams& P);
}
