#pragma once

#include "CoreMinimal.h"

/**
 * Un arbol que ha muerto este tick, con los datos que la CAPA DE RENDER necesita
 * para representar su muerte (Fase 5): la caida, el tocon/snag y la hojarasca.
 *
 * La simulacion (UEcosystemSubsystem) va registrando estos eventos en un anillo
 * circular; el render los consume con un cursor monotono y no vuelve a verlos.
 * Es una VISTA de la simulacion (doc. 0, "Propiedad de los datos"): que se
 * consuman o no NO afecta al estado del bosque.
 */
struct FTreeDeathEvent
{
    FVector Position = FVector::ZeroVector; // base del tronco, mundo (cm)
    uint16  SpeciesId = 0;                  // indice en UEcosystemSettings::Species
    uint32  StableId = 0;                   // id estable del arbol muerto (yaw/variante del tocon)
    float   Biomass = 0.f;                  // biomasa al morir (tamano del tocon/hojarasca)
    float   HeightCm = 0.f;                 // altura al morir (para el snag)
    int64   Tick = 0;                       // tick en que murio (edad del evento)
};