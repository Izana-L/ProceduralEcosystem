#pragma once

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

/**
 * =============================================================================
 *  INSTRUMENTACION DEL PROYECTO (Fase 6, doc. 6.4: "medir primero, optimizar
 *  despues").
 * =============================================================================
 *
 * Hasta la Fase 5 la unica medida era Eco.Profile: una media exponencial hecha
 * a mano con FPlatformTime::Seconds(). Sirve para saber si un tick cuesta 2 o 20
 * ms, pero NO se ve en las herramientas del motor, que es donde de verdad se
 * decide si el cuello es CPU o GPU. Este fichero conecta el proyecto con las
 * tres vias oficiales:
 *
 *   1. STAT GROUPS  -> consola: `stat EcoSim`, `stat EcoRender`.
 *      Contadores por frame superpuestos en pantalla, junto a `stat Unit`,
 *      `stat GPU` y `stat RHI`. Es el primer vistazo del "orden de ataque" del
 *      doc. 6.4 (¿game thread o render thread?).
 *
 *   2. UNREAL INSIGHTS -> TRACE_CPUPROFILER_EVENT_SCOPE.
 *      Los mismos ambitos aparecen como bloques nombrados en la timeline de
 *      Insights, anidados: se ve el reparto REAL dentro de un tick y cuanto
 *      ocupa el tick dentro del frame. Se captura con:
 *         UnrealEditor.exe <Proyecto> -trace=cpu,frame,bookmark,counters
 *
 *   3. CSV PROFILER -> categoria "Eco".
 *      Escribe una fila por frame a un .csv (consola: `CsvCategory Eco 1` y
 *      `Csv.Start` / `Csv.Stop`, o Eco.Frame.Capture N). Es EXACTAMENTE el
 *      material del capitulo de resultados de la Fase 7: curvas de framerate
 *      frente al numero de arboles, sin post-procesar logs a mano.
 *
 * COSTE: los tres se compilan a nada en Shipping (o con stats desactivadas), y
 * en Development son unos pocos nanosegundos por ambito. No cambia el resultado
 * de la simulacion: la instrumentacion NO consume RNG ni toca estado.
 */

// -----------------------------------------------------------------------------
//  Grupos de stats (consola: `stat EcoSim`, `stat EcoRender`)
// -----------------------------------------------------------------------------
DECLARE_STATS_GROUP(TEXT("EcoSim"), STATGROUP_EcoSim, STATCAT_Advanced);
DECLARE_STATS_GROUP(TEXT("EcoRender"), STATGROUP_EcoRender, STATCAT_Advanced);

// --- Simulacion: las cinco etapas del bucle de tick (doc. 2.5) ---
DECLARE_CYCLE_STAT_EXTERN(TEXT("Tick (total)"), STAT_EcoTickTotal, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Spatial hash"), STAT_EcoHash, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Grid de luz grueso"), STAT_EcoLight, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Paso paralelo"), STAT_EcoParallel, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Reduccion serial"), STAT_EcoReduce, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Regeneracion de campos"), STAT_EcoRegen, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Muertes + germinacion"), STAT_EcoGermination, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Arboles vivos"), STAT_EcoPopulation, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Ticks por frame"), STAT_EcoTicksThisFrame, STATGROUP_EcoSim, PROCEDURALECOSYSTEM_API);

// --- Render: el gestor de LOD (doc. 4.3/4.4) ---
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
