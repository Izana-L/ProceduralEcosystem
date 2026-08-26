#include "Geometry/TreeMeshBuilder.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeWindData.h"     // Fase 6: pivotes de rama, balanceo y AO
#include "Geometry/TreeFoliage.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"

namespace
{
    constexpr float MinBranchRadiusCm = 0.05f;
    constexpr float UvAlongScale = 1.f / 100.f; // 1 unidad de UV.v por metro de rama
    constexpr float TipConeLengthFrac = 0.55f;  // largo del apice como fraccion del internodo
    constexpr float MinTipConeRadii = 2.f;      // ... con un minimo en radios de la ramilla

    /** Una perpendicular cualquiera y estable a T (T debe venir normalizado). */
    FVector AnyPerpendicular(const FVector& T)
    {
        FVector P = FVector::CrossProduct(T, FVector::RightVector);
        if (P.IsNearlyZero())
        {
            P = FVector::CrossProduct(T, FVector::ForwardVector);
        }
        return P.GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);
    }
}

namespace TreeMeshBuilder
{
    void BuildMesh(const FTreeSkeleton& Skeleton, const USpeciesData& Species, uint32 Seed,
        FTreeMeshData& OutMesh, const FTreeLightGridFine* FineLight)
    {
        OutMesh.Reset();

        const int32 N = Skeleton.Num();
        if (N < 2)
        {
            return; // sin al menos un internodo no hay tubo que construir
        }

        const int32 K = FMath::Clamp(Species.RingSegments, 3, 16);

        // ===================================================================
        // FASE 6: atributos de viento y AO por NODO, antes de mallar.
        //
        // Se calculan a partir del esqueleto (padres, radios del pipe model) y
        // de la rejilla de luz fina que dejo el SCA: no hay que reconstruir
        // ninguna jerarquia ni hornear texturas de pivotes (doc. 6.1).
        //
        // FTreeWindData deja ademas publicadas las dos pasadas O(N) que comparte
        // con el mallador (ChildCount y AlongLen): aqui se reutilizan, no se
        // recalculan.
        // ===================================================================
        FTreeWindData Wind;
        Wind.Build(Skeleton, Species, FineLight, Seed);

        const TArray<int32>& ChildCount = Wind.ChildCount;
        const TArray<float>& AlongLen = Wind.AlongLen;

        // --- Marcos de rotacion minima ---
        // Se calculan en orden de indice (padre antes que hijo, por la invariante
        // de FTreeSkeleton), asi el marco del hijo deriva del del padre.
        TArray<FVector> FrameN; FrameN.SetNumUninitialized(N); // normal del anillo
        TArray<FVector> FrameB; FrameB.SetNumUninitialized(N); // binormal del anillo

        for (int32 i = 0; i < N; ++i)
        {
            const FBranchNode& Node = Skeleton.Nodes[i];
            const FVector T = Node.Dir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);

            FVector Nrm;
            if (Node.Parent < 0)
            {
                Nrm = AnyPerpendicular(T);
            }
            else
            {
                const int32 P = Node.Parent;
                const FVector Tp = Skeleton.Nodes[P].Dir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);

                // Transporte paralelo: rota el marco del padre por la rotacion que
                // lleva su tangente a la de este nodo -> minima torsion.
                const FQuat Q = FQuat::FindBetweenNormals(Tp, T);
                FVector Np = Q.RotateVector(FrameN[P]);
                // Reortogonaliza contra T (elimina deriva numerica).
                Np = (Np - FVector::DotProduct(Np, T) * T).GetSafeNormal(SMALL_NUMBER, AnyPerpendicular(T));
                Nrm = Np;
            }

