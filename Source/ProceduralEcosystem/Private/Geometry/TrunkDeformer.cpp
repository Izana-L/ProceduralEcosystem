#include "Geometry/TrunkDeformer.h"
#include "Geometry/TreeSkeleton.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand

namespace
{
    /**
     * Angulo de la capa a la altura normalizada t (0 = base del tronco, 1 =
     * punta del arbol sin deformar). Es LA funcion que define como se lee cada
     * tipo, y esta sola en su sitio para poder anadir tipos nuevos sin tocar el
     * re-encadenado:
     *
     *   Lean   -> constante: TODO el arbol gira el mismo angulo alrededor de su
     *             pie. Se lee como "plantado torcido" (o crecido en ladera), no
     *             como arqueado: usalo con angulos pequenos (<=10 grados) o
     *             parecera que se cae.
     *   Arc    -> Angle * t^Param: el fuste sale vertical del suelo y se va
     *             tumbando con la altura. ES el arbol arqueado, y el unico tipo
     *             cuya lectura mejora con angulos grandes. Param < 1 arquea
     *             desde abajo; Param > 1 mantiene el pie recto y vuelca arriba.
     *   SCurve -> Angle * sin(2*PI*Param*t + Phase): el tronco serpentea. Param
     *             es el nº de ondas en toda la altura; con 1.5-2 se lee como
     *             madera de ribera, con mas se lee como ruido.
     */
    FORCEINLINE float LayerAngleAt(const TrunkDeformer::FTrunkDeformLayerState& L, float t)
    {
        switch (static_cast<ETrunkDeformType>(L.Type))
        {
        case ETrunkDeformType::Lean:
            return L.AngleRad;

        case ETrunkDeformType::Arc:
            // Pow con exponente fraccionario sobre t >= 0: seguro.
            return L.AngleRad * FMath::Pow(t, FMath::Max(L.Param, 0.01f));

        case ETrunkDeformType::SCurve:
            return L.AngleRad * FMath::Sin(2.f * PI * L.Param * t + L.Phase);

        default:
            return 0.f;
        }
    }

    /**
     * Vector de doblado a la altura t: suma de las capas, cada una como
     * "eje de rotacion horizontal x angulo" (representacion rotation-vector).
     *
     * El eje de una capa que inclina el arbol HACIA el azimut A es el horizontal
     * perpendicular a A, o sea (-sin A, cos A, 0): rotar la vertical alrededor
     * de el la lleva a (cos A * sin O, sin A * sin O, cos O). Sumar en forma de
     * rotation-vector (y no componer cuaterniones) es lo que permite que dos
     * capas con azimuts distintos se combinen en una sola rotacion suave.
     */
    FORCEINLINE FVector BendVectorAt(const TrunkDeformer::FTrunkDeformState& State, float t)
    {
        FVector Bend = FVector::ZeroVector;
        for (const TrunkDeformer::FTrunkDeformLayerState& L : State.Layers)
        {
            const float Angle = LayerAngleAt(L, t);
            Bend += FVector(-FMath::Sin(L.AzimuthRad), FMath::Cos(L.AzimuthRad), 0.f) * Angle;
        }

        // Tope del doblado acumulado: preserva la DIRECCION de la suma y solo
        // recorta su magnitud, asi que dos capas que se cancelan siguen
        // cancelandose (a diferencia de clampar cada capa por separado).
        const FVector::FReal Mag = Bend.Size();
        if (Mag > TrunkDeformer::MaxTrunkBendRad)
        {
            Bend *= (TrunkDeformer::MaxTrunkBendRad / Mag);
        }
        return Bend;
    }

    /** Rotacion a partir del rotation-vector, con identidad para |v| ~ 0.
        Nunca GetSafeNormal a un eje arbitrario: con angulo ~0 el eje es ruido
        numerico puro y una rotacion de 0 radianes alrededor de un eje aleatorio
        es identidad de todos modos, pero la de 1e-8 radianes NO es reproducible. */
    FORCEINLINE FQuat QuatFromBendVector(const FVector& Bend)
    {
        const FVector::FReal Mag = Bend.Size();
        if (Mag <= KINDA_SMALL_NUMBER)
        {
            return FQuat::Identity;
        }
        return FQuat(Bend / Mag, Mag);
    }
}

