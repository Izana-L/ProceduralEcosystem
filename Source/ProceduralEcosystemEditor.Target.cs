// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * @file ProceduralEcosystemEditor.Target.cs
 * @author Juan Luque Roldán
 * @brief Reglas del objetivo de compilación de editor: el simulador cargado dentro de Unreal.
 *
 * Gemelo del objetivo de juego salvo por el tipo: enlaza el mismo módulo runtime
 * `ProceduralEcosystem` dentro del editor y repite el mismo versionado de compatibilidad,
 * para que el binario de editor y el de juego se compilen con idéntica semántica y una
 * corrida en PIE valga como ensayo de la corrida cocinada. No añade ningún módulo
 * editor-only propio porque el proyecto no tiene código de editor separado: las tareas de
 * autoría se resuelven desde runtime, como la exportación del relieve a PNG de 16 bits que
 * después se importa a mano como heightmap del Landscape.
 *
 * @ingroup eco_build
 * @see @ref bib_epicuebuild
 */

using UnrealBuildTool;
using System.Collections.Generic;

/** Objetivo de compilación de editor: carga el módulo del simulador dentro de Unreal Editor. */
public class ProceduralEcosystemEditorTarget : TargetRules
{
	/** Fija el tipo de objetivo, el versionado de compatibilidad y los módulos a enlazar. */
	public ProceduralEcosystemEditorTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;

		// Mismos valores que el objetivo de juego: los dos binarios comparten la semántica
		// de compilación de UE 5.7 en vez de los modos de compatibilidad heredados.
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;

		// Mismo módulo runtime que el objetivo de juego; no hay módulo editor-only.
		ExtraModuleNames.Add("ProceduralEcosystem");
	}
}
