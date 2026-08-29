/**
 * @file LightFieldCoarse.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de la rejilla gruesa de luz: depósito, extinción acumulada y muestreo.
 *
 * Contiene el ciclo completo que la rejilla recorre en cada tick: la reserva de la
 * geometría con sus guardas, el reparto del área foliar de cada copa entre las
 * columnas y las capas que ocupa, la suma prefija descendente que la convierte en
 * LAI acumulado y los dos muestreadores —vecino más cercano e interpolado— que
 * cierran con la ley de Beer-Lambert. Los accesos inline y la conversión vertical
 * viven en la cabecera.
 *
 * @ingroup eco_terrain
 * @see @ref bib_monsisaeki1953
 * @see @ref bib_watson1947
 */

#include "Terrain/LightFieldCoarse.h"

#include "Async/ParallelFor.h"

void FLightFieldCoarse::Init(int32 InWidth, int32 InHeight, int32 InLayers,
    double InCellSizeXY, double InCellSizeZ,
    const FVector2D& InOrigin, double InBaseZ)
{
    Width = FMath::Max(1, InWidth);
    Height = FMath::Max(1, InHeight);
    Layers = FMath::Clamp(InLayers, 1, 512);
    // Ambos tamaños son divisores en WorldToColumnClamped, ColumnLayerCoord y
    // SampleLightSmooth: un cero llegado de configuración daría índices no finitos
    // y corrompería el muestreo. Misma guarda que FTreeLightGridFine y FSpatialHash.
    CellSizeXY = FMath::Max(InCellSizeXY, 1.0);
    CellSizeZ = FMath::Max(InCellSizeZ, 1.0);
    Origin = InOrigin;
    BaseZ = InBaseZ;

    GroundZ.Reset();
    LeafArea.SetNumZeroed(Width * Height * Layers);
}

void FLightFieldCoarse::SetGroundHeights(TArray<float>&& InGroundZ)
{
    if (InGroundZ.Num() == Width * Height)
    {
        GroundZ = MoveTemp(InGroundZ);
    }
    else
    {
        GroundZ.Reset(); // Tamaño incoherente: la rejilla se queda absoluta.
    }
}

void FLightFieldCoarse::ClearShadow()
{
    EcoGrid::ZeroFloats(LeafArea); // Memset compartido con la rejilla de luz fina.
}

