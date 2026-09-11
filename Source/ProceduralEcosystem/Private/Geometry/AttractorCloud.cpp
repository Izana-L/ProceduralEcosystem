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
 * caen en cada uno. La copa columnar es un cilindro de extremos recogidos con la panza
 * en el tercio bajo, y se siembra estratificada en altura y en espiral áurea en azimut
 * para que ninguna cota concentre un corro de atractores. El índice es un counting sort
 * en tres pasadas sobre la celda de cada atractor. La poda marca muertos los atractores
 * por debajo del umbral de luz.
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

namespace
{
    // ==== Perfil de la copa columnar ====
    //
    // No es un cilindro recto ni un ovoide, sino un cilindro con los dos extremos
    // recogidos y la panza en el tercio bajo: la copa arranca estrecha en su base, se
    // hincha hasta el radio máximo en ColumnarPeakT, se mantiene casi cilíndrica por la
    // mitad de la copa y se va cerrando hacia el ápice, que queda más estrecho que la
    // base. Es la silueta del ciprés o del chopo lombardo: la masa cuelga del tercio
    // bajo y la punta afila sin llegar a cero.
    //
    // El perfil anterior era casi recto con el radio máximo en la base de copa: el
    // anillo más ancho caía a una sola cota, justo donde el tronco desnudo entra en la
    // copa, y de ese anillo salía un verticilo de ramas horizontales en todas
    // direcciones.

    /** Radio en la base de copa, como fracción de CrownRadiusCm. */
    constexpr float ColumnarBaseR = 0.45f;

    /** Radio en el ápice, como fracción de CrownRadiusCm. */
    constexpr float ColumnarApexR = 0.25f;

    /** Altura normalizada de la panza, donde la envolvente alcanza CrownRadiusCm entero. */
    constexpr float ColumnarPeakT = 0.35f;

    /**
     * Radio de la envolvente columnar a la altura normalizada T (0 = base de copa,
     * 1 = ápice), como fracción de CrownRadiusCm.
     *
     * Dos tramos que empalman en la panza con tangente horizontal, para que el radio
     * máximo sea una zona y no un anillo a una sola cota. Por debajo, una rampa suave
     * (smoothstep) de ColumnarBaseR a 1: la base es un tramo estrecho que se hincha.
     * Por encima, @f$(1 - u^2)^{0.75}@f$ de 1 a ColumnarApexR: conserva el radio casi
     * entero hasta dos tercios de la copa —el tramo cilíndrico— y lo recoge en el
     * último tercio hacia la punta.
     */
    float ColumnarProfile(float T)
    {
        T = FMath::Clamp(T, 0.f, 1.f);
        if (T < ColumnarPeakT)
        {
            const float U = T / ColumnarPeakT;                          // 0 en la base, 1 en la panza
            return FMath::Lerp(ColumnarBaseR, 1.f, U * U * (3.f - 2.f * U));
        }
        const float U = (T - ColumnarPeakT) / (1.f - ColumnarPeakT);   // 0 en la panza, 1 en el ápice
        return FMath::Lerp(ColumnarApexR, 1.f, FMath::Pow(1.f - U * U, 0.75f));
    }

    /**
     * Ángulo áureo, @f$2\pi(1 - 1/\varphi) \approx 137{,}5^\circ@f$: el giro entre dos
     * atractores consecutivos de la copa columnar. Es el reparto más uniforme posible
     * alrededor de un eje para cualquier número de puntos (la espiral de Vogel del
     * girasol), y con él dos atractores contiguos en altura nunca caen del mismo lado.
     */
    constexpr float GoldenAngleRad = 2.39996323f;

