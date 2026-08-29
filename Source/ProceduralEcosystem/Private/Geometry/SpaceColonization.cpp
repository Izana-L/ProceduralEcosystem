/**
 * @file SpaceColonization.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del motor de colonización del espacio y del pipeline de generación.
 *
 * Contiene las piezas puras que declara la cabecera —mezcla de tropismos, jitter de
 * dirección, ángulo de inserción, pipe model y perfil de tronco—, los dos helpers
 * privados del crecimiento —la dirección del eje principal, sinuosa por ruido de
 * gradiente 1D sobre la altura, y la marca de atractores alcanzados— y GrowTree, que
 * orquesta la secuencia completa: sembrar la nube, dimensionar y sembrar la rejilla de
 * luz fina, podar lo que nace en sombra, indexar, encadenar el eje y entrar en el bucle
 * asociar-crecer-matar con refresco periódico de luz, para terminar doblando el árbol y
 * asignándole radios. Es la única secuencia del módulo: no hay tick, toda la carga es
 * puntual en el momento de generar un árbol.
 *
 * @ingroup eco_geometry
 * @see @ref bib_runions2007
 * @see @ref bib_prusinkiewicz1990
 * @see @ref bib_shinozaki1964
 * @see @ref bib_metzger1893
 * @see @ref bib_perlin1985
 */

#include "Geometry/SpaceColonization.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeLightGridFine.h"
#include "Geometry/AttractorCloud.h"
#include "Geometry/TrunkDeformer.h" // doblado del esqueleto terminado, por árbol
#include "Terrain/LightFieldCoarse.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h"     // EcoRand: generador determinista
#include "Core/EcoGeometry.h" // AnyPerpendicular: copia única del proyecto

namespace
{
    /**
     * Tope de inclinación del eje respecto a la vertical, unos 20 grados.
     *
     * No es solo estético: el bucle que encadena el eje avanza @f$D\cos\theta@f$ en Z,
     * no @f$D@f$, y su condición de parada es en Z. Sin tope, un ángulo grande hace que
     * el eje avance casi nada por paso y el bucle se dispare.
     */
    constexpr float MaxAxisTiltRad = 0.35f;

