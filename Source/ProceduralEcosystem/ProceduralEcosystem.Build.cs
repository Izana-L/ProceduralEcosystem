// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * @file ProceduralEcosystem.Build.cs
 * @author Juan Luque Roldán
 * @brief Reglas del módulo runtime único del proyecto: modo de PCH y dependencias del motor.
 *
 * UnrealBuildTool lee estas reglas una vez por objetivo para saber qué módulos del motor
 * necesita el simulador y con qué visibilidad enlazarlos. La lista es deliberadamente corta:
 * el núcleo de UObject y de mundo, el sistema de configuración del que cuelga
 * `UEcosystemSettings`, RenderCore y RHI para volcar los campos del terreno a textura, y las
 * utilidades de malla con las que se construye geometría de árbol en runtime. Todo el código
 * del proyecto se compila aquí dentro: los grupos de la arquitectura son capas lógicas de un
 * mismo módulo, no módulos separados del sistema de compilación.
 *
 * @note `InputCore` y `Landscape` figuran en la lista pública pero ningún fichero del módulo
 *       los incluye: el proyecto no lee entrada por `FKey` y trata el `ALandscape` como mera
 *       representación visual del relieve, cuya fuente de verdad es `FHeightField`.
 *
 * @ingroup eco_build
 * @see @ref bib_epicuebuild
 */

using UnrealBuildTool;

/** Reglas del módulo `ProceduralEcosystem`, de tipo runtime, que contiene el simulador entero. */
public class ProceduralEcosystem : ModuleRules
{
	/** Fija el modo de cabeceras precompiladas y declara las dependencias del motor. */
	public ProceduralEcosystem(ReadOnlyTargetRules Target) : base(Target)
	{
		// Cada .cpp declara sus propios includes en vez de heredarlos de una PCH monolítica
		// del módulo, que es lo coherente con el orden de includes de UE 5.7 de los objetivos.
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
         {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "DeveloperSettings",        // UEcosystemSettings: tabla de calibración desde el .ini
            "RenderCore",               // volcado de los campos a textura en FieldVisualizer
            "RHI",
            "Landscape",
            "ProceduralMeshComponent",  // malla generada del hero tree (AHeroTreeActor)
            "MeshDescription",          // TreeMeshBaker: bake de un UStaticMesh en runtime
            "StaticMeshDescription"     // atributos de vértice de esa malla (FStaticMeshAttributes)
        });

        // Privada porque solo la usa la exportación del relieve, que codifica el heightfield
        // como PNG de 16 bits en escala de grises para importarlo como heightmap del Landscape.
        PrivateDependencyModuleNames.AddRange(new string[] { "ImageWrapper" });
    }
}
