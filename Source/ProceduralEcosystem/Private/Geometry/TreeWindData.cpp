/**
 * @file TreeWindData.cpp
 * @author Juan Luque Roldán
 * @brief Cálculo de los atributos de viento y oclusión: ramas, pivotes, balanceo, desfase y AO.
 *
 * Implementa FTreeWindData::Build, que recorre el esqueleto una sola vez por dato: cuenta
 * hijos y longitudes acumuladas, decide qué hijo CONTINÚA cada rama —el más grueso, con el
 * eje principal ganando siempre— para que el tronco no se parta en cada inserción lateral,
 * propaga raíz de rama y nivel aprovechando que el índice del padre siempre es menor, y de
 * ahí deriva el pivote, el peso de balanceo, el desfase y el tinte estables por rama, y la
 * oclusión de copa muestreada en la rejilla de luz fina. El balanceo no resuelve ninguna
 * ecuación: aproxima el perfil de una viga empotrada con una ley de potencia modulada por
 * el grosor del nodo y la rigidez de la especie.
 *
 * @ingroup eco_geometry
 * @see @ref bib_pivotpainter
 * @see @ref bib_zhukov1998
 */

#include "Geometry/TreeWindData.h"

#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand::HashUnit: hash estable -> [0,1), copia única

namespace
{
    /** Suelo de la oclusión de copa: por debajo no se oscurece más, para no dejar
        manchas de negro plano en el interior de la copa. */
    constexpr float MinCanopyAO = 0.15f;

    /**
     * Exponente de la curva de balanceo a lo largo de la rama. Por encima de 1 concentra
     * el movimiento en la punta, que es como flexa una viga empotrada: la base apenas se
     * mueve y el extremo describe casi todo el recorrido.
     */
    constexpr float SwayFalloffExp = 1.35f;
}

void FTreeWindData::Reset()
{
    Nodes.Reset();
    BranchRoot.Reset();
    ChildCount.Reset();
    AlongLen.Reset();
}

bool FTreeWindData::IsValidFor(const FTreeSkeleton& Skeleton) const
{
    return Nodes.Num() == Skeleton.Num() && Nodes.Num() > 0;
}

