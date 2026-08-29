/**
 * @file CarbonModel.h
 * @author Juan Luque Roldán
 * @brief Multiplicador analítico de CO2 sobre el vigor, sin campo ni estado.
 *
 * Recoge el gradiente vertical de CO2 del interior de un dosel —dentro de una masa
 * densa de follaje el aire se renueva peor y la fotosíntesis del propio dosel deprime
 * el CO2, mientras que por encima la mezcla con la atmósfera libre lo devuelve al
 * valor de fondo— sin simularlo: ni rejilla, ni memoria, ni una línea más en el bucle
 * caliente que una multiplicación. Reutiliza dos números que el tick ya tiene en la
 * mano, la luz Q como proxy de la densidad de follaje local (la sombra la depositan
 * las copas) y la altura de copa como proxy de la mezcla, y devuelve un factor que
 * multiplica al vigor. Es un mecanismo más, pequeño, que favorece a los dominantes.
 *
 * @ingroup eco_ecology
 * @see @ref bib_co2dosel
 * @see EcoVigor::EvaluateVigor
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Factor de CO2 del vigor: funciones puras, inline y sin estado.
 *
 * @code
 * Shade = clamp(1 - Q/FullSunlight, 0, 1)       densidad de follaje local
 * Mix   = clamp(H / FullMixingHeightCm, 0, 1)   fracción de aire bien mezclado
 * fCO2  = 1 - MaxReduction * Shade * (1 - Mix)  en [1 - MaxReduction, 1]
 * vigor = min(fL, fW, fN) * fCO2
 * @endcode
 *
 * Se aplica multiplicando al mínimo de Liebig y no como cuarto factor del min(): la
 * ley del mínimo describe recursos que se consumen y se agotan localmente, y aquí el
 * CO2 no se simula como tal —no hay campo, ni consumo, ni agotamiento— sino como una
 * modulación suave de la eficiencia fotosintética. Dentro del min() pasaría a decidir
 * el crecimiento en cuanto bajara de los otros tres; como multiplicador afina y nunca
 * decide.
 *
 * @note Con MaxReduction = 0.15 el efecto tope es un -15% de vigor para una plántula
 *       bajo dosel cerrado y prácticamente nulo para un adulto o en un claro.
 * @warning No consume RNG, pero sí cambia el resultado de la simulación: dos corridas
 *          con la misma semilla y distinto bEnabled divergen. Apagado desde la consola
 *          (Eco.CO2.Enable 0) o con bEnableCO2Factor devuelve 1.0 exacto, que es la
 *          condición para que la ablación sea bit a bit idéntica.
 */
namespace EcoCarbon
{
    /** Parámetros del multiplicador; se rellenan desde UEcosystemSettings. */
    struct FCO2Params
    {
        /** false -> CO2Factor devuelve 1.0 exacto (ablación). */
        bool  bEnabled = true;

        /** Reducción máxima del vigor, en fracción. 0.15 = hasta -15%. */
        float MaxReduction = 0.15f;

        /** Altura (cm) por encima de la cual se considera aire bien mezclado;
            típicamente, la altura del dosel dominante. */
        float FullMixingHeightCm = 2500.f;

        /** Luz plena de referencia (FLightFieldCoarse::FullSunlight). Se pasa explícita
            para que esta cabecera no dependa de la rejilla de luz. */
        float FullSunlight = 1.f;
    };

    /**
     * Multiplicador de vigor por disponibilidad de CO2.
     *
     * @param LightQ         Luz disponible en el punto, la misma que alimenta el factor
     *                       de luz del vigor.
     * @param CanopyHeightCm Altura de la copa del árbol; 0 para una semilla o para un
     *                       punto de suelo.
     * @return Factor en [1 - MaxReduction, 1]. MaxReduction se recorta a 0.9, así que el
     *         resultado nunca baja de 0.1 por mucho que se configure.
     */
    FORCEINLINE float CO2Factor(float LightQ, float CanopyHeightCm, const FCO2Params& P)
    {
        if (!P.bEnabled || P.MaxReduction <= 0.f)
        {
            return 1.f; // ablación: elemento neutro exacto, coste cero
        }

        const float Full = FMath::Max(P.FullSunlight, KINDA_SMALL_NUMBER);
        const float Shade = FMath::Clamp(1.f - LightQ / Full, 0.f, 1.f);
        const float Mix = FMath::Clamp(CanopyHeightCm / FMath::Max(P.FullMixingHeightCm, 1.f), 0.f, 1.f);

        const float Reduction = FMath::Clamp(P.MaxReduction, 0.f, 0.9f) * Shade * (1.f - Mix);
        return FMath::Clamp(1.f - Reduction, 0.1f, 1.f);
    }

    /** Atajo: aplica el multiplicador a un vigor ya combinado. */
    FORCEINLINE float ApplyToVigor(float LiebigVigor, float LightQ, float CanopyHeightCm, const FCO2Params& P)
    {
        return LiebigVigor * CO2Factor(LightQ, CanopyHeightCm, P);
    }
}
