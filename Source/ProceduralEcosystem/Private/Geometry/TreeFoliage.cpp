#include "Geometry/TreeFoliage.h"

#include "Geometry/TreeMeshBuilder.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeWindData.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h"
#include "Core/EcoGeometry.h" // PerpendicularTo (copia unica, ver la cabecera)

namespace
{
    enum : uint32
    {
        SaltSkip  = 0x9E3779B9u,
        SaltSize  = 0x85EBCA6Bu,
        SaltRoll  = 0xC2B2AE35u,
        SaltPhase = 0x27D4EB2Fu
    };

    constexpr float MaxRollRad = 0.44f;
    constexpr float MinSizeScale = 0.80f;
    constexpr float MaxSizeScale = 1.25f;
    constexpr float MinAttachRadiusCm = 0.05f;

    /** Valor estable en [0,1) para (arbol, rama, ranura de hoja): solo mezcla la
        clave y delega en la copia unica del hash (EcoRand::HashUnit). */
    FORCEINLINE float LeafUnit(uint32 Seed, int32 BranchRoot, int32 Slot, uint32 Salt)
    {
        return EcoRand::HashUnit(Seed
            ^ (static_cast<uint32>(BranchRoot) * 2654435761u)
            ^ (static_cast<uint32>(Slot) * 40503u), Salt);
    }

    /** Perpendicular a Along lo mas cerca posible de Pref, con dos reservas.
        La logica es la compartida de EcoGeometry (misma que usan el SCA y el
        mallador); aqui solo cambia el vector de ultimo recurso. */
    FORCEINLINE FVector SideAxis(const FVector& Along, const FVector& Pref, const FVector& Fallback)
    {
        return EcoGeometry::PerpendicularTo(Along, Pref, Fallback, FVector::RightVector);
    }
}

