#include "Geometry/TreeMeshBuilder.h"
#include "Geometry/TreeSkeleton.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand

namespace
{
    constexpr float MinBranchRadiusCm = 0.05f;
    constexpr float UvAlongScale = 1.f / 100.f; // 1 unidad de UV.v por metro de rama

    /** Vector unitario aleatorio, reproducible desde RngState. */
    FVector RandUnitVector(uint32& Rng)
    {
        const FVector V(
            EcoRand::NextRange(Rng, -1.f, 1.f),
            EcoRand::NextRange(Rng, -1.f, 1.f),
            EcoRand::NextRange(Rng, -1.f, 1.f));
        return V.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
    }

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
    void BuildMesh(const FTreeSkeleton& Skeleton, const USpeciesData& Species, uint32& RngState, FTreeMeshData& OutMesh)
    {
        OutMesh.Reset();

        const int32 N = Skeleton.Num();
        if (N < 2)
        {
            return; // sin al menos un internodo no hay tubo que construir
        }

        const int32 K = FMath::Clamp(Species.RingSegments, 3, 16);

        // --- Marcos de rotacion minima + longitud acumulada + nº de hijos ---
        // Se calculan en orden de indice (padre antes que hijo, por la invariante
        // de FTreeSkeleton), asi el marco del hijo deriva del del padre.
        TArray<FVector> FrameN; FrameN.SetNumUninitialized(N); // normal del anillo
        TArray<FVector> FrameB; FrameB.SetNumUninitialized(N); // binormal del anillo
        TArray<float>   AlongLen; AlongLen.SetNumZeroed(N);    // longitud de rama para UV.v
        TArray<int32>   ChildCount; ChildCount.SetNumZeroed(N);

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

                AlongLen[i] = AlongLen[P] + FVector::Dist(Node.Pos, Skeleton.Nodes[P].Pos);
                ChildCount[P] += 1;
            }

            FrameN[i] = Nrm;
            FrameB[i] = FVector::CrossProduct(T, Nrm).GetSafeNormal(SMALL_NUMBER, FVector::CrossProduct(T, AnyPerpendicular(T)));
        }

        // --- MADERA: un anillo de K vertices por nodo ---
        FTreeMeshBuffers& W = OutMesh.Wood;
        const int32 VertCount = N * K;
        W.Vertices.SetNumUninitialized(VertCount);
        W.Normals.SetNumUninitialized(VertCount);
        W.UVs.SetNumUninitialized(VertCount);
        W.Tangents.SetNumUninitialized(VertCount);

        for (int32 i = 0; i < N; ++i)
        {
            const FBranchNode& Node = Skeleton.Nodes[i];
            const FVector Nrm = FrameN[i];
            const FVector Bin = FrameB[i];
            const float Radius = FMath::Max(Node.Radius, MinBranchRadiusCm);
            const float V = AlongLen[i] * UvAlongScale;

            for (int32 k = 0; k < K; ++k)
            {
                const float Ang = 2.f * PI * (float)k / (float)K;
                const float C = FMath::Cos(Ang);
                const float S = FMath::Sin(Ang);
                const FVector Off = C * Nrm + S * Bin; // radial hacia fuera, unitario

                const int32 Vi = i * K + k;
                W.Vertices[Vi] = Node.Pos + Radius * Off;
                W.Normals[Vi] = Off;                       // normal exterior
                W.UVs[Vi] = FVector2D((float)k / (float)K, V);
                W.Tangents[Vi] = (-S * Nrm + C * Bin);      // direccion U (alrededor del tronco)
            }
        }

        // --- MADERA: conectar cada anillo con el de su padre ---
        // Winding elegido para cara exterior. NOTA: si al probar en UE la corteza
        // sale invertida (se ve el interior), intercambia dos indices de cada
        // triangulo o pon el material de corteza a two-sided.
        for (int32 i = 0; i < N; ++i)
        {
            const int32 P = Skeleton.Nodes[i].Parent;
            if (P < 0) { continue; }

            const int32 BaseI = i * K;
            const int32 BaseP = P * K;
            for (int32 k = 0; k < K; ++k)
            {
                const int32 K2 = (k + 1) % K;

                W.Triangles.Add(BaseP + k);  W.Triangles.Add(BaseI + k);  W.Triangles.Add(BaseP + K2);
                W.Triangles.Add(BaseP + K2); W.Triangles.Add(BaseI + k);  W.Triangles.Add(BaseI + K2);
            }
        }

        // --- HOJAS: leaf cards en los nodos terminales ---
        FTreeMeshBuffers& L = OutMesh.Leaves;
        const float Half = FMath::Max(Species.LeafSizeCm * 0.5f, 0.5f);
        const float Density = FMath::Clamp(Species.LeafDensity, 0.f, 1.f);

        for (int32 i = 0; i < N; ++i)
        {
            if (ChildCount[i] != 0) { continue; }                 // solo puntas
            if (EcoRand::NextUnit(RngState) > Density) { continue; }

            // Normal de la hoja: mayormente hacia la luz (arriba) con jitter.
            const FVector LeafN = (FVector::UpVector + 0.6f * RandUnitVector(RngState))
                .GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
            FVector U = FVector::CrossProduct(LeafN, FVector::ForwardVector);
            if (U.IsNearlyZero()) { U = FVector::CrossProduct(LeafN, FVector::RightVector); }
            U = U.GetSafeNormal(SMALL_NUMBER, FVector::RightVector);
            const FVector Vv = FVector::CrossProduct(LeafN, U).GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);

            const FVector Cn = Skeleton.Nodes[i].Pos;
            const int32 B = L.Vertices.Num();

            L.Vertices.Add(Cn - U * Half - Vv * Half);
            L.Vertices.Add(Cn + U * Half - Vv * Half);
            L.Vertices.Add(Cn + U * Half + Vv * Half);
            L.Vertices.Add(Cn - U * Half + Vv * Half);

            for (int32 j = 0; j < 4; ++j)
            {
                L.Normals.Add(LeafN);
                L.Tangents.Add(U);
            }
            L.UVs.Add(FVector2D(0.f, 0.f));
            L.UVs.Add(FVector2D(1.f, 0.f));
            L.UVs.Add(FVector2D(1.f, 1.f));
            L.UVs.Add(FVector2D(0.f, 1.f));

            // Dos triangulos (material de hoja two-sided: el winding da igual).
            L.Triangles.Add(B + 0); L.Triangles.Add(B + 1); L.Triangles.Add(B + 2);
            L.Triangles.Add(B + 0); L.Triangles.Add(B + 2); L.Triangles.Add(B + 3);
        }
    }
}