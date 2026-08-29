/**
 * @file EcoStats.h
 * @author Juan Luque Roldán
 * @brief Declaración de toda la instrumentación del simulador: grupos de stats,
 *        ámbitos de Unreal Insights y categoría del CSV profiler.
 *
 * Conecta el proyecto con las tres vías oficiales de medición del motor y las mapea
 * una a una sobre las etapas del bucle de tick y del gestor de niveles de representación,
 * de modo que leer `stat EcoSim` equivale a leer el diagrama de flujo del simulador. Los
 * contadores se declaran aquí con PROCEDURALECOSYSTEM_API y se definen en EcoStats.cpp;
 * los ámbitos de traza se abren en los puntos de uso.
 *
 * @par Las tres vías y cómo se leen
 * @li Grupos de stats: contadores por frame en pantalla con `stat EcoSim` y
 *     `stat EcoRender`, junto a `stat Unit`, `stat GPU` y `stat RHI`. Primer vistazo
 *     para situar el cuello en el game thread o en el render thread.
 * @li Unreal Insights: los mismos ámbitos, abiertos con TRACE_CPUPROFILER_EVENT_SCOPE,
 *     salen anidados en la timeline y dan el reparto real dentro de un tick. Se captura
 *     lanzando el editor con `-trace=cpu,frame,bookmark,counters`.
 * @li CSV profiler: la categoría Eco escribe una fila por frame a un .csv
 *     (`CsvCategory Eco 1`, `Csv.Start` / `Csv.Stop`, o `Eco.Frame.Capture N`).
 *
 * @note Las tres se compilan a nada en Shipping o con las stats desactivadas, y no
 *       consumen RNG ni tocan estado: activarlas no altera el bosque resultante.
 *
 * @ingroup eco_core
 * @see @ref bib_epicueperfilado
 */

#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// -----------------------------------------------------------------------------
//  Grupos de stats (consola: `stat EcoSim`, `stat EcoRender`)
// -----------------------------------------------------------------------------
DECLARE_STATS_GROUP(TEXT("EcoSim"), STATGROUP_EcoSim, STATCAT_Advanced);
DECLARE_STATS_GROUP(TEXT("EcoRender"), STATGROUP_EcoRender, STATCAT_Advanced);

// --- Simulacion: una entrada por etapa del bucle de tick ---
DECLARE_CYCLE_STAT_EXTERN(TEXT("Tick (total)"), STAT_EcoTickTotal, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Spatial hash"), STAT_EcoHash, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Grid de luz grueso"), STAT_EcoLight, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Paso paralelo"), STAT_EcoParallel, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Reduccion serial"), STAT_EcoReduce, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Regeneracion de campos"), STAT_EcoRegen, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Muertes + germinacion"), STAT_EcoGermination, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Arboles vivos"), STAT_EcoPopulation, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Ticks por frame"), STAT_EcoTicksThisFrame, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);

// --- Render: etapas y poblaciones del gestor de niveles de representación ---
DECLARE_CYCLE_STAT_EXTERN(TEXT("Re-nivelado de LOD"), STAT_EcoRelevel, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Flush de instancias"), STAT_EcoFlushInstances, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Generacion de hero trees"), STAT_EcoHeroGen, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Horneado de arquetipos"), STAT_EcoBake, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Hero trees"), STAT_EcoNumHero, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Instancias"), STAT_EcoNumInstance, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Impostors"), STAT_EcoNumImpostor, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Fuera de rango"), STAT_EcoNumCulled, STATGROUP_EcoRender, PROCEDURALECOSYSTEM_API);

// -----------------------------------------------------------------------------
//  Categoria del CSV profiler (una columna por metrica, una fila por frame)
// -----------------------------------------------------------------------------
CSV_DECLARE_CATEGORY_EXTERN(Eco);
