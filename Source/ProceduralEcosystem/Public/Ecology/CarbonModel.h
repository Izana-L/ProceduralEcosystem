#pragma once

#include "CoreMinimal.h"

/**
 * =============================================================================
 *  CO2 COMO CAPA DE REALISMO BARATA (Fase 6, doc. 6.3)
 * =============================================================================
 *
 * El documento es explicito en lo que NO hay que hacer: "no una sim volumetrica",
 * "evitar adveccion en octree: cuesta mucho para un efecto imperceptible". Lo que
 * pide es "un multiplicador analitico del vigor que dependa un poco de la altura
 * o de la densidad de follaje local (menos CO2 dentro de una copa muy densa)",
 * con "coste ~0".
 *
 * Esta cabecera es exactamente eso: una funcion pura, inline, sin estado y sin
 * ninguna estructura de datos nueva. NO hay campo de CO2, no hay rejilla, no hay
 * memoria extra, no hay una sola linea mas en el bucle caliente que una
 * multiplicacion. Reutiliza dos numeros que el tick YA tiene en la mano:
 *
 *   - Q  : la luz disponible en el punto, que ya se muestreo para f_luz. Como la
 *          sombra de la rejilla de luz la depositan las COPAS, (1 - Q) es un
 *          proxy directo y gratis de la densidad de follaje local. Ecologicamente
 *          se sostiene: dentro de un dosel cerrado el aire se renueva peor y la
 *          fotosintesis del propio dosel deprime el CO2 (es un efecto real y
 *          medido en bosques densos, el llamado gradiente vertical de CO2).
 *
 *   - H  : la altura de la copa del arbol, que ya esta cacheada en la poblacion.
 *          Por encima del dosel el aire esta bien MEZCLADO con la atmosfera libre
 *          y el CO2 vuelve al valor de fondo. Por eso un arbol dominante casi no
 *          nota la penalizacion y una plantula del sotobosque si: es otro
 *          mecanismo mas -pequeno- que favorece a los dominantes, en la misma
 *          direccion que la competencia por luz.
 *
 * FORMULA
 *
 *     Shade = clamp(1 - Q, 0, 1)                        densidad de follaje local
 *     Mix   = clamp(H / FullMixingHeightCm, 0, 1)       fraccion de "aire libre"
 *     fCO2  = 1 - MaxReduction * Shade * (1 - Mix)
 *
 * Rango: [1 - MaxReduction, 1]. Con MaxReduction = 0.15 el efecto tope es un
 * -15% de vigor para una plantula bajo dosel cerrado, y practicamente 0 para un
 * arbol adulto o en un claro. Es "leve", como pide el Apendice A.
 *
 * ACOPLAMIENTO AL VIGOR
 *
 * Se aplica como MULTIPLICADOR sobre el minimo de Liebig, no como un cuarto
 * factor del min():
 *
 *     vigor = min(fL, fW, fN) * fCO2
 *
 * Es deliberado y conviene poder defenderlo: la ley del minimo describe recursos
 * que se CONSUMEN y se agotan localmente (agua, nutrientes, luz interceptada).
 * El CO2 aqui no se simula como recurso consumible -no hay campo, no hay
 * consumo, no hay agotamiento-, sino como una modulacion suave de la eficiencia
 * fotosintetica. Meterlo en el min() lo convertiria en limitante en cuanto
 * bajara de los otros tres y cambiaria el balance ecologico que se ajusto en la
 * Fase 2; como multiplicador solo "afina" y nunca decide.
 *
 * REPRODUCIBILIDAD
 *
 * Es determinista (no consume RNG) pero SI cambia el resultado de la simulacion:
 * un bosque con CO2 activado y otro sin el divergen. Para comparar corridas de la
 * memoria con las de la Fase 5, o para la ablacion de la Fase 7, se apaga con
 * la consola (Eco.CO2.Enable 0) o con bEnableCO2Factor en Project Settings, y
 * entonces la funcion devuelve 1.0 EXACTO -> resultados identicos bit a bit a
 * los de antes de la Fase 6.
 */
namespace EcoCarbon
{
    /** Parametros del multiplicador (se rellenan desde UEcosystemSettings). */
    struct FCO2Params
    {
        /** false -> CO2Factor devuelve 1.0 exacto (ablacion). */
        bool  bEnabled = true;

        /** Reduccion maxima del vigor, en fraccion. 0.15 = hasta -15%. */
        float MaxReduction = 0.15f;

        /** Altura (cm) por encima de la cual se considera aire bien mezclado
            (tipicamente, la altura del dosel dominante). */
        float FullMixingHeightCm = 2500.f;

        /** Luz plena de referencia (FLightFieldCoarse::FullSunlight). Se pasa
            explicita para que la cabecera no dependa del grid de luz. */
        float FullSunlight = 1.f;
    };

    /**
     * Multiplicador de vigor por disponibilidad de CO2 en [1-MaxReduction, 1].
     *
     * @param LightQ         Luz disponible en el punto (la misma que alimenta f_luz).
     * @param CanopyHeightCm Altura de la copa del arbol (0 para una semilla / punto de suelo).
     */
    FORCEINLINE float CO2Factor(float LightQ, float CanopyHeightCm, const FCO2Params& P)
    {
        if (!P.bEnabled || P.MaxReduction <= 0.f)
        {
            return 1.f; // ablacion: cero efecto y cero coste
        }

        const float Full = FMath::Max(P.FullSunlight, KINDA_SMALL_NUMBER);
        const float Shade = FMath::Clamp(1.f - LightQ / Full, 0.f, 1.f);
        const float Mix = FMath::Clamp(CanopyHeightCm / FMath::Max(P.FullMixingHeightCm, 1.f), 0.f, 1.f);

        const float Reduction = FMath::Clamp(P.MaxReduction, 0.f, 0.9f) * Shade * (1.f - Mix);
        return FMath::Clamp(1.f - Reduction, 0.1f, 1.f);
    }

    /** Atajo: aplica el multiplicador a un vigor de Liebig ya calculado. */
    FORCEINLINE float ApplyToVigor(float LiebigVigor, float LightQ, float CanopyHeightCm, const FCO2Params& P)
    {
        return LiebigVigor * CO2Factor(LightQ, CanopyHeightCm, P);
    }
}
