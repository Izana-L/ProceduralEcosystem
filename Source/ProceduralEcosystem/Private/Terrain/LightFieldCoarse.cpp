#include "Terrain/LightFieldCoarse.h"

void FLightFieldCoarse::Init(int32 InWidth, int32 InHeight, int32 InLayers,
    double InCellSizeXY, double InCellSizeZ,
    const FVector2D& InOrigin, double InBaseZ)
{
    Width = FMath::Max(1, InWidth);
    Height = FMath::Max(1, InHeight);
    Layers = FMath::Clamp(InLayers, 1, 512);
    // Ambos tamanos son divisores en WorldToColumnClamped / WorldToVoxelClamped
    // / SampleLightSmooth: un 0 llegado de config produce indices no finitos y
    // corrompe el muestreo de luz. Misma guarda que FTreeLightGridFine (que ya
    // hace Max(InVoxelSizeCm, 1.f)) y FSpatialHash.
    CellSizeXY = FMath::Max(InCellSizeXY, 1.0);
    CellSizeZ = FMath::Max(InCellSizeZ, 1.0);
    Origin = InOrigin;
    BaseZ = InBaseZ;

    GroundZ.Reset();
    Shadow.SetNumZeroed(Width * Height * Layers);
}

void FLightFieldCoarse::SetGroundHeights(TArray<float>&& InGroundZ)
{
    if (InGroundZ.Num() == Width * Height)
    {
        GroundZ = MoveTemp(InGroundZ);
    }
    else
    {
        GroundZ.Reset(); // tamano incoherente: se queda absoluta (comportamiento previo)
    }
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
    WorldToColumnClamped(WorldPos.X, WorldPos.Y, OutIx, OutIy);

    // Altura RELATIVA a la cota de referencia de esta columna (terreno si lo hay).
    const double RelZ = WorldPos.Z - ReferenceZ(OutIx, OutIy) - BaseZ;
    OutIz = EcoGrid::WorldToCellClamped(RelZ, 0.0, CellSizeZ, Layers);
}

