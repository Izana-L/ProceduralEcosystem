#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"
#include "Terrain/EcoNoise.h"
#include "Terrain/TerrainErosion.h"

/**
 * Parametros de generacion del relieve. Los rellena el subsistema desde
 * UEcosystemSettings (alli estan documentados en unidades de editor, metros);
 * aqui todo esta ya en cm. Los offsets de ruido NO se rellenan a mano:
 * Generate los deriva de Seed.
 */
struct PROCEDURALECOSYSTEM_API FTerrainGenParams
{
    int32  Width = 512;
    int32  Height = 512;
    double CellSizeCm = 200.0;
    uint32 Seed = 12345u;

    /** Amplitud pico-valle (cm) tras normalizar el ruido. */
    double HeightScaleCm = 30000.0;

    /** Forma del ruido: warp + hybrid + ridged (ver EcoNoise). */
    EcoNoise::FTerrainNoiseParams Noise;

    /** Erosion (bake unico tras el ruido; ver TerrainErosion). */
    bool bErosion = true;
    TerrainErosion::FHydraulicParams Hydraulic;
    TerrainErosion::FThermalParams   Thermal;
};

/**
 * Campo de altura: la FUENTE DE VERDAD del relieve para la simulacion.
 *
 * Se muestrea en C++ (altura / pendiente / normal) de forma barata y
 * determinista, sin depender del ALandscape. El Landscape es solo la
 * representacion VISUAL; la simulacion consulta este FHeightField para que
 * lo que ve el jugador y lo que "sabe" la ecologia sean coherentes.
 *
 * Implementacion: compone un FField2D (Field) que aporta la rejilla, el
 * almacenamiento y el muestreo bilineal genericos. FHeightField solo anade
 * lo especifico de terreno: la sintesis de relieve y la normal.
 *
 * Pipeline de Generate (todo determinista por Seed):
 *   1. Ruido compuesto (EcoNoise::TerrainSample): domain warp de Quilez +
 *      hybrid multifractal de Musgrave como base + crestas ridged en las
 *      zonas altas. Con recorte de Nyquist: ninguna octava baja de 2*CellSize.
 *   2. Normalizacion al rango real -> [0, HeightScaleCm].
 *   3. Erosion (TerrainErosion): hidraulica de gotas (talla el drenaje) y
 *      termica (relaja pendientes sobre el talud).
 *
 * Unidades: coordenadas de mundo en cm (unidades de Unreal). La altura
 * tambien en cm.
 */
struct PROCEDURALECOSYSTEM_API FHeightField
{
    /** Rejilla base: aqui viven Width/Height/CellSize/Origin/Data. */
    FField2D Field;

    bool IsValid() const { return Field.IsValid(); }

    /** Genera el relieve completo (ruido + erosion) segun Params. */
    void Generate(const FTerrainGenParams& Params);

    /** Altura del terreno (cm) en mundo (Xcm, Ycm), interpolacion bilineal. */
    FORCEINLINE float SampleHeight(double Xcm, double Ycm) const
    {
        return Field.SampleBilinear(Xcm, Ycm);
    }

    /** Normal del terreno (unitaria, Z hacia arriba). */
    FVector SampleNormal(double Xcm, double Ycm) const;

    /** Extension del terreno en mundo (cm). */
    FBox2D GetWorldBounds() const { return Field.GetWorldBounds(); }
};
