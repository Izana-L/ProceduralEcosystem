// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * @file ProceduralEcosystem.cpp
 * @author Juan Luque Roldán
 * @brief Punto de entrada del módulo primario de juego.
 *
 * Registra el módulo con la implementación por defecto del motor: no hay
 * StartupModule ni ShutdownModule propios porque el simulador se inicializa y se
 * detiene a través de sus subsistemas, cuyo ciclo de vida gestiona el propio motor.
 *
 * @ingroup eco_core
 */

#include "ProceduralEcosystem.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_PRIMARY_GAME_MODULE( FDefaultGameModuleImpl, ProceduralEcosystem, "ProceduralEcosystem" );
