/**
 * @file EcoCore.h
 * @author Juan Luque Roldán
 * @brief Cimiento de reproducibilidad del simulador: generador xorshift32, hashes
 *        estables y streams de RNG derivados de una única semilla maestra.
 *
 * Concentra en un solo sitio todo aquello cuya duplicación rompería el determinismo
 * del ecosistema. Ofrece dos caminos que no se mezclan: las funciones `Next*`
 * consumen un estado explícito y son exclusivas de la simulación, con un orden de
 * consumo que forma parte del contrato; las funciones `Hash*` son puras y sin
 * estado, de modo que mallador, viento y render obtienen aleatoriedad estable sin
 * desplazar la secuencia que gasta la simulación. De la semilla maestra salen, por
 * derivación, tanto los streams por subsistema (@ref FEcosystemRng) como las
 * semillas del terreno, del ruido y de los arquetipos.
 *
 * @ingroup eco_core
 * @see @ref bib_marsaglia2003
 * @see @ref bib_salmon2011
 */

#pragma once

#include "CoreMinimal.h"
#include "EcoCore.generated.h"

// La categoría de log LogEco NO se declara aquí: un DECLARE_LOG_CATEGORY_EXTERN en
// una cabecera reflejada por UHT (con UENUM/USTRUCT) hace que UHT deje de registrar
// los tipos que vengan detrás. Como solo la usa EcosystemSubsystem.cpp, allí se
// declara y define con DEFINE_LOG_CATEGORY_STATIC.

/**
 * @brief Aleatoriedad reproducible del proyecto: xorshift32 con estado explícito y
 *        valores estables derivados por hash.
 *
 * Regla de oro de reproducibilidad: nada estocástico sale de `FMath::Rand()` ni de
 * `rand()`; todo pasa por un estado que aporta el llamador. Así una misma semilla da
 * siempre el mismo bosque y el resultado no depende del hilo que procese cada agente.
 * El estado cabe en un `uint32`, por lo que cada árbol lleva su propio stream dentro
 * del SoA de la población sin coste de memoria apreciable.
 */
namespace EcoRand
{
    /**
     * Avanza el generador xorshift de 32 bits con la terna de desplazamientos
     * (13, 17, 5), de periodo máximo @f$2^{32}-1@f$.
     *
     * @param State Estado del stream; se actualiza in situ.
     * @return El nuevo estado, utilizable directamente como valor pseudoaleatorio.
     * @note El cero es punto fijo del xorshift, así que se sustituye por la constante
     *       áurea en cada llamada y no solo al sembrar: un estado corrompido o
     *       deserializado a cero no congela la secuencia.
     */
    FORCEINLINE uint32 NextU32(uint32& State)
    {
        uint32 x = (State != 0u) ? State : 0x9E3779B9u; // el estado 0 es absorbente
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        State = x;
        return x;
    }

    /**
     * Convierte 32 bits crudos en un float uniforme en @f$[0, 1)@f$.
     *
     * Usa los 24 bits altos, que son exactamente la mantisa de un float, y descarta
     * los 8 bajos, los de peor calidad en un xorshift. Copia única de la conversión:
     * la comparten NextUnit, HashUnit, los hashes estables de los arquetipos y los
     * desfases de viento, de modo que todos los consumidores tienen la misma
     * distribución y el mismo redondeo.
     */
    FORCEINLINE float UnitFromBits(uint32 Bits)
    {
        return static_cast<float>(Bits >> 8) * (1.0f / 16777216.0f);
    }

    /** Float uniforme en [0, 1) consumiendo una extracción del stream. */
    FORCEINLINE float NextUnit(uint32& State)
    {
        return UnitFromBits(NextU32(State));
    }

    /** Float en [Min, Max). */
    FORCEINLINE float NextRange(uint32& State, float Min, float Max)
    {
        return Min + (Max - Min) * NextUnit(State);
    }

    /**
     * Entero en [Min, Max], ambos inclusive.
     * @note Reduce por módulo: el sesgo es despreciable a los rangos que maneja el
     *       modelo, muy pequeños frente a @f$2^{32}@f$.
     */
    FORCEINLINE int32 NextRangeInt(uint32& State, int32 Min, int32 Max)
    {
        if (Max <= Min) { return Min; }
        const uint32 Span = static_cast<uint32>(Max - Min) + 1u;
        return Min + static_cast<int32>(NextU32(State) % Span);
    }

    // ==== Muestreadores de dominio (todos consumen del uint32& State recibido) ====

