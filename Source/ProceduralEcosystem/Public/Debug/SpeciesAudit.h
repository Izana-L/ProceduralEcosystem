/**
 * @file SpeciesAudit.h
 * @author Juan Luque Roldán
 * @brief Auditoría estática del catálogo de especies: por qué una calibración degenera en
 *        monocultivo, sin correr la simulación.
 *
 * Declara el punto de entrada de la auditoría, que responde a la pregunta «por qué acaba
 * ganando una sola especie» mirando solo los parámetros de entrada: los ajustes del proyecto
 * y los assets de especie. Busca las cuatro patologías que deciden el resultado antes del
 * primer tick —condena por construcción en el eje de luz, pozo de recurso insostenible,
 * desacople entre raíz y copa y dominancia de una especie sobre otra— y las vuelca al log.
 *
 * @ingroup eco_debug
 * @see @ref bib_exclusioncompetitiva
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Auditoría estática del conjunto de especies (consola: `Eco.AuditarEspecies`).
 *
 * No mira el bosque: mira los números con los que arranca, que es donde buena parte del
 * resultado ya está decidida. Comprueba cuatro cosas, en este orden:
 *
 * @li Estrés a pleno sol: la especie cuyo factor de luz máximo no alcanza el umbral de
 *     estrés se muere sola, en suelo perfecto y sin un solo vecino. Eso no es competencia,
 *     es una condena por construcción.
 * @li Sostenibilidad del pozo: si la demanda anual de un adulto supera a la recarga de las
 *     celdas que cubre su disco radicular, el recurso se clava en cero bajo cada árbol y la
 *     competencia degenera en una carrera al fondo que gana quien menos consume.
 * @li Solape raíz/copa: con la raíz mucho más corta que la copa, la sombra se solapa entre
 *     vecinos y el suelo no, y la luz queda como único eje de competencia real.
 * @li Dominancia: si una especie gana o empata a otra en todos los ejes monótonos, la
 *     exclusión competitiva está garantizada. Es el único hallazgo que no arregla ningún
 *     ajuste de números: hace falta un compromiso entre rasgos.
 *
 * @note Solo lee ajustes y assets de especie, así que no toca estado de simulación, no
 *       consume aleatoriedad y no necesita mundo: se ejecuta desde el editor y se repite
 *       sin efectos secundarios.
 */
namespace EcoSpeciesAudit
{
    /** Ejecuta las comprobaciones y vuelca el informe completo a la categoría LogEcoAudit. */
    PROCEDURALECOSYSTEM_API void RunAndLog();
}