void FTreeWindData::Build(const FTreeSkeleton& Skeleton, const USpeciesData& Species,
    const FTreeLightGridFine* FineLight, uint32 Seed)
{
    Reset();

    const int32 N = Skeleton.Num();
    if (N == 0)
    {
        return;
    }

    const FVector Base = Skeleton.Nodes[0].Pos;

    // ==== HIJOS POR NODO Y LONGITUD ACUMULADA ====
    // Pasadas compartidas del esqueleto, las mismas que consumen el mallador y el
    // follaje. Los hijos hacen falta antes de decidir dónde empieza cada rama: un nodo
    // abre rama nueva si su padre bifurcó.
    Skeleton.ComputeChildCounts(ChildCount);
    Skeleton.ComputeAlongLengths(AlongLen);

    // ==== RAMAS Y NIVELES ====
    // Una sola pasada de índice creciente, sin recursión ni ordenación, gracias a la
    // invariante de FTreeSkeleton: el índice del padre siempre es menor que el del hijo.
    BranchRoot.SetNumUninitialized(N);
    TArray<int32> Level;      Level.SetNumZeroed(N);

    // ==== QUÉ HIJO CONTINÚA LA RAMA DE SU PADRE ====
    // No basta con «si el padre bifurcó, el hijo abre rama nueva»: el eje principal
    // atraviesa la copa y bifurca en cada inserción lateral, así que se contaría a sí
    // mismo como rama nueva en cada una, su pivote se reiniciaría a media altura y el
    // tronco se balancearía como una ramita colgada del punto equivocado.
    //
    // La regla es que continúa el hijo más grueso, y el eje gana siempre; los hermanos
    // más finos sí abren rama. Coincide con lo botánicamente correcto: una rama no
    // termina donde le sale una hija, termina donde se acaba.
    //
    // El grosor se mide con el radio estructural del pipe model: el de mallado lleva
    // encima el ensanche de pie, que cerca del suelo invierte el orden de grosores.
    TArray<int32> Continuation; Continuation.Init(INDEX_NONE, N);
    {
        TArray<float> BestScore; BestScore.Init(-1.f, N);
        for (int32 i = 1; i < N; ++i)
        {
            const int32 P = FMath::Clamp(Skeleton.Nodes[i].Parent, 0, N - 1);

            float Score = Skeleton.Nodes[i].GetPipeRadius();
            if (Skeleton.Nodes[i].IsAxis() && Skeleton.Nodes[P].IsAxis())
            {
                Score += 1.e6f; // el eje continúa al eje, pase lo que pase
            }

            // Comparación estricta: en empate gana el de índice menor, así que el
            // resultado no depende del orden de recorrido y es determinista.
            if (Score > BestScore[P])
            {
                BestScore[P] = Score;
                Continuation[P] = i;
            }
        }
    }

    BranchRoot[0] = 0;
    Level[0] = 0;

    int32 MaxLevel = 0;
    float MaxAlong = 0.f;

    for (int32 i = 1; i < N; ++i)
    {
        const FBranchNode& Node = Skeleton.Nodes[i];
        const int32 P = FMath::Clamp(Node.Parent, 0, N - 1);

        MaxAlong = FMath::Max(MaxAlong, AlongLen[i]);

        if (ChildCount[P] > 1 && Continuation[P] != i)
        {
            // El padre bifurcó y éste no es el hijo que continúa la rama: el nodo
            // arranca rama nueva y su pivote es el punto de inserción, el propio padre.
            BranchRoot[i] = i;
            Level[i] = Level[P] + 1;
        }
        else
        {
            // Continuación de la misma rama: hereda pivote y nivel.
            BranchRoot[i] = BranchRoot[P];
            Level[i] = Level[P];
        }
        MaxLevel = FMath::Max(MaxLevel, Level[i]);
    }

    // ==== REFERENCIAS PARA NORMALIZAR ====
    const float InvMaxAlong = (MaxAlong > KINDA_SMALL_NUMBER) ? (1.f / MaxAlong) : 0.f;
    const float InvMaxLevel = (MaxLevel > 0) ? (1.f / static_cast<float>(MaxLevel)) : 0.f;

    // Radio del tronco en la base: es la referencia de grosor máximo con la que se
    // decide qué es rígido y qué es flexible. De nuevo el radio estructural y no el de
    // mallado, porque el ensanche de pie casi duplica el radio del tronco sin que el
    // árbol sea un gramo más rígido, y con él el árbol entero se leería como fino y se
    // balancearía de más.
    const float BaseRadius = FMath::Max(Skeleton.Nodes[0].GetPipeRadius(), KINDA_SMALL_NUMBER);

    // Rigidez de la especie: 1 = conífera rígida que apenas se mueve, 0 = especie de rama
    // larga y flexible. Queda incorporada al peso de balanceo de cada vértice, así que el
    // material no necesita saber de qué especie es el árbol.
    const float Stiffness = FMath::Clamp(Species.WindStiffness, 0.f, 1.f);
    const float SpeciesFlex = 1.f - 0.65f * Stiffness;

    // ==== ATRIBUTOS POR NODO ====
    Nodes.SetNumUninitialized(N);
    for (int32 i = 0; i < N; ++i)
    {
        const FBranchNode& Node = Skeleton.Nodes[i];
        FTreeWindNode& Out = Nodes[i];

        // Pivote: el punto de inserción de la rama, relativo a la base del tronco. No es
        // el primer nodo de la rama sino su PADRE, la horquilla de la que cuelga. Con el
        // primer nodo, el internodo que va de la horquilla hasta él no rotaría y la rama
        // se despegaría visiblemente de su padre al moverse; con la horquilla, la rama
        // entera gira alrededor del punto donde de verdad está unida. El tronco no tiene
        // padre y pivota sobre su propia base, que es el origen local de la malla.
        const int32 Root = BranchRoot[i];
        const int32 PivotNode = (Skeleton.Nodes[Root].Parent >= 0) ? Skeleton.Nodes[Root].Parent : Root;
        Out.PivotLocalCm = Skeleton.Nodes[PivotNode].Pos - Base;

        Out.BranchLevel01 = Level[i] * InvMaxLevel;

        // Peso de balanceo, producto de dos factores: la posición a lo largo del árbol,
        // con exponente mayor que 1 porque la base está empotrada y la punta describe
        // casi todo el recorrido, y el grosor, porque una rama gruesa no flexa aunque
        // esté lejos de la base. El grosor lo da el pipe model, que ya calculó el radio
        // estructural de cada nodo.
        const float T = FMath::Clamp(AlongLen[i] * InvMaxAlong, 0.f, 1.f);
        const float Thin = 1.f - FMath::Clamp(Node.GetPipeRadius() / BaseRadius, 0.f, 1.f);
        const float Shape = FMath::Pow(T, SwayFalloffExp) * (0.30f + 0.70f * Thin);

        Out.SwayWeight = FMath::Clamp(Shape * SpeciesFlex, 0.f, 1.f);

        // Desfase y tinte, estables por rama y no por nodo: con un desfase por nodo, los
        // vértices consecutivos de un mismo tubo se moverían en direcciones distintas y
        // la rama se retorcería como una goma.
        Out.Phase01 = EcoRand::HashUnit(Seed ^ (static_cast<uint32>(Root) * 2654435761u));
        Out.TintVariation = EcoRand::HashUnit(Seed ^ (static_cast<uint32>(Root) * 40503u + 0x9E3779B9u));

        // Oclusión de copa leída de la rejilla de luz fina: los mismos vóxeles que
        // decidieron la autopoda oscurecen ahora el interior de la copa.
        if (FineLight && FineLight->IsValid())
        {
            const float Q = FineLight->SampleLightSmooth(Node.Pos) / FTreeLightGridFine::FullSunlight;
            Out.CanopyAO = FMath::Lerp(MinCanopyAO, 1.f, FMath::Clamp(Q, 0.f, 1.f));
        }
        else
        {
            Out.CanopyAO = 1.f;
        }
    }
}
