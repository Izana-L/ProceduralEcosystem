#include "Geometry/TreeWindData.h"

#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand::Hash32

namespace
{
    /** Valor estable en [0,1) a partir de un entero (mismo patron que TreeArchetype). */
    FORCEINLINE float StableUnit(uint32 Value)
    {
        return static_cast<float>(EcoRand::Hash32(Value) >> 8) * (1.f / 16777216.f);
    }

    /** Suelo del AO: por debajo de esto no se oscurece mas (evita negros planos). */
    constexpr float MinCanopyAO = 0.15f;

    /**
     * Exponente de la curva de balanceo a lo largo de la rama. > 1 concentra el
     * movimiento en la punta, que es como flexa una viga empotrada: la base casi
     * no se mueve y el extremo describe casi todo el recorrido.
     */
    constexpr float SwayFalloffExp = 1.35f;
}

void FTreeWindData::Reset()
{
    Nodes.Reset();
    BranchRoot.Reset();
    TreeHeightCm = 0.f;
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

    // -----------------------------------------------------------------------
    // 1) Hijos por nodo. Hace falta ANTES de decidir donde empieza cada rama:
    //    un nodo abre rama nueva si su padre bifurco (mas de un hijo).
    // -----------------------------------------------------------------------
    TArray<int32> ChildCount;
    ChildCount.SetNumZeroed(N);
    for (int32 i = 0; i < N; ++i)
    {
        const int32 P = Skeleton.Nodes[i].Parent;
        if (P >= 0 && P < N)
        {
            ++ChildCount[P];
        }
    }

    // -----------------------------------------------------------------------
    // 2) Ramas, niveles y longitud acumulada, en UNA pasada de indice creciente.
    //    Funciona sin recursion ni ordenacion gracias a la invariante de
    //    FTreeSkeleton: Parent < indice del hijo, siempre.
    // -----------------------------------------------------------------------
    BranchRoot.SetNumUninitialized(N);
    TArray<int32> Level;      Level.SetNumZeroed(N);
    TArray<float> AlongLen;   AlongLen.SetNumZeroed(N);   // distancia recorrida desde la raiz

    BranchRoot[0] = 0;
    Level[0] = 0;

    int32 MaxLevel = 0;
    float MaxAlong = 0.f;
    float MaxZ = 0.f;

    for (int32 i = 1; i < N; ++i)
    {
        const FBranchNode& Node = Skeleton.Nodes[i];
        const int32 P = FMath::Clamp(Node.Parent, 0, N - 1);

        AlongLen[i] = AlongLen[P] + static_cast<float>(FVector::Dist(Node.Pos, Skeleton.Nodes[P].Pos));
        MaxAlong = FMath::Max(MaxAlong, AlongLen[i]);
        MaxZ = FMath::Max(MaxZ, static_cast<float>(Node.Pos.Z - Base.Z));

        if (ChildCount[P] > 1)
        {
            // El padre bifurco: este nodo ARRANCA una rama nueva y su pivote es
            // el punto de insercion, o sea el propio padre.
            BranchRoot[i] = i;
            Level[i] = Level[P] + 1;
        }
        else
        {
            // Continuacion de la misma rama: hereda pivote y nivel.
            BranchRoot[i] = BranchRoot[P];
            Level[i] = Level[P];
        }
        MaxLevel = FMath::Max(MaxLevel, Level[i]);
    }

    TreeHeightCm = MaxZ;

    // -----------------------------------------------------------------------
    // 3) Referencias para normalizar.
    // -----------------------------------------------------------------------
    const float InvMaxAlong = (MaxAlong > KINDA_SMALL_NUMBER) ? (1.f / MaxAlong) : 0.f;
    const float InvMaxLevel = (MaxLevel > 0) ? (1.f / static_cast<float>(MaxLevel)) : 0.f;

    // Radio del tronco en la base: sirve de referencia de "grosor maximo" para
    // decidir que es rigido y que es flexible.
    const float BaseRadius = FMath::Max(Skeleton.Nodes[0].Radius, KINDA_SMALL_NUMBER);

    // Rigidez de la especie: 1 = conifera rigida (apenas se mueve), 0 = especie
    // de rama larga y flexible. Se dobla en la malla, asi que el material no
    // necesita saber de que especie es el arbol.
    const float Stiffness = FMath::Clamp(Species.WindStiffness, 0.f, 1.f);
    const float SpeciesFlex = 1.f - 0.65f * Stiffness;

    // -----------------------------------------------------------------------
    // 4) Atributos por nodo.
    // -----------------------------------------------------------------------
    Nodes.SetNumUninitialized(N);
    for (int32 i = 0; i < N; ++i)
    {
        const FBranchNode& Node = Skeleton.Nodes[i];
        FTreeWindNode& Out = Nodes[i];

        // --- Pivote: el PUNTO DE INSERCION de la rama, relativo a la base ---
        // Ojo al detalle: no es el primer nodo de la rama, sino su PADRE, o sea
        // la horquilla de la que cuelga. Si se usara el primer nodo, el primer
        // internodo -el que va de la horquilla a ese nodo- no rotaria y la rama
        // se separaria visiblemente de su padre al moverse. Con la horquilla, la
        // rama entera gira alrededor del punto donde de verdad esta unida.
        // Para el tronco (sin padre) el pivote es su propia base, que ademas es
        // el origen local de la malla: el tronco flexa sobre el suelo.
        const int32 Root = BranchRoot[i];
        const int32 PivotNode = (Skeleton.Nodes[Root].Parent >= 0) ? Skeleton.Nodes[Root].Parent : Root;
        Out.PivotLocalCm = Skeleton.Nodes[PivotNode].Pos - Base;

        // --- Nivel de rama normalizado ---
        Out.BranchLevel01 = Level[i] * InvMaxLevel;

        // --- Peso de balanceo ---
        // Dos ingredientes multiplicativos:
        //   (a) posicion a lo largo del arbol: la base esta empotrada, la punta
        //       describe casi todo el recorrido -> curva con exponente > 1.
        //   (b) GROSOR: una rama gruesa no flexa aunque este lejos de la base.
        //       Sale gratis del pipe model de la Fase 3, que ya calculo el radio
        //       de cada nodo -- otra sinergia de generar la geometria nosotros.
        const float T = FMath::Clamp(AlongLen[i] * InvMaxAlong, 0.f, 1.f);
        const float Thin = 1.f - FMath::Clamp(Node.Radius / BaseRadius, 0.f, 1.f);
        const float Shape = FMath::Pow(T, SwayFalloffExp) * (0.30f + 0.70f * Thin);

        Out.SwayWeight = FMath::Clamp(Shape * SpeciesFlex, 0.f, 1.f);

        // --- Desfase estable POR RAMA (no por nodo) ---
        // Por rama y no por nodo a proposito: si cada nodo tuviera su propio
        // desfase, los vertices consecutivos de un mismo tubo se moverian en
        // direcciones distintas y la rama se retorceria como una goma.
        Out.Phase01 = StableUnit(Seed ^ (static_cast<uint32>(Root) * 2654435761u));
        Out.TintVariation = StableUnit(Seed ^ (static_cast<uint32>(Root) * 40503u + 0x9E3779B9u));

        // --- AO de copa desde la rejilla fina del SCA (doc. 6.2) ---
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
