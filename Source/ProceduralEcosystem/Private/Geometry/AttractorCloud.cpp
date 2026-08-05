#include "Geometry/AttractorCloud.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand: RNG determinista por-arbol

int32 FAttractorCloud::CountAlive() const
{
    int32 N = 0;
    for (const FAttractor& A : Attractors)
    {
        if (A.bAlive) { ++N; }
    }
    return N;
}

void FAttractorCloud::Reset()
{
    Attractors.Reset();
    CellStart.Reset();
    SortedIdx.Reset();
    CellSize = 0.f;
    GridW = GridH = GridD = 0;
    GridOrigin = FVector::ZeroVector;
}

void FAttractorCloud::SampleCrownEnvelope(const USpeciesData& Species, const FVector& TrunkBaseWorld, uint32& RngState)
{
    Attractors.Reset();

    const int32 N = FMath::Max(1, Species.NumAttractors);
    Attractors.Reserve(N);

    // La copa ocupa CrownHeightCm; bajo ella hay un tronco desnudo que es una
    // fraccion TrunkFraction de la altura TOTAL del arbol.
    const float Frac = FMath::Clamp(Species.TrunkFraction, 0.f, 0.95f);
    const float CrownH = FMath::Max(Species.CrownHeightCm, 1.f);
    const float CrownR = FMath::Max(Species.CrownRadiusCm, 1.f);
    const float TotalH = CrownH / (1.f - Frac);
    const float TrunkH = TotalH - CrownH;              // = Frac * TotalH
    const float CrownBaseZ = TrunkBaseWorld.Z + TrunkH;

    for (int32 i = 0; i < N; ++i)
    {
        // t = altura normalizada dentro de la copa: 0 = base de copa, 1 = apice.
        const float T = EcoRand::NextUnit(RngState);

        // Radio de la envolvente a esa altura, segun la forma de la especie.
        float RadiusAtT = 0.f;
        switch (Species.CrownShape)
        {
        case ECrownShape::Conical:
            RadiusAtT = CrownR * (1.f - T);                 // ancha abajo, punta arriba
            break;

        case ECrownShape::Columnar:
            RadiusAtT = CrownR * (1.f - 0.15f * T);         // casi recta, leve estrechamiento
            break;

        case ECrownShape::Spherical:
        default:
        {
            const float U = 2.f * T - 1.f;                  // -1..1 (centro de copa en T=0.5)
            RadiusAtT = CrownR * FMath::Sqrt(FMath::Max(0.f, 1.f - U * U)); // elipsoide
            break;
        }
        }

        // Disco horizontal area-uniforme (r = R*sqrt(U), sin apelmazar en el eje).
        const float Angle = EcoRand::NextRange(RngState, 0.f, 2.f * PI);
        const float Rr = RadiusAtT * FMath::Sqrt(EcoRand::NextUnit(RngState));

        FAttractor A;
        A.Pos = FVector(
            TrunkBaseWorld.X + FMath::Cos(Angle) * Rr,
            TrunkBaseWorld.Y + FMath::Sin(Angle) * Rr,
            CrownBaseZ + T * CrownH);
        A.bAlive = true;
        A.BestNode = INDEX_NONE;
        A.BestDist = 0.f;
        Attractors.Add(A);
    }
}

void FAttractorCloud::BuildIndex(float InCellSize)
{
    CellSize = FMath::Max(InCellSize, KINDA_SMALL_NUMBER);
    CellStart.Reset();
    SortedIdx.Reset();

    const int32 N = Attractors.Num();
    if (N == 0)
    {
        GridW = GridH = GridD = 0;
        return;
    }

    // Limites de la nube -> geometria de la rejilla.
    FBox Bounds(ForceInit);
    for (const FAttractor& A : Attractors)
    {
        Bounds += A.Pos;
    }
    GridOrigin = Bounds.Min;
    constexpr int32 MaxCellsPerAxis = 256;
    EcoGrid::DimensionsFromBounds(Bounds, CellSize, MaxCellsPerAxis, GridW, GridH, GridD);

    // Counting sort (CSR) compartido: contar, prefijo acumulado, volcar. O(N).
    // Orden fijo (indice creciente) -> determinista. El indice se construye una
    // vez por arbol, asi que el cursor puede ser un scratch local.
    TArray<int32> Cursor;
    EcoGrid::BuildCSR(GridW * GridH * GridD, N,
        [this](int32 i) { return CellOf(Attractors[i].Pos); },
        CellStart, SortedIdx, Cursor);
}

void FAttractorCloud::CullByShade(const FTreeLightGridFine& Light, float LightThreshold)
{
    for (FAttractor& A : Attractors)
    {
        if (A.bAlive && Light.IsShaded(A.Pos, LightThreshold))
        {
            A.bAlive = false;
        }
    }
}