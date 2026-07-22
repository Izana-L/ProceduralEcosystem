#include "Geometry/SpaceColonization.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeLightGridFine.h"
#include "Geometry/AttractorCloud.h"
#include "Terrain/LightFieldCoarse.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand

namespace SpaceColonization
{
    FVector BlendGrowthDirection(
        const FVector& DirSCA, const FVector& DirPrev, const FVector& LightGradient,
        float wSCA, float wGrav, float wPhot, float wPrev)
    {
        const FVector Blended =
            wSCA * DirSCA
            + wGrav * FVector::UpVector
            + wPhot * LightGradient
            + wPrev * DirPrev;

        // Si todo se cancela, caemos a la direccion del SCA (o arriba).
        const FVector FallBack = DirSCA.IsNearlyZero() ? FVector::UpVector : DirSCA;
        return Blended.GetSafeNormal(SMALL_NUMBER, FallBack);
    }

    FVector JitterDirection(const FVector& Dir, float NoiseAmount, uint32& RngState)
    {
        if (NoiseAmount <= 0.f)
        {
            return Dir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        }

        // Vector aleatorio en el cubo [-1,1]^3 -> direccion aleatoria; se suma a
        // Dir escalado por el ruido y se renormaliza. Un angulo mayor cuanto
        // mayor NoiseAmount, acotado para no invertir la direccion.
        const FVector R(
            EcoRand::NextRange(RngState, -1.f, 1.f),
            EcoRand::NextRange(RngState, -1.f, 1.f),
            EcoRand::NextRange(RngState, -1.f, 1.f));

        const FVector Perturbed = Dir + FMath::Clamp(NoiseAmount, 0.f, 1.f) * R;
        return Perturbed.GetSafeNormal(SMALL_NUMBER, Dir);
    }

    void ComputeRadii(FTreeSkeleton& Skeleton, const USpeciesData& Species)
    {
        const int32 N = Skeleton.Num();
        if (N == 0)
        {
            return;
        }

        const double PipeExp = FMath::Max(1.0, (double)Species.PipeExp);
        const double TipR = FMath::Max((double)KINDA_SMALL_NUMBER, (double)Species.TipRadiusCm);

        // Acc[i] = suma de r^n de los hijos de i. Recorremos en indice
        // decreciente: por la invariante Parent < indice, eso procesa cada
        // hijo antes que su padre (doc. 3.6, "NodesByDecreasingDepth").
        TArray<double> Acc;
        Acc.Init(0.0, N);

        for (int32 i = N - 1; i >= 0; --i)
        {
            const double R = (Acc[i] > 0.0) ? FMath::Pow(Acc[i], 1.0 / PipeExp) : TipR;
            Skeleton.Nodes[i].Radius = (float)R;

            const int32 P = Skeleton.Nodes[i].Parent;
            if (P >= 0)
            {
                Acc[P] += FMath::Pow(R, PipeExp);
            }
        }
    }

