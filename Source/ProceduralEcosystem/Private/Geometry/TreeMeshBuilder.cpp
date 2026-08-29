/**
 * @file TreeMeshBuilder.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del mallador: del esqueleto de ramas a los buffers de madera.
 *
 * Contiene la única función del namespace, BuildMesh, y con ella la secuencia completa
 * del mallado: los atributos de viento y oclusión por nodo, los marcos de rotación
 * mínima propagados de padre a hijo, el reparto del buffer de vértices entre anillos,
 * ápices y tapa de base, los anillos con su deformación de sección —lóbulos angulares,
 * relieve de corteza por ruido de gradiente y giro con la altura—, la triangulación de
 * paredes y cierres, y el recálculo de normales que hace que la deformación se vea en la
 * luz y no solo en la silueta. Termina delegando las hojas en el follaje. No consume
 * ningún flujo de RNG: toda su variación sale de hashes estables de la semilla.
 *
 * @ingroup eco_geometry
 * @see @ref bib_marcorotacionminima
 * @see @ref bib_max1999
 * @see @ref bib_perlin1985
 */

#include "Geometry/TreeMeshBuilder.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeWindData.h"     // atributos de viento y oclusión por nodo
#include "Geometry/TreeFoliage.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h"     // EcoRand::HashUnit: hashes estables, sin consumo de RNG
#include "Core/EcoGeometry.h" // AnyPerpendicular: copia única compartida del helper

namespace
{
    constexpr float MinBranchRadiusCm = 0.05f;  ///< Suelo del radio: evita anillos degenerados.
    constexpr float UvAlongScale = 1.f / 100.f; ///< Una unidad de v por metro de rama.
    constexpr float TipConeLengthFrac = 0.55f;  ///< Largo del ápice, como fracción del internodo.
    constexpr float MinTipConeRadii = 2.f;      ///< ... con un mínimo en radios de la ramilla.

    /** Por debajo de esta amplitud la deformación de sección no llega a verse, y no
        compensa pagar ni los segmentos de anillo extra ni la pasada de normales. */
    constexpr float MinSectionDeform = 0.01f;

    /** Mínimo de segmentos de anillo cuando hay deformación. Un hexágono no puede tener
        tres lóbulos: sin resolución angular suficiente, el relieve no existe. */
    constexpr int32 MinRingSegmentsForRelief = 8;

    /** Vueltas que da la fase de los lóbulos a lo largo del árbol. Es lo que se lee como
        tronco retorcido: una sección lobulada que no gira parece un prisma extruido, tan
        artificial como el cilindro. */
    constexpr float SectionTwistTurns = 0.4f;

    /** Longitud de onda del relieve grueso de corteza, en radios de la base del tronco. */
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

        // ==== Deformación de sección: el tronco deja de ser un cilindro ====
        const float LobeAmp = FMath::Clamp(Species.SectionLobeAmount, 0.f, 0.4f);
        const int32 LobeCount = FMath::Clamp(Species.SectionLobeCount, 2, 6);
        const float ReliefAmp = FMath::Clamp(Species.BarkReliefAmount, 0.f, 0.2f);
        const bool bDeformSection = (LobeAmp > MinSectionDeform) || (ReliefAmp > MinSectionDeform);

        int32 K = FMath::Clamp(Species.RingSegments, 3, 16);
        if (bDeformSection)
        {
            K = FMath::Max(K, MinRingSegmentsForRelief);
        }

        // ==== Atributos de viento y oclusión por nodo, antes de mallar ====
        // Salen del propio esqueleto (padres y radios estructurales) y de la rejilla de
        // luz fina, sin reconstruir jerarquías ni hornear texturas de pivotes. La pasada
        // publica además ChildCount y AlongLen, que el mallador reutiliza en vez de
        // recalcularlos.
        FTreeWindData Wind;
        Wind.Build(Skeleton, Species, FineLight, Seed);

        const TArray<int32>& ChildCount = Wind.ChildCount;
        const TArray<float>& AlongLen = Wind.AlongLen;

        // ==== Marcos de rotación mínima ====
        // Se recorren en orden de índice: por la invariante del esqueleto el padre viene
        // antes que el hijo, así que su marco ya está resuelto cuando se necesita.
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

