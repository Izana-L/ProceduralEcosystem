#include "Geometry/TreeMeshBuilder.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeWindData.h"     // Fase 6: pivotes de rama, balanceo y AO
#include "Geometry/TreeFoliage.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h"     // EcoRand::HashUnit (hashes estables, NO consumo de RNG)
#include "Core/EcoGeometry.h" // AnyPerpendicular (copia unica, ver la cabecera)

namespace
{
    constexpr float MinBranchRadiusCm = 0.05f;
    constexpr float UvAlongScale = 1.f / 100.f; // 1 unidad de UV.v por metro de rama
    constexpr float TipConeLengthFrac = 0.55f;  // largo del apice como fraccion del internodo
    constexpr float MinTipConeRadii = 2.f;      // ... con un minimo en radios de la ramilla

    /** Por debajo de esta amplitud, la deformacion de seccion no se ve: no vale
        la pena pagar ni los segmentos extra ni la pasada de normales. */
    constexpr float MinSectionDeform = 0.01f;

    /** Minimo de segmentos de anillo con deformacion activa. Un hexagono no
        puede tener tres lobulos: sin resolucion angular, el relieve no existe. */
    constexpr int32 MinRingSegmentsForRelief = 8;

    /** Vueltas que da la fase de los lobulos a lo largo del arbol. Es lo que se
        lee como "tronco retorcido": una seccion lobulada que NO gira se lee como
        un prisma extruido, que es tan artificial como el cilindro. */
    constexpr float SectionTwistTurns = 0.4f;

    /** Escala del relieve grueso, en radios de la base del tronco. */
    constexpr float ReliefWaveInBaseRadii = 1.2f;
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

        // --- Deformacion de seccion (el tronco deja de ser un cilindro) ---
        const float LobeAmp = FMath::Clamp(Species.SectionLobeAmount, 0.f, 0.4f);
        const int32 LobeCount = FMath::Clamp(Species.SectionLobeCount, 2, 6);
        const float ReliefAmp = FMath::Clamp(Species.BarkReliefAmount, 0.f, 0.2f);
        const bool bDeformSection = (LobeAmp > MinSectionDeform) || (ReliefAmp > MinSectionDeform);

