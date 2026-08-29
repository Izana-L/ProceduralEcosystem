/**
 * @file TrunkDeformer.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del doblado de tronco: ángulo por tipo de capa, composición en
 *        rotation-vector y re-encadenado isométrico del esqueleto.
 *
 * Contiene la función de ángulo de cada tipo de capa (inclinación constante, arco
 * @f$\theta t^{k}@f$ y onda @f$\theta\sin(2\pi k t+\varphi)@f$), la composición de las
 * capas activas sumando rotation-vectors —eje horizontal por ángulo— con un recorte de
 * magnitud que preserva la dirección de la suma, la conversión a cuaternión con guarda de
 * degeneración, el sorteo por capa que fija la curvatura de un árbol concreto y la pasada
 * única que recoloca todos los nodos conservando la longitud de cada internodo.
 *
 * @ingroup eco_geometry
 * @see @ref bib_barr1984
 */

#include "Geometry/TrunkDeformer.h"
#include "Geometry/TreeSkeleton.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand

namespace
{
    /**
     * Ángulo de la capa a la altura normalizada t, con 0 en la base del tronco y 1 en la
     * punta del árbol sin deformar:
     *
     * @li Lean: @f$\theta@f$ constante, el árbol entero gira alrededor de su pie.
     * @li Arc: @f$\theta\,t^{k}@f$, el fuste sale vertical y se tumba con la altura;
     *     @f$k<1@f$ arquea desde abajo y @f$k>1@f$ mantiene el pie recto.
     * @li SCurve: @f$\theta\sin(2\pi k t+\varphi)@f$, el tronco serpentea con @f$k@f$
     *     ondas en toda la altura.
     *
     * Es la única pieza que depende del tipo de capa: añadir un tipo nuevo no toca el
     * re-encadenado.
     *
     * @see ETrunkDeformType
     */
    FORCEINLINE float LayerAngleAt(const TrunkDeformer::FTrunkDeformLayerState& L, float t)
    {
        switch (static_cast<ETrunkDeformType>(L.Type))
        {
        case ETrunkDeformType::Lean:
            return L.AngleRad;

        case ETrunkDeformType::Arc:
            // t nunca es negativo, así que el exponente fraccionario es seguro.
            return L.AngleRad * FMath::Pow(t, FMath::Max(L.Param, 0.01f));

        case ETrunkDeformType::SCurve:
            return L.AngleRad * FMath::Sin(2.f * PI * L.Param * t + L.Phase);

        default:
            return 0.f;
        }
    }

    /**
     * Vector de doblado a la altura t: suma de las capas, cada una como eje de rotación
     * horizontal por ángulo (representación rotation-vector).
     *
     * El eje de una capa que inclina el árbol HACIA el azimut @f$A@f$ es el horizontal
     * perpendicular a él, @f$(-\sin A,\ \cos A,\ 0)@f$: rotar la vertical alrededor de ese
     * eje la lleva a @f$(\cos A\sin\Theta,\ \sin A\sin\Theta,\ \cos\Theta)@f$. Sumar en
     * forma de rotation-vector, en vez de componer cuaterniones, es lo que permite que dos
     * capas con azimuts distintos se combinen en una sola rotación suave.
     */
    FORCEINLINE FVector BendVectorAt(const TrunkDeformer::FTrunkDeformState& State, float t)
    {
        FVector Bend = FVector::ZeroVector;
        for (const TrunkDeformer::FTrunkDeformLayerState& L : State.Layers)
        {
            const float Angle = LayerAngleAt(L, t);
            Bend += FVector(-FMath::Sin(L.AzimuthRad), FMath::Cos(L.AzimuthRad), 0.f) * Angle;
        }

        // Tope del doblado acumulado: preserva la DIRECCIÓN de la suma y solo recorta su
        // magnitud, así que dos capas que se cancelan siguen cancelándose, a diferencia
        // de lo que ocurriría recortando cada capa por separado.
        const FVector::FReal Mag = Bend.Size();
        if (Mag > TrunkDeformer::MaxTrunkBendRad)
        {
            Bend *= (TrunkDeformer::MaxTrunkBendRad / Mag);
        }
        return Bend;
    }

