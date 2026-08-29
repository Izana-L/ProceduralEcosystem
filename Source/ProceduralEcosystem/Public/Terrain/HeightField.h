/**
 * @file HeightField.h
 * @author Juan Luque Roldán
 * @brief Campo de altura del terreno: la fuente de verdad del relieve para la simulación.
 *
 * Declara FHeightField, el relieve matemático que consulta toda la ecología
 * (altura, normal, extensión del mundo), y FTerrainGenParams, el juego de
 * parámetros con el que se sintetiza. El relieve se hornea una sola vez en el
 * arranque, de forma determinista por semilla: ruido multifractal con recorte de
 * Nyquist, normalización a la amplitud pico-valle pedida y una pasada de erosión
 * hidráulica y térmica. El ALandscape del nivel es únicamente la representación
 * visual de este campo y nunca se consulta desde la simulación.
 *
 * @ingroup eco_terrain
 * @see @ref bib_musgrave1989
 */

#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"
#include "Terrain/EcoNoise.h"
#include "Terrain/TerrainErosion.h"

/**
 * Parámetros de generación del relieve, ya en unidades de mundo.
 *
 * Los rellena el subsistema a partir de UEcosystemSettings, donde están
 * expresados en unidades de editor (metros); aquí todo está en cm.
 *
 * @note Los offsets de ruido no se rellenan a mano: FHeightField::Generate los
 *       deriva de Seed con una sal por componente.
 */
struct PROCEDURALECOSYSTEM_API FTerrainGenParams
{
    /** Nodos de la rejilla en X. */
    int32  Width = 512;

    /** Nodos de la rejilla en Y. */
    int32  Height = 512;

    /** Separación entre nodos (cm). Fija la resolución y con ella el corte de Nyquist. */
    double CellSizeCm = 200.0;

    /** Semilla del relieve; de ella salen todos los streams de ruido y de erosión. */
    uint32 Seed = 12345u;

    /** Amplitud pico-valle (cm) tras normalizar el ruido. */
    double HeightScaleCm = 30000.0;

    /** Forma del ruido: warp + hybrid + ridged (ver EcoNoise). */
    EcoNoise::FTerrainNoiseParams Noise;

    /** Aplica o no la pasada de erosión al terminar el ruido. */
    bool bErosion = true;

    /** Parámetros de la erosión hidráulica de gotas (ver TerrainErosion). */
    TerrainErosion::FHydraulicParams Hydraulic;

    /** Parámetros de la erosión térmica hacia el ángulo de reposo. */
    TerrainErosion::FThermalParams   Thermal;
};

/**
 * Campo de altura: la FUENTE DE VERDAD del relieve para la simulación.
 *
 * Se muestrea desde C++ (altura y normal) de forma barata y determinista, sin
 * depender del ALandscape. El Landscape es solo la representación visual; la
 * ecología consulta este campo, de modo que lo que ve el jugador y lo que sabe
 * la simulación describen el mismo terreno.
 *
 * Compone un FField2D, que aporta la rejilla, el almacenamiento y el muestreo
 * bilineal genéricos; FHeightField añade únicamente lo propio del terreno: la
 * síntesis del relieve y la normal.
 *
 * @note Coordenadas de mundo y alturas en cm (unidades de Unreal).
 */
struct PROCEDURALECOSYSTEM_API FHeightField
{
    /** Rejilla base: aquí viven Width, Height, CellSize, Origin y Data. */
    FField2D Field;

    /** Cierto si el relieve se ha generado y es utilizable. */
    bool IsValid() const { return Field.IsValid(); }

    /**
     * Genera el relieve completo, en tres etapas deterministas por Params.Seed:
     *
     * 1. Ruido compuesto (EcoNoise::TerrainSample): domain warp, hybrid
     *    multifractal como base y crestas ridged en las zonas altas, con recorte
     *    de Nyquist para que ninguna octava baje de 2*CellSizeCm.
     * 2. Normalización del ruido al rango [0, HeightScaleCm].
     * 3. Erosión (TerrainErosion), si Params.bErosion: hidráulica de gotas, que
     *    talla la red de drenaje, y después térmica, que relaja las pendientes
     *    por encima del talud.
     *
     * @note Es un bake: se paga una vez en el arranque y después el campo solo
     *       se muestrea.
     */
    void Generate(const FTerrainGenParams& Params);

    /** Altura del terreno (cm) en la posición de mundo (Xcm, Ycm), bilineal. */
    FORCEINLINE float SampleHeight(double Xcm, double Ycm) const
    {
        return Field.SampleBilinear(Xcm, Ycm);
    }

    /**
     * Normal del terreno en (Xcm, Ycm): vector unitario con Z hacia arriba.
     *
     * @pre El relieve tiene que estar generado; sobre un campo vacío devuelve la
     *      vertical.
     */
    FVector SampleNormal(double Xcm, double Ycm) const;

    /** Extensión del terreno en mundo (cm). */
    FBox2D GetWorldBounds() const { return Field.GetWorldBounds(); }
};
