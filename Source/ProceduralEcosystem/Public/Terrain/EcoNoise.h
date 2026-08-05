#pragma once

#include "CoreMinimal.h"

/**
 * Ruido procedural compartido. La UNICA copia del bucle de fBm por octavas que
 * antes vivia duplicado en FHeightField::GenerateFractalNoise y
 * FNutrientField::GeneratePatchyBase: si se cambia el algoritmo (warp de
 * dominio, otra acumulacion...), se cambia aqui y ambos campos lo heredan.
 */
namespace EcoNoise
{
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
}
