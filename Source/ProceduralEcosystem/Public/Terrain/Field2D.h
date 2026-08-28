#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "Async/ParallelFor.h"
#include "Core/GridMath.h"

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

    /** Extension en mundo (cm) que cubre la rejilla. */
    FBox2D GetWorldBounds() const;

    /** Coordenada de mundo (cm) del NODO (Ix, Iy). Es la convencion del campo
        -el valor vive en el nodo, no en el centro de celda- y estaba reescrita a
        mano en el bake de idoneidad, en el heatmap de luz y en la exportacion de
        heightmap. */
    FORCEINLINE double NodeWorldX(int32 Ix) const { return Origin.X + Ix * CellSize; }
    FORCEINLINE double NodeWorldY(int32 Iy) const { return Origin.Y + Iy * CellSize; }

    /** Mundo -> coordenadas de rejilla (en muestras, fraccional). */
    FORCEINLINE void WorldToGrid(double Xcm, double Ycm, double& OutGx, double& OutGy) const
    {
        OutGx = EcoGrid::ToGridCoord(Xcm, Origin.X, CellSize);
        OutGy = EcoGrid::ToGridCoord(Ycm, Origin.Y, CellSize);
    }

    /** Min y max de un buffer de valores (barrido serial O(N)). UNICA copia del
        patron que antes reimplementaban los tres generadores de campo, el
        visualizador y el log de rangos. Con Values vacio deja min > max. */
    static void MinMax(const TArray<float>& Values, float& OutMin, float& OutMax);

    /**
     * Escribe en Data la normalizacion lineal de Raw a [0, OutputMax]:
     * t = (v - min) / (max - min), Data[i] = t * OutputMax. Paralelo por filas
     * (cada fila escribe celdas disjuntas -> determinista). Raw debe tener el
     * mismo numero de celdas que la rejilla.
     */
    void FillNormalizedFrom(const TArray<float>& Raw, float OutputMax);

    /**
     * Genera el campo entero desde una funcion de rejilla y lo normaliza a
     * [0, OutputMax]: Gen(x, y) -> valor crudo.
     *
     * Es el patron "buffer crudo -> ParallelFor por filas -> FillNormalizedFrom"
     * que estaba copiado literalmente en FHeightField::Generate y en
     * FNutrientField::GeneratePatchyBase (y que cualquier campo nuevo volveria a
     * copiar). Aqui vive una sola vez, con la MISMA particion por filas: cada
     * fila escribe celdas disjuntas y Gen debe ser pura, asi que el resultado no
     * depende del numero de hilos -> determinista, como exige la Fase 0.
     *
     * Es plantilla (no TFunctionRef) a proposito: Gen se inlinea dentro del
     * bucle, que es lo que se quiere en un bake de cientos de miles de celdas.
     */
    template <typename FGen>
    void GenerateNormalized(FGen&& Gen, float OutputMax)
    {
        if (!IsValid())
        {
            return;
        }

        const int32 W = Width;
        TArray<float> Raw;
        Raw.SetNumUninitialized(W * Height);

        ParallelFor(Height, [&Raw, &Gen, W](int32 y)
            {
                for (int32 x = 0; x < W; ++x)
                {
                    Raw[y * W + x] = Gen(x, y);
                }
            });

        FillNormalizedFrom(Raw, OutputMax);
    }
};