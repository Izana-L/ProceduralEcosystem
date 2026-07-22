#pragma once

#include "CoreMinimal.h"

struct FLightFieldCoarse; // Terrain/LightFieldCoarse.h (se incluye en el .cpp)
struct FTreeSkeleton;      // Geometry/TreeSkeleton.h    (se incluye en el .cpp)

/**
 * Rejilla 3D de luz FINA y LOCAL a un solo hero tree (doc. Fase 3, 3.2/3.5).
 * Es la resolucion (b) del diseno; la (a) -gruesa, global entre arboles- es
 * FLightFieldCoarse de la Fase 1/2.
 *
 * La copa cabe en una caja pequena (~10x10x15 m); a 25-50 cm/voxel son decenas
 * de miles de celdas: trivial, y solo la tienen los hero trees. Una rejilla
 * regular (no octree) da dos cosas: la cascada de sombra es natural en celdas
 * uniformes, y sirve de indice espacial de atractores para el SCA (ese indice
 * vive en FAttractorCloud, Paso 4; aqui solo la luz).
 *
 * Cumple DOS papeles, ambos via culling de atractores mas que por un termino
 * de fototropismo a mano:
 *   1. CONEXION MICRO<-MACRO. Antes de crecer, se siembra la rejilla con la
 *      sombra del grid grueso global (la de los VECINOS): SeedFromCoarse. Un
 *      hero tree pegado a un vecino grande encuentra sus atractores de ese
 *      lado ya en sombra -> crece ladeado, alejandose del vecino. Es el pago
 *      de la arquitectura en dos escalas.
 *   2. AUTOPODA INTERNA. Cada cierto numero de iteraciones el arbol deposita
 *      la sombra de su PROPIO follaje (DepositLeafShadow); los atractores en
 *      voxels sombreados se eliminan y las ramas interiores/bajas dejan de
 *      alargarse. La poda de la copa interior sale sola.
 *
 * Convencion: Shadow acumulada por voxel, 0 = sin sombra. Luz disponible
 * Q = clamp(FullSunlight - Shadow, 0). Muestrea en CENTROS de voxel.
 */
struct PROCEDURALECOSYSTEM_API FTreeLightGridFine
{
    int32 Width = 0;  // voxels en X
    int32 Height = 0; // voxels en Y
    int32 Layers = 0; // voxels en Z (altura)

    float VoxelSizeCm = 35.f;                     // voxel cubico (doc: 25-50 cm)
    FVector OriginWorld = FVector::ZeroVector;    // esquina min del voxel (0,0,0), mundo cm

    /** Sombra por voxel, tamano Width*Height*Layers. 0 = sin sombra. */
    TArray<float> Shadow;

    /** C: luz plena normalizada (cielo despejado), igual que FLightFieldCoarse. */
    static constexpr float FullSunlight = 1.f;

    bool IsValid() const
    {
        return Width > 0 && Height > 0 && Layers > 0
            && VoxelSizeCm > 0.f
            && Shadow.Num() == Width * Height * Layers;
    }

    /**
     * Dimensiona la rejilla para cubrir WorldBounds (p.ej. la envolvente de la
     * copa o FTreeSkeleton::ComputeBounds) con voxels de InVoxelSizeCm, mas un
     * margen PaddingCm por cada lado. Deja la sombra a 0.
     */
    void InitForBounds(const FBox& WorldBounds, float InVoxelSizeCm, float PaddingCm = 0.f);

    /** Pone toda la sombra a 0. */
    void ClearShadow();

    /**
     * Papel 1 (micro<-macro). ESTABLECE la sombra base de cada voxel a partir
     * de la luz del grid grueso global en el centro del voxel: importa la
     * sombra de los vecinos. Sobrescribe (no acumula): es la base sobre la que
     * luego DepositLeafShadow anade el follaje propio.
     */
    void SeedFromCoarse(const FLightFieldCoarse& Coarse);

    /**
     * Papel 2 (autopoda). ANADE la sombra del follaje propio: por cada nodo del
     * esqueleto deposita una sombra que cae hacia abajo (la luz viene de
     * arriba). Pensado para llamarse tras SeedFromCoarse en cada refresco de
     * luz del SCA.
     */
    void DepositLeafShadow(const FTreeSkeleton& Skeleton, float RadiusCm, float DepthCm, float PerNodeDensity);

    /**
     * Anade una sombra que decae hacia abajo desde FromWorld: columna de radio
     * RadiusCm que llega hasta DepthCm por debajo, mas intensa junto al follaje
     * y mas debil al bajar. Es la primitiva que usa DepositLeafShadow.
     */
    void DepositDownwardShadow(const FVector& FromWorld, float RadiusCm, float DepthCm, float Density);

    /** Q disponible (vecino mas cercano): clamp(FullSunlight - Shadow, 0). */
    float SampleLight(const FVector& WorldPos) const;

    /** Igual, pero con interpolacion TRILINEAL entre los 8 voxels vecinos
        (mas suave; lo usa GradientOfLight y el fototropismo). */
    float SampleLightSmooth(const FVector& WorldPos) const;

    /** true si la luz en WorldPos esta por debajo de LightThreshold (voxel
        "sombreado"): es el criterio de la autopoda (CullByShade del Paso 4/5). */
    bool IsShaded(const FVector& WorldPos, float LightThreshold) const;

    /**
     * Gradiente de la luz en WorldPos (diferencias centrales sobre la luz
     * suavizada), NORMALIZADO: apunta hacia mas luz. Es el vector del termino
     * de fototropismo (wPhot) del SCA. Devuelve ZeroVector si no hay pendiente.
     */
    FVector GradientOfLight(const FVector& WorldPos) const;

private:
    FORCEINLINE int32 IndexOf(int32 Ix, int32 Iy, int32 Iz) const
    {
        return (Iz * Height + Iy) * Width + Ix;
    }

    /** Centro de mundo (cm) del voxel (ix,iy,iz). */
    FORCEINLINE FVector VoxelCenter(int32 Ix, int32 Iy, int32 Iz) const
    {
        return OriginWorld + FVector(Ix + 0.5f, Iy + 0.5f, Iz + 0.5f) * VoxelSizeCm;
    }

    /** Mundo -> indice de voxel con clamp a los bordes. */
    void WorldToVoxelClamped(const FVector& WorldPos, int32& OutIx, int32& OutIy, int32& OutIz) const;

    /** Sombra en WorldPos: vecino mas cercano y trilineal. */
    float SampleShadowNearest(const FVector& WorldPos) const;
    float SampleShadowTrilinear(const FVector& WorldPos) const;
};