    void GrowTree(
        const USpeciesData& Species,
        uint32& RngState,
        const FVector& TrunkBaseWorld,
        const FLightFieldCoarse* CoarseLight,
        const FSpaceColonizationConfig& Config,
        FTreeSkeleton& OutSkeleton,
        FTreeLightGridFine& OutFineLight,
        FAttractorCloud& OutAttractors)
    {
        // Parametros de la especie (con guardas para no dividir por 0 / bucles vacios).
        const float d_i = FMath::Max(Species.InfluenceRadiusDi, 1.f);
        const float d_k = FMath::Max(Species.KillRadiusDk, 0.1f);
        const float D = FMath::Max(Species.StepLengthD, 1.f);
        const int32 MaxIter = FMath::Max(Species.MaxIter, 1);
        const float LeafShadowR = D * FMath::Max(Config.LeafShadowRadiusScale, 0.f);
        const float LeafShadowDepth = D * FMath::Max(Config.LeafShadowDepthScale, 0.f);
        const bool  bHasCoarse = (CoarseLight != nullptr) && CoarseLight->IsValid();

        // --- 1) Sembrar atractores en la copa (determinista desde RngState) ---
        OutAttractors.SampleCrownEnvelope(Species, TrunkBaseWorld, RngState);

        // --- 2) Rejilla fina sobre la envolvente + trunk base; sombra de vecinos ---
        FBox EnvBounds(ForceInit);
        for (const FAttractor& A : OutAttractors.Attractors)
        {
            EnvBounds += A.Pos;
        }
        EnvBounds += TrunkBaseWorld; // que la rejilla cubra tambien el tronco
        OutFineLight.InitForBounds(EnvBounds, Species.FineVoxelSizeCm, Config.FineGridPaddingCm);

        if (bHasCoarse)
        {
            OutFineLight.SeedFromCoarse(*CoarseLight);
        }

        // --- 3) Cull inicial (micro<-macro): atractores en la sombra de vecinos ---
        OutAttractors.CullByShade(OutFineLight, Config.LightCullThreshold);

        // --- 4) Indice espacial de atractores (celda = d_i) ---
        OutAttractors.BuildIndex(d_i);

        // --- 5) Nodo raiz en la base del tronco ---
        OutSkeleton.Reset();
        OutSkeleton.Reserve(OutAttractors.Num() * 2 + 16);
        OutSkeleton.InitRoot(TrunkBaseWorld, FVector::UpVector);

        // Scratch reutilizado entre iteraciones.
        TArray<FVector> SumDir;
        TArray<int32>   Count;
        TArray<int32>   NewChildren;

        for (int32 Iter = 0; Iter < MaxIter; ++Iter)
        {
            const int32 NumNodes = OutSkeleton.Num();

            // ---- ASOCIAR: cada atractor vivo -> su nodo mas cercano dentro de d_i ----
            for (FAttractor& A : OutAttractors.Attractors)
            {
                if (A.bAlive)
                {
                    A.BestNode = INDEX_NONE;
                    A.BestDist = d_i;
                }
            }

            for (int32 v = 0; v < NumNodes; ++v)
            {
                const FVector NodePos = OutSkeleton.Nodes[v].Pos;
                OutAttractors.ForEachInRange(NodePos, d_i, [&](int32 Ai)
                    {
                        FAttractor& A = OutAttractors.Attractors[Ai];
                        if (!A.bAlive) { return; }
                        const float Dd = FVector::Dist(A.Pos, NodePos);
                        if (Dd < A.BestDist)
                        {
                            A.BestDist = Dd;
                            A.BestNode = v;
                        }
                    });
            }

            // ---- Acumular direccion promedio hacia los atractores por nodo ----
            SumDir.Reset(); SumDir.SetNumZeroed(NumNodes);
            Count.Reset();  Count.SetNumZeroed(NumNodes);

            for (const FAttractor& A : OutAttractors.Attractors)
            {
                if (A.bAlive && A.BestNode != INDEX_NONE)
                {
                    const int32 v = A.BestNode;
                    SumDir[v] += (A.Pos - OutSkeleton.Nodes[v].Pos).GetSafeNormal();
                    Count[v]++;
                }
            }

            // ---- CRECER: un hijo por nodo activo (SCA + tropismos + jitter) ----
            const int32 Base = NumNodes;
            NewChildren.Reset();

            for (int32 v = 0; v < Base; ++v)
            {
                if (Count[v] <= 0) { continue; }

                const FVector NodePos = OutSkeleton.Nodes[v].Pos;
                const FVector NodeDir = OutSkeleton.Nodes[v].Dir;

                const FVector DirSCA = SumDir[v].GetSafeNormal(SMALL_NUMBER, NodeDir);
                const FVector LightGrad = (Config.bEnablePhototropism && Species.wPhot > 0.f)
                    ? OutFineLight.GradientOfLight(NodePos)
                    : FVector::ZeroVector;

                FVector Dir = BlendGrowthDirection(
                    DirSCA, NodeDir, LightGrad,
                    Species.wSCA, Species.wGrav, Species.wPhot, Species.wPrev);
                Dir = JitterDirection(Dir, Species.DirNoise, RngState);

                const FVector ChildPos = NodePos + D * Dir;
                const int32 Ci = OutSkeleton.AddChild(v, ChildPos, Dir);
                if (Ci != INDEX_NONE)
                {
                    NewChildren.Add(Ci);
                }
            }

            if (NewChildren.Num() == 0)
            {
                break; // no crecio nada: hemos terminado
            }

            // ---- MATAR: atractores dentro de d_k de algun hijo nuevo ----
            const float d_k2 = d_k * d_k;
            for (int32 Ci : NewChildren)
            {
                const FVector ChildPos = OutSkeleton.Nodes[Ci].Pos;
                OutAttractors.ForEachInRange(ChildPos, d_k, [&](int32 Ai)
                    {
                        FAttractor& A = OutAttractors.Attractors[Ai];
                        if (A.bAlive && FVector::DistSquared(A.Pos, ChildPos) <= d_k2)
                        {
                            A.bAlive = false;
                        }
                    });
            }

            // ---- Refresco de luz / autopoda emergente ----
            if (Config.bEnableSelfPruning && Species.LightEvery > 0 && (Iter % Species.LightEvery == 0))
            {
                // Base = sombra de vecinos (o limpia) + follaje propio actual.
                if (bHasCoarse) { OutFineLight.SeedFromCoarse(*CoarseLight); }
                else { OutFineLight.ClearShadow(); }

                OutFineLight.DepositLeafShadow(OutSkeleton, LeafShadowR, LeafShadowDepth, Config.PerNodeShadowDensity);
                OutAttractors.CullByShade(OutFineLight, Config.LightCullThreshold);
            }

            if (OutAttractors.CountAlive() == 0)
            {
                break; // no quedan atractores que perseguir
            }
        }

        // --- Radios de rama: pipe model sobre el esqueleto terminado ---
        ComputeRadii(OutSkeleton, Species);
    }
}