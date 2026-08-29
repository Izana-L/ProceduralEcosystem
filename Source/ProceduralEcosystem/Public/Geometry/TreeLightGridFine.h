/**
 * @file TreeLightGridFine.h
 * @author Juan Luque Roldán
 * @brief Rejilla de luz fina y local a un solo árbol: sombra de los vecinos y autopoda de la copa.
 *
 * Declara FTreeLightGridFine, la resolución fina de la rejilla de luz del proyecto: la copa de
 * un árbol cabe en una caja de pocos metros, así que a 25-50 cm por vóxel son decenas de
 * miles de celdas y solo la tienen los hero trees. Cumple dos papeles, ambos por poda de
 * atractores más que por un término de fototropismo a mano: importa la sombra de los
 * vecinos desde la rejilla gruesa global (@ref FLightFieldCoarse), de modo que un árbol
 * pegado a otro grande crece ladeado, y acumula la sombra del follaje propio, de modo que
 * las ramas interiores dejan de alargarse. Su gradiente es además el término de
 * fototropismo de la colonización del espacio, y su muestreo alimenta la oclusión de copa
 * que el mallador escribe por vértice.
 *
 * @ingroup eco_geometry
 * @see @ref bib_palubicki2009
 * @see @ref bib_mech1996
 */

#pragma once

#include "CoreMinimal.h"
#include "Core/GridMath.h"

struct FLightFieldCoarse; // Terrain/LightFieldCoarse.h (se incluye en el .cpp)
struct FTreeSkeleton;      // Geometry/TreeSkeleton.h    (se incluye en el .cpp)

/**
 * Rejilla 3D de vóxeles con la sombra acumulada alrededor de un solo árbol.
 *
 * La rejilla es regular y no un octree porque la cascada de sombra hacia abajo es natural
 * en celdas uniformes y porque el mismo esquema de indexado sirve de índice espacial; el
 * de atractores vive en FAttractorCloud, aquí solo la luz.
 *
 * Convención: Shadow es sombra acumulada por vóxel, 0 = sin sombra, y la luz disponible
 * sale de restarla al techo, @f$Q = \max(FullSunlight - Shadow,\ 0)@f$. Todos los
 * muestreadores trabajan en CENTROS de vóxel.
 *
 * @note La resta lineal es una desviación deliberada respecto a la rejilla gruesa, que
 *       atenúa por Beer-Lambert: a escala de un árbol la sombra es una oclusión
 *       geométrica acotada y lo que se busca es un gradiente barato.
 *       @see EcoGrid::LightFromShadow
 */
struct PROCEDURALECOSYSTEM_API FTreeLightGridFine
{
    int32 Width = 0;  ///< Vóxeles en X.
    int32 Height = 0; ///< Vóxeles en Y.
    int32 Layers = 0; ///< Vóxeles en Z (altura).

    float VoxelSizeCm = 35.f;                   ///< Arista del vóxel cúbico, en cm (uso: 25-50).
    FVector OriginWorld = FVector::ZeroVector;  ///< Esquina mínima del vóxel (0,0,0), en cm.

    TArray<float> Shadow;                       ///< Sombra acumulada por vóxel; 0 = sin sombra.

    static constexpr float FullSunlight = EcoGrid::FullSunlight; ///< Techo de luz compartido.

    /** true si las dimensiones son positivas y el buffer de sombra las respeta. */
    bool IsValid() const
    {
        return Width > 0 && Height > 0 && Layers > 0
            && VoxelSizeCm > 0.f
            && Shadow.Num() == Width * Height * Layers;
    }

    /**
     * Dimensiona la rejilla para cubrir una caja de mundo y deja la sombra a 0.
     *
     * @param WorldBounds   Caja a cubrir en cm de mundo, normalmente la envolvente de copa
     *                      que construye @ref SpaceColonization::GrowTree.
     * @param InVoxelSizeCm Arista del vóxel; se fuerza a un mínimo de 1 cm.
     * @param PaddingCm     Margen añadido por cada lado antes de dimensionar.
     * @note Con una caja inválida la rejilla queda vacía, IsValid() pasa a false y los
     *       muestreadores devuelven luz plena.
     */
    void InitForBounds(const FBox& WorldBounds, float InVoxelSizeCm, float PaddingCm = 0.f);

