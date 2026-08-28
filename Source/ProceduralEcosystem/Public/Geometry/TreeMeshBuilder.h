#pragma once

#include "CoreMinimal.h"
#include "Geometry/TreeWindData.h" // FTreeWindNode: los canales de viento por nodo

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
    /** Fase 6: los pivotes viajan en METROS (ver la nota de precision de TreeWindData.h). */
    static constexpr float CmToM = 0.01f;

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

    /** Reserva de golpe los OCHO arrays por-vertice (van en lockstep). */
    void ReserveVertices(int32 Count)
    {
        Vertices.Reserve(Count);
        Normals.Reserve(Count);
        UVs.Reserve(Count);
        UV1.Reserve(Count);
        UV2.Reserve(Count);
        UV3.Reserve(Count);
        Tangents.Reserve(Count);
        Colors.Reserve(Count);
    }

    /** Dimensiona de golpe los OCHO arrays por-vertice, para escritura indexada. */
    void SetNumVertices(int32 Count)
    {
        Vertices.SetNumUninitialized(Count);
        Normals.SetNumUninitialized(Count);
        UVs.SetNumUninitialized(Count);
        UV1.SetNumUninitialized(Count);
        UV2.SetNumUninitialized(Count);
        UV3.SetNumUninitialized(Count);
        Tangents.SetNumUninitialized(Count);
        Colors.SetNumUninitialized(Count);
    }

    /** Fase 6: canales de viento/AO de UN vertice ya existente. */
    void SetWindVertex(int32 Vi, const FVector& PivotLocalCm, float BranchLevel01,
        float SwayWeight, float Phase01, float CanopyAO, float Tint)
    {
        UV1[Vi] = FVector2D(PivotLocalCm.X * CmToM, PivotLocalCm.Y * CmToM);
        UV2[Vi] = FVector2D(PivotLocalCm.Z * CmToM, BranchLevel01);
        UV3[Vi] = FVector2D(SwayWeight, Phase01);
        Colors[Vi] = FLinearColor(CanopyAO, Tint, BranchLevel01, 1.f);
    }

    /** Sobrecarga: los canales de UN vertice tomados directamente del nodo de
        viento. Los tres sitios del mallador que escribian estos seis campos
        repetian la MISMA lista de argumentos; asi solo se nombra el nodo. */
    void SetWindVertex(int32 Vi, const FTreeWindNode& Wn)
    {
        SetWindVertex(Vi, Wn.PivotLocalCm, Wn.BranchLevel01,
            Wn.SwayWeight, Wn.Phase01, Wn.CanopyAO, Wn.TintVariation);
    }

    /** Igual, anadiendo al final (los buffers que crecen vertice a vertice).
        DELEGA en SetWindVertex: el empaquetado de los canales estaba escrito dos
        veces, y son el contrato con el material -si las dos copias divergen, el
        hero tree y la instancia horneada dejan de moverse igual. */
    void AppendWindVertex(const FVector& PivotLocalCm, float BranchLevel01,
        float SwayWeight, float Phase01, float CanopyAO, float Tint)
    {
        const int32 Vi = UV1.AddUninitialized();
        UV2.AddUninitialized();
        UV3.AddUninitialized();
        Colors.AddUninitialized();
        SetWindVertex(Vi, PivotLocalCm, BranchLevel01, SwayWeight, Phase01, CanopyAO, Tint);
    }

    /** Idem, anadiendo al final. */
    void AppendWindVertex(const FTreeWindNode& Wn, float SwayWeight, float Phase01)
    {
        AppendWindVertex(Wn.PivotLocalCm, Wn.BranchLevel01,
            SwayWeight, Phase01, Wn.CanopyAO, Wn.TintVariation);
    }

    bool IsEmpty() const { return Vertices.Num() == 0 || Triangles.Num() == 0; }
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
 * CIERRES: el tubo se cierra por los dos extremos. Cada nodo terminal remata en
 * un vertice APICE unido a su anillo por un abanico de K triangulos (la punta
 * converge en un punto en vez de dejar una boca abierta), y el anillo de la
 * raiz se tapa con un abanico plano.
 *
 * HOJAS: las coloca TreeFoliage a lo largo de las ramillas por filotaxis; ver
 * Geometry/TreeFoliage.h.
 *
 * VIENTO Y AO (Fase 6): ademas de la geometria, cada vertice se etiqueta con el
 * pivote de su rama, su nivel jerarquico, cuanto debe balancearse y su oclusion
 * de copa. Sale gratis porque el esqueleto ya conoce padres y radios; es la
 * "sinergia con la Fase 3" del doc. 6.1.
 *
 * Determinista y SIN CONSUMIR RNG: toda la variacion (desfases, jitter de hoja)
 * sale de hashes de Seed. Asi el mallado nunca desplaza la secuencia aleatoria
 * que el SCA consumio antes sobre el mismo stream.
 */
namespace TreeMeshBuilder
{
    /**
     * @param Seed       Semilla del arbol. Solo se hashea, nunca se avanza.
     * @param FineLight  Rejilla de luz fina del SCA, para el AO de copa por
     *                   vertice (doc. 6.2). nullptr = AO neutro (todo a 1),
     *                   que es lo correcto para mallas sin contexto.
     */
    PROCEDURALECOSYSTEM_API void BuildMesh(
        const FTreeSkeleton& Skeleton,
        const USpeciesData& Species,
        uint32 Seed,
        FTreeMeshData& OutMesh,
        const FTreeLightGridFine* FineLight = nullptr);
}
