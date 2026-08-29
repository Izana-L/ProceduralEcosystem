/**
 * @file EcoNoise.h
 * @author Juan Luque Roldán
 * @brief Ruido procedural compartido: fBm, multifractales de Musgrave y domain warp.
 *
 * Declara el namespace EcoNoise, con las dos parametrizaciones de ruido que
 * conviven en el proyecto: FractalPerlin, un fBm por índice de celda
 * parametrizado en FRECUENCIA que usa la base de nutrientes, y la síntesis de
 * relieve que consume FHeightField, parametrizada en LONGITUD DE ONDA (cm) y
 * compuesta de domain warp, hybrid multifractal y ridged multifractal. Todo se
 * apoya en el ruido de gradiente de FMath::PerlinNoise2D; los desplazamientos en
 * el espacio del ruido se derivan de la semilla maestra con una sal distinta por
 * componente, de forma que los campos quedan descorrelacionados entre sí.
 *
 * @ingroup eco_terrain
 * @see @ref bib_perlin1985
 * @see @ref bib_musgrave1989
 * @see @ref bib_quilezdomainwarp
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Primitivas de ruido procedural del simulador.
 *
 * Reúne la generación fractal que alimenta el relieve, la base de nutrientes y
 * la derivación determinista de offsets por semilla. Ninguna función guarda
 * estado: todas son puras y por tanto seguras dentro de un ParallelFor.
 */
namespace EcoNoise
{
    /** Amplitud real que alcanza el PerlinNoise2D de UE: no llega a @f$ \pm 1 @f$,
        se queda en @f$ \pm\sqrt{2}/2 @f$. Dividir por ella devuelve el ruido al
        rango [-1, 1] nominal. */
    inline constexpr float PerlinAmplitude2D = 0.70710678f;

    /**
     * Una muestra de fBm (fractal Brownian motion) sobre PerlinNoise2D, indexada
     * por celda de rejilla.
     *
     * Variante con persistencia 0.5 y lacunaridad 2.0 embebidas y parametrizada
     * en frecuencia. La usa la base de nutrientes, cuyo relieve estadístico no
     * necesita ni multifractales ni control de longitud de onda.
     *
     * @param X, Y           Índices de celda de la rejilla.
     * @param CellSize       cm por celda (convierte índice -> distancia de mundo).
     * @param OffX, OffY     Desplazamiento en el espacio del ruido derivado de la
     *                       semilla; descorrelaciona este campo de los demás.
     * @param BaseFrequency  Frecuencia de la primera octava, en cm^-1.
     * @param Octaves        Nº de octavas (amp *= 0.5, freq *= 2 por octava).
     * @return               Valor normalizado por la suma de amplitudes. No divide
     *                       por PerlinAmplitude2D, así que su rango real es
     *                       ~[-0.707, 0.707]; inocuo porque el campo se normaliza
     *                       después con FField2D::FillNormalizedFrom.
     * @see @ref bib_mandelbrot1968
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
    // Síntesis de relieve
    // ------------------------------------------------------------------

    /**
     * Parámetros de la forma del relieve, tal como los consume TerrainSample.
     *
     * Se expresan en LONGITUD DE ONDA (cm) y no en frecuencia porque «el ancho de
     * las formas» es ajustable a ojo desde el editor y una frecuencia en cm^-1 no
     * lo es. Los cuatro offsets son desplazamientos en el espacio del ruido, uno
     * por stream independiente (base, crestas y las dos coordenadas del warp);
     * los rellena FHeightField::Generate a partir de la semilla, no se ponen a
     * mano.
     */
    struct FTerrainNoiseParams
    {
        double BaseWavelengthCm = 70000.0; ///< Ancho de las formas grandes del relieve (700 m).
        double Persistence = 0.5;          ///< Fracción de amplitud conservada por octava.
        double Lacunarity = 2.0;           ///< Multiplicador de frecuencia por octava.
        int32  Octaves = 8;                ///< Octavas del fractal base, antes de Nyquist.

        double WarpStrengthCm = 15000.0;   ///< Amplitud del domain warp (cm); 0 lo desactiva.
        double WarpWavelengthCm = 40000.0; ///< Ancho de las formas del campo que distorsiona.
        int32  WarpOctaves = 3;            ///< Octavas del fBm del warp.

        float  RidgeWeight = 0.45f;        ///< Crestas: 0 = colinas suaves, 1 = cordillera.