            FrameN[i] = Nrm;
            FrameB[i] = FVector::CrossProduct(T, Nrm).GetSafeNormal(SMALL_NUMBER, FVector::CrossProduct(T, AnyPerpendicular(T)));
        }

        // --- MADERA: reparto del buffer de vertices ---
        //   [0, N*RingVerts)          anillos, uno por nodo
        //   BaseCapVert               centro de la tapa de la base del tronco
        //   ApexVert[i]               apice de cada nodo terminal
        FTreeMeshBuffers& W = OutMesh.Wood;
        const int32 RingVerts = K + 1;          // <-- +1: vertice de costura (duplicado en u=1)
        const int32 RingBlock = N * RingVerts;
        const int32 BaseCapVert = RingBlock;

        TArray<int32> ApexVert;
        ApexVert.Init(INDEX_NONE, N);

        int32 VertCount = RingBlock + 1;
        int32 NumApex = 0;
        for (int32 i = 0; i < N; ++i)
        {
            if (ChildCount[i] == 0 && Skeleton.Nodes[i].Parent >= 0)
            {
                ApexVert[i] = VertCount++;
                ++NumApex;
            }
        }

        W.SetNumVertices(VertCount);
        W.Triangles.Reserve(((N - 1) * K * 2 + NumApex * K + K) * 3);

        for (int32 i = 0; i < N; ++i)
        {
            const FBranchNode& Node = Skeleton.Nodes[i];
            const FVector Nrm = FrameN[i];
            const FVector Bin = FrameB[i];
            const float Radius = FMath::Max(Node.Radius, MinBranchRadiusCm);
            const float V = AlongLen[i] * UvAlongScale;

            // Fase 6: TODOS los vertices del anillo comparten los atributos de su
            // nodo. Es importante que sea asi: si variasen alrededor del anillo,
            // el tubo se deformaria al aplicar el desplazamiento en el material.
            const FTreeWindNode& Wn = Wind.Nodes[i];

            for (int32 k = 0; k <= K; ++k)
            {
                const float Ang = 2.f * PI * (float)k / (float)K;
                const float C = FMath::Cos(Ang);
                const float S = FMath::Sin(Ang);
                const FVector Off = C * Nrm + S * Bin;

                const int32 Vi = i * RingVerts + k;
                W.Vertices[Vi] = Node.Pos + Radius * Off;
                W.Normals[Vi] = Off;
                W.UVs[Vi] = FVector2D((float)k / (float)K, V);
                W.Tangents[Vi] = (-S * Nrm + C * Bin);

                W.SetWindVertex(Vi, Wn.PivotLocalCm, Wn.BranchLevel01,
                    Wn.SwayWeight, Wn.Phase01, Wn.CanopyAO, Wn.TintVariation);
            }
        }

        // --- MADERA: vertice apice de cada punta ---
        // El eje sale del SEGMENTO padre->nodo, no de Node.Dir: el SCA puede
        // dejar nodos cuya Dir apunta hacia atras, y ahi el cono se meteria
        // dentro del tubo del padre.
        for (int32 i = 0; i < N; ++i)
        {
            const int32 Av = ApexVert[i];
            if (Av == INDEX_NONE)
            {
                continue;
            }

            const FBranchNode& Node = Skeleton.Nodes[i];
            const FVector Seg = Node.Pos - Skeleton.Nodes[Node.Parent].Pos;
            const float SegLen = (float)Seg.Size();
            const FVector Axis = Seg.GetSafeNormal(SMALL_NUMBER, Node.Dir);
            const float Radius = FMath::Max(Node.Radius, MinBranchRadiusCm);
            const float TipLen = FMath::Max(SegLen * TipConeLengthFrac, Radius * MinTipConeRadii);

            const FTreeWindNode& Wn = Wind.Nodes[i];
            W.Vertices[Av] = Node.Pos + Axis * TipLen;
            W.Normals[Av] = Axis;
            W.UVs[Av] = FVector2D(0.5f, (AlongLen[i] + TipLen) * UvAlongScale);
            W.Tangents[Av] = FrameN[i];
            W.SetWindVertex(Av, Wn.PivotLocalCm, Wn.BranchLevel01,
                Wn.SwayWeight, Wn.Phase01, Wn.CanopyAO, Wn.TintVariation);
        }

        // --- MADERA: centro de la tapa de la base ---
        {
            const FBranchNode& Root = Skeleton.Nodes[0];
            const FTreeWindNode& Wn = Wind.Nodes[0];
            W.Vertices[BaseCapVert] = Root.Pos;
            W.Normals[BaseCapVert] = -Root.Dir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
            W.UVs[BaseCapVert] = FVector2D(0.5f, 0.f);
            W.Tangents[BaseCapVert] = FrameN[0];
            W.SetWindVertex(BaseCapVert, Wn.PivotLocalCm, Wn.BranchLevel01,
                Wn.SwayWeight, Wn.Phase01, Wn.CanopyAO, Wn.TintVariation);
        }

        // --- MADERA: conectar cada anillo con el de su padre ---
        // Winding elegido para cara exterior. NOTA: si al probar en UE la corteza
        // sale invertida (se ve el interior), intercambia dos indices de cada
        // triangulo o pon el material de corteza a two-sided.
        for (int32 i = 0; i < N; ++i)
        {
            const int32 P = Skeleton.Nodes[i].Parent;
            if (P < 0) { continue; }

            const int32 BaseI = i * RingVerts;   // <-- paso RingVerts, no K
            const int32 BaseP = P * RingVerts;
            for (int32 k = 0; k < K; ++k)        // K quads; k+1 nunca se sale (hay K+1 vertices)
            {
                W.Triangles.Add(BaseP + k);     W.Triangles.Add(BaseI + k);     W.Triangles.Add(BaseP + k + 1);
                W.Triangles.Add(BaseP + k + 1); W.Triangles.Add(BaseI + k);     W.Triangles.Add(BaseI + k + 1);
            }
        }

        // --- MADERA: cierres ---
        // El abanico del apice es el quad de pared con el anillo del hijo
        // colapsado en un punto; la tapa de la base, con el del padre. De ahi
        // sale el winding sin tener que razonarlo de nuevo.
        for (int32 i = 0; i < N; ++i)
        {
            const int32 Av = ApexVert[i];
            if (Av == INDEX_NONE) { continue; }

            const int32 BaseI = i * RingVerts;
            for (int32 k = 0; k < K; ++k)
            {
                W.Triangles.Add(BaseI + k); W.Triangles.Add(Av); W.Triangles.Add(BaseI + k + 1);
            }
        }

        for (int32 k = 0; k < K; ++k)
        {
            W.Triangles.Add(BaseCapVert); W.Triangles.Add(k); W.Triangles.Add(k + 1);
        }

        // --- HOJAS ---
        TreeFoliage::Build(Skeleton, Wind, Species, FrameN, FrameB, FineLight, Seed, OutMesh.Leaves);
    }
}
