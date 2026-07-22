#include "Geometry/TreeLightGridFine.h"
#include "Terrain/LightFieldCoarse.h"
#include "Geometry/TreeSkeleton.h"

namespace
{
    // Tope defensivo de resolucion por eje: una copa normal cabe de sobra por
    // debajo. Evita reservar gigas si llega una caja degenerada/enorme.
    constexpr int32 MaxVoxelsPerAxis = 256;
}

void FTreeLightGridFine::InitForBounds(const FBox& WorldBounds, float InVoxelSizeCm, float PaddingCm)
{
    VoxelSizeCm = FMath::Max(InVoxelSizeCm, 1.f);

    if (!WorldBounds.IsValid)
    {
        // Sin caja no hay rejilla: dejarla vacia (IsValid() dara false).
        Width = Height = Layers = 0;
        Shadow.Reset();
        OriginWorld = FVector::ZeroVector;
        return;
    }

    const FVector Pad(FMath::Max(PaddingCm, 0.f));
    const FVector Min = WorldBounds.Min - Pad;
    const FVector Max = WorldBounds.Max + Pad;
    const FVector Size = (Max - Min).ComponentMax(FVector(VoxelSizeCm)); // al menos 1 voxel por eje

    OriginWorld = Min;
    Width = FMath::Clamp(FMath::CeilToInt(Size.X / VoxelSizeCm), 1, MaxVoxelsPerAxis);
    Height = FMath::Clamp(FMath::CeilToInt(Size.Y / VoxelSizeCm), 1, MaxVoxelsPerAxis);
    Layers = FMath::Clamp(FMath::CeilToInt(Size.Z / VoxelSizeCm), 1, MaxVoxelsPerAxis);

    Shadow.Reset();
    Shadow.SetNumZeroed(Width * Height * Layers);
}

void FTreeLightGridFine::ClearShadow()
{
    for (float& S : Shadow) { S = 0.f; }
}

void FTreeLightGridFine::SeedFromCoarse(const FLightFieldCoarse& Coarse)
{
    if (!IsValid() || !Coarse.IsValid())
    {
        return;
    }

    // Cada voxel toma la sombra que proyectan los vecinos: Sombra = C - Q,
    // con Q leida (suave) del grid grueso en el centro del voxel.
    for (int32 Iz = 0; Iz < Layers; ++Iz)
    {
        for (int32 Iy = 0; Iy < Height; ++Iy)
        {
            for (int32 Ix = 0; Ix < Width; ++Ix)
            {
                const float Q = Coarse.SampleLightSmooth(VoxelCenter(Ix, Iy, Iz));
                Shadow[IndexOf(Ix, Iy, Iz)] = FMath::Max(0.f, FullSunlight - Q);
            }
        }
    }
}

void FTreeLightGridFine::DepositDownwardShadow(const FVector& FromWorld, float RadiusCm, float DepthCm, float Density)
{
    if (!IsValid() || Density <= 0.f || RadiusCm <= 0.f || DepthCm <= 0.f)
    {
        return;
    }

    // Caja de voxels afectados: disco de radio RadiusCm en XY, columna hacia
    // abajo entre From.Z-DepthCm y From.Z.
    const FVector BoxMin(FromWorld.X - RadiusCm, FromWorld.Y - RadiusCm, FromWorld.Z - DepthCm);
    const FVector BoxMax(FromWorld.X + RadiusCm, FromWorld.Y + RadiusCm, FromWorld.Z);

    int32 Ix0, Iy0, Iz0, Ix1, Iy1, Iz1;
    WorldToVoxelClamped(BoxMin, Ix0, Iy0, Iz0);
    WorldToVoxelClamped(BoxMax, Ix1, Iy1, Iz1);

    const float InvRadius = 1.f / RadiusCm;
    const float InvDepth = 1.f / DepthCm;

    for (int32 Iz = Iz0; Iz <= Iz1; ++Iz)
    {
        for (int32 Iy = Iy0; Iy <= Iy1; ++Iy)
        {
            for (int32 Ix = Ix0; Ix <= Ix1; ++Ix)
            {
                const FVector C = VoxelCenter(Ix, Iy, Iz);

                const float Dz = FromWorld.Z - C.Z; // >=0 hacia abajo
                if (Dz < 0.f || Dz > DepthCm) { continue; }

                const float Horiz = FVector2D(C.X - FromWorld.X, C.Y - FromWorld.Y).Size();
                if (Horiz > RadiusCm) { continue; }

                // Mas intensa junto al follaje (Dz pequeno) y en el eje (Horiz
                // pequeno); decae linealmente hacia los bordes.
                const float RadialFalloff = 1.f - Horiz * InvRadius;
                const float DepthFalloff = 1.f - Dz * InvDepth;
                Shadow[IndexOf(Ix, Iy, Iz)] += Density * RadialFalloff * DepthFalloff;
            }
        }
    }
}

