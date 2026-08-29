/**
 * @file NutrientField.h
 * @author Juan Luque Roldán
 * @brief Base de fertilidad del suelo: manchas de nutrientes por ruido Perlin fractal.
 *
 * Declara FNutrientField, el segundo recurso abiótico que consume la función de
 * vigor. A diferencia del agua, que es causal y se deriva del relieve por
 * hidrología, la fertilidad se modela como un patrón geológicamente arbitrario y
 * deliberadamente NO correlacionado con la forma del terreno: se sintetiza con
 * fBm sobre ruido Perlin a partir de sales de semilla propias. La frecuencia base
 * por defecto es mucho menor y las octavas muchas menos que las del relieve, para
 * obtener manchas amplias en vez de rugosidad. Compone un FField2D, se genera una
 * vez de forma determinista y después solo se muestrea.
 *
 * @ingroup eco_terrain
 * @see @ref bib_perlin1985
 */

#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"

/**
 * Base de nutrientes del suelo: campo de fertilidad normalizado sobre la rejilla
 * del mundo.
 *
 * Representa la fertilidad POTENCIAL del terreno, congelada tras el bake; el
 * nivel disponible en cada tick, con su consumo por las raíces y su recarga hacia
 * esta base, vive en el pool de recursos.
 *
 * @see FResourcePool
 */
struct PROCEDURALECOSYSTEM_API FNutrientField
{
    /** Nutrientes base, normalizados a [0, OutputMax]. */
    FField2D Field;

    /** true si el campo tiene rejilla y datos. */
    bool IsValid() const { return Field.IsValid(); }

    /**
     * Genera la base de fertilidad con fBm sobre ruido Perlin y la normaliza.
     *
     * @param CellSize        Separación entre nodos de la rejilla, en cm.
     * @param Seed            Semilla maestra. De ella salen, con sales propias y
     *                        distintas de las del relieve, los dos offsets de
     *                        ruido del campo: así el patrón de fertilidad no queda
     *                        correlacionado con la forma del terreno.
     * @param OutputMax       Cota superior del campo normalizado; casa el rango
     *                        con el del agua.
     * @param PatchFrequency  Frecuencia de la primera octava, en cm^-1: cuanto más
     *                        baja, más grandes los parches.
     * @param Octaves         Octavas del fractal. Pocas dan manchas limpias;
     *                        muchas, rugosidad de escala fina.
     */
    void GeneratePatchyBase(int32 Width, int32 Height, double CellSize,
        const FVector2D& Origin, uint32 Seed,
        float OutputMax = 10.f,
        double PatchFrequency = 0.00015, int32 Octaves = 3);

    /** Fertilidad disponible en el punto de mundo (Xcm, Ycm), por bilineal. */
    FORCEINLINE float SampleNutrient(double Xcm, double Ycm) const
    {
        return Field.SampleBilinear(Xcm, Ycm);
    }

    /** Rectángulo de mundo que cubre el campo, en cm. */
    FBox2D GetWorldBounds() const { return Field.GetWorldBounds(); }
};