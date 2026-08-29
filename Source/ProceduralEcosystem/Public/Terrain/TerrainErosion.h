/**
 * @file TerrainErosion.h
 * @author Juan Luque Roldán
 * @brief Erosión hidráulica y térmica del relieve, aplicada como bake posterior al ruido.
 *
 * Declara el namespace TerrainErosion y los dos juegos de parámetros que lo
 * gobiernan. Es la pasada que convierte un fractal estadísticamente homogéneo en
 * un relieve que se lee como real: la erosión hidráulica por gotas talla la red
 * de drenaje —valles en V, abanicos de depósito— y la térmica relaja hacia el
 * ángulo de reposo toda pendiente que lo supere, dejando laderas de derrubios.
 * Trabaja in situ sobre el campo de alturas del relieve y no conoce el ruido que
 * lo generó: FHeightField::Generate encadena la hidráulica y después la térmica
 * sobre el campo ya normalizado a la amplitud pedida.
 *
 * @ingroup eco_terrain
 * @see @ref bib_musgrave1989
 * @see @ref bib_olsen2004
 * @see @ref bib_beyer2015
 */

#pragma once

#include "CoreMinimal.h"

struct FField2D;

/**
 * Erosión del relieve: dos algoritmos independientes que se encadenan sobre un
 * campo de alturas ya sintetizado.
 *
 * Es un bake, no una simulación por tick: se paga una vez en el arranque y el
 * resultado queda congelado en el relieve.
 *
 * Contrato de determinismo: la hidráulica consume un stream xorshift propio
 * derivado de la semilla y recorre las gotas EN SERIE, porque cada gota ve el
 * terreno que dejaron las anteriores y paralelizarlas rompería la
 * reproducibilidad. La térmica es un gather de dos pasadas con doble buffer, así
 * que corre en ParallelFor y da el mismo resultado con cualquier número de hilos.
 */
namespace TerrainErosion
{
    /**
     * Parámetros de la erosión térmica: relajación iterativa del relieve hacia el
     * ángulo de reposo del material suelto.
     *
     * @see @ref bib_olsen2004
     */
    struct FThermalParams
    {
        /** Iteraciones de relajación. Cada una acerca las laderas al talud. */
        int32 Iterations = 25;

        /** Ángulo de reposo (grados). Derrubios naturales: ~30-37. */
        float TalusAngleDeg = 34.f;

        /** Fracción del exceso sobre el talud movida por iteración (0..0.8].
            Valores mayores se acotan a 0.8: por encima, los vertidos
            simultáneos sobre un fondo de valle pueden sobrepasarse y oscilar. */
        float Strength = 0.5f;
    };

    /**
     * Parámetros del modelo de gotas, con los valores calibrados del método.
     *
     * Las magnitudes con unidades están en el espacio NORMALIZADO en el que
     * trabaja el algoritmo (alturas en [0,1], XY en celdas), no en centímetros de
     * mundo: HydraulicErode normaliza el campo antes de simular.
     *
     * @see @ref bib_beyer2015
     */
    struct FHydraulicParams
    {
        int32 Droplets = 120000;             ///< Gotas simuladas; 0 desactiva la pasada.
        float Strength = 0.5f;               ///< Escala la tasa de arranque (0..1].
        int32 MaxLifetime = 30;              ///< Pasos de vida de cada gota.
        float Inertia = 0.05f;               ///< 0 sigue el gradiente, 1 va en recta.
        float SedimentCapacityFactor = 4.f;  ///< Capacidad ~ pendiente*velocidad*agua.
        float MinSedimentCapacity = 0.01f;   ///< Capacidad mínima; el llano no queda a 0.
        float DepositSpeed = 0.3f;           ///< Fracción del excedente depositada.
        float ErodeSpeed = 0.3f;             ///< Fracción del déficit arrancada.
        float EvaporateSpeed = 0.01f;        ///< Agua perdida por paso, en tanto por uno.
        float Gravity = 4.f;                 ///< Convierte desnivel en velocidad.
        int32 BrushRadius = 3;               ///< Radio del pincel de arranque, en celdas.
        float InitialWater = 1.f;            ///< Agua con la que nace cada gota.
        float InitialSpeed = 1.f;            ///< Velocidad con la que nace cada gota.
    };

    /**
     * Aplica erosión térmica in situ sobre un campo de alturas en cm.
     *
     * En cada iteración, toda celda cuya pendiente con un vecino supera el talud
     * cede material, repartido entre los vecinos en exceso proporcionalmente al
     * de cada uno.
     *
     * @param HeightCm Alturas en cm; se modifican en su sitio.
     * @param P        Ángulo de reposo, iteraciones e intensidad del vertido.
     * @note No hace nada si el campo no es válido, si Iterations <= 0 o si
     *       Strength <= 0.
     * @see @ref bib_olsen2004
     */
    PROCEDURALECOSYSTEM_API void ThermalErode(FField2D& HeightCm, const FThermalParams& P);

    /**
     * Aplica erosión hidráulica por gotas in situ sobre un campo de alturas en cm.
     *
     * Normaliza internamente las alturas a [0,1], simula las gotas y devuelve el
     * campo a centímetros con el mismo factor, sin volver a normalizar: el
     * encogimiento del rango —picos rebajados, valles rellenados— es el resultado
     * físico de la pasada.
     *
     * @param HeightCm Alturas en cm; se modifican en su sitio.
     * @param Seed     Abre el stream xorshift de las gotas, independiente del
     *                 resto de streams del proyecto.
     * @param P        Parámetros del modelo, en espacio normalizado.
     * @note No hace nada si el campo no es válido, si es plano (rango nulo), si
     *       Droplets <= 0 o si Strength <= 0.
     * @see @ref bib_beyer2015
     */
    PROCEDURALECOSYSTEM_API void HydraulicErode(FField2D& HeightCm, uint32 Seed, const FHydraulicParams& P);
}
