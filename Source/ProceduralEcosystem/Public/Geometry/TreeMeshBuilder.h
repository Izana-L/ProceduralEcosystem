/**
 * @file TreeMeshBuilder.h
 * @author Juan Luque Roldán
 * @brief Formato neutro de los buffers de malla de un árbol y API del mallador.
 *
 * Declara FTreeMeshBuffers —los arrays por vértice de una sección renderizable—,
 * FTreeMeshData —la malla partida en madera y follaje— y BuildMesh, que convierte el
 * esqueleto de ramas en tubos con sus cierres y delega las hojas en el follaje. Fija
 * también el contrato de canales con el material: además de la geometría, cada vértice
 * lleva el pivote de su rama, su nivel jerárquico, su peso de balanceo y su oclusión de
 * copa, empaquetados en UV0..UV3 y en el color de vértice. Ese formato es el que
 * soportan a la vez el UProceduralMeshComponent de un hero tree y el UStaticMesh
 * horneado de la librería, de modo que un mismo material sirve a los dos niveles de
 * representación y un árbol no cambia de aspecto ni deja de moverse al pasar de uno a
 * otro.
 *
 * @ingroup eco_geometry
 * @see @ref bib_pivotpainter
 * @see @ref bib_marcorotacionminima
 */

#pragma once

#include "CoreMinimal.h"
#include "Geometry/TreeWindData.h" // FTreeWindNode: los canales de viento por nodo

class USpeciesData;
struct FTreeSkeleton;
struct FTreeLightGridFine;

/**
 * Buffers de una sección renderizable de un árbol: la madera o el follaje.
 *
 * Layout SoA: los ocho arrays por vértice —Vertices, Normals, UVs, UV1, UV2, UV3,
 * Tangents y Colors— van en lockstep, un mismo índice es siempre el mismo vértice, y se
 * dimensionan de golpe. El formato es neutro, sin tipos de ninguna API de malla: las
 * tangentes son FVector y no FProcMeshTangent, y la conversión la hace quien consume.
 * Así los mismos buffers alimentan el UProceduralMeshComponent de un hero tree y el
 * horneado a UStaticMesh de la librería de arquetipos.
 *
 * Contrato de canales con el material, que además de texturar hace el balanceo de viento
 * y modula la luz ambiente:
 *
 * @verbatim
 *   UV0   = (u, v)                   textura: corteza cilíndrica u hoja
 *   UV1   = (PivotX, PivotY)         pivote de la rama, en metros y relativo a la
 *   UV2   = (PivotZ, BranchLevel01)    base del tronco (= origen local de la malla)
 *   UV3   = (SwayWeight, Phase01)    cuánto se mueve y con qué desfase
 *   Color = (CanopyAO, TintVariation, BranchLevel01, 1)
 * @endverbatim
 *
 * CanopyAO vale 1 en un vértice expuesto y cae hacia 0 en el interior sombreado de la
 * copa; TintVariation es una variación estable por rama que rompe la uniformidad del
 * material; BranchLevel01 se duplica en el color por servir de máscara barata de
 * distancia al tronco. Los valores los calcula FTreeWindData: aquí solo se empaquetan.
 *
 * @see FTreeWindNode
 */
struct FTreeMeshBuffers
{
    /** Centímetros a metros. Los pivotes viajan en metros porque un UStaticMesh guarda
        sus UV en float16: a escala de centímetros el pivote saldría escalonado. */
    static constexpr float CmToM = 0.01f;

    TArray<FVector>   Vertices; ///< Posición por vértice, en el espacio en que se generó.
    TArray<int32>     Triangles;///< Índices, de tres en tres.
    TArray<FVector>   Normals;  ///< Normal por vértice.
    TArray<FVector2D> UVs;      ///< UV0: coordenadas de textura.
    TArray<FVector2D> UV1;      ///< Pivote de la rama, componentes X e Y en metros.
    TArray<FVector2D> UV2;      ///< (Pivote.Z en metros, nivel jerárquico de la rama).
    TArray<FVector2D> UV3;      ///< (Peso de balanceo, desfase).
    TArray<FVector>   Tangents; ///< Dirección U por vértice.
    TArray<FLinearColor> Colors;///< (AO de copa, tinte por rama, nivel de rama, 1).

    /** Vacía los nueve arrays conservando la memoria ya reservada. */
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

    /** Reserva de golpe los ocho arrays por vértice, que van en lockstep. */
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

    /**
     * Dimensiona de golpe los ocho arrays por vértice, sin inicializarlos, para poder
     * escribirlos por índice.
     * @pre Quien llame debe escribir después todos los canales de todos los vértices.
     */
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

    /**
     * Empaqueta los canales de viento y oclusión de un vértice ya dimensionado.
     *
     * Punto único donde se materializa el contrato con el material: cualquier consumidor
     * de los buffers pasa por aquí, de modo que el hero tree y la instancia horneada no
     * pueden acabar moviéndose de forma distinta.
     *
     * @param Vi            Índice del vértice, ya reservado por SetNumVertices.
     * @param PivotLocalCm  Pivote de la rama en centímetros; se convierte a metros aquí.
     */
    void SetWindVertex(int32 Vi, const FVector& PivotLocalCm, float BranchLevel01,
        float SwayWeight, float Phase01, float CanopyAO, float Tint)
    {
        UV1[Vi] = FVector2D(PivotLocalCm.X * CmToM, PivotLocalCm.Y * CmToM);
        UV2[Vi] = FVector2D(PivotLocalCm.Z * CmToM, BranchLevel01);
        UV3[Vi] = FVector2D(SwayWeight, Phase01);
        Colors[Vi] = FLinearColor(CanopyAO, Tint, BranchLevel01, 1.f);
    }