    /**
     * Anchura del jitter de azimut de la espiral, como fracción del ángulo áureo. Con
     * 0,6 el giro entre vecinos queda en 137,5 ± 41 grados: rompe la regularidad de la
     * espiral sin que dos vecinos lleguen a coincidir de lado.
     */
    constexpr float ColumnarAzimuthJitter = 0.6f;
}

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
    const int32 NumCrown = N - NumSkirt;               // >= 1

    // Muestreo estratificado, solo en la copa columnar. Con muestreo blanco la altura
    // de cada atractor es independiente de las demás y salen corros de varios a la
    // misma cota; en una copa ancha el corro se reparte por un disco grande y pasa
    // desapercibido, pero en la columnar, estrecha, cada corro se convierte en un
    // verticilo de ramas que salen del mismo tramo de fuste en todas direcciones. La
    // columnar reparte por eso las alturas en franjas —una por atractor, con jitter
    // dentro de ella— y los azimuts en espiral áurea con jitter, de modo que ningún
    // tramo del eje ve más atractores que otro y dos contiguos en altura nunca caen
    // del mismo lado. Las otras dos formas conservan el muestreo blanco: cambiarlo
    // movería todos los arquetipos ya calibrados.
    const bool bStratified = (Species.CrownShape == ECrownShape::Columnar);

    // Radio con el que arranca la falda justo bajo la base de copa, como fracción de
    // CrownR. En la columnar continúa el perfil, que ahí es estrecho: un escalón hacia
    // fuera dejaría el faldón más ancho que la base de la propia copa.
    const float SkirtTopR = bStratified ? ColumnarBaseR : 0.55f;

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
        const bool bSkirt = (i >= NumCrown);

        // Altura normalizada y radio de la envolvente a esa altura. Los dos
        // caminos consumen el MISMO número de valores del generador (3), para que
        // la secuencia no dependa de cuántos atractores caen en la falda.
        float T = 0.f;
        float RadiusAtT = 0.f;
        float Z = 0.f;
        float NoiseT = 0.f;   // coordenada vertical del ruido de contorno

        // Primera extracción: la coordenada vertical. Estratificada, es el jitter
        // dentro de la franja que le toca al atractor por su índice; blanca, es la
        // altura misma.
        const float U = EcoRand::NextUnit(RngState);

        if (bSkirt)
        {
            // Falda: unas pocas ramas bajas dispersas por el fuste, con la
            // densidad y el alcance cayendo hacia el suelo (el cuadrado sesga las
            // muestras hacia la copa). Sin ella el tronco desnudo es una zona
            // vedada para las ramas, la copa arranca de golpe en un plano y toda
            // la ramificación se concentra en la punta del fuste.
            const float S = bStratified ? ((float)(i - NumCrown) + U) / (float)NumSkirt : U;
            const float Down = S * S;
            T = 0.f;
            NoiseT = -0.6f * Down; // el ruido de contorno continúa por debajo de la copa
            Z = CrownBaseZ - Down * TrunkH * 0.85f;
            RadiusAtT = CrownR * FMath::Lerp(SkirtTopR, 0.12f, Down);
        }
        else
        {
            // T = altura normalizada dentro de la copa: 0 = base de copa, 1 = ápice.
            // El exponente Gamma sesga la densidad en vertical sin cambiar la forma;
            // aplicado sobre franjas estratificadas las deforma pero no las solapa.
            const float Unit = bStratified ? ((float)i + U) / (float)NumCrown : U;
            T = FMath::Pow(Unit, Gamma);
            NoiseT = T;
            Z = CrownBaseZ + T * CrownH;

            // Radio de la envolvente a esa altura, según la forma de la especie.
            switch (Species.CrownShape)
            {
            case ECrownShape::Conical:
                RadiusAtT = CrownR * (1.f - T);                 // ancha abajo, punta arriba
                break;

            case ECrownShape::Columnar:
                RadiusAtT = CrownR * ColumnarProfile(T);        // estrecha abajo, panza en el tercio bajo, cierra arriba
                break;

            case ECrownShape::Spherical:
            default:
            {
                const float U2 = 2.f * T - 1.f;                 // -1..1 (centro de copa en T=0.5)
                RadiusAtT = CrownR * FMath::Sqrt(FMath::Max(0.f, 1.f - U2 * U2)); // elipsoide
                break;
            }
            }
        }

        // Azimut del disco horizontal; el radio se sortea más abajo. Segunda
        // extracción: en la copa columnar es el jitter alrededor de la espiral áurea
        // —cada atractor gira 137,5 grados respecto al anterior—; en las demás formas,
        // el azimut mismo, uniforme en la vuelta completa.
        const float AzimuthU = EcoRand::NextUnit(RngState);
        const float Angle = bStratified
            ? (float)i * GoldenAngleRad + (AzimuthU - 0.5f) * (GoldenAngleRad * ColumnarAzimuthJitter)
            : (2.f * PI) * AzimuthU;

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
        // ÁREA y evita el apelmazamiento junto al eje. Tercera extracción. Se llama al
        // helper de radio y no al de disco completo porque el ruido de contorno se
        // intercala entre el azimut y el radio; la fórmula sigue teniendo una única
        // copia.
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