    /**
     * Dirección unitaria del eje principal a la altura H sobre la base del tronco.
     *
     * Dos capas de ruido de gradiente 1D evaluadas sobre la ALTURA, no una tirada
     * aleatoria por nodo: así la sucesión de direcciones es continua y el eje describe
     * una curva en vez de una poligonal temblorosa.
     *
     * @li sweep: longitud de onda del orden del árbol entero, la inclinación suave del
     *     fuste completo.
     * @li wobble: longitud de onda corta, el serpenteo que rompe la lectura de
     *     extrusión perfecta.
     *
     * @param SweepAzim Azimut base de la inclinación; el ruido lo modula, de modo que un
     *                  árbol no se inclina siempre hacia el mismo lado.
     * @return Vector unitario, la vertical exacta si ambas amplitudes son nulas.
     */
    FVector AxisDirection(float H, float SweepRad, float WobbleRad,
        float SweepWaveCm, float WobbleWaveCm,
        float SweepPhase, float WobblePhase, float SweepAzim)
    {
        if (SweepRad <= 0.f && WobbleRad <= 0.f)
        {
            return FVector::UpVector; // sin sinuosidad: eje perfectamente recto
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

    /**
     * Paso MATAR: marca como alcanzados los atractores vivos a menos de RadiusCm de Pos.
     *
     * Copia única del criterio de «alcanzado» —consulta por rango sobre el índice y
     * distancia real al cuadrado—, que se aplica en dos sitios: sobre cada nodo del eje
     * pre-construido, que atraviesa la copa, y sobre cada hijo nuevo de cada iteración.
     */
    void KillAttractorsNear(FAttractorCloud& Cloud, const FVector& Pos, float RadiusCm)
    {
        const float Radius2 = RadiusCm * RadiusCm;
        Cloud.ForEachInRange(Pos, RadiusCm, [&Cloud, &Pos, Radius2](int32 Ai)
            {
                FAttractor& A = Cloud.Attractors[Ai];
                if (A.bAlive && FVector::DistSquared(A.Pos, Pos) <= Radius2)
                {
                    A.bAlive = false;
                }
            });
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

        // Si los términos se cancelan entre sí, se cae a la dirección de los
        // atractores; si ésa también es nula, a la vertical.
        const FVector FallBack = DirSCA.IsNearlyZero() ? FVector::UpVector : DirSCA;
        return Blended.GetSafeNormal(SMALL_NUMBER, FallBack);
    }

    FVector JitterDirection(const FVector& Dir, float NoiseAmount, uint32& RngState)
    {
        if (NoiseAmount <= 0.f)
        {
            return Dir.GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
        }

        // Vector aleatorio en el cubo [-1,1]^3, sumado a Dir escalado por el ruido y
        // renormalizado. La desviación angular crece con NoiseAmount; el clamp a 1
        // impide que el término perturbador domine y llegue a invertir la dirección.
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
            return Dn; // ya se separa lo suficiente: se respeta lo que eligieron los atractores
        }

        // Componente de Dir perpendicular al padre. Girar DENTRO de ese plano es el
        // giro mínimo que alcanza el ángulo pedido, o sea el que menos estropea la
        // dirección hacia los atractores. Si Dir es paralelo al padre no hay plano
        // que preservar y cualquier perpendicular sirve.
        FVector Perp = Dn - C * P;
        if (Perp.IsNearlyZero())
        {
            Perp = EcoGeometry::AnyPerpendicular(P);
        }
        Perp = Perp.GetSafeNormal(SMALL_NUMBER, EcoGeometry::AnyPerpendicular(P));

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

        // Acc[i] = suma de r^n de los hijos de i. El recorrido en índice decreciente
        // procesa cada hijo antes que su padre por la invariante Parent < índice, de
        // modo que Acc[i] ya está completo cuando se llega a i. Sin ordenar por
        // profundidad y sin recursión.
        TArray<double> Acc;
        Acc.Init(0.0, N);

        for (int32 i = N - 1; i >= 0; --i)
        {
            const bool bTip = (Acc[i] <= 0.0);
            const double R = bTip ? TipR : FMath::Pow(Acc[i], 1.0 / PipeExp);

            // El padre acumula el radio SIN afilar: el afilado es un remate visual de
            // la punta y no debe adelgazar toda la cadena hasta el tronco.
            const int32 P = Skeleton.Nodes[i].Parent;
            if (P >= 0)
            {
                Acc[P] += FMath::Pow(R, PipeExp);
            }

            // Radius es lo que se malla y ApplyTrunkProfile lo pisa después;
            // PipeRadius conserva el radio estructural para quien pregunte por
            // rigidez y no por geometría (@ref FBranchNode::PipeRadius).
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
            return; // perfil desactivado: Radius se queda como lo dejó el pipe model
        }

        TArray<float> AlongLen;
        Skeleton.ComputeAlongLengths(AlongLen);

        // Referencia de grosor: el radio de la base. El peso del perfil se expresa
        // RELATIVO a ella y no en cm absolutos, con lo que es invariante de escala y
        // los buckets de edad pequeños no necesitan un ajuste aparte.
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

            // A quién se aplica el perfil, por dos señales. Ser EJE es la señal exacta
            // y la marca el propio crecimiento: es la que hace que el afilado recorra
            // el fuste hasta el ápice. Ser MADERA GRUESA extiende el ensanche a la
            // inserción de una rama baja de tamaño considerable, como ocurre de
            // verdad. El umbral es relativo al radio de la base y no en cm, para que
            // una ramilla que casualmente pase cerca del suelo no se infle: el
            // ensanche es del tronco, no de todo lo que esté bajo.
            const float W = Node.IsAxis()
                ? 1.f
                : FMath::SmoothStep(BaseR * 0.15f, BaseR * 0.55f, R);
            if (W <= 0.f)
            {
                continue; // ramilla: el perfil de tronco no la toca
            }

            const float H = AlongLen[i];

            // Ensanche de pie: decae exponencialmente con la longitud de arco, que es
            // la estadística del butt swell real (a 3*FlareH ya no queda nada).
            const float FlareMul = 1.f + Flare * FMath::Exp(-H / FlareH);

            // Afilado del fuste con la altura, normalizada a la longitud del eje.
            const float T = FMath::Clamp(H * InvAxisLen, 0.f, 1.f);
            const float TaperMul = FMath::Lerp(1.f, TopTaper, FMath::Pow(T, TaperExp));

            Node.Radius = R * FMath::Lerp(1.f, FlareMul * TaperMul, W);
        }

        // ==== Monotonía: ningún nodo más fino que su hijo más grueso ====
        // Recorrer en índice decreciente visita cada hijo antes que su padre, así que
        // una sola pasada basta. Sin ella, el afilado puede dejar el eje más fino que
        // el primer nodo de copa y aparece un estrangulamiento en cono invertido justo
        // bajo la copa.
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
        // Parámetros de la especie, con guardas para no dividir por cero ni entrar en
        // bucles vacíos. La relación de diseño entre ellos es d_k < D < d_i.
        const float d_i = FMath::Max(Species.InfluenceRadiusDi, 1.f);
        const float d_k = FMath::Max(Species.KillRadiusDk, 0.1f);
        const float D = FMath::Max(Species.StepLengthD, 1.f);
        const int32 MaxIter = FMath::Max(Species.MaxIter, 1);
        const int32 MaxChildren = FMath::Max(Config.MaxChildrenPerNode, 1);
        const int32 MaxAxisChildren = FMath::Max(Config.MaxAxisChildrenPerNode, MaxChildren);

        // Cono de percepción: 180 grados equivale a la esfera completa, o sea desactivado.
        const float CosPerception = FMath::Cos(FMath::DegreesToRadians(
            FMath::Clamp(Species.PerceptionAngleDeg, 20.f, 180.f)));
        const float BranchAngleDeg = FMath::Clamp(Species.BranchAngleDeg, 0.f, 85.f);

        // Semilla del árbol, tomada ANTES de consumir nada. De ella cuelgan por hash
        // los sub-streams de las características que no deben desplazar el stream
        // principal: el eje aquí, la envolvente en SampleCrownEnvelope, la deformación
        // justo debajo. Si el eje tirase de RngState, tocar el tronco cambiaría también
        // todo el jitter posterior de la copa.
        const uint32 TreeSeed = RngState;

        // Curvatura de tronco de ESTE árbol. Se resuelve antes que nada porque su
        // alcance lateral dimensiona el margen de la rejilla de luz fina, más abajo.
        const uint32 DeformSeed = (Config.DeformSeedOverride >= 0)
            ? static_cast<uint32>(Config.DeformSeedOverride)
            : EcoRand::Hash32(TreeSeed ^ 0x0DEF0B75u);
        const TrunkDeformer::FTrunkDeformState Deform = TrunkDeformer::Sample(Species, DeformSeed);

        // Geometría vertical del árbol. Se resuelve aquí porque TotalH hace falta para
        // el margen de la rejilla fina y para la altura normalizada del deformador.
        const float TrunkFrac = FMath::Clamp(Species.TrunkFraction, 0.f, 0.95f);
        const float CrownH = FMath::Max(Species.CrownHeightCm, 1.f);
        const float TrunkH = CrownH * TrunkFrac / (1.f - TrunkFrac);
        const float CrownBaseZ = (float)TrunkBaseWorld.Z + TrunkH;
        const float TotalH = TrunkH + CrownH;

        const float LeafShadowR = D * FMath::Max(Config.LeafShadowRadiusScale, 0.f);
        const float LeafShadowDepth = D * FMath::Max(Config.LeafShadowDepthScale, 0.f);
        const bool  bHasCoarse = (CoarseLight != nullptr) && CoarseLight->IsValid();

        // ==== Sembrar los atractores en la copa ====
        OutAttractors.SampleCrownEnvelope(Species, TrunkBaseWorld, RngState);

        // ==== Rejilla de luz fina sobre la envolvente y sombra de los vecinos ====
        FBox EnvBounds(ForceInit);
        for (const FAttractor& A : OutAttractors.Attractors)
        {
            EnvBounds += A.Pos;
        }
        EnvBounds += TrunkBaseWorld; // que la rejilla cubra también el tronco

        // Margen extra por si a este árbol le toca doblarse: la copa deformada muestrea
        // el AO por vértice y el heliotropismo de la hoja en posiciones que una rejilla
        // ajustada a la envolvente recta ya no cubriría. No hay riesgo de desbordar
        // —el muestreo clampa a los bordes—, pero el gradiente se aplana contra el
        // borde y las hojas dejan de orientarse hacia la luz.
        //
        // El margen se calcula con los máximos del asset y no con la tirada de este
        // árbol, para que todos los de la especie usen la misma rejilla y no haya un
        // salto de calidad del AO entre el ejemplar recto y el arqueado.
        OutFineLight.InitForBounds(EnvBounds, Species.FineVoxelSizeCm,
            Config.FineGridPaddingCm + TrunkDeformer::MaxLateralReachCm(Species, TotalH));

        if (bHasCoarse)
        {
            OutFineLight.SeedFromCoarse(*CoarseLight);
        }

        // ==== Poda inicial: atractores que nacen en la sombra de los vecinos ====
        // Es la conexión micro<-macro, lo que hace que un árbol pegado a otro grande
        // crezca ladeado en vez de ignorarlo.
        OutAttractors.CullByShade(OutFineLight, Config.LightCullThreshold);

        // ==== Índice espacial de atractores, con celda igual a d_i ====
        OutAttractors.BuildIndex(d_i);

        // ==== Nodo raíz y eje principal: tronco desnudo más líder ====
        OutSkeleton.Reset();
        OutSkeleton.Reserve(OutAttractors.Num() * 2 + 64);
        OutSkeleton.InitRoot(TrunkBaseWorld, FVector::UpVector);

        // El eje no muere en la base de la copa: la atraviesa hasta LeaderFraction de
        // su altura (@ref USpeciesData::LeaderFraction). Parado abajo, su punta sería
        // el único nodo que ve los atractores —y en copa cónica el radio máximo cae
        // justo ahí—, se los llevaría todos y saldría la silueta de paraguas.
        const float LeaderFrac = FMath::Clamp(Species.LeaderFraction, 0.f, 1.f);
        const float AxisTopZ = CrownBaseZ + CrownH * LeaderFrac;

        // Sinuosidad del eje, con fase y azimut de un sub-stream propio derivado por hash.
        const float SweepRad = FMath::DegreesToRadians(FMath::Clamp(Species.TrunkSweepDeg, 0.f, 20.f));
        const float WobbleRad = FMath::DegreesToRadians(FMath::Clamp(Species.TrunkWobbleDeg, 0.f, 8.f));
        uint32 AxisRng = EcoRand::Hash32(TreeSeed ^ 0x5EED1A5Fu);
        const float SweepPhase = EcoRand::NextRange(AxisRng, 0.f, 512.f);
        const float SweepAzim = EcoRand::NextRange(AxisRng, 0.f, 2.f * PI);
        const float WobblePhase = EcoRand::NextRange(AxisRng, 0.f, 512.f);
        // Sweep: una sola ondulación en todo el árbol, porque un árbol se inclina
        // como un todo y no como un muelle. Wobble: una fracción pequeña de eso.
        const float SweepWaveCm = FMath::Max(TotalH, D * 4.f);
        const float WobbleWaveCm = FMath::Max(TotalH * 0.15f, D * 1.5f);

        {
            int32 AxisTip = 0;

            // Techo de pasos. El paso avanza D*cos(Theta) en Z, no D, y la condición
            // de parada es en Z: con el ángulo acotado por MaxAxisTiltRad el avance no
            // puede anularse, pero un bucle de crecimiento sin techo se dispararía ante
            // un asset con valores extremos.
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

        // El eje atraviesa la copa y ocupa atractores por el camino. Si no se dan por
        // alcanzados, el crecimiento saca muñones diminutos pegados al fuste que
        // apuntan a puntos que el propio fuste ya ocupa.
        {
            const int32 NumAxisNodes = OutSkeleton.Num();
            for (int32 v = 0; v < NumAxisNodes; ++v)
            {
                KillAttractorsNear(OutAttractors, OutSkeleton.Nodes[v].Pos, d_k);
            }
        }

        // Hijos ya emitidos por cada nodo. Arranca del eje recién encadenado y se
        // mantiene de forma incremental al añadir hijos; es lo que consulta el
        // presupuesto por nodo.
        TArray<int32> Degree;
        OutSkeleton.ComputeChildCounts(Degree);

        // Scratch reutilizado entre iteraciones para no realojar cada vuelta.
        TArray<FVector> SumDir;
        TArray<int32>   Count;
        TArray<int32>   NewChildren;

        for (int32 Iter = 0; Iter < MaxIter; ++Iter)
        {
            const int32 NumNodes = OutSkeleton.Num();
            Degree.SetNumZeroed(NumNodes); // los nodos nacidos en la iteración previa entran a 0

            // ==== ASOCIAR: cada atractor vivo elige su nodo más cercano dentro de d_i ====
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
                // Un nodo saturado no compite por atractores: si lo hiciera, los suyos
                // quedarían asignados a un nodo que ya no puede crecer y no se
                // consumirían nunca. Los nodos del eje tienen presupuesto propio
                // porque su continuación viene pre-construida y les gasta un hijo de
                // entrada.
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

                        // Cono de percepción: un nodo no reclama lo que tiene DETRÁS.
                        // Sin él, la punta del eje, por estar centrada, resulta ser la
                        // más cercana a casi todo y se lo lleva todo: de ahí sale el
                        // abanico de ramas.
                        if (Dd > KINDA_SMALL_NUMBER &&
                            FVector::DotProduct(ToA / Dd, NodeDir) < CosPerception)
                        {
                            return;
                        }

                        A.BestDist = Dd;
                        A.BestNode = v;
                    });
            }

