#include "Terrain/Field2D.h"

void FField2D::Init(int32 InWidth, int32 InHeight, double InCellSize,
    const FVector2D& InOrigin, float InitialValue)
{
    Width = FMath::Max(2, InWidth);
    Height = FMath::Max(2, InHeight);
    CellSize = InCellSize;
    Origin = InOrigin;

    // Fast path: si el valor inicial es 0, SetNumZeroed hace un memset en
    // lugar de reservar sin inicializar y luego recorrer celda a celda.
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