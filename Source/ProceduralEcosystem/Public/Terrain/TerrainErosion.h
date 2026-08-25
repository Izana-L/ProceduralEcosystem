#pragma once

#include "CoreMinimal.h"

struct FField2D;

/**
 * Erosion de relieve como BAKE unico tras la sintesis de ruido (no corre por
 * tick). Es lo que convierte un fractal "correcto pero sintetico" en un
 * relieve que se lee como real: la hidraulica talla la red de drenaje (valles
 * en V, abanicos de deposito) y la termica relaja las pendientes por encima
 * del angulo de talud (laderas de derrubios).
 *
 * Referencias:
 * - Musgrave, Kolb & Mace (SIGGRAPH 1989): erosion sobre terreno fractal.
 * - Olsen, "Realtime Procedural Terrain Generation" (2004): erosion termica.
 * - Beyer, "Implementation of a method for hydraulic erosion" (2015): el
 *   modelo de gotas que usan World Machine/Gaea y las implementaciones
 *   habituales en juegos.
 *
 * Determinismo: la hidraulica consume un stream xorshift propio derivado de
 * la semilla y recorre las gotas EN SERIE (cada gota ve el resultado de las
 * anteriores; paralelizarlas romperia la reproducibilidad). La termica es
 * gather de dos pasadas con doble buffer: paralela y determinista.
 */
namespace TerrainErosion
{
    /** Erosion termica (Olsen 2004): cada celda reparte material entre los
        vecinos hacia los que su pendiente supera el talud, proporcionalmente
        al exceso de cada uno. */
    struct FThermalParams
    {
        int32 Iterations = 25;

        /** Angulo de reposo (grados). Derrubios naturales: ~30-37. */
        float TalusAngleDeg = 34.f;

        /** Fraccion del exceso sobre el talud movida por iteracion (0..0.8].
            Valores mayores se acotan a 0.8: por encima, los vertidos
            simultaneos sobre un fondo de valle pueden sobrepasarse y oscilar. */
        float Strength = 0.5f;
    };

    /** Erosion hidraulica por gotas (Beyer 2015). Los parametros con unidades
        estan en el espacio NORMALIZADO del modelo (alturas [0,1], XY en
        celdas): son los valores calibrados estandar del metodo. */
    struct FHydraulicParams
    {
        /** Nº de gotas simuladas (0 = off). Mas gotas = drenaje mas marcado. */
        int32 Droplets = 120000;

        /** Escala global de la tasa de arranque de material (0..1]. */
        float Strength = 0.5f;

        int32 MaxLifetime = 30;              // pasos de vida de cada gota
        float Inertia = 0.05f;               // 0 = sigue el gradiente, 1 = recta
        float SedimentCapacityFactor = 4.f;  // capacidad ~ pendiente*velocidad*agua
        float MinSedimentCapacity = 0.01f;   // evita capacidad 0 en llano
        float DepositSpeed = 0.3f;           // fraccion del excedente depositada
        float ErodeSpeed = 0.3f;             // fraccion del deficit arrancada
        float EvaporateSpeed = 0.01f;        // agua perdida por paso
        float Gravity = 4.f;                 // acelera la gota cuesta abajo
        int32 BrushRadius = 3;               // radio (celdas) del pincel de arranque
        float InitialWater = 1.f;
        float InitialSpeed = 1.f;
    };

    /** Aplica erosion termica in-place sobre alturas en cm. */
    PROCEDURALECOSYSTEM_API void ThermalErode(FField2D& HeightCm, const FThermalParams& P);

    /** Aplica erosion hidraulica in-place sobre alturas en cm. Seed abre el
        stream de RNG de las gotas (independiente del resto de streams). */
    PROCEDURALECOSYSTEM_API void HydraulicErode(FField2D& HeightCm, uint32 Seed, const FHydraulicParams& P);
}