TrunkDeformer::FTrunkDeformState TrunkDeformer::Sample(const USpeciesData& Species, uint32 DeformSeed)
{
    FTrunkDeformState State;

    // Copia local del estado: esta funcion no puede tener efectos sobre ningun
    // stream del llamante (ver la nota de determinismo del header).
    uint32 Rng = DeformSeed;

    for (const FTrunkDeformLayerSpec& Spec : Species.TrunkDeformLayers)
    {
        // --- 4 extracciones SIEMPRE, en orden fijo, ANTES de la puerta ---
        // Es el contrato de muestreo del header: cambiar Probability no puede
        // desplazar las muestras de las capas siguientes.
        const float Gate = EcoRand::NextUnit(Rng);
        const float AngleU = EcoRand::NextUnit(Rng);
        const float AzimuthU = EcoRand::NextUnit(Rng);
        const float PhaseU = EcoRand::NextUnit(Rng);

        if (Gate >= FMath::Clamp(Spec.Probability, 0.f, 1.f))
        {
            continue; // a este arbol no le toca esta capa
        }

        // Min > Max en el asset no es fatal: se ordena y se sigue (IsDataValid
        // ya avisa en el editor).
        const float MinDeg = FMath::Min(Spec.MinAngleDeg, Spec.MaxAngleDeg);
        const float MaxDeg = FMath::Max(Spec.MinAngleDeg, Spec.MaxAngleDeg);

        FTrunkDeformLayerState L;
        L.Type = static_cast<uint8>(Spec.Type);
        L.AngleRad = FMath::DegreesToRadians(FMath::Lerp(MinDeg, MaxDeg, AngleU));
        L.AzimuthRad = AzimuthU * 2.f * PI;
        L.Phase = PhaseU * 2.f * PI;
        L.Param = FMath::Max(Spec.ShapeParam, 0.01f);

        // No hace falta sortear ademas el SIGNO del angulo: el azimut ya recorre
        // la circunferencia entera, y "angulo negativo hacia A" es exactamente
        // "angulo positivo hacia A + PI". En SCurve lo cubre igualmente la fase
        // (sin(x + PI) = -sin(x)). Anadir un sorteo de signo solo correlacionaria
        // dos muestras que deben ser independientes.
        State.Layers.Add(L);
    }

    return State;
}

float TrunkDeformer::MaxLateralReachCm(const USpeciesData& Species, float TotalHeightCm)
{
    if (Species.TrunkDeformLayers.Num() == 0 || TotalHeightCm <= 0.f)
    {
        return 0.f;
    }

    // Peor caso: todas las capas activas, todas a su angulo maximo y todas
    // apuntando al mismo azimut.
    float SumRad = 0.f;
    for (const FTrunkDeformLayerSpec& Spec : Species.TrunkDeformLayers)
    {
        if (Spec.Probability <= 0.f) { continue; }
        SumRad += FMath::DegreesToRadians(FMath::Max(Spec.MinAngleDeg, Spec.MaxAngleDeg));
    }

    return TotalHeightCm * FMath::Sin(FMath::Min(SumRad, MaxTrunkBendRad));
}

void TrunkDeformer::ApplyToSkeleton(FTreeSkeleton& Skeleton, const FTrunkDeformState& State,
    const FVector& TrunkBaseWorld, float TotalHeightCm)
{
    const int32 NumNodes = Skeleton.Num();
    if (State.IsIdentity() || NumNodes < 2)
    {
        return;
    }

    const float InvTotalH = 1.f / FMath::Max(TotalHeightCm, KINDA_SMALL_NUMBER);
    const double BaseZ = TrunkBaseWorld.Z;

    // Posiciones ORIGINALES: el re-encadenado necesita el vector padre->hijo sin
    // deformar, y para cuando le toca al hijo su padre ya se ha movido.
    TArray<FVector> OldPos;
    OldPos.SetNumUninitialized(NumNodes);
    for (int32 i = 0; i < NumNodes; ++i)
    {
        OldPos[i] = Skeleton.Nodes[i].Pos;
    }

    // La raiz NO se mueve (el arbol sigue plantado donde estaba), pero su
    // direccion si rota: la consume el primer anillo de seccion del mallador.
    Skeleton.Nodes[0].Dir = QuatFromBendVector(BendVectorAt(State, 0.f))
        .RotateVector(Skeleton.Nodes[0].Dir).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);

    // Una pasada en orden de indice CRECIENTE: la invariante Parent < indice
    // garantiza que el padre ya esta en su sitio nuevo.
    for (int32 i = 1; i < NumNodes; ++i)
    {
        FBranchNode& Node = Skeleton.Nodes[i];
        const int32 P = Node.Parent;
        if (!Skeleton.Nodes.IsValidIndex(P))
        {
            continue; // esqueleto malformado: no es asunto de este paso
        }

        // Altura normalizada del PADRE en el arbol SIN deformar. Del padre y no
        // del propio nodo para que todos los hijos de un mismo nodo reciban la
        // MISMA rotacion: si cada hijo usara su altura, una rama horizontal y su
        // continuacion vertical se separarian en el nacimiento.
        const float t = FMath::Clamp(static_cast<float>((OldPos[P].Z - BaseZ) * InvTotalH), 0.f, 1.f);

        const FQuat R = QuatFromBendVector(BendVectorAt(State, t));

        Node.Pos = Skeleton.Nodes[P].Pos + R.RotateVector(OldPos[i] - OldPos[P]);
        Node.Dir = R.RotateVector(Node.Dir).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
    }
}