void FTreeLightGridFine::DepositLeafShadow(const FTreeSkeleton& Skeleton, float RadiusCm, float DepthCm, float PerNodeDensity)
{
    if (!IsValid())
    {
        return;
    }

    for (const FBranchNode& N : Skeleton.Nodes)
    {
        DepositDownwardShadow(N.Pos, RadiusCm, DepthCm, PerNodeDensity);
    }
}

void FTreeLightGridFine::WorldToVoxelClamped(const FVector& WorldPos, int32& OutIx, int32& OutIy, int32& OutIz) const
{
    OutIx = FMath::Clamp(FMath::FloorToInt((WorldPos.X - OriginWorld.X) / VoxelSizeCm), 0, Width - 1);
    OutIy = FMath::Clamp(FMath::FloorToInt((WorldPos.Y - OriginWorld.Y) / VoxelSizeCm), 0, Height - 1);
    OutIz = FMath::Clamp(FMath::FloorToInt((WorldPos.Z - OriginWorld.Z) / VoxelSizeCm), 0, Layers - 1);
}

float FTreeLightGridFine::SampleShadowNearest(const FVector& WorldPos) const
{
    int32 Ix, Iy, Iz;
    WorldToVoxelClamped(WorldPos, Ix, Iy, Iz);
    return Shadow[IndexOf(Ix, Iy, Iz)];
}

float FTreeLightGridFine::SampleShadowTrilinear(const FVector& WorldPos) const
{
    // Coordenadas continuas en espacio de CENTROS de voxel (de ahi el -0.5).
    const float Gx = (WorldPos.X - OriginWorld.X) / VoxelSizeCm - 0.5f;
    const float Gy = (WorldPos.Y - OriginWorld.Y) / VoxelSizeCm - 0.5f;
    const float Gz = (WorldPos.Z - OriginWorld.Z) / VoxelSizeCm - 0.5f;

    const int32 X0 = FMath::Clamp(FMath::FloorToInt(Gx), 0, Width - 1);
    const int32 Y0 = FMath::Clamp(FMath::FloorToInt(Gy), 0, Height - 1);
    const int32 Z0 = FMath::Clamp(FMath::FloorToInt(Gz), 0, Layers - 1);
    const int32 X1 = FMath::Min(X0 + 1, Width - 1);
    const int32 Y1 = FMath::Min(Y0 + 1, Height - 1);
    const int32 Z1 = FMath::Min(Z0 + 1, Layers - 1);

    const float Tx = FMath::Clamp(Gx - X0, 0.f, 1.f);
    const float Ty = FMath::Clamp(Gy - Y0, 0.f, 1.f);
    const float Tz = FMath::Clamp(Gz - Z0, 0.f, 1.f);

    auto S = [&](int32 Ix, int32 Iy, int32 Iz) { return Shadow[IndexOf(Ix, Iy, Iz)]; };

    const float C00 = FMath::Lerp(S(X0, Y0, Z0), S(X1, Y0, Z0), Tx);
    const float C10 = FMath::Lerp(S(X0, Y1, Z0), S(X1, Y1, Z0), Tx);
    const float C01 = FMath::Lerp(S(X0, Y0, Z1), S(X1, Y0, Z1), Tx);
    const float C11 = FMath::Lerp(S(X0, Y1, Z1), S(X1, Y1, Z1), Tx);

    const float C0 = FMath::Lerp(C00, C10, Ty);
    const float C1 = FMath::Lerp(C01, C11, Ty);
    return FMath::Lerp(C0, C1, Tz);
}

float FTreeLightGridFine::SampleLight(const FVector& WorldPos) const
{
    if (!IsValid()) { return FullSunlight; }
    return FMath::Max(0.f, FullSunlight - SampleShadowNearest(WorldPos));
}

float FTreeLightGridFine::SampleLightSmooth(const FVector& WorldPos) const
{
    if (!IsValid()) { return FullSunlight; }
    return FMath::Max(0.f, FullSunlight - SampleShadowTrilinear(WorldPos));
}

bool FTreeLightGridFine::IsShaded(const FVector& WorldPos, float LightThreshold) const
{
    return SampleLight(WorldPos) < LightThreshold;
}

FVector FTreeLightGridFine::GradientOfLight(const FVector& WorldPos) const
{
    if (!IsValid()) { return FVector::ZeroVector; }

    const float H = VoxelSizeCm;
    const float Dx = SampleLightSmooth(WorldPos + FVector(H, 0, 0)) - SampleLightSmooth(WorldPos - FVector(H, 0, 0));
    const float Dy = SampleLightSmooth(WorldPos + FVector(0, H, 0)) - SampleLightSmooth(WorldPos - FVector(0, H, 0));
    const float Dz = SampleLightSmooth(WorldPos + FVector(0, 0, H)) - SampleLightSmooth(WorldPos - FVector(0, 0, H));

    // Apunta hacia mas luz. Normalizado; ZeroVector si el entorno es plano.
    return FVector(Dx, Dy, Dz).GetSafeNormal();
}