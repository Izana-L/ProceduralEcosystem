#include "Geometry/TreeMeshBuilder.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeWindData.h"     // Fase 6: pivotes de rama, balanceo y AO
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand

namespace
{
    constexpr float MinBranchRadiusCm = 0.05f;
    constexpr float UvAlongScale = 1.f / 100.f; // 1 unidad de UV.v por metro de rama
    constexpr float CmToM = 0.01f;              // Fase 6: los pivotes viajan en METROS

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

    /**
     * Fase 6: escribe los canales de viento/AO de UN vertice.
     *
     * PivotLocalCm viene relativo a la base del tronco y sale en METROS (ver la
     * nota de precision de TreeWindData.h). El resto son escalares [0,1].
     */
    void WriteWindVertex(FTreeMeshBuffers& B, int32 Vi, const FVector& PivotLocalCm,
        float BranchLevel01, float SwayWeight, float Phase01, float CanopyAO, float Tint)
    {
        B.UV1[Vi] = FVector2D(PivotLocalCm.X * CmToM, PivotLocalCm.Y * CmToM);
        B.UV2[Vi] = FVector2D(PivotLocalCm.Z * CmToM, BranchLevel01);
        B.UV3[Vi] = FVector2D(SwayWeight, Phase01);
        B.Colors[Vi] = FLinearColor(CanopyAO, Tint, BranchLevel01, 1.f);
    }

    /** Igual, pero anadiendo al final de los arrays (las hojas se van anadiendo). */
    void AppendWindVertex(FTreeMeshBuffers& B, const FVector& PivotLocalCm,
        float BranchLevel01, float SwayWeight, float Phase01, float CanopyAO, float Tint)
    {
        B.UV1.Add(FVector2D(PivotLocalCm.X * CmToM, PivotLocalCm.Y * CmToM));
        B.UV2.Add(FVector2D(PivotLocalCm.Z * CmToM, BranchLevel01));
        B.UV3.Add(FVector2D(SwayWeight, Phase01));
        B.Colors.Add(FLinearColor(CanopyAO, Tint, BranchLevel01, 1.f));
    }
}

namespace TreeMeshBuilder
{
    void BuildMesh(const FTreeSkeleton& Skeleton, const USpeciesData& Species, uint32& RngState,
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
        // NOTA DE DETERMINISMO: FTreeWindData NO consume RngState. Los desfases
        // salen de hashes del indice de rama y de la semilla del arbol. Asi la
        // geometria que produce este mallador es BIT A BIT la misma que antes de
        // la Fase 6 -las mallas de la libreria no cambian, solo se les anaden
        // canales- y los bakes de la Fase 5 siguen cuadrando.
        // ===================================================================
        FTreeWindData Wind;
        Wind.Build(Skeleton, Species, FineLight, /*Seed*/ RngState);

        // --- Marcos de rotacion minima + longitud acumulada + n de hijos ---
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
        const int32 RingVerts = K + 1;          // <-- +1: vertice de costura (duplicado en u=1)
        const int32 VertCount = N * RingVerts;
        W.Vertices.SetNumUninitialized(VertCount);
        W.Normals.SetNumUninitialized(VertCount);
        W.UVs.SetNumUninitialized(VertCount);
        W.Tangents.SetNumUninitialized(VertCount);
        // Fase 6: canales de viento/AO, en lockstep con los demas.
        W.UV1.SetNumUninitialized(VertCount);
        W.UV2.SetNumUninitialized(VertCount);
        W.UV3.SetNumUninitialized(VertCount);
        W.Colors.SetNumUninitialized(VertCount);

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

                WriteWindVertex(W, Vi, Wn.PivotLocalCm, Wn.BranchLevel01,
                    Wn.SwayWeight, Wn.Phase01, Wn.CanopyAO, Wn.TintVariation);
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

            const int32 BaseI = i * RingVerts;   // <-- paso RingVerts, no K
            const int32 BaseP = P * RingVerts;
            for (int32 k = 0; k < K; ++k)        // K quads; k+1 nunca se sale (hay K+1 vertices)
            {
                W.Triangles.Add(BaseP + k);     W.Triangles.Add(BaseI + k);     W.Triangles.Add(BaseP + k + 1);
                W.Triangles.Add(BaseP + k + 1); W.Triangles.Add(BaseI + k);     W.Triangles.Add(BaseI + k + 1);
            }
        }

        // --- HOJAS: leaf cards en los nodos terminales ---
        FTreeMeshBuffers& L = OutMesh.Leaves;
        const float Half = FMath::Max(Species.LeafSizeCm * 0.5f, 0.5f);
        const float Density = FMath::Clamp(Species.LeafDensity, 0.f, 1.f);
        // Fase 6: la hoja SIEMPRE se mueve algo, aunque cuelgue de una rama
        // gruesa: una hoja pesa nada. Se mezcla el balanceo de su rama con un
        // suelo propio y se escala con el aleteo de la especie.
        const float Flutter = FMath::Clamp(Species.LeafFlutterScale, 0.f, 2.f);

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

            // --- Fase 6: viento/AO de la card ---
            // Pivote: el de la RAMA de la que cuelga (no el de la hoja). Asi la
            // hoja acompana el balanceo de su rama; el aleteo de alta frecuencia
            // lo anade el material como un offset pequeno, que no necesita pivote.
            const FTreeWindNode& Wn = Wind.Nodes[i];
            const float LeafSway = FMath::Clamp((0.35f + 0.65f * Wn.SwayWeight) * Flutter, 0.f, 1.f);

            // Desfase POR HOJA, derivado por hash del indice de nodo (NO del
            // RngState: asi no se altera la secuencia que coloca las hojas y la
            // geometria sigue siendo identica a la de la Fase 5).
            const float LeafPhase = FMath::Frac(Wn.Phase01 + 0.6180339887f * static_cast<float>(i));

            for (int32 j = 0; j < 4; ++j)
            {
                AppendWindVertex(L, Wn.PivotLocalCm, Wn.BranchLevel01,
                    LeafSway, LeafPhase, Wn.CanopyAO, Wn.TintVariation);
            }

            // Dos triangulos (material de hoja two-sided: el winding da igual).
            L.Triangles.Add(B + 0); L.Triangles.Add(B + 1); L.Triangles.Add(B + 2);
            L.Triangles.Add(B + 0); L.Triangles.Add(B + 2); L.Triangles.Add(B + 3);
        }
    }
}