    /** Igual, tomando los seis canales directamente del nodo de viento. */
    void SetWindVertex(int32 Vi, const FTreeWindNode& Wn)
    {
        SetWindVertex(Vi, Wn.PivotLocalCm, Wn.BranchLevel01,
            Wn.SwayWeight, Wn.Phase01, Wn.CanopyAO, Wn.TintVariation);
    }

    /**
     * Añade al final los canales de un vértice nuevo, para los buffers que crecen
     * vértice a vértice en lugar de dimensionarse de golpe (el follaje).
     *
     * @note Solo hace crecer UV1, UV2, UV3 y Colors; posición, normal y tangente las
     *       añade quien llama, y es su responsabilidad mantener el lockstep.
     */
    void AppendWindVertex(const FVector& PivotLocalCm, float BranchLevel01,
        float SwayWeight, float Phase01, float CanopyAO, float Tint)
    {
        const int32 Vi = UV1.AddUninitialized();
        UV2.AddUninitialized();
        UV3.AddUninitialized();
        Colors.AddUninitialized();
        SetWindVertex(Vi, PivotLocalCm, BranchLevel01, SwayWeight, Phase01, CanopyAO, Tint);
    }

    /**
     * Igual, tomando del nodo de viento todo menos el balanceo y el desfase, que la hoja
     * calcula por su cuenta: aletea más y con otra fase que la ramilla que la sostiene.
     */
    void AppendWindVertex(const FTreeWindNode& Wn, float SwayWeight, float Phase01)
    {
        AppendWindVertex(Wn.PivotLocalCm, Wn.BranchLevel01,
            SwayWeight, Phase01, Wn.CanopyAO, Wn.TintVariation);
    }

    /** Cierto si la sección no tiene nada que subir: sin vértices o sin triángulos. */
    bool IsEmpty() const { return Vertices.Num() == 0 || Triangles.Num() == 0; }
};

/**
 * Malla completa de un árbol, partida en las dos secciones que llevan material distinto.
 *
 * La madera es geometría leñosa densa y opaca; el follaje son tarjetas de hoja con material
 * enmascarado y traslucidez, que es lo caro de renderizar. Separarlas permite darles
 * material, ordenación y coste propios sin tocar la geometría.
 */
struct FTreeMeshData
{
    FTreeMeshBuffers Wood;   ///< Tronco y ramas, malladas como tubos cerrados.
    FTreeMeshBuffers Leaves; ///< Tarjetas de hoja, un quad por hoja.

    void Reset()
    {
        Wood.Reset();
        Leaves.Reset();
    }
};

/**
 * Paso de esqueleto de ramas a malla.
 *
 * Cada nodo aporta un anillo de K vértices perpendicular a su dirección, con el radio
 * que dejaron el pipe model y el perfil de tronco, y cada anillo se cose al de su padre
 * con K quads. Para que el tubo no se retuerza, el marco del anillo no se recalcula en
 * cada nodo sino que se transporta del padre al hijo por rotación mínima. Las UV son
 * cilíndricas —u alrededor del anillo, v a lo largo de la rama— y las uniones entre una
 * rama y su padre se dejan solapar, que a este grosor no se distingue de una unión
 * resuelta y sale mucho más barato.
 *
 * El tubo se cierra por los dos extremos: cada nodo terminal remata en un vértice ápice
 * unido a su anillo por un abanico, y el anillo de la raíz se tapa con un abanico plano.
 * Además de la geometría, cada vértice se etiqueta con los canales de viento y oclusión
 * de copa, que salen del propio esqueleto sin ningún paso adicional. Las hojas las coloca
 * el follaje sobre las ramillas.
 *
 * Es determinista y no consume ningún flujo de RNG: toda la variación sale de hashes
 * estables de la semilla, así que mallar nunca desplaza la secuencia que la colonización
 * del espacio gastó antes sobre el mismo árbol.
 *
 * @see TreeFoliage::Build
 * @see FTreeWindData::Build
 */
namespace TreeMeshBuilder
{
    /**
     * Malla el esqueleto ya terminado y escribe las dos secciones en @p OutMesh.
     *
     * @param Skeleton   Esqueleto con sus radios ya asignados.
     * @param Seed       Semilla del árbol: solo se hashea, nunca se avanza.
     * @param OutMesh    Destino; se vacía al entrar.
     * @param FineLight  Rejilla de luz fina del árbol, de la que sale la oclusión de copa
     *                   por vértice. Sin ella el AO queda neutro a 1, que es lo correcto
     *                   para una malla horneada sin vecinos alrededor.
     * @pre  El esqueleto necesita al menos un internodo; con menos no se emite nada.
     * @note No modifica el esqueleto ni la rejilla de luz.
     */
    PROCEDURALECOSYSTEM_API void BuildMesh(
        const FTreeSkeleton& Skeleton,
        const USpeciesData& Species,
        uint32 Seed,
        FTreeMeshData& OutMesh,
        const FTreeLightGridFine* FineLight = nullptr);
}
