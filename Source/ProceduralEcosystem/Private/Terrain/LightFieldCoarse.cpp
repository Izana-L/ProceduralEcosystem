#include "Terrain/LightFieldCoarse.h"

void FLightFieldCoarse::Init(int32 InWidth, int32 InHeight, int32 InLayers,
    double InCellSizeXY, double InCellSizeZ,
    const FVector2D& InOrigin, double InBaseZ)
{
    Width = FMath::Max(1, InWidth);
    Height = FMath::Max(1, InHeight);
    Layers = FMath::Max(1, InLayers);
    CellSizeXY = InCellSizeXY;
    CellSizeZ = InCellSizeZ;
    Origin = InOrigin;
    BaseZ = InBaseZ;

    Shadow.SetNumZeroed(Width * Height * Layers);
}

void FLightFieldCoarse::ClearShadow()
{
    // memset rapido: mas barato que un bucle celda a celda.
    if (Shadow.Num() > 0)
    {
        FMemory::Memzero(Shadow.GetData(), Shadow.Num() * sizeof(float));
    }
}

void FLightFieldCoarse::WorldToVoxelClamped(const FVector& WorldPos, int32& OutIx, int32& OutIy, int32& OutIz) const
{
    const int32 ix = FMath::FloorToInt((WorldPos.X - Origin.X) / CellSizeXY);
    const int32 iy = FMath::FloorToInt((WorldPos.Y - Origin.Y) / CellSizeXY);
    const int32 iz = FMath::FloorToInt((WorldPos.Z - BaseZ) / CellSizeZ);

    OutIx = FMath::Clamp(ix, 0, Width - 1);
    OutIy = FMath::Clamp(iy, 0, Height - 1);
    OutIz = FMath::Clamp(iz, 0, Layers - 1);
}

void FLightFieldCoarse::DepositCanopyShadow(const FVector& ApexWorldPos, float CanopyRadiusCm,
    float CanopyDepthCm, float Density)
{
    if (!IsValid() || CanopyDepthCm <= 0.f || CanopyRadiusCm <= 0.f) return;

    // Rango de capas Z afectadas: desde la capa de la copa hasta CanopyDepthCm
    // por debajo. Fuera de ese rango, la sombra de esta copa es 0.
    const int32 izTop = FMath::Clamp(
        FMath::FloorToInt((ApexWorldPos.Z - BaseZ) / CellSizeZ), 0, Layers - 1);
    const int32 izBottom = FMath::Clamp(
        FMath::FloorToInt((ApexWorldPos.Z - CanopyDepthCm - BaseZ) / CellSizeZ), 0, Layers - 1);

    // El radio maximo posible (en el fondo del rango) acota que columnas
    // ix/iy merece la pena visitar, para no recorrer toda la rejilla.
    const float MaxRadiusCm = CanopyRadiusCm * 2.f; // ensanchamiento maximo (ver bucle)
    const int32 ixMin = FMath::Clamp(FMath::FloorToInt((ApexWorldPos.X - MaxRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 ixMax = FMath::Clamp(FMath::CeilToInt((ApexWorldPos.X + MaxRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 iyMin = FMath::Clamp(FMath::FloorToInt((ApexWorldPos.Y - MaxRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);
    const int32 iyMax = FMath::Clamp(FMath::CeilToInt((ApexWorldPos.Y + MaxRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);

    for (int32 iz = izTop; iz >= izBottom; --iz)
    {
        const double voxelCenterZ = BaseZ + (iz + 0.5) * CellSizeZ;
        const float  depth = static_cast<float>(ApexWorldPos.Z - voxelCenterZ); // >= 0
        if (depth < 0.f) continue;

        const float t = FMath::Clamp(depth / CanopyDepthCm, 0.f, 1.f); // 0 en la copa, 1 en el fondo
        const float radiusAtDepth = CanopyRadiusCm * (1.f + t); // se ensancha hasta 2x
        const float verticalFalloff = 1.f - t;                    // se debilita con la profundidad
        if (verticalFalloff <= 0.f) continue;

        for (int32 iy = iyMin; iy <= iyMax; ++iy)
        {
            const double voxelCenterY = Origin.Y + (iy + 0.5) * CellSizeXY;

            for (int32 ix = ixMin; ix <= ixMax; ++ix)
            {
                const double voxelCenterX = Origin.X + (ix + 0.5) * CellSizeXY;

                const float dist = static_cast<float>(
                    FVector2D(voxelCenterX - ApexWorldPos.X, voxelCenterY - ApexWorldPos.Y).Size());
                if (dist > radiusAtDepth) continue;

                const float radialFalloff = 1.f - (dist / radiusAtDepth); // 1 en el eje, 0 en el borde
                const float shadowAdd = Density * verticalFalloff * radialFalloff;

                const int32 idx = IndexOf(ix, iy, iz);
                Shadow[idx] += shadowAdd;
            }
        }
    }
}

float FLightFieldCoarse::SampleLight(const FVector& WorldPos) const
{
    if (!IsValid()) return FullSunlight;

    int32 ix, iy, iz;
    WorldToVoxelClamped(WorldPos, ix, iy, iz);

    const float S = Shadow[IndexOf(ix, iy, iz)];
    return FMath::Max(FullSunlight - S, 0.f);
}

float FLightFieldCoarse::SampleLightSmooth(const FVector& WorldPos) const
{
    if (!IsValid()) return FullSunlight;

    // Coordenadas continuas relativas a los CENTROS de voxel (de ahi el -0.5):
    // asi la interpolacion casa con como se deposita la sombra (centros).
    const double u = (WorldPos.X - Origin.X) / CellSizeXY - 0.5;
    const double v = (WorldPos.Y - Origin.Y) / CellSizeXY - 0.5;
    const double w = (WorldPos.Z - BaseZ) / CellSizeZ - 0.5;

    const int32 x0 = FMath::FloorToInt(u);
    const int32 y0 = FMath::FloorToInt(v);
    const int32 z0 = FMath::FloorToInt(w);
    const float fx = static_cast<float>(u - x0);
    const float fy = static_cast<float>(v - y0);
    const float fz = static_cast<float>(w - z0);

    // Lector con clamp a los bordes (mismo criterio que WorldToVoxelClamped).
    auto S = [this](int32 ix, int32 iy, int32 iz) -> float
        {
            ix = FMath::Clamp(ix, 0, Width - 1);
            iy = FMath::Clamp(iy, 0, Height - 1);
            iz = FMath::Clamp(iz, 0, Layers - 1);
            return Shadow[IndexOf(ix, iy, iz)];
        };

    const float c000 = S(x0, y0, z0), c100 = S(x0 + 1, y0, z0);
    const float c010 = S(x0, y0 + 1, z0), c110 = S(x0 + 1, y0 + 1, z0);
    const float c001 = S(x0, y0, z0 + 1), c101 = S(x0 + 1, y0, z0 + 1);
    const float c011 = S(x0, y0 + 1, z0 + 1), c111 = S(x0 + 1, y0 + 1, z0 + 1);

    const float c00 = FMath::Lerp(c000, c100, fx);
    const float c10 = FMath::Lerp(c010, c110, fx);
    const float c01 = FMath::Lerp(c001, c101, fx);
    const float c11 = FMath::Lerp(c011, c111, fx);
    const float c0 = FMath::Lerp(c00, c10, fy);
    const float c1 = FMath::Lerp(c01, c11, fy);
    const float shadow = FMath::Lerp(c0, c1, fz);

    return FMath::Max(FullSunlight - shadow, 0.f);
}