#include "Terrain/NutrientField.h"
#include "Terrain/EcoNoise.h"
#include "Core/EcoCore.h"

void FNutrientField::GeneratePatchyBase(int32 Width, int32 Height, double CellSize,
    const FVector2D& Origin, uint32 Seed,
    float OutputMax, double PatchFrequency, int32 Octaves)
{
    Field.Init(Width, Height, CellSize, Origin, 0.f);

    // Sales DISTINTAS a las que usa FHeightField::Generate (EcoNoise::SeedOffset):
    // aunque compartan la misma MasterSeed del proyecto, el desplazamiento debe
    // salir distinto para que el patron de nutrientes no quede pegado al
    // relieve (no son la misma causa fisica). La CONVERSION hash -> offset si es
    // la misma que la del relieve, y ahora se lee de un solo sitio.
    const double OffX = EcoNoise::OffsetFromHash(EcoRand::Hash32(Seed ^ 0xA24BAED4u));
    const double OffY = EcoNoise::OffsetFromHash(EcoRand::Hash32(Seed ^ 0x5BD1E995u));

    // fBm crudo en paralelo + normalizacion a [0, OutputMax] (mismo criterio que
    // el agua, para que ambos campos entren en la formula de vigor con rangos
    // comparables). Las dos pasadas son FField2D::GenerateNormalized, la misma
    // que usa el relieve.
    Field.GenerateNormalized(
        [CellSize, OffX, OffY, PatchFrequency, Octaves](int32 x, int32 y)
        {
            return EcoNoise::FractalPerlin(x, y, CellSize, OffX, OffY, PatchFrequency, Octaves);
        },
        OutputMax);
}