    /**
     * Número de eventos con distribución de Poisson de media Lambda. Alimenta la
     * lluvia de semillas por árbol y tick y el número de claros de perturbación.
     *
     * Dos regímenes para no pagar @f$O(\lambda)@f$ extracciones:
     * @li Lambda < 30: método exacto de Knuth por producto de uniformes.
     * @li Lambda >= 30: aproximación normal @f$N(\lambda, \lambda)@f$ por
     *     Box-Muller, redondeada y acotada a cero. Con biomasa y fecundidad altas
     *     el método exacto dispararía cientos de extracciones por árbol; aquí el
     *     coste, y con él el avance del stream, quedan fijados en dos extracciones.
     *
     * @param Lambda Media de la distribución; los valores no positivos devuelven 0.
     * @return Número de eventos, siempre >= 0.
     * @note El sesgo de la aproximación normal, sin corrección de continuidad, es
     *       despreciable a las escalas de fecundidad del modelo.
     * @see @ref bib_knuthpoisson
     * @see @ref bib_boxmuller1958
     */
    FORCEINLINE int32 PoissonInt(uint32& State, float Lambda)
    {
        if (Lambda <= 0.f)
        {
            return 0;
        }

        if (Lambda < 30.f)
        {
            const float L = FMath::Exp(-Lambda);
            int32 k = 0;
            float p = 1.f;
            do
            {
                ++k;
                p *= NextUnit(State);
            } while (p > L);
            return k - 1;
        }

        // Aproximación normal para Lambda grande: N(Lambda, Lambda).
        const float u1 = FMath::Max(NextUnit(State), 1e-7f); // evita log(0)
        const float u2 = NextUnit(State);
        const float z = FMath::Sqrt(-2.f * FMath::Loge(u1)) * FMath::Cos(2.f * PI * u2);
        return FMath::Max(0, FMath::RoundToInt(Lambda + FMath::Sqrt(Lambda) * z));
    }

    /**
     * Distancia en centímetros de una semilla respecto a su parental, dentro de un
     * disco de radio MaxRadiusCm.
     *
     * Aplica @f$r = R\sqrt{U}@f$, la inversa de la acumulada @f$F(r) = r^2/R^2@f$,
     * con lo que la densidad resulta uniforme POR ÁREA; con @f$r = R\,U@f$ las
     * semillas se apelmazan junto al tronco.
     *
     * @note El kernel de dispersión del modelo es uniforme por área y no decreciente
     *       con la distancia, así que no reproduce la agregación junto al parental ni
     *       las colas largas descritas en la literatura. Es el único punto que hay que
     *       tocar para cambiarlo por un kernel decreciente.
     * @see @ref bib_discouniforme
     * @see @ref bib_dispersionsemillas
     */
    FORCEINLINE float SampleDispersalDistance(uint32& State, float MaxRadiusCm)
    {
        return MaxRadiusCm * FMath::Sqrt(NextUnit(State));
    }

    /**
     * Desplazamiento XY en centímetros, uniforme por área dentro de un disco de radio
     * RadiusCm: ángulo uniforme más radio con la corrección de
     * @ref EcoRand::SampleDispersalDistance.
     *
     * Copia única del patrón, compartida por la dispersión de semillas
     * (`EcologyRules::SampleSeedOffsetCm`), el scatter de hojarasca de la capa de
     * suelo y el disco horizontal de la envolvente de copa.
     *
     * @warning El orden de consumo del RNG es parte del contrato de reproducibilidad:
     *          primero el ángulo, después el radio. Quien necesite intercalar algo
     *          entre los dos, como la envolvente de copa cuando perturba el radio con
     *          ruido, debe llamar a NextRange y a SampleDispersalDistance por
     *          separado y en ese mismo orden.
     */
    FORCEINLINE FVector2D SampleDiscOffsetCm(uint32& State, float RadiusCm)
    {
        const float Angle = NextRange(State, 0.f, 2.f * PI);
        const float Dist = SampleDispersalDistance(State, RadiusCm);
        return FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Dist;
    }

    /**
     * Mezclador de bits de 32 bits: alterna desplazamiento-xor a la derecha y
     * multiplicación por constante impar, y cierra con otro desplazamiento-xor.
     *
     * Es la primitiva de derivación de semillas de todo el proyecto: función pura, sin
     * estado y por tanto llamable desde cualquier hilo o desde el render.
     *
     * @note Las constantes y los desplazamientos son los del mezclador lowbias32,
     *       hallado por búsqueda exhaustiva y de menor sesgo que el fmix32 de
     *       MurmurHash3, del que solo se toma la estructura.
     * @see @ref bib_wellons2018
     * @see @ref bib_appleby2011
     */
    FORCEINLINE uint32 Hash32(uint32 x)
    {
        x ^= x >> 16; x *= 0x7feb352du;
        x ^= x >> 15; x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }

    /**
     * Valor estable en [0, 1) derivado de un entero, sin consumir stream.
     *
     * Aleatoriedad reproducible y sin estado: el mallador, el follaje, el viento y el
     * render la usan a partir de un identificador (árbol, rama, hoja) sin desplazar la
     * secuencia que gasta la simulación.
     *
     * @param Value Identificador del que se deriva el valor.
     * @param Salt  Constante que se combina por xor para obtener varios valores
     *              independientes del mismo identificador.
     */
    FORCEINLINE float HashUnit(uint32 Value, uint32 Salt = 0u)
    {
        return UnitFromBits(Hash32(Value ^ Salt));
    }

