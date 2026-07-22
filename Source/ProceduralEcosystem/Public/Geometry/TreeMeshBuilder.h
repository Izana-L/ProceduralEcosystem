#pragma once

#include "CoreMinimal.h"

class USpeciesData;
struct FTreeSkeleton;

/**
 * Buffers de UNA seccion renderizable (madera o follaje). Formato NEUTRO, no
 * atado a ninguna API de UE: el actor hero los sube a un UProceduralMeshComponent
 * (Fase 3) y la Fase 4 los horneara a UStaticMesh via FMeshDescription. Por eso
 * las tangentes son FVector (no FProcMeshTangent): la conversion la hace quien
 * consume, no el mallador.
 *
 * Todos los arrays por-vertice van en lockstep (mismo indice = mismo vertice):
 * Vertices, Normals, UVs, Tangents (y Colors si se usa).
 */
struct FTreeMeshBuffers
{
    TArray<FVector>   Vertices;
    TArray<int32>     Triangles;
    TArray<FVector>   Normals;
    TArray<FVector2D> UVs;
    TArray<FVector>   Tangents; // direccion U por vertice
    TArray<FColor>    Colors;   // opcional (reservado para AO de copa, Fase 6)

    void Reset()
    {
        Vertices.Reset();
        Triangles.Reset();
        Normals.Reset();
        UVs.Reset();
        Tangents.Reset();
        Colors.Reset();
    }

    bool IsEmpty() const { return Vertices.Num() == 0 || Triangles.Num() == 0; }
};

/**
 * Malla de un arbol: dos secciones con MATERIAL distinto (doc. 3.7). La madera
 * es geometria leñosa densa (bien para Nanite); el follaje son leaf cards con
 * material masked/subsurface, que es lo caro de renderizar y conviene separado.
 */
struct FTreeMeshData
{
    FTreeMeshBuffers Wood;
    FTreeMeshBuffers Leaves;

    void Reset()
    {
        Wood.Reset();
        Leaves.Reset();
    }
};

/**
 * De esqueleto a malla (doc. Fase 3, 3.7).
 *
 * Ramas como TUBOS: por cada nodo, un anillo de K vertices perpendicular a su
 * direccion con el radio del pipe model; anillos consecutivos (nodo <-> padre)
 * se conectan con quads. Para que el tubo no se retuerza se usan marcos de
 * rotacion minima (rotation-minimizing frames) propagados del padre al hijo,
 * en vez de recalcular una base por anillo. UVs cilindricas (u alrededor, v a
 * lo largo) para la corteza. Las uniones se dejan solapar (pragmatico, doc.).
 *
 * HOJAS: leaf cards (quads) en los nodos terminales, con densidad y orientacion
 * (mayormente hacia la luz = arriba) con jitter por-arbol.
 *
 * Determinista: la seleccion y orientacion de hojas sale del RngState.
 */
namespace TreeMeshBuilder
{
    PROCEDURALECOSYSTEM_API void BuildMesh(
        const FTreeSkeleton& Skeleton,
        const USpeciesData& Species,
        uint32& RngState,
        FTreeMeshData& OutMesh);
}