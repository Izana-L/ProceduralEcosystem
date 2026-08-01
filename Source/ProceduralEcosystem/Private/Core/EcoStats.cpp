#include "Core/EcoStats.h"

// Definicion (una sola unidad de traduccion) de los contadores declarados en
// EcoStats.h. Sin este .cpp los DECLARE_*_EXTERN quedan sin simbolo y el
// enlazado falla.

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

// La categoria arranca ACTIVADA: si el usuario lanza una captura con Csv.Start
// (o con Eco.Frame.Capture) sale con las columnas del ecosistema puestas, sin
// tener que acordarse de `CsvCategory Eco 1`.
CSV_DEFINE_CATEGORY(Eco, true);
