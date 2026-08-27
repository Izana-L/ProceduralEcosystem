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
    Root.Flags = BNF_Axis; // la raiz es, por definicion, el pie del eje principal

    return Nodes.Add(Root); // siempre 0: Nodes se acaba de vaciar
}

int32 FTreeSkeleton::AddChild(int32 ParentIndex, const FVector& Pos, const FVector& Dir, uint8 InFlags)
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
    for (int32 i = 1; i < N; ++i)
    {
        const int32 P = FMath::Clamp(Nodes[i].Parent, 0, N - 1);
        OutAlongLen[i] = OutAlongLen[P] + static_cast<float>(FVector::Dist(Nodes[i].Pos, Nodes[P].Pos));
    }
}