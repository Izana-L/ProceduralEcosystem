#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"

struct FHeightField;

/**
 * Campo de agua causal ligado al relieve: Topographic Wetness Index (TWI).
 *
 * TWI = ln( acumulacion_de_flujo / tan(pendiente) )
 *
 * Los valles (mucha acumulacion, poca pendiente) salen humedos; las crestas
 * (poca acumulacion, mucha pendiente) salen secas. Se calcula UNA VEZ a
 * partir del FHeightField (el relieve no cambia en runtime) y luego solo se
 * muestrea, igual que el heightfield.
 *
 * Compone un FField2D (igual patron que FHeightField): la geometria y el
 * muestreo bilineal genericos viven ahi; esta clase solo aporta el enrutado
 * de flujo (D8), el rellenado de sumideros y la formula del TWI.
 *
 * Antes del D8 se rellenan las depresiones (priority-flood + epsilon,
 * Barnes et al. 2014): sin esto, el D8 sobre ruido fractal deja muchos
 * minimos locales que fragmentan la acumulacion en microcuencas inconexas.
 * Rellenar conecta la red de drenaje y da valles humedos realistas.
 *
 * Referencia: Beven & Kirkby (1979) - Topographic Wetness Index.
 */
struct PROCEDURALECOSYSTEM_API FWaterField
{
    /** TWI ya normalizado a [0, OutputMax], listo para la formula de vigor (Monod, Fase 2). */
    FField2D Field;

    bool IsValid() const { return Field.IsValid(); }

    /**
     * Calcula el TWI a partir del relieve: rellenado de sumideros, enrutado
     * D8, acumulacion de flujo, formula TWI y normalizacion a [0, OutputMax].
     * Coste: O(N log N) por el priority-flood y el ordenado por altura; se
     * paga UNA vez.
     *
     * @param OutputMax   Rango de salida del campo (para casar con nutrientes).
     * @param bFillSinks  Rellena depresiones antes del D8 (recomendado). Ponlo
     *                    a false para la ablacion (comparar con/sin rellenado).
     */
    void BakeFromHeightField(const FHeightField& Height, float OutputMax = 10.f,
        bool bFillSinks = true);

    /** Disponibilidad de agua en mundo (Xcm, Ycm), interpolacion bilineal. */
    FORCEINLINE float SampleWater(double Xcm, double Ycm) const
    {
        return Field.SampleBilinear(Xcm, Ycm);
    }

    FBox2D GetWorldBounds() const { return Field.GetWorldBounds(); }
};