// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * @file ProceduralEcosystem.h
 * @author Juan Luque Roldán
 * @brief Declaración del módulo primario: no aporta tipos propios, solo la pareja .h/.cpp
 *        que exige UnrealBuildTool.
 *
 * El módulo no declara tipos ni interfaz propios: se mantiene por la convención de
 * UnrealBuildTool, que espera la pareja .h/.cpp del módulo primario. Todo el ciclo de
 * vida del simulador cuelga de subsistemas del motor (UEcosystemSubsystem,
 * TreeRenderSubsystem, TreeSoilSubsystem) y no del módulo.
 *
 * @ingroup eco_core
 */

#pragma once

#include "CoreMinimal.h"

