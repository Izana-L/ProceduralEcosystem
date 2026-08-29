/**
 * @file NutrientField.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del bake de la base de nutrientes.
 *
 * Define FNutrientField::GeneratePatchyBase: deriva de la semilla los dos offsets
 * de ruido del campo con sales propias y delega el bake en
 * FField2D::GenerateNormalized, evaluando fBm sobre Perlin nodo a nodo y
 * normalizando el resultado al rango de salida pedido.
 *
 * @ingroup eco_terrain
 * @see @ref bib_perlin1985
 */

#include "Terrain/NutrientField.h"
#include "Terrain/EcoNoise.h"
#include "Core/EcoCore.h"

void FNutrientField::GeneratePatchyBase(int32 Width, int32 Height, double CellSize,
    const FVector2D& Origin, uint32 Seed,
    float OutputMax, double PatchFrequency, int32 Octaves)
{
    Field.Init(Width, Height, CellSize, Origin, 0.f);

    // Sales DISTINTAS de las que usa FHeightField::Generate (EcoNoise::SeedOffset):
    // aunque compartan la misma semilla maestra del proyecto, el desplazamiento en
    // el espacio del ruido debe salir distinto para que el patrón de fertilidad no
    // quede pegado al relieve, que no es su causa física. La CONVERSIÓN
    // hash -> offset sí es la misma, y vive en EcoNoise::OffsetFromHash.
    const double OffX = EcoNoise::OffsetFromHash(EcoRand::Hash32(Seed ^ 0xA24BAED4u));
    const double OffY = EcoNoise::OffsetFromHash(EcoRand::Hash32(Seed ^ 0x5BD1E995u));

    // fBm crudo en paralelo y normalización a [0, OutputMax], el mismo criterio
    // que el agua, para que ambos campos entren en la función de vigor con rangos
    // comparables. El generador es puro y la partición es por filas, así que el
    // bake sale idéntico con cualquier número de hilos.
    Field.GenerateNormalized(
        [CellSize, OffX, OffY, PatchFrequency, Octaves](int32 x, int32 y)
        {
            return EcoNoise::FractalPerlin(x, y, CellSize, OffX, OffY, PatchFrequency, Octaves);
        },
        OutputMax);
}