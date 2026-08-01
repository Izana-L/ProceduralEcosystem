#pragma once

#include "CoreMinimal.h"

class USpeciesData;
struct FTreeSkeleton;
struct FTreeLightGridFine;

/**
 * Buffers de UNA seccion renderizable (madera o follaje). Formato NEUTRO, no
 * atado a ninguna API de UE: el actor hero los sube a un UProceduralMeshComponent
 * (Fase 3) y la Fase 4 los hornea a UStaticMesh via FMeshDescription. Por eso
 * las tangentes son FVector (no FProcMeshTangent): la conversion la hace quien
 * consume, no el mallador.
 *
 * Todos los arrays por-vertice van en lockstep (mismo indice = mismo vertice):
 * Vertices, Normals, UVs, UV1, UV2, UV3, Tangents, Colors.
 *
 * =============================================================================
 *  CANALES DE LA FASE 6 (viento + AO). Contrato con el material.
 * =============================================================================
 * Ver Geometry/TreeWindData.h para el porque y Docs/Fase6_Guia.md para el
 * material que los consume. Resumen:
 *
 *   UV0 = (u, v)                    textura (corteza cilindrica / hoja)
 *   UV1 = (PivotX, PivotY)          pivote de la rama, en METROS, relativo a la
 *   UV2 = (PivotZ, BranchLevel01)     base del tronco (= origen local de la malla)
 *   UV3 = (SwayWeight, Phase01)     cuanto se mueve y con que desfase
 *
 *   Colors.R = CanopyAO       1 = expuesto, 0 = interior de copa sombreado
 *   Colors.G = TintVariation  variacion estable por rama (rompe uniformidad)
 *   Colors.B = BranchLevel01  mascara barata de "que tan lejos del tronco"
 *   Colors.A = 1
 *
 * Los cuatro canales UV son los que soportan A LA VEZ el UProceduralMeshComponent
 * del hero tree y el UStaticMesh de la libreria, de modo que el MISMO material
 * funciona en los dos niveles de detalle: un arbol no cambia de aspecto ni deja
 * de moverse al pasar de hero a instancia. Esa continuidad es justo lo que hace
 * invisible el puente de escala de la Fase 4.
 */
struct FTreeMeshBuffers
{
    TArray<FVector>   Vertices;
    TArray<int32>     Triangles;
    TArray<FVector>   Normals;
    TArray<FVector2D> UVs;      // UV0: textura
    TArray<FVector2D> UV1;      // Fase 6: pivote.XY (metros, local)
    TArray<FVector2D> UV2;      // Fase 6: (pivote.Z, nivel de rama)
    TArray<FVector2D> UV3;      // Fase 6: (peso de balanceo, desfase)
    TArray<FVector>   Tangents; // direccion U por vertice
    TArray<FLinearColor> Colors;// Fase 6: (AO de copa, tinte, nivel, 1)

    void Reset()
    {
        Vertices.Reset();
        Triangles.Reset();
        Normals.Reset();
        UVs.Reset();
        UV1.Reset();
        UV2.Reset();
        UV3.Reset();
        Tangents.Reset();
        Colors.Reset();
    }

    bool IsEmpty() const { return Vertices.Num() == 0 || Triangles.Num() == 0; }

    /** true si los canales de la Fase 6 estan rellenos y alineados con Vertices. */
    bool HasWindData() const
    {
        const int32 N = Vertices.Num();
        return N > 0 && UV1.Num() == N && UV2.Num() == N && UV3.Num() == N;
    }
};

/**
 * Malla de un arbol: dos secciones con MATERIAL distinto (doc. 3.7). La madera
 * es geometria lenosa densa (bien para Nanite); el follaje son leaf cards con
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
 * De esqueleto a malla (doc. Fase 3, 3.7 + Fase 6, 6.1/6.2).
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
 * VIENTO Y AO (Fase 6): ademas de la geometria, cada vertice se etiqueta con el
 * pivote de su rama, su nivel jerarquico, cuanto debe balancearse y su oclusion
 * de copa. Sale gratis porque el esqueleto ya conoce padres y radios; es la
 * "sinergia con la Fase 3" del doc. 6.1.
 *
 * Determinista: la seleccion y orientacion de hojas sale del RngState.
 */
namespace TreeMeshBuilder
{
    /**
     * @param FineLight  Rejilla de luz fina del SCA, para el AO de copa por
     *                   vertice (doc. 6.2). nullptr = AO neutro (todo a 1),
     *                   que es lo correcto para mallas sin contexto.
     */
    PROCEDURALECOSYSTEM_API void BuildMesh(
        const FTreeSkeleton& Skeleton,
        const USpeciesData& Species,
        uint32& RngState,
        FTreeMeshData& OutMesh,
        const FTreeLightGridFine* FineLight = nullptr);
}
