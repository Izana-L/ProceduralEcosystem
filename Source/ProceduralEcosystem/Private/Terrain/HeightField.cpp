#include "Terrain/HeightField.h"
#include "Core/EcoCore.h"

void FHeightField::Generate(const FTerrainGenParams& Params)
{
    // Prepara la rejilla base (geometria + almacenamiento); el valor inicial
    // da igual, se sobrescribe entero a continuacion.
    Field.Init(Params.Width, Params.Height, Params.CellSizeCm, FVector2D::ZeroVector, 0.f);

    // -----------------------------------------------------------------
    // 1) Parametros de ruido derivados: offsets por semilla (streams
    //    independientes para base, crestas y las dos coordenadas del warp) y
    //    recorte de Nyquist (una octava con longitud de onda < 2*CellSize no
    //    se puede representar en la rejilla y solo mete aliasing: los
    //    "pinchos" por vertice del relieve antiguo).
    // -----------------------------------------------------------------
    EcoNoise::FTerrainNoiseParams N = Params.Noise;
    N.BaseOffset = EcoNoise::SeedOffset(Params.Seed, 0x0000000Du);
    N.RidgeOffset = EcoNoise::SeedOffset(Params.Seed, 0x51ED270Bu);
    N.WarpOffsetA = EcoNoise::SeedOffset(Params.Seed, 0x85EBCA6Bu);
    N.WarpOffsetB = EcoNoise::SeedOffset(Params.Seed, 0xC2B2AE35u);
    N.Octaves = EcoNoise::ClampOctavesToNyquist(N.Octaves,
        N.BaseWavelengthCm, N.Lacunarity, Params.CellSizeCm);
    N.WarpOctaves = EcoNoise::ClampOctavesToNyquist(N.WarpOctaves,
        N.WarpWavelengthCm, /*Lacunarity del warp*/ 2.0, Params.CellSizeCm);

    const double Cell = Field.CellSize;

    // -----------------------------------------------------------------
    // 2 y 3) Ruido compuesto en paralelo por filas y normalizacion al rango
    //    REAL del ruido (el relieve ocupa todo [0, HeightScaleCm], que es la
    //    amplitud pico-valle). Las dos cosas las hace FField2D::
    //    GenerateNormalized, que es la copia unica del patron: TerrainSample es
    //    puro y cada fila escribe celdas distintas -> determinista.
    // -----------------------------------------------------------------
    Field.GenerateNormalized(
        [Cell, &N](int32 x, int32 y) { return EcoNoise::TerrainSample(x * Cell, y * Cell, N); },
        static_cast<float>(Params.HeightScaleCm));

    // -----------------------------------------------------------------
    // 4) Erosion (bake unico): la hidraulica talla la red de drenaje y
    //    despues la termica relaja cualquier pendiente que quede por encima
    //    del talud. El stream de las gotas se deriva de Seed con su propia
    //    sal: no perturba ningun otro stream del proyecto.
    // -----------------------------------------------------------------
    if (Params.bErosion)
    {
        TerrainErosion::HydraulicErode(Field, EcoRand::Hash32(Params.Seed ^ 0x5EEDDA7Au), Params.Hydraulic);
        TerrainErosion::ThermalErode(Field, Params.Thermal);
    }
}

FVector FHeightField::SampleNormal(double Xcm, double Ycm) const
{
    const double e = Field.CellSize;
    const float  hL = SampleHeight(Xcm - e, Ycm);
    const float  hR = SampleHeight(Xcm + e, Ycm);
    const float  hD = SampleHeight(Xcm, Ycm - e);
    const float  hU = SampleHeight(Xcm, Ycm + e);

    const float dzdx = (hR - hL) / static_cast<float>(2.0 * e);
    const float dzdy = (hU - hD) / static_cast<float>(2.0 * e);
    return FVector(-dzdx, -dzdy, 1.f).GetSafeNormal();
}