        int32 K = FMath::Clamp(Species.RingSegments, 3, 16);
        if (bDeformSection)
        {
            K = FMath::Max(K, MinRingSegmentsForRelief);
        }

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
                Nrm = EcoGeometry::AnyPerpendicular(T);
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
                Np = (Np - FVector::DotProduct(Np, T) * T).GetSafeNormal(SMALL_NUMBER, EcoGeometry::AnyPerpendicular(T));
                Nrm = Np;
            }

            FrameN[i] = Nrm;
            FrameB[i] = FVector::CrossProduct(T, Nrm).GetSafeNormal(SMALL_NUMBER, FVector::CrossProduct(T, EcoGeometry::AnyPerpendicular(T)));
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

        // Escalas de la deformacion, derivadas del arbol (no hay que exponerlas
        // como parametros ni escalarlas por bucket: salen ya proporcionadas).
        const float TrunkBaseRadius = FMath::Max(Skeleton.Nodes[0].Radius, MinBranchRadiusCm);
        const float InvReliefWave = 1.f / FMath::Max(TrunkBaseRadius * ReliefWaveInBaseRadii, 2.f);
        float MaxAlongLen = 0.f;
        for (int32 i = 0; i < N; ++i) { MaxAlongLen = FMath::Max(MaxAlongLen, AlongLen[i]); }
        const float InvMaxAlong = (MaxAlongLen > KINDA_SMALL_NUMBER) ? (1.f / MaxAlongLen) : 0.f;

        // Fase de los lobulos y offset del ruido: hash de la semilla, NUNCA
        // EcoRand::Next*. El mallador no debe consumir RNG (ver la cabecera):
        // desplazaria el stream que el SCA gasto antes sobre el mismo arbol.
        const float LobeSeedPhase = EcoRand::HashUnit(Seed, 0x7F4A7C15u) * 2.f * PI;
        const FVector ReliefOffset(
            EcoRand::HashUnit(Seed, 0x9E3779B9u) * 512.f,
            EcoRand::HashUnit(Seed, 0x85EBCA6Bu) * 512.f,
            EcoRand::HashUnit(Seed, 0xC2B2AE35u) * 512.f);

        for (int32 i = 0; i < N; ++i)
        {
            const FBranchNode& Node = Skeleton.Nodes[i];
            const FVector Nrm = FrameN[i];
            const FVector Bin = FrameB[i];
            const float Radius = FMath::Max(Node.Radius, MinBranchRadiusCm);
            const float V = AlongLen[i] * UvAlongScale;

            // La deformacion se apaga en la madera fina: una ramilla de 1 cm no
            // tiene lobulos, y ahi el desplazamiento solo produce ruido.
            const float DeformWeight = bDeformSection
                ? FMath::SmoothStep(TrunkBaseRadius * 0.06f, TrunkBaseRadius * 0.30f, Radius)
                : 0.f;

            // Fase de los lobulos: gira lentamente con la altura.
            const float LobePhase = LobeSeedPhase
                + SectionTwistTurns * 2.f * PI * (AlongLen[i] * InvMaxAlong);

            // Fase 6: TODOS los vertices del anillo comparten los atributos de su
            // nodo. Es importante que sea asi: si variasen alrededor del anillo,
            // el tubo se deformaria al aplicar el desplazamiento en el material.
            const FTreeWindNode& Wn = Wind.Nodes[i];

            for (int32 k = 0; k <= K; ++k)
            {
                // COSTURA: k = K es el MISMO punto que k = 0, duplicado solo para
                // poder cerrar la UV en u = 1. Reusar literalmente el angulo de
                // k = 0 hace que los dos vertices sean identicos BIT A BIT; si se
                // dejara al azar de la aritmetica de coma flotante (cos(2*PI) no
                // es exactamente cos(0)), cualquier deformacion no periodica
                // abriria una raja a lo largo de todo el tronco.
                const int32 kWrap = (k == K) ? 0 : k;
                const float Ang = 2.f * PI * (float)kWrap / (float)K;
                const float C = FMath::Cos(Ang);
                const float S = FMath::Sin(Ang);
                const FVector Off = C * Nrm + S * Bin;

                float RadiusScale = 1.f;
                if (DeformWeight > 0.f)
                {
                    // (a) Lobulos: serie de Fourier en el angulo -> periodica en
                    //     2*PI por construccion, sin costura posible.
                    const float Lobes = LobeAmp * FMath::Cos((float)LobeCount * Ang + LobePhase);

                    // (b) Relieve grueso: ruido 3D muestreado en la POSICION del
                    //     vertice sin deformar. Al ser funcion de la posicion,
                    //     sale coherente entre anillos vecinos gratis, sin tener
                    //     que propagar ninguna fase a lo largo de la rama.
                    float Relief = 0.f;
                    if (ReliefAmp > 0.f)
                    {
                        const FVector SamplePos = (Node.Pos + Radius * Off) * InvReliefWave + ReliefOffset;
                        Relief = ReliefAmp * FMath::PerlinNoise3D(SamplePos);
                    }

                    RadiusScale = FMath::Max(1.f + DeformWeight * (Lobes + Relief), 0.25f);
                }

                const int32 Vi = i * RingVerts + k;
                W.Vertices[Vi] = Node.Pos + (Radius * RadiusScale) * Off;
                W.Normals[Vi] = Off;
                W.UVs[Vi] = FVector2D((float)k / (float)K, V);
                W.Tangents[Vi] = (-S * Nrm + C * Bin);

                W.SetWindVertex(Vi, Wn);
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
            W.SetWindVertex(Av, Wn);
        }

        // --- MADERA: centro de la tapa de la base ---
        {
            const FBranchNode& Root = Skeleton.Nodes[0];
            const FTreeWindNode& Wn = Wind.Nodes[0];
            W.Vertices[BaseCapVert] = Root.Pos;
            W.Normals[BaseCapVert] = -Root.Dir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
            W.UVs[BaseCapVert] = FVector2D(0.5f, 0.f);
            W.Tangents[BaseCapVert] = FrameN[0];
            W.SetWindVertex(BaseCapVert, Wn);
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

        const int32 BaseCapFirstIndex = W.Triangles.Num();
        for (int32 k = 0; k < K; ++k)
        {
            W.Triangles.Add(BaseCapVert); W.Triangles.Add(k); W.Triangles.Add(k + 1);
        }

        // --- MADERA: normales reales de la superficie deformada ---
        // Con el radio modulado por vertice, la normal RADIAL ya no es la normal
        // de la superficie. Si no se recalcula, el sombreado sigue leyendose como
        // un cilindro liso y toda la deformacion se pierde: el relieve estaria en
        // la silueta pero no en la luz.
        //
        // Se acumula la normal de cada cara en sus tres vertices (ponderada por
        // area, que es lo que da el producto vectorial sin normalizar) y se
        // normaliza al final. Es O(triangulos) y siempre correcta, sin tener que
        // derivar analiticamente el desplazamiento.
        if (bDeformSection)
        {
            const int32 NumWoodVerts = W.Vertices.Num();
            TArray<FVector> Accum;
            Accum.Init(FVector::ZeroVector, NumWoodVerts);

            // La tapa de la base se EXCLUYE: es un disco perpendicular al tubo y
            // mezclarla suavizaria el borde inferior en una especie de embudo.
            // Ademas queda enterrada, asi que no se gana nada.
            for (int32 t = 0; t + 2 < BaseCapFirstIndex; t += 3)
            {
                const int32 I0 = W.Triangles[t];
                const int32 I1 = W.Triangles[t + 1];
                const int32 I2 = W.Triangles[t + 2];

                const FVector FaceN = FVector::CrossProduct(
                    W.Vertices[I1] - W.Vertices[I0],
                    W.Vertices[I2] - W.Vertices[I0]);

                Accum[I0] += FaceN;
                Accum[I1] += FaceN;
                Accum[I2] += FaceN;
            }

            // COSTURA: k = 0 y k = K son el mismo punto con indices distintos, asi
            // que cada uno solo ha recibido LA MITAD de sus caras. Sin sumarlos,
            // sus normales difieren y aparece una linea de sombreado recorriendo
            // todo el tubo, aunque la geometria este perfectamente cerrada.
            for (int32 i = 0; i < N; ++i)
            {
                const int32 A = i * RingVerts;
                const int32 B = A + K;
                const FVector Sum = Accum[A] + Accum[B];
                Accum[A] = Sum;
                Accum[B] = Sum;
            }

            for (int32 v = 0; v < NumWoodVerts; ++v)
            {
                FVector Nn = Accum[v].GetSafeNormal();
                if (Nn.IsNearlyZero())
                {
                    continue; // vertice sin caras (o degenerado): se deja la radial
                }

                // Alinear al hemisferio de la normal radial preserva la
                // convencion de cara exterior sin tener que volver a razonar el
                // winding de cada abanico.
                if (FVector::DotProduct(Nn, W.Normals[v]) < 0.f)
                {
                    Nn = -Nn;
                }
                W.Normals[v] = Nn;

                // Reortogonalizar la tangente contra la normal nueva: si no, el
                // espacio tangente deja de ser ortonormal y el normal map de la
                // corteza se descuadra.
                const FVector T = W.Tangents[v];
                const FVector TOrtho = T - FVector::DotProduct(T, Nn) * Nn;
                if (!TOrtho.IsNearlyZero())
                {
                    W.Tangents[v] = TOrtho.GetSafeNormal();
                }
            }
        }

        // --- HOJAS ---
        TreeFoliage::Build(Skeleton, Wind, Species, FrameN, FrameB, FineLight, Seed, OutMesh.Leaves);
    }
}
