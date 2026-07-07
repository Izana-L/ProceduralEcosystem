#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"

/**
 * Campo de altura 2D: la FUENTE DE VERDAD del relieve para la simulación.
 *
 * Se muestrea en C++ (altura / pendiente / normal) de forma barata y
 * determinista, sin depender del ALandscape. El Landscape es solo la
 * representación VISUAL; la simulación consulta este FHeightField para que
 * lo que ve el jugador y lo que "sabe" la ecología sean coherentes.
 *
 * Unidades: coordenadas de mundo en cm (unidades de Unreal). La altura
 * también en cm.
 */
struct PROCEDURALECOSYSTEM_API FHeightField
{
    int32     Width    = 0;                        // nº de muestras en X
    int32     Height   = 0;                        // nº de muestras en Y
    double    CellSize = 100.0;                    // cm entre muestras (resolución del DEM)
    FVector2D Origin   = FVector2D::ZeroVector;    // esquina (0,0) en mundo, cm
    TArray<float> Data;                            // altura en cm, tamaño Width*Height (row-major)

    bool IsValid() const { return Width > 1 && Height > 1 && Data.Num() == Width * Height; }

    /** Genera un relieve fractal (fBm de PerlinNoise) determinista a partir de Seed. */
    void GenerateFractalNoise(int32 InWidth, int32 InHeight, double InCellSize,
                              uint32 Seed, int32 Octaves = 5,
                              double BaseFrequency = 0.0006, double HeightScaleCm = 30000.0);

    /** Altura del terreno (cm) en mundo (Xcm, Ycm), interpolación bilineal. */
    float SampleHeight(double Xcm, double Ycm) const;

    /** Pendiente en radianes (0 = plano) por diferencias centrales. */
    float SampleSlope(double Xcm, double Ycm) const;

    /** Normal del terreno (unitaria, Z hacia arriba). */
    FVector SampleNormal(double Xcm, double Ycm) const;

    /** Extensión del terreno en mundo (cm). */
    FBox2D GetWorldBounds() const;

private:
    /** Acceso con clamp a bordes. */
    FORCEINLINE float At(int32 Ix, int32 Iy) const
    {
        Ix = FMath::Clamp(Ix, 0, Width - 1);
        Iy = FMath::Clamp(Iy, 0, Height - 1);
        return Data[Iy * Width + Ix];
    }

    /** Mundo -> coordenadas de rejilla (en muestras, fraccional). */
    FORCEINLINE void WorldToGrid(double Xcm, double Ycm, double& OutGx, double& OutGy) const
    {
        OutGx = (Xcm - Origin.X) / CellSize;
        OutGy = (Ycm - Origin.Y) / CellSize;
    }
};
