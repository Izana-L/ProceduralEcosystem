#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"

/**
 * Campo de altura: la FUENTE DE VERDAD del relieve para la simulacion.
 *
 * Se muestrea en C++ (altura / pendiente / normal) de forma barata y
 * determinista, sin depender del ALandscape. El Landscape es solo la
 * representacion VISUAL; la simulacion consulta este FHeightField para que
 * lo que ve el jugador y lo que "sabe" la ecologia sean coherentes.
 *
 * Implementacion: compone un FField2D (Field) que aporta la rejilla, el
 * almacenamiento y el muestreo bilineal genericos. FHeightField solo anade
 * lo especifico de terreno: generacion de ruido fractal, pendiente y normal.
 *
 * Unidades: coordenadas de mundo en cm (unidades de Unreal). La altura
 * tambien en cm.
 */
struct PROCEDURALECOSYSTEM_API FHeightField
{
    /** Rejilla base: aqui viven Width/Height/CellSize/Origin/Data. */
    FField2D Field;

    bool IsValid() const { return Field.IsValid(); }

    /**
     * Genera un relieve fractal (fBm de PerlinNoise) determinista a partir de
     * Seed. El resultado se normaliza al rango REAL del ruido, de modo que el
     * relieve ocupa todo [0, HeightScaleCm]: aqui HeightScaleCm es la amplitud
     * pico-valle efectiva. (Las alturas absolutas dependen de los extremos del
     * ruido por seed; para el TWI solo cuentan los desniveles, asi que es
     * irrelevante para el agua.)
     */
    void GenerateFractalNoise(int32 InWidth, int32 InHeight, double InCellSize,
        uint32 Seed, int32 Octaves = 5,
        double BaseFrequency = 0.0006, double HeightScaleCm = 30000.0);

    /** Altura del terreno (cm) en mundo (Xcm, Ycm), interpolacion bilineal. */
    FORCEINLINE float SampleHeight(double Xcm, double Ycm) const
    {
        return Field.SampleBilinear(Xcm, Ycm);
    }

    /** Pendiente en radianes (0 = plano) por diferencias centrales. */
    float SampleSlope(double Xcm, double Ycm) const;

    /** Normal del terreno (unitaria, Z hacia arriba). */
    FVector SampleNormal(double Xcm, double Ycm) const;

    /** Extension del terreno en mundo (cm). */
    FBox2D GetWorldBounds() const { return Field.GetWorldBounds(); }
};