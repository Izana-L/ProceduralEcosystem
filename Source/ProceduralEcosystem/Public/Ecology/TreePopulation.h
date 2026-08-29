/**
 * @file TreePopulation.h
 * @author Juan Luque Roldán
 * @brief Población de árboles en structure-of-arrays y estados de su ciclo de vida.
 *
 * Declara ETreeState —los estados por los que pasa un árbol— y FTreePopulation, el
 * contenedor de los agentes de la simulación. Los datos van en once arrays paralelos y no
 * en un array de structs porque cada pasada del tick toca dos o tres campos de decenas de
 * miles de árboles, y en SoA esos campos viajan contiguos por la caché. Es un contenedor
 * pasivo, sin conocimiento de campos de recursos, RNG ni reglas: solo datos y las
 * operaciones de gestión del array, todas apoyadas en el visitor ForEachArray para que la
 * lista de campos exista una sola vez.
 *
 * @ingroup eco_ecology
 * @see @ref bib_acton2014
 * @see @ref bib_dinamicadeclaros
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Estado biológico de un árbol dentro de su ciclo de vida.
 *
 * Enum plano y no UENUM: vive en un array de uint8 dentro de una estructura caliente que
 * se recorre miles de veces por tick y no necesita exponerse a Blueprint ni al editor.
 */
enum class ETreeState : uint8
{
    /** Plántula recién germinada, de biomasa baja y todavía sin madurez reproductiva. */
    Sapling,

    /** Adulto: crece a pleno rendimiento y se reproduce desde USpeciesData::MaturityAge. */
    Mature,

    /**
     * Declive por EDAD: apenas crece, se reproduce menos y su probabilidad de morir se
     * multiplica. Es irreversible, porque el envejecimiento no se revierte.
     *
     * @see EcologyRules::IsSenescentByAge
     */
    Senescent,

    /**
     * Declive por ESTRÉS: crecimiento casi detenido, pero reversible —se entra y se sale
     * con histéresis— y con un hazard propio de la especie que puede ser MENOR que el
     * normal.
     *
     * Es el estado que sostiene el banco de plántulas: una especie tolerante sobrevive
     * décadas suprimida bajo el dosel y hereda el hueco cuando cae el dominante. Que sea
     * un estado distinto de la senescencia por edad es lo que impide que unos pocos años
     * de mala racha marquen de por vida a una plántula capaz de vivir siglos.
     *
     * @see EcologyRules::UpdateSuppression
     */
    Suppressed,

    /** Marcado para eliminar en la siguiente compactación: ni se dibuja ni se procesa. */
    Dead
};

/** true si el árbol cuenta como vivo, es decir, todo salvo Dead. Copia única del criterio. */
FORCEINLINE bool IsAliveState(ETreeState State) { return State != ETreeState::Dead; }

/**
 * true si el árbol ha alcanzado la madurez reproductiva.
 *
 * Un suprimido sigue reproduciéndose, con menos fuerza: cortarlo a cero le quitaría a la
 * especie tolerante la única vía por la que compensa su lentitud.
 */
FORCEINLINE bool IsReproductiveState(ETreeState State)
{
    return State == ETreeState::Mature || State == ETreeState::Senescent || State == ETreeState::Suppressed;
}

/**
 * Población de árboles en structure-of-arrays: once arrays paralelos indexados por el
 * mismo entero.
 *
 * A veinte mil agentes, cada pasada del tick —crecimiento, sombreado, mortalidad— toca
 * dos o tres campos como mucho. En SoA esos campos van contiguos y la caché los
 * prefetchea; en array-of-structs cada acceso arrastraría los bytes completos del agente.
 *
 * El índice `i` identifica a un árbol de forma estable mientras está vivo, y es la
 * referencia que usan el índice espacial y el scratch por tarea dentro de un mismo tick.
 * Solo cambia al compactar.
 *
 * Contenedor pasivo: no conoce los campos de recursos, ni los streams RNG, ni las reglas
 * de ecología. Esas viven en EcologyRules y en el bucle de tick de UEcosystemSubsystem;
 * aquí solo hay datos y las operaciones de gestión del array.
 *
 * @warning Los once arrays han de tener siempre la misma longitud.
 * @see AllArraysHaveNum
 */
struct PROCEDURALECOSYSTEM_API FTreePopulation
{

    TArray<FVector> Position;   ///< Posición de mundo, en cm.
    TArray<uint16>  SpeciesId;  ///< Índice en UEcosystemSettings::Species; fijo de por vida.
    TArray<float>   Age;        ///< Años simulados.
    TArray<float>   Biomass;    ///< Proxy de tamaño; su techo es USpeciesData::MaxBiomass.
    TArray<float>   Height;     ///< Altura en cm, cacheada desde Biomass para la rejilla de luz.
    TArray<float>   Stress;     ///< Acumulador de estrés en [0,1].
    TArray<ETreeState> State;   ///< Estado del ciclo de vida.
    TArray<uint32>  RngState;   ///< Stream RNG propio: hace deterministas los sorteos en paralelo.
    TArray<uint32>  StableId;   ///< Id que acompaña al árbol toda su vida, invariante al compactar.

    // ==== Instrumentación ==================================================
    // El tick escribe estos dos arrays y no los lee: los consumen los comandos de
    // demografía. Viven dentro del SoA, y no en un mapa aparte, porque tienen que
    // sobrevivir a CompactDead emparejados con su árbol; cualquier estructura paralela
    // se descuadraría en la primera muerte.