        FVector2D BaseOffset = FVector2D::ZeroVector;  ///< Offset del hybrid multifractal.
        FVector2D RidgeOffset = FVector2D::ZeroVector; ///< Offset del ridged multifractal.
        FVector2D WarpOffsetA = FVector2D::ZeroVector; ///< Offset del warp en X.
        FVector2D WarpOffsetB = FVector2D::ZeroVector; ///< Offset del warp en Y.
    };

    /**
     * Hash de 32 bits -> desplazamiento en el espacio del ruido.
     *
     * Quedarse con 16 bits y escalar por 0.1 sitúa el offset en [0, 6553.6]:
     * lejos del origen, para que dos campos con sales distintas no compartan
     * vecindad, pero sin degradar la precisión del double dentro de
     * PerlinNoise2D. Es la única conversión hash -> offset del proyecto: la
     * comparten el relieve y la base de nutrientes.
     */
    FORCEINLINE double OffsetFromHash(uint32 Hashed)
    {
        return (Hashed & 0xFFFF) * 0.1;
    }

    /**
     * Desplazamiento en el espacio del ruido derivado de (Seed, Salt).
     *
     * @param Salt Discrimina el stream: cada componente del relieve usa la suya,
     *             de modo que retocar una no desplaza a las demás.
     * @return Las dos coordenadas del offset, derivadas de dos hashes distintos.
     */
    PROCEDURALECOSYSTEM_API FVector2D SeedOffset(uint32 Seed, uint32 Salt);

    /**
     * Recorta las octavas que caen por debajo del límite de Nyquist de la rejilla.
     *
     * Una octava de longitud de onda menor que 2*CellSizeCm no es representable
     * en la rejilla: no aporta forma, solo aliasing en forma de pinchos por nodo.
     *
     * @return Número de octavas utilizable, siempre >= 1.
     * @see @ref bib_nyquistshannon
     */
    PROCEDURALECOSYSTEM_API int32 ClampOctavesToNyquist(int32 Octaves,
        double BaseWavelengthCm, double Lacunarity, double CellSizeCm);

    /**
     * fBm en espacio de mundo (cm), parametrizado en longitud de onda.
     *
     * @param PosCm  Posición de mundo a muestrear, en cm.
     * @param Offset Desplazamiento del stream de ruido (ver SeedOffset).
     * @return Suma de octavas normalizada por la suma de amplitudes, en ~[-1, 1].
     */
    PROCEDURALECOSYSTEM_API float Fbm(const FVector2D& PosCm, const FVector2D& Offset,
        double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves);

    /**
     * Hybrid multifractal: fractal cuya rugosidad depende de la altitud.
     *
     * El peso de cada octava se realimenta con la señal acumulada, así que las
     * zonas bajas pierden el detalle fino (valles lisos) y las altas lo conservan
     * (cumbres rugosas). Es la heterogeneidad estadística que distingue un
     * relieve real de un fractal puro, y da la forma base del terreno.
     *
     * @return Valor aproximadamente en [0, 1] (puede asomar levemente fuera).
     */
    PROCEDURALECOSYSTEM_API float HybridMultifractal(const FVector2D& PosCm, const FVector2D& Offset,
        double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves);

    /**
     * Ridged multifractal: crestas afiladas y conectadas de cordillera.
     *
     * La forma @f$ (1 - |n|)^2 @f$ convierte en máximo cada cruce por cero del
     * ruido, y el peso realimentado afila esas crestas y alisa las zonas bajas.
     *
     * @return Valor aproximadamente en [0, 1].
     */
    PROCEDURALECOSYSTEM_API float RidgedMultifractal(const FVector2D& PosCm, const FVector2D& Offset,
        double BaseWavelengthCm, double Persistence, double Lacunarity, int32 Octaves);

    /**
     * Muestra completa del relieve en la posición de mundo (Xcm, Ycm).
     *
     * Compone las tres piezas: distorsiona el dominio con un fBm independiente,
     * evalúa el hybrid multifractal como base y mezcla sobre él el ridged en las
     * zonas altas, con una máscara suave de altitud modulada por RidgeWeight.
     *
     * @return Valor aproximadamente en [0, 1]. FHeightField lo normaliza después
     *         al rango de altura real, así que solo importa la FORMA, no la
     *         escala absoluta.
     * @note Es una función pura: apta para evaluarse dentro de un ParallelFor.
     */
    PROCEDURALECOSYSTEM_API float TerrainSample(double Xcm, double Ycm, const FTerrainNoiseParams& P);
}
