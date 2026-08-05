#include "Terrain/Field2D.h"
#include "Async/ParallelFor.h"

void FField2D::Init(int32 InWidth, int32 InHeight, double InCellSize,
    const FVector2D& InOrigin, float InitialValue)
{
    Width = FMath::Max(2, InWidth);
    Height = FMath::Max(2, InHeight);
    // CellSize es divisor en WorldToGrid (y por tanto en todos los Sample*): un
    // 0 llegado de config (el ClampMin del UPROPERTY es solo restriccion de UI)
    // produciria NaN en todas las posiciones. Misma guarda que FSpatialHash,
    // FAttractorCloud y FTreeLightGridFine.
    CellSize = FMath::Max(InCellSize, static_cast<double>(KINDA_SMALL_NUMBER));
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