void FLightFieldCoarse::DepositCanopyLeafArea(const FVector& ApexWorldPos, float CanopyRadiusCm,
    float CanopyDepthCm, float LeafAreaIndex)
{
    if (!IsValid() || CanopyDepthCm <= 0.f || CanopyRadiusCm <= 0.f || LeafAreaIndex <= 0.f) return;

    // Caja de columnas que toca la copa. El radio es constante en toda su altura:
    // la copa no se ensancha hacia abajo, y lo que oscurece el suelo no es la
    // huella del depósito sino la extinción acumulada por encima de cada vóxel.
    const int32 ixMin = FMath::Clamp(FMath::FloorToInt32((ApexWorldPos.X - CanopyRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 ixMax = FMath::Clamp(FMath::CeilToInt32((ApexWorldPos.X + CanopyRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 iyMin = FMath::Clamp(FMath::FloorToInt32((ApexWorldPos.Y - CanopyRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);
    const int32 iyMax = FMath::Clamp(FMath::CeilToInt32((ApexWorldPos.Y + CanopyRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);

    // Se reparte área foliar en cm2, no «LAI por vóxel». El LAI es área de hoja por
    // área de SUELO: sumarlo directamente en el vóxel tocado haría que una plántula
    // de copa mucho menor que un vóxel de 4 m oscureciera su celda igual que un
    // adulto de dosel, e invertiría la competencia por luz. Repartiendo área y
    // dividiendo luego por la huella del vóxel, cada copa aporta lo que le toca.
    //
    // El área total es la integral del perfil radial sobre el disco, y no pi*R^2,
    // para que LeafAreaIndex signifique el LAI medido en el eje de la copa.
    const double RadiusSq = (double)CanopyRadiusCm * (double)CanopyRadiusCm;
    const double TotalLeafAreaCm2 = LeafAreaIndex * 0.5 * PI * RadiusSq;
    const double VoxelFootprintCm2 = CellSizeXY * CellSizeXY;

    const double CrownTopZ = ApexWorldPos.Z;
    const double CrownBottomZ = ApexWorldPos.Z - CanopyDepthCm;

    // ==== Peso radial de cada columna alcanzada ====
    // El reparto vertical es idéntico en todas las columnas, así que factoriza y
    // basta con normalizar en horizontal. El allocator inline evita reservar en el
    // montón: una copa cubre unas pocas columnas.
    TArray<float, TInlineAllocator<64>> ColumnWeights;
    double WeightSum = 0.0;
    for (int32 iy = iyMin; iy <= iyMax; ++iy)
    {
        const double dy = Origin.Y + (iy + 0.5) * CellSizeXY - ApexWorldPos.Y;
        for (int32 ix = ixMin; ix <= ixMax; ++ix)
        {
            const double dx = Origin.X + (ix + 0.5) * CellSizeXY - ApexWorldPos.X;
            const double DistSq = dx * dx + dy * dy;

            // Perfil radial 1-(d/R)^2: denso en el eje, nulo en el borde. Se trabaja
            // con la distancia al cuadrado para no pagar una raíz por columna.
            const float W = (DistSq <= RadiusSq) ? static_cast<float>(1.0 - DistSq / RadiusSq) : 0.f;
            ColumnWeights.Add(W);
            WeightSum += W;
        }
    }

    if (WeightSum <= KINDA_SMALL_NUMBER)
    {
        // Copa mucho menor que un vóxel y caída entre centros: ninguna columna
        // recibe peso. Para no perder su área foliar se deposita entera en la
        // columna que la contiene.
        int32 ix, iy;
        WorldToColumnClamped(ApexWorldPos.X, ApexWorldPos.Y, ix, iy);
        ColumnWeights.Reset();
        ColumnWeights.Add(1.f);
        WeightSum = 1.0;

        DepositColumnLeafArea(ix, iy, CrownTopZ, CrownBottomZ, CanopyDepthCm,
            static_cast<float>(TotalLeafAreaCm2 / VoxelFootprintCm2));
        return;
    }

    // ==== Reparto del área foliar con los pesos ya normalizados ====
    const double InvWeightSum = 1.0 / WeightSum;
    int32 WeightIdx = 0;
    for (int32 iy = iyMin; iy <= iyMax; ++iy)
    {
        for (int32 ix = ixMin; ix <= ixMax; ++ix)
        {
            const float W = ColumnWeights[WeightIdx++];
            if (W <= 0.f) continue;

            // LAI de esta columna: el área foliar que recibe entre su huella.
            const double ColumnLai = TotalLeafAreaCm2 * (W * InvWeightSum) / VoxelFootprintCm2;
            DepositColumnLeafArea(ix, iy, CrownTopZ, CrownBottomZ, CanopyDepthCm, static_cast<float>(ColumnLai));
        }
    }
}

void FLightFieldCoarse::DepositColumnLeafArea(int32 Ix, int32 Iy,
    double CrownTopZ, double CrownBottomZ, float CanopyDepthCm, float ColumnLai)
{
    if (ColumnLai <= 0.f) return;

    const double ColumnRefZ = ReferenceZ(Ix, Iy) + BaseZ;
    const int32 izTop = FMath::Clamp(FMath::FloorToInt32((CrownTopZ - ColumnRefZ) / CellSizeZ), 0, Layers - 1);
    const int32 izBottom = FMath::Clamp(FMath::FloorToInt32((CrownBottomZ - ColumnRefZ) / CellSizeZ), 0, Layers - 1);

    for (int32 iz = izBottom; iz <= izTop; ++iz)
    {
        // Fracción de esta capa que cae dentro de la copa. Reparte el LAI de forma
        // continua, sin escalones al cambiar de vóxel, y las fracciones suman
        // exactamente 1 a lo largo de la copa.
        const double LayerLoZ = ColumnRefZ + iz * CellSizeZ;
        const double Overlap = FMath::Min(LayerLoZ + CellSizeZ, CrownTopZ) - FMath::Max(LayerLoZ, CrownBottomZ);
        if (Overlap <= 0.0) continue;

        LeafArea[IndexOf(Ix, Iy, iz)] += ColumnLai * static_cast<float>(Overlap / CanopyDepthCm);
    }
}

void FLightFieldCoarse::AccumulateExtinction()
{
    if (!IsValid()) return;

    // Una fila de columnas por tarea: cada columna se resuelve entera y no toca
    // ninguna otra, así que el resultado es bit a bit idéntico al secuencial con
    // cualquier número de hilos, igual que el resto de pasadas por filas del módulo.
    ParallelFor(Height, [this](int32 iy)
        {
            for (int32 ix = 0; ix < Width; ++ix)
            {
                float Above = 0.f;
                for (int32 iz = Layers - 1; iz >= 0; --iz)
                {
                    const int32 Cell = IndexOf(ix, iy, iz);
                    const float Own = LeafArea[Cell];

                    // El centro del vóxel está a media capa dentro de su propio
                    // follaje: ve todo lo de arriba más la mitad de lo suyo. Esa
                    // media capa es también lo que evita que un árbol muestreado en
                    // el techo de su copa se autosombree entero.
                    LeafArea[Cell] = Above + 0.5f * Own;
                    Above += Own;
                }
            }
        });
}

float FLightFieldCoarse::SampleLight(const FVector& WorldPos) const
{
    if (!IsValid()) return FullSunlight;

    int32 ix, iy;
    WorldToColumnClamped(WorldPos.X, WorldPos.Y, ix, iy);
    const int32 iz = FMath::Clamp(
        FMath::FloorToInt32(ColumnLayerCoord(ix, iy, WorldPos.Z) + 0.5), 0, Layers - 1);

    return EcoGrid::LightFromExtinction(LeafArea[IndexOf(ix, iy, iz)], FullSunlight, ExtinctionK, DiffuseFloor);
}

float FLightFieldCoarse::SampleLightSmooth(const FVector& WorldPos) const
{
    if (!IsValid()) return FullSunlight;

    // Coordenadas continuas referidas a los CENTROS de vóxel, de ahí el -0.5: es la
    // misma convención con la que se calculan los pesos radiales del depósito.
    const double u = (WorldPos.X - Origin.X) / CellSizeXY - 0.5;
    const double v = (WorldPos.Y - Origin.Y) / CellSizeXY - 0.5;

    const int32 x0 = FMath::FloorToInt32(u);
    const int32 y0 = FMath::FloorToInt32(v);
    const float fx = static_cast<float>(u - x0);
    const float fy = static_cast<float>(v - y0);

    // La vertical se resuelve por columna, no como un trilineal ortodoxo: con la
    // rejilla relativa al terreno cada una de las cuatro columnas vecinas puede
    // estar a una cota distinta, así que el índice de capa se calcula dentro del
    // lector. Interpolar en Z dentro de cada columna mezcla puntos a la misma
    // altura SOBRE EL SUELO, que es lo que significa el eje vertical de la rejilla.
    auto ColumnLeafArea = [this, &WorldPos](int32 ix, int32 iy) -> float
        {
            ix = FMath::Clamp(ix, 0, Width - 1);
            iy = FMath::Clamp(iy, 0, Height - 1);

            // ColumnLayerCoord ya recorta por debajo del terreno: sin ese recorte,
            // una lectura a ras de suelo interpola al 50% contra una capa
            // subterránea que vale cero por construcción y devuelve la mitad del
            // LAI real (ver FLightFieldCoarse::GroundLayerIndex).
            const double w = ColumnLayerCoord(ix, iy, WorldPos.Z);
            const int32  z0 = FMath::FloorToInt32(w);
            const float  fz = static_cast<float>(w - z0);

            const int32 zA = FMath::Clamp(z0, 0, Layers - 1);
            const int32 zB = FMath::Clamp(z0 + 1, 0, Layers - 1);
            return FMath::Lerp(LeafArea[IndexOf(ix, iy, zA)], LeafArea[IndexOf(ix, iy, zB)], fz);
        };

    const float c00 = ColumnLeafArea(x0, y0);
    const float c10 = ColumnLeafArea(x0 + 1, y0);
    const float c01 = ColumnLeafArea(x0, y0 + 1);
    const float c11 = ColumnLeafArea(x0 + 1, y0 + 1);

    const float c0 = FMath::Lerp(c00, c10, fx);
    const float c1 = FMath::Lerp(c01, c11, fx);

    return EcoGrid::LightFromExtinction(FMath::Lerp(c0, c1, fy), FullSunlight, ExtinctionK, DiffuseFloor);
}
