#include "Terrain/LightFieldCoarse.h"

#include "Async/ParallelFor.h"

void FLightFieldCoarse::Init(int32 InWidth, int32 InHeight, int32 InLayers,
    double InCellSizeXY, double InCellSizeZ,
    const FVector2D& InOrigin, double InBaseZ)
{
    Width = FMath::Max(1, InWidth);
    Height = FMath::Max(1, InHeight);
    Layers = FMath::Clamp(InLayers, 1, 512);
    // Ambos tamanos son divisores en WorldToColumnClamped / ColumnLayerCoord
    // y en SampleLightSmooth: un 0 llegado de config produce indices no finitos y
    // corrompe el muestreo de luz. Misma guarda que FTreeLightGridFine (que ya
    // hace Max(InVoxelSizeCm, 1.f)) y FSpatialHash.
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
        GroundZ.Reset(); // tamano incoherente: se queda absoluta (comportamiento previo)
    }
}

void FLightFieldCoarse::ClearShadow()
{
    EcoGrid::ZeroFloats(LeafArea); // memset compartido con el grid fino
}

void FLightFieldCoarse::DepositCanopyLeafArea(const FVector& ApexWorldPos, float CanopyRadiusCm,
    float CanopyDepthCm, float LeafAreaIndex)
{
    if (!IsValid() || CanopyDepthCm <= 0.f || CanopyRadiusCm <= 0.f || LeafAreaIndex <= 0.f) return;

    // La copa YA NO se ensancha hacia abajo. Ese ensanchamiento formaba parte del
    // bug de la sombra invertida: repartia hacia el suelo una sombra que a la vez
    // se desvanecia con la profundidad. Ahora la copa es un volumen con su radio, y
    // lo que llega al suelo es la EXTINCION acumulada.
    const int32 ixMin = FMath::Clamp(FMath::FloorToInt32((ApexWorldPos.X - CanopyRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 ixMax = FMath::Clamp(FMath::CeilToInt32((ApexWorldPos.X + CanopyRadiusCm - Origin.X) / CellSizeXY), 0, Width - 1);
    const int32 iyMin = FMath::Clamp(FMath::FloorToInt32((ApexWorldPos.Y - CanopyRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);
    const int32 iyMax = FMath::Clamp(FMath::CeilToInt32((ApexWorldPos.Y + CanopyRadiusCm - Origin.Y) / CellSizeXY), 0, Height - 1);

    // SE REPARTE AREA FOLIAR (cm2), NO "LAI POR VOXEL", y esa distincion es la que
    // impide que el tamano de la copa frente al del voxel decida la ecologia. El
    // LAI es area de hoja por area de SUELO: si se sumase el LAI directamente en el
    // voxel que toca, una plantula -cuya copa mide bastante menos que un voxel de 4 m-
    // oscureceria su celda igual que un adulto de dosel, y como el perfil de sombra
    // decae con la profundidad, la sombra del sotobosque acabaria dominada por los
    // arboles BAJOS: competencia por luz invertida. Repartiendo area y dividiendo
    // luego por la huella del voxel, una copa pequena aporta lo que le corresponde.
    //
    // El area total sale del perfil radial integrado, no de pi*R^2, para que
    // LeafAreaIndex signifique lo que dice su comentario: el LAI medido EN EL EJE.
    const double RadiusSq = (double)CanopyRadiusCm * (double)CanopyRadiusCm;
    const double TotalLeafAreaCm2 = LeafAreaIndex * 0.5 * PI * RadiusSq;
    const double VoxelFootprintCm2 = CellSizeXY * CellSizeXY;

    const double CrownTopZ = ApexWorldPos.Z;
    const double CrownBottomZ = ApexWorldPos.Z - CanopyDepthCm;

    // --- Pasada 1: peso radial de cada columna alcanzada -------------------
    // El reparto vertical es identico en todas las columnas, asi que factoriza y
    // basta con normalizar en horizontal. Inline: la copa cubre unas pocas columnas.
    TArray<float, TInlineAllocator<64>> ColumnWeights;
    double WeightSum = 0.0;
    for (int32 iy = iyMin; iy <= iyMax; ++iy)
    {
        const double dy = Origin.Y + (iy + 0.5) * CellSizeXY - ApexWorldPos.Y;
        for (int32 ix = ixMin; ix <= ixMax; ++ix)
        {
            const double dx = Origin.X + (ix + 0.5) * CellSizeXY - ApexWorldPos.X;
            const double DistSq = dx * dx + dy * dy;

            // Perfil radial 1-(d/R)^2: densa en el eje, nula en el borde. Se trabaja
            // con la distancia AL CUADRADO para no pagar una raiz por voxel.
            const float W = (DistSq <= RadiusSq) ? static_cast<float>(1.0 - DistSq / RadiusSq) : 0.f;
            ColumnWeights.Add(W);
            WeightSum += W;
        }
    }

    if (WeightSum <= KINDA_SMALL_NUMBER)
    {
        // Copa mucho menor que un voxel y centrada entre centros: no pierde su area
        // foliar, la deposita entera en la columna que la contiene.
        int32 ix, iy;
        WorldToColumnClamped(ApexWorldPos.X, ApexWorldPos.Y, ix, iy);
        ColumnWeights.Reset();
        ColumnWeights.Add(1.f);
        WeightSum = 1.0;

        DepositColumnLeafArea(ix, iy, CrownTopZ, CrownBottomZ, CanopyDepthCm,
            static_cast<float>(TotalLeafAreaCm2 / VoxelFootprintCm2));
        return;
    }

    // --- Pasada 2: repartir el area con los pesos ya calculados ------------
    const double InvWeightSum = 1.0 / WeightSum;
    int32 WeightIdx = 0;
    for (int32 iy = iyMin; iy <= iyMax; ++iy)
    {
        for (int32 ix = ixMin; ix <= ixMax; ++ix)
        {
            const float W = ColumnWeights[WeightIdx++];
            if (W <= 0.f) continue;

            // LAI que le toca a ESTA columna = area foliar que recibe / su huella.
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
        // Fraccion de ESTA capa que cae dentro de la copa: reparte el LAI de forma
        // continua, sin escalones al cambiar de voxel, y suma exactamente 1 a lo
        // largo de toda la copa.
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
    // ninguna otra, asi que el resultado es bit a bit identico al serial con
    // cualquier numero de hilos (mismo patron que el resto de pasadas 2D).
    ParallelFor(Height, [this](int32 iy)
        {
            for (int32 ix = 0; ix < Width; ++ix)
            {
                float Above = 0.f;
                for (int32 iz = Layers - 1; iz >= 0; --iz)
                {
                    const int32 Cell = IndexOf(ix, iy, iz);
                    const float Own = LeafArea[Cell];

                    // El centro del voxel esta a media capa dentro de su propio
                    // follaje: ve todo lo de arriba mas la mitad de lo suyo. Esa
                    // media capa es tambien lo que deja que un arbol muestreado en
                    // el techo de su copa no se autosombree entero.
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
    auto ColumnLeafArea = [this, &WorldPos](int32 ix, int32 iy) -> float
        {
            ix = FMath::Clamp(ix, 0, Width - 1);
            iy = FMath::Clamp(iy, 0, Height - 1);

            // ColumnLayerCoord ya recorta por debajo del terreno: sin ese recorte
            // toda lectura a ras de suelo interpolaba al 50% con una capa
            // subterranea que es estructuralmente cero, y devolvia la mitad del
            // LAI real (ver GroundLayerIndex).
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