            // ==== Dirección media hacia los atractores asignados a cada nodo ====
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

            // ==== CRECER: un hijo por nodo activo, con tropismos y jitter ====
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

                // Ángulo de inserción. Si el nodo ya tiene descendencia —y un nodo
                // interior del eje siempre la tiene, porque su continuación viene
                // pre-construida—, este hijo es una rama LATERAL y debe separarse del
                // padre. Sin ello sale casi paralela al eje y se lee como fuste
                // deshilachado, no como rama.
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
                break; // ningún nodo pudo crecer: el árbol está terminado
            }

            // ==== MATAR: atractores a menos de d_k de algún hijo nuevo ====
            for (int32 Ci : NewChildren)
            {
                KillAttractorsNear(OutAttractors, OutSkeleton.Nodes[Ci].Pos, d_k);
            }

            // ==== Refresco de luz: la autopoda de la copa interior ====
            // No hay ninguna regla explícita de borrar ramas: al sombrearse el interior
            // de la copa, sus atractores se descartan y las ramas de dentro dejan de
            // tener hacia dónde crecer.
            if (Config.bEnableSelfPruning && Species.LightEvery > 0 && (Iter % Species.LightEvery == 0))
            {
                // Se rehace la sombra desde cero: base de vecinos (o limpia) más el
                // follaje propio en su estado actual.
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

        // ==== Deformación de tronco: doblar el árbol ya crecido ====
        // El sitio en la secuencia es obligado. Después del crecimiento, porque dobla
        // el árbol entero —tronco y copa— como una vara: dentro del bucle solo torcería
        // el eje y dejaría las ramas ya colgadas donde estaban. Antes de los radios,
        // para que el ensanche de pie y el afilado sigan al tronco doblado, lo que sale
        // gratis porque el doblado es isométrico y el perfil trabaja sobre longitud de
        // arco. Y antes de mallar, porque el mallador orienta cada anillo de sección
        // con FBranchNode::Dir, que el deformador rota a la vez que Pos.
        TrunkDeformer::ApplyToSkeleton(OutSkeleton, Deform, TrunkBaseWorld, TotalH);

        // ==== Radios de rama: pipe model sobre el esqueleto terminado ====
        ComputeRadii(OutSkeleton, Species);

        // ==== Perfil de tronco encima del pipe model ====
        // El orden es deliberado: el pipe model da la estructura y esto es el acabado
        // geométrico. Solo pisa Radius y deja PipeRadius intacto.
        ApplyTrunkProfile(OutSkeleton, Species);
    }
}