    /**
     * Semilla de un elemento a partir de su índice, no del orden en que se procesa.
     *
     * Multiplica el índice por la constante de Knuth @f$2^{32}/\varphi@f$ para
     * descorrelacionar índices consecutivos antes de mezclar. Es lo que permite que la
     * semilla de un árbol recién nacido sea la misma con cualquier número de hilos.
     *
     * @see @ref bib_knuthhashing
     */
    FORCEINLINE uint32 SeedForIndex(uint32 MasterSeed, int32 Index)
    {
        return Hash32(MasterSeed ^ Hash32(static_cast<uint32>(Index) * 2654435761u));
    }
}

/**
 * @brief Subsistemas que disponen de un stream de RNG propio dentro de FEcosystemRng.
 *
 * Cada uno avanza su secuencia sin desplazar la de los demás, de modo que activar o
 * desactivar un subsistema no altera el resto del mundo generado.
 *
 * @warning El orden de los valores fija las semillas derivadas en
 *          @ref FEcosystemRng::Init: insertar o reordenar entradas cambia el stream de
 *          todas las posteriores y con él el bosque resultante.
 */
UENUM(BlueprintType)
enum class EEcoRngStream : uint8
{
    Colonization,
    Dispersal,
    Mortality,
    Morphology,
    /** Herramientas de depuración; stream aparte para no perturbar la simulación. */
    Debug,

    /** Perturbación (claros). Stream propio para que una corrida con régimen de claros
        y otra sin él partan del MISMO bosque y la comparación mida solo su efecto. */
    Disturbance,

    Count UMETA(Hidden)
};

/**
 * @brief Contenedor de un stream de RNG por subsistema, derivados de una única semilla
 *        maestra.
 *
 * Struct plano y no reflejado, alojado en el subsistema de simulación. Al ser un bloque
 * de enteros contiguo puede serializarse tal cual dentro de un bake, lo que permite
 * reanudar una corrida en el punto exacto de la secuencia en que se guardó.
 */
struct FEcosystemRng
{
    /** Semilla del mundo; fijarla fija el bosque entero. */
    uint32 MasterSeed = 1u;

    /** Estado vivo de cada stream, indexado por EEcoRngStream. */
    uint32 State[static_cast<int32>(EEcoRngStream::Count)] = {};

    /**
     * Deriva los streams de una semilla maestra con
     * @f$State_i = Hash32(MasterSeed \oplus 0x01000193 \cdot (i+1))@f$.
     *
     * El factor es el primo de 32 bits de FNV, usado aquí solo como multiplicador que
     * descorrelaciona índices de stream contiguos antes de la mezcla.
     *
     * @param InMasterSeed Semilla del mundo; el cero se sustituye por 1, porque es el
     *                     punto fijo del generador.
     * @see @ref bib_fnv1991
     */
    void Init(uint32 InMasterSeed)
    {
        MasterSeed = (InMasterSeed != 0u) ? InMasterSeed : 1u;
        for (int32 i = 0; i < static_cast<int32>(EEcoRngStream::Count); ++i)
        {
            State[i] = EcoRand::Hash32(MasterSeed ^ (0x01000193u * static_cast<uint32>(i + 1)));
        }
    }

    /** Float en [0, 1) del stream S. */
    FORCEINLINE float  Unit(EEcoRngStream S) { return EcoRand::NextUnit(State[static_cast<int32>(S)]); }

    /** Entero de 32 bits del stream S. */
    FORCEINLINE uint32 U32(EEcoRngStream S) { return EcoRand::NextU32(State[static_cast<int32>(S)]); }

    /** Float en [Min, Max) del stream S. */
    FORCEINLINE float  RangeF(EEcoRngStream S, float Min, float Max)
    {
        return EcoRand::NextRange(State[static_cast<int32>(S)], Min, Max);
    }

    /** Entero en [Min, Max], ambos inclusive, del stream S. */
    FORCEINLINE int32  RangeI(EEcoRngStream S, int32 Min, int32 Max)
    {
        return EcoRand::NextRangeInt(State[static_cast<int32>(S)], Min, Max);
    }
};

/**
 * @brief Marcador esférico de depuración que el subsistema de simulación dibuja en el
 *        mundo cuando se activa la CVar correspondiente.
 *
 * Es independiente de la población simulada: sirve para señalar puntos de interés
 * (muestreos del relieve, posiciones candidatas) sin tocar el SoA de agentes.
 */
USTRUCT()
struct FEcoDebugAgent
{
    GENERATED_BODY()

    /** Posición en el mundo, en centímetros. */
    UPROPERTY() FVector Position = FVector::ZeroVector;

    /** Color de la esfera de depuración. */
    UPROPERTY() FColor  Color = FColor::Green;

    /** Radio de la esfera, en centímetros. */
    UPROPERTY() float   Radius = 100.f;
};