    /**
     * Rotación a partir del rotation-vector, con identidad para @f$|v|\approx 0@f$.
     *
     * @warning No sustituir la guarda por un GetSafeNormal con eje arbitrario: con ángulo
     *          casi nulo el eje es ruido numérico puro, y aunque una rotación de 0
     *          radianes alrededor de cualquier eje sea la identidad, una de 1e-8 radianes
     *          no es reproducible entre ejecuciones.
     */
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

    // Copia local del estado del generador: esta función no puede tener efectos sobre
    // ningún stream del llamante.
    uint32 Rng = DeformSeed;

    for (const FTrunkDeformLayerSpec& Spec : Species.TrunkDeformLayers)
    {
        // Cuatro extracciones SIEMPRE, en orden fijo y ANTES de la puerta: es el contrato
        // de muestreo, y garantiza que cambiar Probability no desplaza las muestras de
        // las capas siguientes.
        const float Gate = EcoRand::NextUnit(Rng);
        const float AngleU = EcoRand::NextUnit(Rng);
        const float AzimuthU = EcoRand::NextUnit(Rng);
        const float PhaseU = EcoRand::NextUnit(Rng);

        if (Gate >= FMath::Clamp(Spec.Probability, 0.f, 1.f))
        {
            continue; // a este árbol no le toca esta capa
        }

        // Un Min mayor que Max en el asset no es fatal: se ordenan y se sigue, que
        // IsDataValid ya avisa en el editor.
        const float MinDeg = FMath::Min(Spec.MinAngleDeg, Spec.MaxAngleDeg);
        const float MaxDeg = FMath::Max(Spec.MinAngleDeg, Spec.MaxAngleDeg);

        FTrunkDeformLayerState L;
        L.Type = static_cast<uint8>(Spec.Type);
        L.AngleRad = FMath::DegreesToRadians(FMath::Lerp(MinDeg, MaxDeg, AngleU));
        L.AzimuthRad = AzimuthU * 2.f * PI;
        L.Phase = PhaseU * 2.f * PI;
        L.Param = FMath::Max(Spec.ShapeParam, 0.01f);

        // El signo del ángulo no se sortea: el azimut ya recorre la circunferencia
        // entera, y un ángulo negativo hacia A es exactamente uno positivo hacia A + PI.
        // En SCurve lo cubre igualmente la fase, con sin(x + PI) = -sin(x). Un sorteo de
        // signo adicional solo correlacionaría dos muestras que deben ser independientes.
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

    // Peor caso: todas las capas activas, cada una a su ángulo máximo y todas apuntando
    // al mismo azimut.
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
    // deformar, y cuando le llega el turno al hijo su padre ya se ha movido.
    TArray<FVector> OldPos;
    OldPos.SetNumUninitialized(NumNodes);
    for (int32 i = 0; i < NumNodes; ++i)
    {
        OldPos[i] = Skeleton.Nodes[i].Pos;
    }

    // La raíz NO se mueve, porque el árbol sigue plantado donde estaba, pero su dirección
    // sí rota: la consume el primer anillo de sección del mallador.
    Skeleton.Nodes[0].Dir = QuatFromBendVector(BendVectorAt(State, 0.f))
        .RotateVector(Skeleton.Nodes[0].Dir).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);

    // Una pasada en orden de índice CRECIENTE: la invariante Parent < índice garantiza
    // que el padre ya está en su sitio nuevo.
    for (int32 i = 1; i < NumNodes; ++i)
    {
        FBranchNode& Node = Skeleton.Nodes[i];
        const int32 P = Node.Parent;
        if (!Skeleton.Nodes.IsValidIndex(P))
        {
            continue; // esqueleto malformado: no es asunto de este paso
        }

        // Altura normalizada del PADRE en el árbol SIN deformar. Del padre y no del
        // propio nodo para que todos los hijos de un mismo nodo reciban la MISMA
        // rotación: con la altura de cada hijo, una rama horizontal y su continuación
        // vertical se separarían en el nacimiento.
        const float t = FMath::Clamp(static_cast<float>((OldPos[P].Z - BaseZ) * InvTotalH), 0.f, 1.f);

        const FQuat R = QuatFromBendVector(BendVectorAt(State, t));

        Node.Pos = Skeleton.Nodes[P].Pos + R.RotateVector(OldPos[i] - OldPos[P]);
        Node.Dir = R.RotateVector(Node.Dir).GetSafeNormal(SMALL_NUMBER, FVector::UpVector);
    }
}