    TArray<float>   Vigor;      ///< Vigor del último tick en [0,1], de EcoVigor::EvaluateVigor.
    TArray<uint8>   Limiter;    ///< Limitante del último tick: @ref EEcoLimiter como uint8.

    /**
     * Siguiente identificador estable a repartir.
     *
     * CopyFrom lo propaga al buffer de escritura para que los ids que reparta la
     * germinación de este tick no dependan de nada más que del estado de lectura.
     */
    uint32 NextStableId = 1;

    /**
     * Única enumeración de los arrays del SoA: invoca `Visit(Array)` sobre los once, en
     * orden fijo.
     *
     * Reserve, CompactDead, AllArraysHaveNum, CopyFrom y la serialización del bake pasan
     * por aquí, de modo que añadir un campo al agente solo exige declararlo y sumarlo a
     * esta lista. Repetir la lista sitio por sitio no protege de nada: olvidar una línea
     * no rompe la compilación, deja un array descuadrado que mezcla datos de árboles
     * distintos a mitad de una partida larga.
     *
     * @param Visit Lambda genérica (`auto&`), porque los arrays son de tipos distintos.
     *              Se expande e inlinea entera, sin coste en runtime.
     */
    template <typename FVisit>
    FORCEINLINE void ForEachArray(FVisit&& Visit)
    {
        Visit(Position);  Visit(SpeciesId); Visit(Age);   Visit(Biomass);
        Visit(Height);    Visit(Stress);    Visit(State); Visit(RngState);
        Visit(StableId);  Visit(Vigor);     Visit(Limiter);
    }

    /** Sobrecarga de solo lectura de @ref ForEachArray. */
    template <typename FVisit>
    FORCEINLINE void ForEachArray(FVisit&& Visit) const
    {
        Visit(Position);  Visit(SpeciesId); Visit(Age);   Visit(Biomass);
        Visit(Height);    Visit(Stress);    Visit(State); Visit(RngState);
        Visit(StableId);  Visit(Vigor);     Visit(Limiter);
    }

    /**
     * Igual que @ref ForEachArray, pero emparejando los arrays de dos poblaciones: invoca
     * `Visit(ArrayDeDst, ArrayDeSrc)` en el mismo orden fijo. Lo usa CopyFrom.
     */
    template <typename FVisit>
    FORCEINLINE static void ForEachArrayPair(FTreePopulation& Dst, const FTreePopulation& Src, FVisit&& Visit)
    {
        Visit(Dst.Position,  Src.Position);  Visit(Dst.SpeciesId, Src.SpeciesId);
        Visit(Dst.Age,       Src.Age);       Visit(Dst.Biomass,   Src.Biomass);
        Visit(Dst.Height,    Src.Height);    Visit(Dst.Stress,    Src.Stress);
        Visit(Dst.State,     Src.State);     Visit(Dst.RngState,  Src.RngState);
        Visit(Dst.StableId,  Src.StableId);  Visit(Dst.Vigor,     Src.Vigor);
        Visit(Dst.Limiter,   Src.Limiter);
    }
    /** Árboles actualmente en el array: vivos más muertos aún sin compactar. */
    int32 Num() const { return Position.Num(); }

    /**
     * Comprueba la invariante del SoA: que los once arrays paralelos tengan exactamente
     * ExpectedNum elementos.
     *
     * La verifica CompactDead y la usa la carga de un bake para rechazar un estado
     * descuadrado antes de pisar el que está vivo.
     *
     * @return true si todas las longitudes coinciden con ExpectedNum.
     */
    bool AllArraysHaveNum(int32 ExpectedNum) const
    {
        bool bOk = true;
        ForEachArray([ExpectedNum, &bOk](const auto& Array) { bOk = bOk && (Array.Num() == ExpectedNum); });
        return bOk;
    }

    /** Reserva de golpe en los once arrays, para evitar realojos en una germinación masiva. */
    void Reserve(int32 ExpectedNum);

    /**
     * Da de alta un árbol nuevo y le asigna el siguiente StableId.
     *
     * @param InRngState Semilla del stream propio del árbol. Debe derivarse por hash de
     *                   datos ya deterministas —posición de la madre, tick, contador de
     *                   semilla— y nunca de un contador global, o el stream dependería del
     *                   orden en que los hilos procesen la germinación.
     * @return Índice del árbol recién añadido.
     * @note Un estado RNG de valor 0 se sustituye por 1: el cero es absorbente para
     *       xorshift32 y dejaría al árbol con una secuencia constante.
     */
    int32 Add(const FVector& InPosition, uint16 InSpeciesId, uint32 InRngState,
        float InAge = 0.f, float InBiomass = 0.f);

    /**
     * Elimina los árboles marcados como ETreeState::Dead preservando el orden relativo de
     * los vivos (compactación estable).
     *
     * No es el clásico swap-with-last: ese reordena la población y rompe la
     * correspondencia entre índice y orden de nacimiento de la que depende la
     * reproducibilidad de la corrida.
     *
     * @return Cuántos árboles se eliminaron.
     */
    int32 CompactDead();

    /**
     * Copia el contenido de Src sobre esta población, redimensionando si hace falta.
     *
     * Abre cada tick: prepara el buffer de escritura a partir del snapshot de lectura,
     * NextStableId incluido.
     */
    void CopyFrom(const FTreePopulation& Src);
};