/**
 * @file TreeSkeleton.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del contenedor de nodos: alta de la raíz y de los hijos, y las
 *        dos pasadas topológicas compartidas.
 *
 * Contiene la gestión del array de FTreeSkeleton —creación de la raíz, alta de hijos con
 * derivación de la profundidad y normalización defensiva de la dirección— y las dos
 * pasadas @f$O(N)@f$ que explotan la invariante `Parent` < índice: el número de hijos de
 * cada nodo y la longitud de arco acumulada desde la raíz. Viven aquí, y no en cada
 * consumidor, para que el mallador, el follaje y los datos de viento no las
 * reimplementen.
 *
 * @ingroup eco_geometry
 */

#include "Geometry/TreeSkeleton.h"

void FTreeSkeleton::Reset()
{
    Nodes.Reset();
}

void FTreeSkeleton::Reserve(int32 ExpectedNodes)
{
    Nodes.Reserve(FMath::Max(0, ExpectedNodes));
}

int32 FTreeSkeleton::InitRoot(const FVector& TrunkBaseWorld, const FVector& InitialDir)
{
    Nodes.Reset();

    FBranchNode Root;
    Root.Pos = TrunkBaseWorld;
    Root.Parent = -1;
    Root.Depth = 0;
    Root.Dir = InitialDir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
    Root.Radius = 0.f;
    Root.PipeRadius = 0.f;
    Root.Flags = BNF_Axis; // la raíz es, por definición, el pie del eje principal

    return Nodes.Add(Root); // siempre 0: Nodes se acaba de vaciar
}

int32 FTreeSkeleton::AddChild(int32 ParentIndex, const FVector& Pos, const FVector& Dir, uint8 InFlags)
{
    // La invariante Parent < índice exige que el padre ya exista: con un índice inválido
    // se devuelve INDEX_NONE en vez de corromper el esqueleto.
    if (!Nodes.IsValidIndex(ParentIndex))
    {
        return INDEX_NONE;
    }

    FBranchNode Child;
    Child.Pos = Pos;
    Child.Parent = ParentIndex;
    Child.Depth = Nodes[ParentIndex].Depth + 1;
    Child.Dir = Dir.GetSafeNormal(SMALL_NUMBER, Nodes[ParentIndex].Dir);
    Child.Radius = 0.f;
    Child.PipeRadius = 0.f;
    Child.Flags = InFlags;

    return Nodes.Add(Child);
}

void FTreeSkeleton::ComputeChildCounts(TArray<int32>& OutChildCount) const
{
    const int32 N = Nodes.Num();
    OutChildCount.Reset();
    OutChildCount.SetNumZeroed(N);
    for (int32 i = 0; i < N; ++i)
    {
        const int32 P = Nodes[i].Parent;
        if (P >= 0 && P < N)
        {
            ++OutChildCount[P];
        }
    }
}

void FTreeSkeleton::ComputeAlongLengths(TArray<float>& OutAlongLen) const
{
    const int32 N = Nodes.Num();
    OutAlongLen.Reset();
    OutAlongLen.SetNumZeroed(N);
    // Índice creciente: con la invariante Parent < índice, el acumulado del padre ya
    // está resuelto cuando se lee. El clamp solo acota un Parent corrupto al array.
    for (int32 i = 1; i < N; ++i)
    {
        const int32 P = FMath::Clamp(Nodes[i].Parent, 0, N - 1);
        OutAlongLen[i] = OutAlongLen[P] + static_cast<float>(FVector::Dist(Nodes[i].Pos, Nodes[P].Pos));
    }
}