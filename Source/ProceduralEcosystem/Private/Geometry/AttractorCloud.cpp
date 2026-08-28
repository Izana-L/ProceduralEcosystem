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

    const float EnvNoise = FMath::Clamp(Species.EnvelopeNoise, 0.f, 0.6f);
    const float Gamma = FMath::Clamp(Species.CrownVerticalBias, 0.25f, 4.f);
    const float SkirtFrac = FMath::Clamp(Species.SubCrownFraction, 0.f, 0.4f);
    const int32 NumSkirt = FMath::Clamp(FMath::RoundToInt(N * SkirtFrac), 0, N - 1);

    // Offsets del ruido de envolvente desde un SUB-STREAM derivado por hash del
    // estado de entrada: asi la envolvente blanda no consume del stream
    // principal y no desplaza el jitter que el SCA gastara despues.
    const uint32 EnvSeed = RngState;
    const FVector NoiseOffset(
        (float)(EcoRand::Hash32(EnvSeed ^ 0x1B873593u) % 8192u) * 0.125f,
        (float)(EcoRand::Hash32(EnvSeed ^ 0xCC9E2D51u) % 8192u) * 0.125f,
        (float)(EcoRand::Hash32(EnvSeed ^ 0x85EBCA6Bu) % 8192u) * 0.125f);

    for (int32 i = 0; i < N; ++i)
    {
        // Los ultimos NumSkirt van a la FALDA de sub-copa (bajo la base de copa).
        const bool bSkirt = (i >= N - NumSkirt);

        // Altura normalizada y radio de la envolvente a esa altura. Los dos
        // caminos consumen el MISMO numero de valores del RNG (3), para que la
        // secuencia no dependa de cuantos atractores caen en la falda.
        float T = 0.f;
        float RadiusAtT = 0.f;
        float Z = 0.f;
        float NoiseT = 0.f;   // coordenada vertical del ruido de contorno

        if (bSkirt)
        {
            // Falda: unas pocas ramas bajas dispersas por el fuste, con la
            // densidad y el alcance cayendo hacia el suelo (el cuadrado sesga
            // las muestras hacia la copa).
            //
            // Sin esto el tronco desnudo es una zona PROHIBIDA para las ramas y
            // la copa arranca de golpe en un plano: eso es exactamente lo que
            // concentra todas las ramas en la punta del fuste.
            const float S = EcoRand::NextUnit(RngState);
            const float Down = S * S;
            T = 0.f;
            NoiseT = -0.6f * Down; // el ruido continua por debajo de la copa
            Z = CrownBaseZ - Down * TrunkH * 0.85f;
            RadiusAtT = CrownR * FMath::Lerp(0.55f, 0.12f, Down);
        }
        else
        {
            // t = altura normalizada dentro de la copa: 0 = base de copa, 1 = apice.
            // El exponente Gamma sesga la densidad en vertical sin cambiar la forma.
            T = FMath::Pow(EcoRand::NextUnit(RngState), Gamma);
            NoiseT = T;
            Z = CrownBaseZ + T * CrownH;

            // Radio de la envolvente a esa altura, segun la forma de la especie.
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
        }

        // Disco horizontal area-uniforme (r = R*sqrt(U), sin apelmazar en el eje).
        const float Angle = EcoRand::NextRange(RngState, 0.f, 2.f * PI);

        // Ruido de contorno. Tiene que ser COHERENTE en azimut y altura, no
        // blanco: con ruido blanco la silueta no cambiaria, porque el maximo
        // estadistico de cientos de muestras reconstruye la envolvente exacta.
        // Se muestrea en (cos, sin, altura), lo que ademas lo hace periodico en
        // el azimut por construccion (no hay costura en Angle = 0).
        if (EnvNoise > 0.f)
        {
            const FVector NoisePos = NoiseOffset + FVector(
                FMath::Cos(Angle) * 1.9f,
                FMath::Sin(Angle) * 1.9f,
                NoiseT * 2.6f);
            RadiusAtT *= FMath::Clamp(1.f + EnvNoise * FMath::PerlinNoise3D(NoisePos), 0.15f, 1.85f);
        }

        // Misma correccion sqrt que la dispersion de semillas y la hojarasca
        // (EcoRand::SampleDispersalDistance): aqui no se puede usar el helper de
        // disco completo porque el ruido de contorno se intercala entre el
        // angulo y el radio, pero la formula del radio es la UNICA copia.
        const float Rr = EcoRand::SampleDispersalDistance(RngState, RadiusAtT);

        FAttractor A;
        A.Pos = FVector(
            TrunkBaseWorld.X + FMath::Cos(Angle) * Rr,
            TrunkBaseWorld.Y + FMath::Sin(Angle) * Rr,
            Z);
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