                // Transporte paralelo: al marco del padre se le aplica la rotación que
                // lleva su tangente a la de este nodo, que es la de torsión mínima. Un
                // marco recalculado por anillo retorcería el tubo.
                const FQuat Q = FQuat::FindBetweenNormals(Tp, T);
                FVector Np = Q.RotateVector(FrameN[P]);
                // Reortogonaliza contra T para eliminar la deriva numérica acumulada.
                Np = (Np - FVector::DotProduct(Np, T) * T).GetSafeNormal(SMALL_NUMBER, EcoGeometry::AnyPerpendicular(T));
                Nrm = Np;
            }

            FrameN[i] = Nrm;
            FrameB[i] = FVector::CrossProduct(T, Nrm).GetSafeNormal(SMALL_NUMBER, FVector::CrossProduct(T, EcoGeometry::AnyPerpendicular(T)));
        }

        // ==== Madera: reparto del buffer de vértices ====
        //   [0, N*RingVerts)          anillos, uno por nodo
        //   BaseCapVert               centro de la tapa de la base del tronco
        //   ApexVert[i]               ápice de cada nodo terminal
        FTreeMeshBuffers& W = OutMesh.Wood;
        const int32 RingVerts = K + 1;          // el +1 es el vértice de costura, en u = 1
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

        // Escalas de la deformación derivadas del propio árbol: al ir referidas al radio
        // de la base salen ya proporcionadas a su tamaño, sin parámetros de especie ni
        // reescalado por bucket.
        const float TrunkBaseRadius = FMath::Max(Skeleton.Nodes[0].Radius, MinBranchRadiusCm);
        const float InvReliefWave = 1.f / FMath::Max(TrunkBaseRadius * ReliefWaveInBaseRadii, 2.f);
        float MaxAlongLen = 0.f;
        for (int32 i = 0; i < N; ++i) { MaxAlongLen = FMath::Max(MaxAlongLen, AlongLen[i]); }
        const float InvMaxAlong = (MaxAlongLen > KINDA_SMALL_NUMBER) ? (1.f / MaxAlongLen) : 0.f;

        // La fase de los lóbulos y el desplazamiento del ruido salen de un hash estable,
        // nunca EcoRand::Next*. Consumir aquí desplazaría el flujo que la colonización
        // del espacio ya gastó sobre este mismo árbol, y activar la deformación cambiaría
        // la forma del esqueleto.
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

            // La deformación se apaga en la madera fina: una ramilla de un centímetro no
            // tiene lóbulos, y a esa escala el desplazamiento solo produce ruido.
            const float DeformWeight = bDeformSection
                ? FMath::SmoothStep(TrunkBaseRadius * 0.06f, TrunkBaseRadius * 0.30f, Radius)
                : 0.f;

            // La fase de los lóbulos gira lentamente con la longitud recorrida.
            const float LobePhase = LobeSeedPhase
                + SectionTwistTurns * 2.f * PI * (AlongLen[i] * InvMaxAlong);

            // Los vértices de un anillo comparten los atributos de su nodo, sin excepción:
            // si variasen alrededor del anillo, el desplazamiento que aplica el material
            // deformaría la sección del tubo.
            const FTreeWindNode& Wn = Wind.Nodes[i];

            for (int32 k = 0; k <= K; ++k)
            {
                // Costura: k = K es el mismo punto que k = 0, duplicado solo para poder
                // cerrar la UV en u = 1. Reutilizar literalmente el ángulo de k = 0 hace
                // que los dos vértices salgan idénticos bit a bit; dejarlo a la aritmética
                // de coma flotante (cos(2*PI) no es exactamente cos(0)) abriría una raja a
                // lo largo del tronco en cuanto la deformación no fuese periódica.
                const int32 kWrap = (k == K) ? 0 : k;
                const float Ang = 2.f * PI * (float)kWrap / (float)K;
                const float C = FMath::Cos(Ang);
                const float S = FMath::Sin(Ang);
                const FVector Off = C * Nrm + S * Bin;

                float RadiusScale = 1.f;
                if (DeformWeight > 0.f)
                {
                    // Lóbulos: un término de serie de Fourier en el ángulo, periódico en
                    // 2*PI por construcción, de modo que la costura nunca puede abrirse.
                    const float Lobes = LobeAmp * FMath::Cos((float)LobeCount * Ang + LobePhase);

                    // Relieve grueso: ruido de gradiente 3D muestreado en la posición del
                    // vértice sin deformar. Al ser función de la posición sale coherente
                    // entre anillos vecinos, sin propagar nada a lo largo de la rama.
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

        // ==== Madera: vértice ápice de cada punta ====
        // El eje del cono sale del segmento padre-nodo, no de Node.Dir: la colonización
        // del espacio puede dejar nodos cuya dirección apunta hacia atrás, y con ella el
        // cono se metería dentro del tubo del padre.
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

        // ==== Madera: centro de la tapa de la base ====
        {
            const FBranchNode& Root = Skeleton.Nodes[0];
            const FTreeWindNode& Wn = Wind.Nodes[0];
            W.Vertices[BaseCapVert] = Root.Pos;
            W.Normals[BaseCapVert] = -Root.Dir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
            W.UVs[BaseCapVert] = FVector2D(0.5f, 0.f);
            W.Tangents[BaseCapVert] = FrameN[0];
            W.SetWindVertex(BaseCapVert, Wn);
        }

        // ==== Madera: coser cada anillo con el de su padre ====
        // El orden de los índices de cada triángulo es el que deja la cara exterior
        // mirando hacia fuera.
        for (int32 i = 0; i < N; ++i)
        {
            const int32 P = Skeleton.Nodes[i].Parent;
            if (P < 0) { continue; }

            const int32 BaseI = i * RingVerts;   // el paso es RingVerts, no K
            const int32 BaseP = P * RingVerts;
            for (int32 k = 0; k < K; ++k)        // K quads; k+1 cabe siempre: hay K+1 vértices
            {
                W.Triangles.Add(BaseP + k);     W.Triangles.Add(BaseI + k);     W.Triangles.Add(BaseP + k + 1);
                W.Triangles.Add(BaseP + k + 1); W.Triangles.Add(BaseI + k);     W.Triangles.Add(BaseI + k + 1);
            }
        }

        // ==== Madera: cierres de los dos extremos ====
        // El abanico del ápice es el quad de pared con el anillo del hijo colapsado en un
        // punto, y la tapa de la base lo mismo con el del padre: de ahí sale el orden de
        // los índices sin volver a razonarlo.
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

        // ==== Madera: normales reales de la superficie deformada ====
        // Con el radio modulado vértice a vértice, la normal radial ya no es la normal de
        // la superficie: sin recalcularla el sombreado sigue leyéndose como un cilindro
        // liso y el relieve queda en la silueta pero no en la luz.
        //
        // Cada cara aporta su producto vectorial sin normalizar a sus tres vértices, lo
        // que pondera automáticamente por área, y al final se normaliza. Es lineal en el
        // número de triángulos y evita derivar analíticamente el desplazamiento.
        if (bDeformSection)
        {
            const int32 NumWoodVerts = W.Vertices.Num();
            TArray<FVector> Accum;
            Accum.Init(FVector::ZeroVector, NumWoodVerts);

            // La tapa de la base se excluye: es un disco perpendicular al tubo y
            // promediarla achaflanaría el borde inferior en forma de embudo. Además queda
            // enterrada, así que no se gana nada a cambio.
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

            // Costura: k = 0 y k = K son el mismo punto con índices distintos, así que
            // cada uno ha recibido solo la mitad de sus caras. Sin sumar los dos
            // acumuladores sus normales difieren y aparece una línea de sombreado a lo
            // largo del tubo, aunque la geometría esté perfectamente cerrada.
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
                    continue; // vértice sin caras o degenerado: se queda con la radial
                }

                // Alinear la normal al hemisferio de la radial previa conserva la
                // convención de cara exterior sin volver a razonar el orden de índices de
                // cada abanico.
                if (FVector::DotProduct(Nn, W.Normals[v]) < 0.f)
                {
                    Nn = -Nn;
                }
                W.Normals[v] = Nn;

                // Reortogonalizar la tangente contra la normal nueva: sin esto el espacio
                // tangente deja de ser ortonormal y el normal map de corteza se descuadra.
                const FVector T = W.Tangents[v];
                const FVector TOrtho = T - FVector::DotProduct(T, Nn) * Nn;
                if (!TOrtho.IsNearlyZero())
                {
                    W.Tangents[v] = TOrtho.GetSafeNormal();
                }
            }
        }

        // ==== Hojas ====
        // Los marcos de rotación mínima se pasan tal cual: el follaje los necesita para
        // que el azimut de la espiral filotáctica no se retuerza a lo largo de la ramilla.
        TreeFoliage::Build(Skeleton, Wind, Species, FrameN, FrameB, FineLight, Seed, OutMesh.Leaves);
    }
}
