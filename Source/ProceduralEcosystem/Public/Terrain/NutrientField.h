#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"

/**
 * Base de nutrientes del suelo: ruido Perlin parcheado, SIN relacion con el
 * relieve (a diferencia del agua, que si es causal). La planificacion lo
 * justifica asi: los nutrientes son "arbitrarios" geologicamente, asi que
 * Perlin encaja como base de fertilidad.
 *
 * Se genera UNA VEZ (determinista por semilla) y luego solo se muestrea.
 * El consumo por las raices y la regeneracion/difusion llegan en la Fase 2,
 * cuando existan agentes-arbol que consuman del campo cada tick.
 *
 * Compone un FField2D, igual patron que FHeightField y FWaterField.
 */
struct PROCEDURALECOSYSTEM_API FNutrientField
{
    /** Nutrientes base, normalizados a [0, OutputMax]. */
    FField2D Field;

    bool IsValid() const { return Field.IsValid(); }

    /**
     * Genera la base de fertilidad con ruido Perlin parcheado (fBm).
     * La semilla se deriva de forma distinta a la del relieve para que el
     * patron de nutrientes NO quede correlacionado con la forma del terreno.
     *
     * @param PatchFrequency  Frecuencia base del ruido: mas baja = parches
     *                        mas grandes. Deliberadamente menor que la del
     *                        relieve para que se vean manchas, no rugosidad.
     * @param Octaves         Menos octavas que el relieve: menos detalle
     *                        fino, parches mas "limpios".
     */
    void GeneratePatchyBase(int32 Width, int32 Height, double CellSize,
        const FVector2D& Origin, uint32 Seed,
        float OutputMax = 10.f,
        double PatchFrequency = 0.00015, int32 Octaves = 3);

    /** Fertilidad disponible en mundo (Xcm, Ycm), interpolacion bilineal. */
    FORCEINLINE float SampleNutrient(double Xcm, double Ycm) const
    {
        return Field.SampleBilinear(Xcm, Ycm);
    }

    FBox2D GetWorldBounds() const { return Field.GetWorldBounds(); }
};