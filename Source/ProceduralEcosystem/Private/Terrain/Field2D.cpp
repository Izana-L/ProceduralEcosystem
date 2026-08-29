/**
 * @file Field2D.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de FField2D: inicialización, muestreo bilineal y normalización.
 *
 * Contiene las operaciones de la rejilla que no son ni triviales ni plantilla:
 * la reserva del almacenamiento con las guardas de geometría, el muestreo
 * bilineal con extensión de bordes, el barrido min-max y la escritura
 * normalizada en paralelo por filas. Los accesos inline y el bake genérico
 * GenerateNormalized viven en la cabecera.
 *
 * @ingroup eco_terrain
 */

#include "Terrain/Field2D.h"
#include "Async/ParallelFor.h"

void FField2D::Init(int32 InWidth, int32 InHeight, double InCellSize,
    const FVector2D& InOrigin, float InitialValue)
{
    Width = FMath::Max(2, InWidth);
    Height = FMath::Max(2, InHeight);
    // CellSize es el divisor de WorldToGrid y por tanto de todo el muestreo: un
    // cero llegado de configuración daría NaN en cualquier posición. El ClampMin
    // del UPROPERTY solo restringe la UI, no la asignación desde código.
    CellSize = FMath::Max(InCellSize, static_cast<double>(KINDA_SMALL_NUMBER));
    Origin = InOrigin;

    // Con valor inicial 0 basta un memset; en otro caso hay que reservar sin
    // inicializar y recorrer las celdas.
    if (InitialValue == 0.f)
    {
        Data.SetNumZeroed(Width * Height);
    }
    else
    {
        Data.SetNumUninitialized(Width * Height);
        Fill(InitialValue);
    }
}

void FField2D::Fill(float Value)
{
    for (float& V : Data)
    {
        V = Value;
    }
}

float FField2D::SampleBilinear(double Xcm, double Ycm) const
{
    if (!IsValid()) return 0.f;

    double gx, gy;
    WorldToGrid(Xcm, Ycm, gx, gy);

    const int32 x0 = FMath::FloorToInt(gx);
    const int32 y0 = FMath::FloorToInt(gy);
    const float fx = static_cast<float>(gx - x0);
    const float fy = static_cast<float>(gy - y0);

    const float v00 = GetAt(x0, y0);
    const float v10 = GetAt(x0 + 1, y0);
    const float v01 = GetAt(x0, y0 + 1);
    const float v11 = GetAt(x0 + 1, y0 + 1);

    const float vx0 = FMath::Lerp(v00, v10, fx);
    const float vx1 = FMath::Lerp(v01, v11, fx);
    return FMath::Lerp(vx0, vx1, fy);
}

FBox2D FField2D::GetWorldBounds() const
{
    const FVector2D Min = Origin;
    const FVector2D Max = Origin + FVector2D((Width - 1) * CellSize, (Height - 1) * CellSize);
    return FBox2D(Min, Max);
}

void FField2D::MinMax(const TArray<float>& Values, float& OutMin, float& OutMax)
{
    OutMin = TNumericLimits<float>::Max();
    OutMax = -TNumericLimits<float>::Max();
    for (const float V : Values)
    {
        OutMin = FMath::Min(OutMin, V);
        OutMax = FMath::Max(OutMax, V);
    }
}

void FField2D::FillNormalizedFrom(const TArray<float>& Raw, float OutputMax)
{
    if (!IsValid() || Raw.Num() != Data.Num())
    {
        return;
    }

    float RawMin, RawMax;
    MinMax(Raw, RawMin, RawMax);
    const float Range = FMath::Max(RawMax - RawMin, KINDA_SMALL_NUMBER);

    const int32 W = Width;
    ParallelFor(Height, [&](int32 y)
        {
            for (int32 x = 0; x < W; ++x)
            {
                const int32 i = y * W + x;
                const float t = (Raw[i] - RawMin) / Range; // [0, 1]
                Data[i] = t * OutputMax;
            }
        });
}