#include "Geometry/SpaceColonization.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeLightGridFine.h"
#include "Geometry/AttractorCloud.h"
#include "Terrain/LightFieldCoarse.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand

namespace
{
    /**
     * Tope de inclinacion del eje respecto a la vertical (~20 grados).
     *
     * No es solo estetico: el bucle que encadena el eje avanza D*cos(Theta) en
     * Z, no D, y su condicion de parada es en Z. Sin tope, un angulo grande
     * hace que el eje avance casi nada por paso y el bucle se dispare.
     */
    constexpr float MaxAxisTiltRad = 0.35f;

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
     * Direccion del eje principal a la altura H sobre la base.
     *
     * Dos capas superpuestas, las dos de ruido 1D sobre la ALTURA (no aleatorio
     * por nodo): asi la sucesion de direcciones es continua y el eje describe
     * una CURVA, no una poligonal temblorosa.
     *   - sweep:  longitud de onda ~ el arbol entero -> la inclinacion suave del
     *             fuste completo, que es como se inclina un arbol de verdad.
     *   - wobble: longitud de onda corta -> el serpenteo que quita la lectura de
     *             "extrusion perfecta".
     */
    FVector AxisDirection(float H, float SweepRad, float WobbleRad,
        float SweepWaveCm, float WobbleWaveCm,
        float SweepPhase, float WobblePhase, float SweepAzim)
    {
        if (SweepRad <= 0.f && WobbleRad <= 0.f)
        {
            return FVector::UpVector; // eje recto: comportamiento anterior
        }

        const float NA = FMath::PerlinNoise1D(SweepPhase + H / SweepWaveCm);
        const float NB = FMath::PerlinNoise1D(WobblePhase + H / WobbleWaveCm);
        const float NC = FMath::PerlinNoise1D(SweepPhase + 37.7f + H / SweepWaveCm);

        const float Theta = FMath::Clamp(SweepRad * NA + WobbleRad * NB, -MaxAxisTiltRad, MaxAxisTiltRad);
        const float Phi = SweepAzim + PI * NC;
        const float TanT = FMath::Tan(Theta);

        return FVector(TanT * FMath::Cos(Phi), TanT * FMath::Sin(Phi), 1.f)
            .GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
    }
}

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

    FVector ApplyBranchAngle(const FVector& Dir, const FVector& ParentDir, float MinAngleDeg)
    {
        const float MinAngle = FMath::DegreesToRadians(FMath::Clamp(MinAngleDeg, 0.f, 85.f));
        if (MinAngle <= 0.f)
        {
            return Dir;
        }

        const FVector P = ParentDir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        const FVector Dn = Dir.GetSafeNormal(SMALL_NUMBER, P);

        const float C = (float)FVector::DotProduct(Dn, P);
        const float CosMin = FMath::Cos(MinAngle);
        if (C <= CosMin)
        {
            return Dn; // ya se separa lo suficiente: no tocamos lo que eligio el SCA
        }

        // Componente de Dir perpendicular al padre. Girar DENTRO de ese plano es
        // el giro minimo que consigue el angulo pedido, o sea el que menos
        // estropea la direccion hacia los atractores. Si Dir es paralelo al
        // padre no hay plano que preservar y cualquier perpendicular sirve.
        FVector Perp = Dn - C * P;
        if (Perp.IsNearlyZero())
        {
            Perp = AnyPerpendicular(P);
        }
        Perp = Perp.GetSafeNormal(SMALL_NUMBER, AnyPerpendicular(P));

        return (CosMin * P + FMath::Sin(MinAngle) * Perp).GetSafeNormal(SMALL_NUMBER, Dn);
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
        const double TipTaper = FMath::Clamp((double)Species.TipTaper, 0.05, 1.0);

        // Acc[i] = suma de r^n de los hijos de i. Recorremos en indice
        // decreciente: por la invariante Parent < indice, eso procesa cada
        // hijo antes que su padre (doc. 3.6, "NodesByDecreasingDepth").
        TArray<double> Acc;
        Acc.Init(0.0, N);

        for (int32 i = N - 1; i >= 0; --i)
        {
            const bool bTip = (Acc[i] <= 0.0);
            const double R = bTip ? TipR : FMath::Pow(Acc[i], 1.0 / PipeExp);

            // El padre hereda el radio SIN afilar: el afilado es un remate de la
            // punta, no debe adelgazar toda la cadena hasta el tronco.
            const int32 P = Skeleton.Nodes[i].Parent;
            if (P >= 0)
            {
                Acc[P] += FMath::Pow(R, PipeExp);
            }

            // Radius es lo que se malla y ApplyTrunkProfile lo pisa despues;
            // PipeRadius conserva el radio ESTRUCTURAL para quien pregunte por
            // rigidez y no por geometria (ver FBranchNode::PipeRadius).
            const float FinalR = (float)(bTip ? R * TipTaper : R);
            Skeleton.Nodes[i].Radius = FinalR;
            Skeleton.Nodes[i].PipeRadius = FinalR;
        }
    }

    void ApplyTrunkProfile(FTreeSkeleton& Skeleton, const USpeciesData& Species)
    {
        const int32 N = Skeleton.Num();
        if (N == 0)
        {
            return;
        }

        const float Flare = FMath::Max(0.f, Species.TrunkFlareStrength);
        const float FlareH = FMath::Max(1.f, Species.TrunkFlareHeightCm);
        const float TopTaper = FMath::Clamp(Species.TrunkTopTaper, 0.2f, 1.f);
        const float TaperExp = FMath::Clamp(Species.TrunkTaperExp, 0.25f, 4.f);

        const bool bNoFlare = (Flare <= KINDA_SMALL_NUMBER);
        const bool bNoTaper = (TopTaper >= 1.f - KINDA_SMALL_NUMBER);
        if (bNoFlare && bNoTaper)
        {
            return; // perfil desactivado: Radius se queda tal cual lo dejo el pipe model
        }

        TArray<float> AlongLen;
        Skeleton.ComputeAlongLengths(AlongLen);

        // Referencia de grosor. El peso se expresa RELATIVO a ella y no en cm
        // absolutos: asi es invariante de escala y los buckets pequenos no
        // necesitan un tuneo aparte.
        const float BaseR = FMath::Max(Skeleton.Nodes[0].PipeRadius, KINDA_SMALL_NUMBER);

        // Longitud del eje principal: es la escala sobre la que afila el fuste.
        float AxisLen = 0.f;
        for (int32 i = 0; i < N; ++i)
        {
            if (Skeleton.Nodes[i].IsAxis())
            {
                AxisLen = FMath::Max(AxisLen, AlongLen[i]);
            }
        }
        const float InvAxisLen = (AxisLen > KINDA_SMALL_NUMBER) ? (1.f / AxisLen) : 0.f;

        for (int32 i = 0; i < N; ++i)
        {
            FBranchNode& Node = Skeleton.Nodes[i];
            const float R = Node.PipeRadius;

            // A quien se aplica el perfil. Dos senales:
            //   - Es EJE: senal directa y exacta, la marca el SCA al construirlo.
            //     Es la que hace que el afilado recorra el fuste hasta el apice.
            //   - Es MADERA GRUESA: para que una rama baja de tamano considerable
            //     tambien ensanche en su insercion, como pasa de verdad.
            // Relativo al radio de la base -no en cm-, para que una ramilla que
            // casualmente pase cerca del suelo no se infle: el flare es del
            // tronco, no de todo lo que este bajo.
            const float W = Node.IsAxis()
                ? 1.f
                : FMath::SmoothStep(BaseR * 0.15f, BaseR * 0.55f, R);
            if (W <= 0.f)
            {
                continue; // ramilla: el perfil de tronco no la toca
            }

            const float H = AlongLen[i];

            // Ensanche de base: decae casi exponencialmente con la altura, que
            // es la estadistica del butt swell real (a 3*FlareH ya no queda nada).
            const float FlareMul = 1.f + Flare * FMath::Exp(-H / FlareH);

            // Afilado del eje con la altura.
            const float T = FMath::Clamp(H * InvAxisLen, 0.f, 1.f);
            const float TaperMul = FMath::Lerp(1.f, TopTaper, FMath::Pow(T, TaperExp));

            Node.Radius = R * FMath::Lerp(1.f, FlareMul * TaperMul, W);
        }

        // --- Monotonia: ningun nodo mas fino que su hijo mas grueso ---
        // Recorrer en indice DECRECIENTE visita cada hijo antes que su padre
        // (invariante de FTreeSkeleton), asi que una sola pasada basta. Sin
        // esto, el afilado puede dejar el eje mas fino que el primer nodo de
        // copa y aparece un estrangulamiento en cono invertido bajo la copa.
        for (int32 i = N - 1; i >= 1; --i)
        {
            const int32 P = Skeleton.Nodes[i].Parent;
            if (P >= 0)
            {
                Skeleton.Nodes[P].Radius = FMath::Max(Skeleton.Nodes[P].Radius, Skeleton.Nodes[i].Radius);
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
        const int32 MaxChildren = FMath::Max(Config.MaxChildrenPerNode, 1);
        const int32 MaxAxisChildren = FMath::Max(Config.MaxAxisChildrenPerNode, MaxChildren);

        // Cono de percepcion: 180 grados = esfera completa = desactivado.
        const float CosPerception = FMath::Cos(FMath::DegreesToRadians(
            FMath::Clamp(Species.PerceptionAngleDeg, 20.f, 180.f)));
        const float BranchAngleDeg = FMath::Clamp(Species.BranchAngleDeg, 0.f, 85.f);

        // Semilla del arbol ANTES de consumir nada. De ella cuelgan los
        // sub-streams (eje aqui, envolvente en SampleCrownEnvelope) que NO deben
        // desplazar el stream principal: si el eje tirase de RngState, tocar el
        // tronco cambiaria tambien todo el jitter posterior de la copa.
        const uint32 TreeSeed = RngState;

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

        // --- 5) Nodo raiz + EJE PRINCIPAL (tronco desnudo + lider) ---
        OutSkeleton.Reset();
        OutSkeleton.Reserve(OutAttractors.Num() * 2 + 64);
        OutSkeleton.InitRoot(TrunkBaseWorld, FVector::UpVector);

        const float TrunkFrac = FMath::Clamp(Species.TrunkFraction, 0.f, 0.95f);
        const float CrownH = FMath::Max(Species.CrownHeightCm, 1.f);
        const float TrunkH = CrownH * TrunkFrac / (1.f - TrunkFrac);
        const float CrownBaseZ = (float)TrunkBaseWorld.Z + TrunkH;
        const float TotalH = TrunkH + CrownH;

        // El eje ya NO muere en la base de la copa: la ATRAVIESA hasta
        // LeaderFraction de su altura (ver USpeciesData::LeaderFraction). Con el
        // eje parado abajo, su punta era el unico nodo que veia los atractores
        // -y en copa conica el radio maximo cae justo ahi-, asi que se los
        // llevaba todos y salia la silueta de paraguas.
        const float LeaderFrac = FMath::Clamp(Species.LeaderFraction, 0.f, 1.f);
        const float AxisTopZ = CrownBaseZ + CrownH * LeaderFrac;

        // Sinuosidad del eje, desde un sub-stream propio derivado por hash.
        const float SweepRad = FMath::DegreesToRadians(FMath::Clamp(Species.TrunkSweepDeg, 0.f, 20.f));
        const float WobbleRad = FMath::DegreesToRadians(FMath::Clamp(Species.TrunkWobbleDeg, 0.f, 8.f));
        uint32 AxisRng = EcoRand::Hash32(TreeSeed ^ 0x5EED1A5Fu);
        const float SweepPhase = EcoRand::NextRange(AxisRng, 0.f, 512.f);
        const float SweepAzim = EcoRand::NextRange(AxisRng, 0.f, 2.f * PI);
        const float WobblePhase = EcoRand::NextRange(AxisRng, 0.f, 512.f);
        // Sweep: UNA ondulacion en todo el arbol (un arbol se inclina como un
        // todo, no como un muelle). Wobble: una fraccion pequena de eso.
        const float SweepWaveCm = FMath::Max(TotalH, D * 4.f);
        const float WobbleWaveCm = FMath::Max(TotalH * 0.15f, D * 1.5f);

        {
            int32 AxisTip = 0;

            // Techo de pasos. El paso avanza D*cos(Theta) en Z, no D, y la
            // condicion de parada es en Z: con el angulo acotado el avance no
            // puede anularse, pero un bucle de crecimiento sin techo es
            // exactamente el fallo que solo asoma con datos raros.
            const float AxisSpanZ = FMath::Max(AxisTopZ - (float)TrunkBaseWorld.Z, 0.f);
            const int32 MaxAxisNodes = FMath::Clamp(
                FMath::CeilToInt(AxisSpanZ / FMath::Max(D * 0.5f, 1.f)) + 4, 0, 4096);

            for (int32 Step = 0; Step < MaxAxisNodes; ++Step)
            {
                if (OutSkeleton.Nodes[AxisTip].Pos.Z + D >= AxisTopZ)
                {
                    break;
                }

                const float H = (float)(OutSkeleton.Nodes[AxisTip].Pos.Z - TrunkBaseWorld.Z);
                const FVector Dir = AxisDirection(H, SweepRad, WobbleRad,
                    SweepWaveCm, WobbleWaveCm, SweepPhase, WobblePhase, SweepAzim);

                const FVector NextPos = OutSkeleton.Nodes[AxisTip].Pos + D * Dir;
                const int32 Ci = OutSkeleton.AddChild(AxisTip, NextPos, Dir, BNF_Axis);
                if (Ci == INDEX_NONE) { break; }
                AxisTip = Ci;
            }
        }

        // El eje atraviesa la copa, o sea que se come atractores por el camino.
        // Si no se dan por alcanzados, el SCA sacaria munones diminutos pegados
        // al fuste apuntando a puntos que el propio fuste ya ocupa.
        {
            const int32 NumAxisNodes = OutSkeleton.Num();
            const float AxisKill2 = d_k * d_k;
            for (int32 v = 0; v < NumAxisNodes; ++v)
            {
                const FVector AxisPos = OutSkeleton.Nodes[v].Pos;
                OutAttractors.ForEachInRange(AxisPos, d_k, [&](int32 Ai)
                    {
                        FAttractor& A = OutAttractors.Attractors[Ai];
                        if (A.bAlive && FVector::DistSquared(A.Pos, AxisPos) <= AxisKill2)
                        {
                            A.bAlive = false;
                        }
                    });
            }
        }

        // Grado alcanzado por cada nodo. Arranca del eje recien encadenado y se
        // mantiene incrementalmente al anadir hijos.
        TArray<int32> Degree;
        OutSkeleton.ComputeChildCounts(Degree);

        // Scratch reutilizado entre iteraciones.
        TArray<FVector> SumDir;
        TArray<int32>   Count;
        TArray<int32>   NewChildren;

        for (int32 Iter = 0; Iter < MaxIter; ++Iter)
        {
            const int32 NumNodes = OutSkeleton.Num();
            Degree.SetNumZeroed(NumNodes); // los nodos nacidos en la iteracion previa entran a 0

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
                // Un nodo saturado no compite por atractores: si lo hiciera, los
                // suyos quedarian asignados a un nodo que ya no puede crecer y
                // nunca se consumirian. Los nodos del eje tienen presupuesto
                // propio porque su continuacion ya viene pre-construida y les
                // gasta un hijo de entrada.
                const int32 Budget = OutSkeleton.Nodes[v].IsAxis() ? MaxAxisChildren : MaxChildren;
                if (Degree[v] >= Budget) { continue; }

                const FVector NodePos = OutSkeleton.Nodes[v].Pos;
                const FVector NodeDir = OutSkeleton.Nodes[v].Dir;
                OutAttractors.ForEachInRange(NodePos, d_i, [&](int32 Ai)
                    {
                        FAttractor& A = OutAttractors.Attractors[Ai];
                        if (!A.bAlive) { return; }

                        const FVector ToA = A.Pos - NodePos;
                        const float Dd = (float)ToA.Size();
                        if (Dd >= A.BestDist) { return; }

                        // Cono de percepcion: un nodo no reclama lo que tiene
                        // DETRAS. Sin esto, la punta del eje -por estar centrada-
                        // resulta ser la mas cercana a casi todo y se lo lleva
                        // todo, que es de donde sale el abanico de ramas.
                        if (Dd > KINDA_SMALL_NUMBER &&
                            FVector::DotProduct(ToA / Dd, NodeDir) < CosPerception)
                        {
                            return;
                        }

                        A.BestDist = Dd;
                        A.BestNode = v;
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

                // Angulo de insercion. Si el nodo YA tiene descendencia -y un
                // nodo interior del eje siempre la tiene, porque su continuacion
                // viene pre-construida-, este hijo es una rama LATERAL y debe
                // separarse del padre. Sin esto sale casi paralela al eje y se
                // lee como fuste deshilachado, no como rama.
                if (Degree[v] > 0 && BranchAngleDeg > 0.f)
                {
                    Dir = ApplyBranchAngle(Dir, NodeDir, BranchAngleDeg);
                }

                const FVector ChildPos = NodePos + D * Dir;
                const int32 Ci = OutSkeleton.AddChild(v, ChildPos, Dir);
                if (Ci != INDEX_NONE)
                {
                    ++Degree[v];
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

        // --- Perfil de tronco encima del pipe model (ensanche de base + afilado) ---
        // Va DESPUES a proposito: el pipe model es la estructura y este es el
        // acabado. ApplyTrunkProfile solo pisa Radius y deja PipeRadius intacto.
        ApplyTrunkProfile(OutSkeleton, Species);
    }
}