    /** Pone toda la sombra a 0. */
    void ClearShadow();

    /**
     * ESTABLECE la sombra base de cada vóxel a partir de la luz que la rejilla gruesa
     * global da en su centro: es la sombra que proyectan los VECINOS.
     *
     * Sobrescribe en vez de acumular, con lo que deja la base sobre la que
     * DepositLeafShadow añade después el follaje propio. Un árbol pegado a un vecino
     * grande encuentra sus atractores de ese lado ya en sombra y crece ladeado.
     */
    void SeedFromCoarse(const FLightFieldCoarse& Coarse);

    /**
     * AÑADE la sombra del follaje propio: una columna descendente por cada nodo del
     * esqueleto. Es lo que hace emerger la autopoda de la copa interior.
     *
     * @param RadiusCm       Radio horizontal de la sombra de un nodo.
     * @param DepthCm        Alcance hacia abajo de esa sombra.
     * @param PerNodeDensity Sombra que aporta un nodo en el centro de su columna.
     * @pre Se llama tras SeedFromCoarse o ClearShadow, en cada refresco de luz.
     */
    void DepositLeafShadow(const FTreeSkeleton& Skeleton, float RadiusCm, float DepthCm, float PerNodeDensity);

    /**
     * Primitiva de DepositLeafShadow: añade bajo FromWorld una columna de sombra de radio
     * RadiusCm y DepthCm de alcance, con caída lineal en radio y en profundidad. Solo
     * sombrea hacia abajo, porque la luz llega de arriba.
     */
    void DepositDownwardShadow(const FVector& FromWorld, float RadiusCm, float DepthCm, float Density);

    /** Luz disponible Q en WorldPos por vecino más cercano; FullSunlight si no hay rejilla. */
    float SampleLight(const FVector& WorldPos) const;

    /** Igual, pero interpolando entre los ocho vóxeles vecinos: la versión suave que piden
        el gradiente de luz y la oclusión de copa. */
    float SampleLightSmooth(const FVector& WorldPos) const;

    /** true si la luz en WorldPos queda por debajo de LightThreshold. Es el criterio con
        el que la generación descarta por sombra los atractores de la copa. */
    bool IsShaded(const FVector& WorldPos, float LightThreshold) const;

    /**
     * Gradiente de la luz suavizada en WorldPos, por diferencias centrales con paso igual
     * al vóxel.
     *
     * @return Dirección unitaria hacia más luz, o ZeroVector si el entorno es plano.
     * @note Es el vector del término de fototropismo (wPhot) de la colonización del
     *       espacio y el que orienta la lámina de la hoja.
     */
    FVector GradientOfLight(const FVector& WorldPos) const;

private:
    FORCEINLINE int32 IndexOf(int32 Ix, int32 Iy, int32 Iz) const
    {
        return EcoGrid::VoxelIndex(Ix, Iy, Iz, Width, Height);
    }

    /** Centro en cm de mundo del vóxel (Ix, Iy, Iz). */
    FORCEINLINE FVector VoxelCenter(int32 Ix, int32 Iy, int32 Iz) const
    {
        return OriginWorld + FVector(Ix + 0.5f, Iy + 0.5f, Iz + 0.5f) * VoxelSizeCm;
    }

    /** Mundo -> índice de vóxel, con los puntos de fuera pegados al borde. */
    void WorldToVoxelClamped(const FVector& WorldPos, int32& OutIx, int32& OutIy, int32& OutIz) const;

    /** Sombra acumulada en WorldPos: por vecino más cercano y por interpolación trilineal. */
    float SampleShadowNearest(const FVector& WorldPos) const;
    float SampleShadowTrilinear(const FVector& WorldPos) const;
};