namespace TreeFoliage
{
    void Build(const FTreeSkeleton& Skeleton, const FTreeWindData& Wind, const USpeciesData& Species,
        const TArray<FVector>& FrameN, const TArray<FVector>& FrameB,
        const FTreeLightGridFine* FineLight, uint32 Seed, FTreeMeshBuffers& OutLeaves)
    {
        OutLeaves.Reset();

        const int32 N = Skeleton.Num();
        if (N < 2 || !Wind.IsValidFor(Skeleton) || FrameN.Num() < N || FrameB.Num() < N)
        {
            return;
        }

        const float Spacing = FMath::Max(Species.LeafSpacingCm, 0.5f);
        const float Divergence = FMath::DegreesToRadians(Species.PhyllotaxisAngleDeg);
        const float Insertion = FMath::DegreesToRadians(Species.LeafInsertionAngleDeg);
        const float MaxRadius = FMath::Max(Species.TipRadiusCm, KINDA_SMALL_NUMBER)
            * FMath::Max(Species.LeafBearingRadiusScale, 1.f);
        const float Length = FMath::Max(Species.LeafSizeCm, 0.5f);
        const float HalfWidth = FMath::Max(Length * Species.LeafWidthRatio * 0.5f, 0.25f);
        const float Petiole = FMath::Max(Species.PetioleLengthCm, 0.f);
        const float Helio = FMath::Clamp(Species.LeafHeliotropism, 0.f, 1.f);
        const float Fill = FMath::Clamp(Species.LeafDensity, 0.f, 1.f);
        const float Flutter = FMath::Clamp(Species.LeafFlutterScale, 0.f, 2.f);
        const float CosI = FMath::Cos(Insertion);
        const float SinI = FMath::Sin(Insertion);
        const bool bHasLight = (FineLight != nullptr) && FineLight->IsValid();

        float BearingLength = 0.f;
        for (int32 i = 1; i < N; ++i)
        {
            const int32 P = Skeleton.Nodes[i].Parent;
            if (P >= 0 && Skeleton.Nodes[i].Radius <= MaxRadius)
            {
                BearingLength += Wind.AlongLen[i] - Wind.AlongLen[P];
            }
        }

        const int32 Expected = FMath::Clamp(FMath::RoundToInt(BearingLength / Spacing * Fill), 0, 200000);
        OutLeaves.ReserveVertices(Expected * 4);
        OutLeaves.Triangles.Reserve(Expected * 6);

        for (int32 i = 1; i < N; ++i)
        {
            const FBranchNode& Node = Skeleton.Nodes[i];
            const int32 P = Node.Parent;
            if (P < 0 || Node.Radius > MaxRadius)
            {
                continue;
            }

            const float Start = Wind.AlongLen[P];
            const float SegLen = Wind.AlongLen[i] - Start;
            if (SegLen <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const int32 FirstSlot = FMath::FloorToInt32(Start / Spacing) + 1;
            const int32 LastSlot = FMath::FloorToInt32(Wind.AlongLen[i] / Spacing);
            if (LastSlot < FirstSlot)
            {
                continue;
            }

            const FVector Anchor = Skeleton.Nodes[P].Pos;
            const FVector Seg = Node.Pos - Anchor;
            const FVector Axis = Seg.GetSafeNormal(SMALL_NUMBER, Node.Dir);
            const FVector& Nrm = FrameN[i];
            const FVector& Bin = FrameB[i];
            const float StemRadius = FMath::Max(Node.Radius, MinAttachRadiusCm);

            const FTreeWindNode& Wn = Wind.Nodes[i];
            const int32 Root = Wind.BranchRoot[i];
            const float Sway = FMath::Clamp((0.35f + 0.65f * Wn.SwayWeight) * Flutter, 0.f, 1.f);

            for (int32 Slot = FirstSlot; Slot <= LastSlot; ++Slot)
            {
                if (LeafUnit(Seed, Root, Slot, SaltSkip) > Fill)
                {
                    continue;
                }

                const float T = FMath::Clamp((Slot * Spacing - Start) / SegLen, 0.f, 1.f);
                const float Phi = static_cast<float>(
                    FMath::Fmod(static_cast<double>(Slot) * Divergence, 2.0 * PI));

                const FVector Radial =
                    (FMath::Cos(Phi) * Nrm + FMath::Sin(Phi) * Bin).GetSafeNormal(SMALL_NUMBER, Nrm);
                const FVector Attach = Anchor + Seg * T + Radial * (StemRadius + Petiole);
                const FVector Along = (Radial * CosI + Axis * SinI).GetSafeNormal(SMALL_NUMBER, Radial);

                FVector Pref = FVector::UpVector;
                if (bHasLight && Helio > 0.f)
                {
                    const FVector Grad = FineLight->GradientOfLight(Attach);
                    if (!Grad.IsNearlyZero())
                    {
                        Pref = FMath::Lerp(FVector::UpVector, Grad, Helio)
                            .GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
                    }
                }

                const FQuat Roll(Along, (2.f * LeafUnit(Seed, Root, Slot, SaltRoll) - 1.f) * MaxRollRad);
                const FVector Side = Roll.RotateVector(SideAxis(Along, Pref, Axis));
                const FVector Norm = FVector::CrossProduct(Side, Along)
                    .GetSafeNormal(SMALL_NUMBER, FVector::UpVector);

                const float Scale = FMath::Lerp(MinSizeScale, MaxSizeScale,
                    LeafUnit(Seed, Root, Slot, SaltSize));
                const FVector HalfSpan = Side * (HalfWidth * Scale);
                const FVector Blade = Along * (Length * Scale);

                const int32 Base = OutLeaves.Vertices.Num();
                OutLeaves.Vertices.Add(Attach - HalfSpan);
                OutLeaves.Vertices.Add(Attach + HalfSpan);
                OutLeaves.Vertices.Add(Attach + Blade + HalfSpan);
                OutLeaves.Vertices.Add(Attach + Blade - HalfSpan);

                OutLeaves.UVs.Add(FVector2D(0.f, 0.f));
                OutLeaves.UVs.Add(FVector2D(1.f, 0.f));
                OutLeaves.UVs.Add(FVector2D(1.f, 1.f));
                OutLeaves.UVs.Add(FVector2D(0.f, 1.f));

                const float Phase = LeafUnit(Seed, Root, Slot, SaltPhase);
                for (int32 j = 0; j < 4; ++j)
                {
                    OutLeaves.Normals.Add(Norm);
                    OutLeaves.Tangents.Add(Side);
                    OutLeaves.AppendWindVertex(Wn, Sway, Phase);
                }

                OutLeaves.Triangles.Add(Base + 0);
                OutLeaves.Triangles.Add(Base + 1);
                OutLeaves.Triangles.Add(Base + 2);
                OutLeaves.Triangles.Add(Base + 0);
                OutLeaves.Triangles.Add(Base + 2);
                OutLeaves.Triangles.Add(Base + 3);
            }
        }
    }
}
