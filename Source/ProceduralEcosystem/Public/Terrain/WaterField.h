/**
 * @file WaterField.h
 * @author Juan Luque Roldán
 * @brief Campo de agua del terreno, derivado del relieve por el índice topográfico de humedad.
 *
 * Declara FWaterField, el recurso abiótico que responde a «cuánta agua hay aquí»
 * sin simular una sola gota: el índice @f$ \mathrm{TWI} = \ln(a / \tan\beta) @f$
 * deduce la humedad del suelo de la sola topografía, de modo que los valles con
 * mucha área contribuyente y poca pendiente salen húmedos y las crestas secas.
 * Compone un FField2D —de donde salen la geometría y el muestreo bilineal— y
 * aporta lo propio de la hidrología: rellenado de depresiones, enrutado de flujo
 * D8, acumulación aguas abajo y la fórmula del índice. Es un bake: el relieve no
 * cambia en runtime, así que se calcula una vez y después solo se muestrea.
 *
 * @ingroup eco_terrain
 * @see @ref bib_beven1979
 * @see @ref bib_ocallaghan1984
 * @see @ref bib_barnes2014
 */

#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"

struct FHeightField;

/**
 * Campo de agua causal ligado al relieve: TWI normalizado sobre la rejilla del
 * terreno.
 *
 * El rellenado de depresiones previo al enrutado es lo que hace utilizable el
 * índice: sobre ruido fractal sin rellenar quedan miles de mínimos locales que
 * fragmentan la acumulación en microcuencas inconexas y nunca llega a formarse
 * una red de drenaje continua.
 *
 * Representa la humedad POTENCIAL del terreno, congelada; el nivel disponible en
 * cada tick, con su consumo y su recarga, vive en el pool de recursos.
 *
 * @note El valor absoluto del índice no es comparable con la literatura: el
 *       numerador es el recuento crudo de celdas drenantes y la salida se
 *       reescala linealmente. Lo que conserva significado es su ordenación
 *       espacial.
 * @see FResourcePool
 */
struct PROCEDURALECOSYSTEM_API FWaterField
{
    /** TWI ya normalizado a [0, OutputMax], listo para la función de vigor. */
    FField2D Field;

    /** true si el campo tiene rejilla y datos. */
    bool IsValid() const { return Field.IsValid(); }

    /**
     * Calcula el campo a partir del relieve: rellenado de depresiones, enrutado
     * D8, acumulación de flujo, fórmula del TWI y normalización a [0, OutputMax].
     *
     * @param Height      Relieve del que se deriva; el campo hereda su rejilla.
     * @param OutputMax   Cota superior del campo normalizado. Casa el rango con el
     *                    de los nutrientes para que ambos entren en la función de
     *                    vigor en igualdad de condiciones.
     * @param bFillSinks  Rellena las depresiones antes del D8. A false se enruta
     *                    sobre el relieve crudo, lo que permite comparar la red de
     *                    drenaje con y sin rellenado.
     * @param bRankNormalize Normaliza el TWI por RANGO (percentil espacial) en vez
     *                    de por min-max lineal. El TWI tiene cola larga: en las
     *                    zonas llanas la pendiente del enlace D8 se clava en el
     *                    mínimo numérico y el índice gana ln(1/tan(min)) ≈ 6,9
     *                    unidades de golpe, con lo que llanuras y fondos forman
     *                    una isla de valores extremos separada del cuerpo del
     *                    campo. Con min-max, esa isla se queda sola la mitad alta
     *                    del rango, el 96% del mapa se comprime bajo 0,3·OutputMax
     *                    y ningún óptimo de especie puede alcanzar las llanuras
     *                    sin abandonar el resto del mapa: quedan sin árboles por
     *                    construcción. El rango conserva exacta la ordenación
     *                    espacial —lo único con significado del índice reescalado
     *                    (ver la nota de la clase)— y hace el campo uniforme por
     *                    área, de modo que WaterOptimum = f significa «más húmedo
     *                    que el f por ciento del mapa». A false se conserva la
     *                    normalización lineal para reproducir corridas antiguas.
     * @note Coste O(N log N), dominado por el priority-flood y las ordenaciones
     *       por cota y por rango; se paga una sola vez.
     */
    void BakeFromHeightField(const FHeightField& Height, float OutputMax = 10.f,
        bool bFillSinks = true, bool bRankNormalize = true);

    /** Disponibilidad de agua en el punto de mundo (Xcm, Ycm), por bilineal. */
    FORCEINLINE float SampleWater(double Xcm, double Ycm) const
    {
        return Field.SampleBilinear(Xcm, Ycm);
    }

    /** Rectángulo de mundo que cubre el campo, en cm. */
    FBox2D GetWorldBounds() const { return Field.GetWorldBounds(); }
};