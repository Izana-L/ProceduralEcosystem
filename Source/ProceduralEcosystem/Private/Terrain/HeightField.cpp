/**
 * @file HeightField.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del bake del relieve y del cálculo de la normal del terreno.
 *
 * Define FHeightField::Generate, que encadena la derivación de los streams de
 * ruido a partir de la semilla, el recorte de octavas al límite de Nyquist, la
 * evaluación paralela del ruido con su normalización y la pasada de erosión; y
 * FHeightField::SampleNormal, que obtiene la normal por diferencias centradas
 * sobre el campo interpolado.
 *
 * @ingroup eco_terrain
 */

#include "Terrain/HeightField.h"
#include "Core/EcoCore.h"

void FHeightField::Generate(const FTerrainGenParams& Params)
{
    // Prepara la rejilla base (geometría y almacenamiento). El valor inicial es
    // irrelevante: el bake la sobrescribe entera.
    Field.Init(Params.Width, Params.Height, Params.CellSizeCm, FVector2D::ZeroVector, 0.f);

    // -----------------------------------------------------------------
    // 1) Parámetros de ruido derivados. Cada componente del relieve abre su
    //    propio stream de offsets con una sal distinta, para que retocar uno no
    //    desplace a los demás, y las octavas se recortan al límite de Nyquist:
    //    por debajo de 2*CellSize una octava no es representable y solo aporta
    //    aliasing en forma de pinchos por nodo.
    // -----------------------------------------------------------------
    EcoNoise::FTerrainNoiseParams N = Params.Noise;
    N.BaseOffset = EcoNoise::SeedOffset(Params.Seed, 0x0000000Du);
    N.RidgeOffset = EcoNoise::SeedOffset(Params.Seed, 0x51ED270Bu);
    N.WarpOffsetA = EcoNoise::SeedOffset(Params.Seed, 0x85EBCA6Bu);
    N.WarpOffsetB = EcoNoise::SeedOffset(Params.Seed, 0xC2B2AE35u);
    N.Octaves = EcoNoise::ClampOctavesToNyquist(N.Octaves,
        N.BaseWavelengthCm, N.Lacunarity, Params.CellSizeCm);
    N.WarpOctaves = EcoNoise::ClampOctavesToNyquist(N.WarpOctaves,
        N.WarpWavelengthCm, /* lacunaridad fija del warp */ 2.0, Params.CellSizeCm);

    const double Cell = Field.CellSize;

    // -----------------------------------------------------------------
    // 2) Ruido compuesto y normalización, ambos en FField2D::GenerateNormalized:
    //    el relieve pasa a ocupar todo [0, HeightScaleCm], que es la amplitud
    //    pico-valle pedida. TerrainSample es pura y la partición es por filas,
    //    así que el resultado no depende del número de hilos.
    // -----------------------------------------------------------------
    Field.GenerateNormalized(
        [Cell, &N](int32 x, int32 y) { return EcoNoise::TerrainSample(x * Cell, y * Cell, N); },
        static_cast<float>(Params.HeightScaleCm));

    // -----------------------------------------------------------------
    // 3) Erosión. El orden importa: la hidráulica talla la red de drenaje y
    //    después la térmica relaja las pendientes que queden por encima del
    //    talud, incluidas las que abre la propia hidráulica. Las gotas consumen
    //    su propio stream, derivado de Seed con una sal exclusiva.
    // -----------------------------------------------------------------
    if (Params.bErosion)
    {
        TerrainErosion::HydraulicErode(Field, EcoRand::Hash32(Params.Seed ^ 0x5EEDDA7Au), Params.Hydraulic);
        TerrainErosion::ThermalErode(Field, Params.Thermal);
    }
}

/**
 * Normal por diferencias centradas con paso e = CellSize.
 *
 * Variante de cuatro vecinos del cálculo clásico de pendiente sobre un modelo
 * digital de elevaciones. Las cuatro muestras se toman con SampleHeight, o sea
 * sobre el campo interpolado y no sobre los nodos crudos, así que la normal sale
 * algo más suave que la del operador discreto.
 *
 * @see @ref bib_horn1981
 */
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
