#pragma once

#include "CoreMinimal.h"

/**
 * Grid de luz GRUESO a escala de paisaje (varios metros/voxel): sombreado
 * entre copas de distintos arboles. Es la resolucion (a) del documento de
 * diseno; la resolucion (b) -fina, local a cada hero tree- es la Fase 3.
 *
 * Mecanica (Palubicki et al. 2009): cada copa deposita sombra en los voxels
 * inferiores en forma de "piramide invertida que decae con la profundidad"
 * - estrecha e intensa justo debajo de la copa, ancha y debil mas abajo.
 * La luz disponible es Q = clamp(FullSunlight - Sombra, 0).
 *
 * ================== REJILLA RELATIVA AL TERRENO (optimizacion C2) ==========
 * La rejilla es 3D (X, Y horizontales + Z en capas), pero la vertical NO es
 * absoluta: la capa 0 de cada columna arranca en la COTA DEL TERRENO de esa
 * columna (mas BaseZ, que suele ser un pequeno margen negativo).
 *
 * POR QUE: con Z absoluta habia que cubrir todo el relieve. Con los valores
 * por defecto (HeightScaleCm = 30.000 cm de desnivel, voxel de 400 cm) salian
 * 95 capas x 256 x 256 = 6,2 M voxels = ~25 MB que ademas se ponen a cero
 * ENTERO cada tick. Pero los arboles solo miden ~2.000 cm: cada columna tenia
 * ~5 capas utiles y ~90 de aire o de roca. Midiendo la altura RESPECTO AL
 * SUELO bastan ~10-12 capas -> ~8-10x menos memoria y un ClearShadow 8-10x mas
 * barato. Y de paso es mas correcto ecologicamente: un arbol del valle y otro
 * de la cresta ocupan las mismas capas relativas, que es lo que de verdad
 * significa "altura de copa".
 *
 * La API publica NO cambia: se sigue muestreando y depositando en COORDENADAS
 * DE MUNDO. La conversion la hace la rejilla por dentro con GroundZ, que se
 * rellena una vez con SetGroundHeights(). Si no se rellena, la rejilla se
 * comporta como antes (absoluta respecto a BaseZ), lo que mantiene validos los
 * tests y cualquier uso suelto.
 * ==========================================================================
 *
 * Muestrea en CENTROS de voxel (a diferencia de FField2D, que muestrea en
 * nodos); es medio voxel de desfase, inocuo a estas resoluciones, pero conviene
 * saberlo al combinar con agua/nutrientes.
 */
struct PROCEDURALECOSYSTEM_API FLightFieldCoarse
{
    int32 Width = 0; // voxels en X
    int32 Height = 0; // voxels en Y
    int32 Layers = 0; // voxels en Z (altura SOBRE EL SUELO, ver nota de arriba)

    double CellSizeXY = 400.0; // cm por voxel horizontal (varios metros)
    double CellSizeZ = 400.0; // cm por voxel vertical

    FVector2D Origin = FVector2D::ZeroVector; // esquina (0,0) en mundo, cm

    /** Offset (cm) de la capa 0 respecto a la cota de referencia de la columna.
        Con GroundZ relleno, esa referencia es el TERRENO y este valor suele ser
        negativo (un poco de margen bajo el suelo para que las pendientes no
        clampeen); sin GroundZ es la Z de mundo absoluta de la capa 0. */
    double BaseZ = 0.0;

    /** Sombra acumulada por voxel, 0 = sin sombra. Width*Height*Layers. */
    TArray<float> Shadow;

    /** Cota de terreno (cm) del centro de cada columna. Vacio = rejilla absoluta. */
    TArray<float> GroundZ;

    /** C: luz plena normalizada (cielo despejado). */
    static constexpr float FullSunlight = 1.f;

    bool IsValid() const
    {
        return Width > 0 && Height > 0 && Layers > 0 && Shadow.Num() == Width * Height * Layers;
    }

    /** true si la vertical se mide respecto al terreno (ver nota de C2). */
    bool IsTerrainRelative() const { return GroundZ.Num() == Width * Height; }

    /** Reserva la rejilla y la deja sin sombra. */
    void Init(int32 InWidth, int32 InHeight, int32 InLayers,
        double InCellSizeXY, double InCellSizeZ,
        const FVector2D& InOrigin, double InBaseZ);

    /**
     * Fija la cota de terreno de cada columna (Width*Height valores, en orden
     * fila-mayor igual que Shadow). A partir de aqui la rejilla es relativa al
     * terreno. Pasar un array de tamano distinto la deja absoluta.
     */
    void SetGroundHeights(TArray<float>&& InGroundZ);

    /** Cota de referencia (cm) de la columna (Ix,Iy): terreno si lo hay, si no BaseZ. */
    FORCEINLINE double ReferenceZ(int32 Ix, int32 Iy) const
    {
        return IsTerrainRelative() ? static_cast<double>(GroundZ[Iy * Width + Ix]) : 0.0;
    }

    /** Pone toda la sombra a 0 (llamar antes de re-depositar en cada tick). */
    void ClearShadow();

    /** Memoria de la rejilla en bytes (para Eco.Profile). */
    int64 MemoryBytes() const
    {
        return (int64)Shadow.Max() * sizeof(float) + (int64)GroundZ.Max() * sizeof(float);
    }

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

    /** Columna de mundo -> indice (Ix,Iy) con clamp a los bordes. */
    FORCEINLINE void WorldToColumnClamped(double Xcm, double Ycm, int32& OutIx, int32& OutIy) const
    {
        OutIx = FMath::Clamp(FMath::FloorToInt32((Xcm - Origin.X) / CellSizeXY), 0, Width - 1);
        OutIy = FMath::Clamp(FMath::FloorToInt32((Ycm - Origin.Y) / CellSizeXY), 0, Height - 1);
    }

    /** Mundo -> indice de voxel, con clamp a los bordes de la rejilla. */
    void WorldToVoxelClamped(const FVector& WorldPos, int32& OutIx, int32& OutIy, int32& OutIz) const;
};
