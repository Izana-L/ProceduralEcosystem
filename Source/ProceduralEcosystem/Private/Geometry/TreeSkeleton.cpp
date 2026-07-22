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

    return Nodes.Add(Root); // siempre 0: Nodes se acaba de vaciar
}

int32 FTreeSkeleton::AddChild(int32 ParentIndex, const FVector& Pos, const FVector& Dir)
{
    // La invariante Parent < indice exige que el padre ya exista. Si no,
    // devolvemos -1 en vez de corromper el esqueleto (el SCA nunca deberia
    // llegar aqui con un indice invalido).
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

    return Nodes.Add(Child);
}

FBox FTreeSkeleton::ComputeBounds() const
{
    FBox Bounds(ForceInit); // caja invalida hasta el primer punto
    for (const FBranchNode& N : Nodes)
    {
        Bounds += N.Pos;
    }
    return Bounds;
}