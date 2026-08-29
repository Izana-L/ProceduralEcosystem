#pragma once

#include "CoreMinimal.h"

/**
 * AUDITORIA ESTATICA DEL CONJUNTO DE ESPECIES (consola: Eco.AuditarEspecies).
 *
 * Responde, SIN correr la simulacion, a la pregunta "por que acaba ganando una
 * sola especie". No mira el bosque: mira los numeros con los que arranca, que
 * es donde el resultado ya esta decidido en buena parte.
 *
 * Comprueba cuatro cosas, en este orden:
 *
 *  1. ESTRES A PLENO SOL. El factor de luz es fL = Q/(Q + Kl), con
 *     Kl = LightHalfSaturationMax*(1 - ShadeTolerance) y Q como maximo
 *     FullSunlight (= 1). Si el fL maximo de una especie no llega al umbral de
 *     estres (UEcosystemSettings::StressVigorThreshold), esa especie acumula
 *     estres a pleno sol, en suelo perfecto y SIN un solo vecino: se muere
 *     sola, y lo que estas viendo no es competencia sino una condena por
 *     construccion.
 *
 *  2. SOSTENIBILIDAD DEL POZO. Un adulto retira Biomasa*Demanda por año
 *     repartido en su disco radicular, mientras la celda se recarga con
 *     Tasa*(Base - Actual). Si la demanda supera a la recarga maxima, el pozo
 *     se clava en cero bajo cada adulto y la competencia degenera en una
 *     carrera al fondo que gana, de forma monotona, quien menos consume.
 *
 *  3. SOLAPE RAIZ/COPA. Si el radio radicular es mucho menor que el de copa,
 *     la competencia subterranea apenas se solapa entre vecinos mientras la
 *     sombra se solapa mucho: la luz pasa a ser el unico eje real.
 *
 *  4. DOMINANCIA. Si una especie es mejor o igual que otra en TODOS los ejes
 *     monotonos del modelo, la exclusion competitiva esta garantizada y el
 *     ganador esta decidido antes del primer tick. Es el diagnostico mas
 *     importante de los cuatro, y el unico que ningun ajuste de numeros
 *     arregla: hace falta un compromiso real entre rasgos.
 *
 * Solo LEE (settings + assets de especie). No toca estado de simulacion, no
 * consume RNG y no necesita mundo: se puede llamar desde el editor sin PIE.
 */
namespace EcoSpeciesAudit
{
    /** Ejecuta las cuatro comprobaciones y las vuelca al log (LogEcoAudit). */
    PROCEDURALECOSYSTEM_API void RunAndLog();
}
