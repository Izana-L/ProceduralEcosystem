/**
 * @file EcoStats.cpp
 * @author Juan Luque Roldán
 * @brief Definición de los contadores de stats y de la categoría CSV declarados en
 *        EcoStats.h.
 *
 * Única unidad de traducción del módulo Core: aporta el símbolo de cada
 * DECLARE_*_EXTERN, sin el cual el enlazado falla, y define la categoría Eco del CSV
 * profiler. No contiene lógica.
 *
 * @ingroup eco_core
 */

#include "Core/EcoStats.h"

DEFINE_STAT(STAT_EcoTickTotal);
DEFINE_STAT(STAT_EcoHash);
DEFINE_STAT(STAT_EcoLight);
DEFINE_STAT(STAT_EcoParallel);
DEFINE_STAT(STAT_EcoReduce);
DEFINE_STAT(STAT_EcoRegen);
DEFINE_STAT(STAT_EcoGermination);
DEFINE_STAT(STAT_EcoPopulation);
DEFINE_STAT(STAT_EcoTicksThisFrame);

DEFINE_STAT(STAT_EcoRelevel);
DEFINE_STAT(STAT_EcoFlushInstances);
DEFINE_STAT(STAT_EcoHeroGen);
DEFINE_STAT(STAT_EcoBake);
DEFINE_STAT(STAT_EcoNumHero);
DEFINE_STAT(STAT_EcoNumInstance);
DEFINE_STAT(STAT_EcoNumImpostor);
DEFINE_STAT(STAT_EcoNumCulled);

// La categoria arranca ACTIVADA: una captura lanzada con Csv.Start o con
// Eco.Frame.Capture sale ya con las columnas del ecosistema, sin tener que
// habilitarla antes con `CsvCategory Eco 1`.
CSV_DEFINE_CATEGORY(Eco, true);
