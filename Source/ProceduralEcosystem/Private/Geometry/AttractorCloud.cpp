/**
 * @file AttractorCloud.cpp
 * @author Juan Luque Roldán
 * @brief Siembra de la envolvente de copa, índice CSR y poda por sombra de la nube.
 *
 * Implementa las tres operaciones de FAttractorCloud. La siembra combina la forma
 * cerrada de la envolvente por especie (cónica, columnar o elipsoidal), un sesgo
 * vertical de la densidad, la falda de sub-copa que reparte unas pocas ramas bajas por
 * el fuste, un ruido de contorno coherente en azimut y altura, y el muestreo
 * area-uniforme del disco horizontal; los dos caminos —copa y falda— consumen el mismo
 * número de valores del generador para que la secuencia no dependa de cuántos puntos
 * caen en cada uno. El índice es un counting sort en tres pasadas sobre la celda de
 * cada atractor. La poda marca muertos los atractores por debajo del umbral de luz.
 *
 * @ingroup eco_geometry
 * @see @ref bib_weberpenn1995
 * @see @ref bib_perlin1985
 * @see @ref bib_discouniforme
 * @see @ref bib_countingsortcsr
 */

#include "Geometry/AttractorCloud.h"
#include "Geometry/TreeLightGridFine.h"
#include "Species/SpeciesData.h"
#include "Core/EcoCore.h" // EcoRand: generador determinista por árbol

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
    // fracción TrunkFraction de la altura TOTAL del árbol.
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

    // Offsets del ruido de envolvente, desde un sub-stream derivado por hash del
    // estado de entrada: así el contorno blando no consume del stream principal y
    // no desplaza el jitter que el crecimiento gasta después.
    const uint32 EnvSeed = RngState;
    const FVector NoiseOffset(
        (float)(EcoRand::Hash32(EnvSeed ^ 0x1B873593u) % 8192u) * 0.125f,
        (float)(EcoRand::Hash32(EnvSeed ^ 0xCC9E2D51u) % 8192u) * 0.125f,
        (float)(EcoRand::Hash32(EnvSeed ^ 0x85EBCA6Bu) % 8192u) * 0.125f);

    for (int32 i = 0; i < N; ++i)
    {
        // Los últimos NumSkirt van a la falda de sub-copa, bajo la base de copa.
        const bool bSkirt = (i >= N - NumSkirt);

        // Altura normalizada y radio de la envolvente a esa altura. Los dos
        // caminos consumen el MISMO número de valores del generador (3), para que
        // la secuencia no dependa de cuántos atractores caen en la falda.
        float T = 0.f;
        float RadiusAtT = 0.f;
        float Z = 0.f;
        float NoiseT = 0.f;   // coordenada vertical del ruido de contorno

        if (bSkirt)
        {
            // Falda: unas pocas ramas bajas dispersas por el fuste, con la
            // densidad y el alcance cayendo hacia el suelo (el cuadrado sesga las
            // muestras hacia la copa). Sin ella el tronco desnudo es una zona
            // vedada para las ramas, la copa arranca de golpe en un plano y toda
            // la ramificación se concentra en la punta del fuste.
            const float S = EcoRand::NextUnit(RngState);
            const float Down = S * S;
            T = 0.f;
            NoiseT = -0.6f * Down; // el ruido de contorno continúa por debajo de la copa
            Z = CrownBaseZ - Down * TrunkH * 0.85f;
            RadiusAtT = CrownR * FMath::Lerp(0.55f, 0.12f, Down);
        }
        else
        {
            // T = altura normalizada dentro de la copa: 0 = base de copa, 1 = ápice.
            // El exponente Gamma sesga la densidad en vertical sin cambiar la forma.
            T = FMath::Pow(EcoRand::NextUnit(RngState), Gamma);
            NoiseT = T;
            Z = CrownBaseZ + T * CrownH;

            // Radio de la envolvente a esa altura, según la forma de la especie.
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

        // Azimut del disco horizontal; el radio se sortea más abajo.
        const float Angle = EcoRand::NextRange(RngState, 0.f, 2.f * PI);

        // Ruido de contorno. Tiene que ser COHERENTE en azimut y altura, no
        // blanco: con ruido blanco la silueta no cambia, porque el máximo
        // estadístico de cientos de muestras reconstruye la envolvente exacta.
        // Se muestrea en (cos, sin, altura), lo que además lo hace periódico en
        // el azimut por construcción: no hay costura en Angle = 0.
        if (EnvNoise > 0.f)
        {
            const FVector NoisePos = NoiseOffset + FVector(
                FMath::Cos(Angle) * 1.9f,
                FMath::Sin(Angle) * 1.9f,
                NoiseT * 2.6f);
            RadiusAtT *= FMath::Clamp(1.f + EnvNoise * FMath::PerlinNoise3D(NoisePos), 0.15f, 1.85f);
        }

        // Radio con la corrección r = R*sqrt(U), que hace la densidad uniforme por
        // ÁREA y evita el apelmazamiento junto al eje. Se llama al helper de radio y
        // no al de disco completo porque el ruido de contorno se intercala entre el
        // azimut y el radio; la fórmula sigue teniendo una única copia.
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

    // Límites de la nube: fijan origen y dimensiones de la rejilla.
    FBox Bounds(ForceInit);
    for (const FAttractor& A : Attractors)
    {
        Bounds += A.Pos;
    }
    GridOrigin = Bounds.Min;

    // Tope defensivo de celdas por eje: una copa enorme con d_i diminuto pediría
    // una rejilla de millones de celdas. Al recortar, la rejilla deja de cubrir la
    // caja entera y CellOf pliega lo que sobresale contra las celdas del borde.
    constexpr int32 MaxCellsPerAxis = 256;
    EcoGrid::DimensionsFromBounds(Bounds, CellSize, MaxCellsPerAxis, GridW, GridH, GridD);

    // Counting sort compartido, en tres pasadas: contar por celda, prefijo acumulado
    // y volcado con cursor, en O(N). Recorrer los atractores en índice creciente fija
    // el orden de SortedIdx y con él el de todas las consultas. El índice se construye
    // una sola vez por árbol, así que el cursor puede ser un scratch local.
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