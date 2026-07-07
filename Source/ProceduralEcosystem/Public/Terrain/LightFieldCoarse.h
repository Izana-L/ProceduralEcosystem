#pragma once

#include "CoreMinimal.h"

/**
 * Grid de luz GRUESO a escala de paisaje (varios metros/voxel): sombreado
 * entre copas de distintos arboles. Es la resolucion (a) del documento de
 * diseno; la resolucion (b) -fina, local a cada hero tree- llega en la
 * Fase 3.
 *
 * Mecanica (Palubicki et al. 2009): cada copa deposita sombra en los voxels
 * inferiores en forma de "piramide invertida que decae con la profundidad"
 * - estrecha e intensa justo debajo de la copa, ancha y debil mas abajo.
 * La luz disponible es Q = clamp(FullSunlight - Sombra, 0).
 *
 * ALCANCE EN FASE 1: solo la infraestructura (rejilla, muestreo, y el
 * MECANISMO de depositar la sombra de UNA copa). El llenado con la sombra
 * de TODA la poblacion (recorrer los arboles cada tick) es de la Fase 2.
 *
 * A diferencia de FField2D (2D), esta rejilla es 3D: X, Y horizontales +
 * Z vertical (capas de altura). Muestrea en CENTROS de voxel (a diferencia
 * de FField2D, que muestrea en nodos); es medio voxel de desfase, inocuo a
 * estas resoluciones, pero conviene saberlo al combinar con agua/nutrientes.
 */
struct PROCEDURALECOSYSTEM_API FLightFieldCoarse
{
    int32 Width = 0; // voxels en X
    int32 Height = 0; // voxels en Y
    int32 Layers = 0; // voxels en Z (altura)

    double CellSizeXY = 400.0; // cm por voxel horizontal (varios metros)
    double CellSizeZ = 400.0; // cm por voxel vertical

    FVector2D Origin = FVector2D::ZeroVector; // esquina (0,0) en mundo, cm
    double BaseZ = 0.0;                       // altura de mundo (cm) del nivel Z=0

    /** Sombra acumulada por voxel, 0 = sin sombra. Width*Height*Layers. */
    TArray<float> Shadow;

    /** C: luz plena normalizada (cielo despejado). */
    static constexpr float FullSunlight = 1.f;

    bool IsValid() const
    {
        return Width > 0 && Height > 0 && Layers > 0 && Shadow.Num() == Width * Height * Layers;
    }

    /** Reserva la rejilla y la deja sin sombra. */
    void Init(int32 InWidth, int32 InHeight, int32 InLayers,
        double InCellSizeXY, double InCellSizeZ,
        const FVector2D& InOrigin, double InBaseZ);

    /** Pone toda la sombra a 0 (llamar antes de re-depositar en cada tick, Fase 2). */
    void ClearShadow();

    /**
     * Deposita la sombra de UNA copa: piramide invertida (estrecha/intensa
     * arriba, ancha/debil abajo) centrada en ApexWorldPos.
     *
     * @param ApexWorldPos    Punto mas alto de la copa (mundo, cm).
     * @param CanopyRadiusCm  Radio horizontal de la copa en su punto mas alto.
     * @param CanopyDepthCm   Hasta que profundidad por debajo de la copa se
     *                        nota su sombra (mas alla, aporte = 0).
     * @param Density         Opacidad de la copa en [0,1] (1 = opaca total).
     */
    void DepositCanopyShadow(const FVector& ApexWorldPos, float CanopyRadiusCm,
        float CanopyDepthCm, float Density = 1.f);

    /** Luz disponible tras sombra (vecino mas cercano): Q = clamp(FullSunlight - Sombra, 0). */
    float SampleLight(const FVector& WorldPos) const;

    /**
     * Igual que SampleLight pero con interpolacion TRILINEAL entre los 8
     * voxels vecinos. Mas suave (sin bloques); util para la funcion de vigor
     * y para sembrar el grid fino del hero tree en la Fase 3. Un pelin mas
     * caro que el vecino mas cercano.
     */
    float SampleLightSmooth(const FVector& WorldPos) const;

private:
    FORCEINLINE int32 IndexOf(int32 Ix, int32 Iy, int32 Iz) const
    {
        return (Iz * Height + Iy) * Width + Ix;
    }

    /** Mundo -> indice de voxel, con clamp a los bordes de la rejilla. */
    void WorldToVoxelClamped(const FVector& WorldPos, int32& OutIx, int32& OutIy, int32& OutIz) const;
};