void FLightFieldCoarse::DepositCanopyShadow(const FVector& ApexWorldPos, float CanopyRadiusCm,
    float CanopyDepthCm, float Density)
{
    if (!IsValid() || CanopyDepthCm <= 0.f || CanopyRadiusCm <= 0.f) return;

    // El radio maximo posible (en el fondo del rango) acota que columnas
    // ix/iy merece la pena visitar, para no recorrer toda la rejilla.
    const float MaxRadiusCm = CanopyRadiusCm * 2.f; // ensanchamiento maximo (ver bucle)
    const int32 ixMin = FMath::Clamp(FMath::FloorToInt32((ApexWorldPos.X - MaxRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 ixMax = FMath::Clamp(FMath::CeilToInt32((ApexWorldPos.X + MaxRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 iyMin = FMath::Clamp(FMath::FloorToInt32((ApexWorldPos.Y - MaxRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);
    const int32 iyMax = FMath::Clamp(FMath::CeilToInt32((ApexWorldPos.Y + MaxRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);

    // COLUMNA a columna: con la rejilla relativa al terreno (C2) cada columna
    // tiene su propia cota de referencia, asi que el rango de capas afectado se
    // calcula por columna y no una sola vez para toda la copa. El bloque que se
    // recorre es minusculo (unas pocas columnas x unas pocas capas), asi que el
    // orden de los bucles no es un problema de cache.
    for (int32 iy = iyMin; iy <= iyMax; ++iy)
    {
        const double voxelCenterY = Origin.Y + (iy + 0.5) * CellSizeXY;
        const double dy = voxelCenterY - ApexWorldPos.Y;

        for (int32 ix = ixMin; ix <= ixMax; ++ix)
        {
            const double voxelCenterX = Origin.X + (ix + 0.5) * CellSizeXY;
            const double dx = voxelCenterX - ApexWorldPos.X;

            const float dist = static_cast<float>(FMath::Sqrt(dx * dx + dy * dy));
            if (dist > MaxRadiusCm) continue; // fuera incluso del ensanchamiento maximo

            const double ColumnRefZ = ReferenceZ(ix, iy) + BaseZ;

            // Rango de capas afectadas EN ESTA COLUMNA: desde la capa del apice
            // hasta CanopyDepthCm por debajo.
            const int32 izTop = FMath::Clamp(
                FMath::FloorToInt32((ApexWorldPos.Z - ColumnRefZ) / CellSizeZ), 0, Layers - 1);
            const int32 izBottom = FMath::Clamp(
                FMath::FloorToInt32((ApexWorldPos.Z - CanopyDepthCm - ColumnRefZ) / CellSizeZ), 0, Layers - 1);

            for (int32 iz = izTop; iz >= izBottom; --iz)
            {
                const double voxelCenterZ = ColumnRefZ + (iz + 0.5) * CellSizeZ;
                const float  depth = static_cast<float>(ApexWorldPos.Z - voxelCenterZ);
                if (depth < 0.f) continue;

                const float t = FMath::Clamp(depth / CanopyDepthCm, 0.f, 1.f); // 0 en la copa, 1 en el fondo
                const float radiusAtDepth = CanopyRadiusCm * (1.f + t); // se ensancha hasta 2x
                const float verticalFalloff = 1.f - t;                  // se debilita con la profundidad
                if (verticalFalloff <= 0.f) continue;
                if (dist > radiusAtDepth) continue;

                const float radialFalloff = 1.f - (dist / radiusAtDepth); // 1 en el eje, 0 en el borde
                Shadow[IndexOf(ix, iy, iz)] += Density * verticalFalloff * radialFalloff;
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
    return EcoGrid::LightFromShadow(S, FullSunlight);
}

float FLightFieldCoarse::SampleLightSmooth(const FVector& WorldPos) const
{
    if (!IsValid()) return FullSunlight;

    // Coordenadas continuas relativas a los CENTROS de voxel (de ahi el -0.5):
    // asi la interpolacion casa con como se deposita la sombra (centros).
    const double u = (WorldPos.X - Origin.X) / CellSizeXY - 0.5;
    const double v = (WorldPos.Y - Origin.Y) / CellSizeXY - 0.5;

    const int32 x0 = FMath::FloorToInt32(u);
    const int32 y0 = FMath::FloorToInt32(v);
    const float fx = static_cast<float>(u - x0);
    const float fy = static_cast<float>(v - y0);

    // La vertical se resuelve POR COLUMNA: con la rejilla relativa al terreno
    // (C2) cada una de las 4 columnas vecinas puede tener una cota distinta, asi
    // que el indice de capa se calcula dentro del lector. Interpolar en Z ya
    // dentro de cada columna es lo correcto: mezcla "misma altura sobre el
    // suelo", que es lo que significa el eje.
    auto ColumnShadow = [this, &WorldPos](int32 ix, int32 iy) -> float
        {
            ix = FMath::Clamp(ix, 0, Width - 1);
            iy = FMath::Clamp(iy, 0, Height - 1);

            const double w = (WorldPos.Z - ReferenceZ(ix, iy) - BaseZ) / CellSizeZ - 0.5;
            const int32  z0 = FMath::FloorToInt32(w);
            const float  fz = static_cast<float>(w - z0);

            const int32 zA = FMath::Clamp(z0, 0, Layers - 1);
            const int32 zB = FMath::Clamp(z0 + 1, 0, Layers - 1);
            return FMath::Lerp(Shadow[IndexOf(ix, iy, zA)], Shadow[IndexOf(ix, iy, zB)], fz);
        };

    const float c00 = ColumnShadow(x0, y0);
    const float c10 = ColumnShadow(x0 + 1, y0);
    const float c01 = ColumnShadow(x0, y0 + 1);
    const float c11 = ColumnShadow(x0 + 1, y0 + 1);

    const float c0 = FMath::Lerp(c00, c10, fx);
    const float c1 = FMath::Lerp(c01, c11, fx);
    const float shadow = FMath::Lerp(c0, c1, fy);

    return EcoGrid::LightFromShadow(shadow, FullSunlight);
}
