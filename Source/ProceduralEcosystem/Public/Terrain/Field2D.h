#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"

/**
 * Rejilla escalar 2D generica: "un TArray<float> con forma de mundo".
 *
 * No sabe que representa (agua, nutrientes, luz...); solo almacena y
 * muestrea. Cada campo concreto (FWaterField, FNutrientField, ...) se
 * construye ENCIMA de esta clase en vez de reimplementar la rejilla.
 *
 * Convencion: el valor de una celda vive EN EL NODO Origin + (Ix,Iy)*CellSize
 * (no en el centro de celda). El muestreo bilineal interpola entre nodos.
 * El grid de luz 3D, en cambio, muestrea en centros de voxel: tenerlo en
 * cuenta al combinar ambos en la funcion de vigor (medio celda de desfase,
 * inocuo a estas resoluciones).
 *
 * Unidades: coordenadas de mundo en cm (unidades de Unreal).
 */
struct PROCEDURALECOSYSTEM_API FField2D
{
    int32     Width = 0;
    int32     Height = 0;
    double    CellSize = 100.0;
    FVector2D Origin = FVector2D::ZeroVector;
    TArray<float> Data;

    bool IsValid() const { return Width > 1 && Height > 1 && Data.Num() == Width * Height; }

    /** Numero de celdas (= Width*Height cuando IsValid()). */
    FORCEINLINE int32 Num() const { return Data.Num(); }

    /** Reserva memoria y fija la geometria de la rejilla. Rellena con InitialValue. */
    void Init(int32 InWidth, int32 InHeight, double InCellSize,
        const FVector2D& InOrigin, float InitialValue = 0.f);

    /** Pone todas las celdas al mismo valor (no cambia geometria). */
    void Fill(float Value);

    /** Valor en mundo (Xcm, Ycm) con interpolacion bilineal. */
    float SampleBilinear(double Xcm, double Ycm) const;

    /** Acceso directo por indice de rejilla, con clamp a los bordes. */
    FORCEINLINE float GetAt(int32 Ix, int32 Iy) const
    {
        Ix = FMath::Clamp(Ix, 0, Width - 1);
        Iy = FMath::Clamp(Iy, 0, Height - 1);
        return Data[Iy * Width + Ix];
    }

    FORCEINLINE void SetAt(int32 Ix, int32 Iy, float Value)
    {
        if (Ix >= 0 && Ix < Width && Iy >= 0 && Iy < Height)
        {
            Data[Iy * Width + Ix] = Value;
        }
    }

    /** Extension en mundo (cm) que cubre la rejilla. */
    FBox2D GetWorldBounds() const;

    /** Mundo -> coordenadas de rejilla (en muestras, fraccional). */
    FORCEINLINE void WorldToGrid(double Xcm, double Ycm, double& OutGx, double& OutGy) const
    {
        OutGx = (Xcm - Origin.X) / CellSize;
        OutGy = (Ycm - Origin.Y) / CellSize;
    }
};