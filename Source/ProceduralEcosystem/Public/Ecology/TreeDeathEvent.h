/**
 * @file TreeDeathEvent.h
 * @author Juan Luque Roldán
 * @brief Registro de la muerte de un árbol, tal y como lo consume la capa de render.
 *
 * Declara FTreeDeathEvent, el POD que la simulación emite cuando un árbol muere y que
 * lleva lo justo para representar el suceso: dónde estaba, de qué especie era, cuánta
 * biomasa y qué altura tenía y en qué tick ocurrió. UEcosystemSubsystem los acumula en un
 * anillo circular y la capa de render los consume con un cursor monótono para generar la
 * caída, el tocón y la hojarasca.
 *
 * @ingroup eco_ecology
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Un árbol muerto en un tick, con los datos que la capa de render necesita para
 * representar su muerte: la caída, el tocón y la hojarasca.
 *
 * Es una vista de la simulación, no parte de su estado: que los eventos se consuman o se
 * pierdan al girar el anillo degrada el efecto visual, pero no altera el bosque.
 */
struct FTreeDeathEvent
{
    FVector Position = FVector::ZeroVector; ///< Base del tronco, en cm de mundo.
    uint16  SpeciesId = 0;                  ///< Índice en UEcosystemSettings::Species.
    uint32  StableId = 0;                   ///< Id estable: fija yaw y variante del tocón.
    float   Biomass = 0.f;                  ///< Biomasa al morir: tamaño del tocón y hojarasca.
    float   HeightCm = 0.f;                 ///< Altura al morir, para el snag.
    int64   Tick = 0;                       ///< Tick en que murió; da la edad del evento.
};