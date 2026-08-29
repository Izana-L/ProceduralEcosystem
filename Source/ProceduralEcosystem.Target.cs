// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * @file ProceduralEcosystem.Target.cs
 * @author Juan Luque Roldán
 * @brief Reglas del objetivo de compilación de juego: el ejecutable del simulador sin editor.
 *
 * Declara a UnrealBuildTool qué es el binario de juego del proyecto: tipo de objetivo,
 * versionado de los valores por defecto de compilación, semántica de includes y módulos
 * que hay que enlazar. Todo el simulador —terreno, ecología, geometría y render— vive en
 * un único módulo de tipo runtime, `ProceduralEcosystem`, de modo que la lista de módulos
 * tiene una sola entrada: los grupos de la arquitectura son capas lógicas, no módulos del
 * sistema de compilación. Anclar el objetivo a los valores modernos de UE 5.7 en lugar de
 * a los modos de compatibilidad heredados es lo que hace que el proyecto se compile con la
 * misma semántica en cualquier máquina que tenga esa versión del motor.
 *
 * @ingroup eco_build
 * @see @ref bib_epicuebuild
 */

using UnrealBuildTool;
using System.Collections.Generic;

/** Objetivo de compilación de juego: ejecutable standalone o cocinado, sin código de editor. */
public class ProceduralEcosystemTarget : TargetRules
{
	/** Fija el tipo de objetivo, el versionado de compatibilidad y los módulos a enlazar. */
	public ProceduralEcosystemTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;

		// Valores por defecto de compilación y orden de includes de UE 5.7. Sin fijarlos,
		// UnrealBuildTool aplicaría los modos de compatibilidad de versiones anteriores.
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;

		// Módulo runtime único que contiene todo el simulador.
		ExtraModuleNames.Add("ProceduralEcosystem");
	}
}
