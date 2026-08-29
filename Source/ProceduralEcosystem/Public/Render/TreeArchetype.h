/**
 * @file TreeArchetype.h
 * @author Juan Luque Roldán
 * @brief Clave de arquetipo, bucketing de tamaño con histéresis y determinismo por árbol.
 *
 * Discretiza el espacio continuo (especie, tamaño) en un catálogo pequeño de arquetipos
 * —especie × bucket de edad × variante morfológica— para que decenas de miles de árboles se
 * dibujen con un centenar de mallas horneadas. Todo lo que declara son funciones puras, sin
 * UObjects ni componentes y por tanto verificables sueltas: el bucket con histéresis, la
 * escala que interpola dentro del bucket y la variante, el giro y el jitter que se derivan
 * por hash del identificador estable del árbol. La variedad que se percibe en pantalla no
 * está en la malla sino en la transformación de instancia. Cierra el fichero la compactación
 * del mapeo instancia→árbol que exige el borrado por lotes de un componente instanciado.
 *
 * @ingroup eco_render
 * @see @ref bib_deussen1998
 * @see @ref bib_clarkjh1976
 */

#pragma once

#include "CoreMinimal.h"
#include "Core/EcoCore.h"
#include "Ecology/EcologyRules.h" // HeightRatioFromBiomass: alometría compartida con el tick

/**
 * Clave de un arquetipo: especie × bucket de edad/tamaño × variante morfológica.
 *
 * Es la unidad de reutilización de la librería de mallas. Ni se pueden hornear 20.000 mallas
 * únicas ni generarlas al vuelo, así que el espacio continuo se discretiza: 3 especies × 5
 * buckets × 8 variantes son 120 mallas, cada una horneada una sola vez con semilla fija.
 * Para que esas 120 mallas parezcan 20.000 árboles distintos, la variedad continua vive en la
 * transformación de instancia —variante estable, escala interpolada dentro del bucket, giro y
 * jitter—, no en la geometría.
 *
 * Struct plano de cuatro bytes útiles que se empaqueta en un `uint32`: ese entero es a la vez
 * clave de `TMap` e identificador legible en los registros.
 */
struct FArchetypeKey
{
    uint16 Species = 0;   ///< Índice en UEcosystemSettings::Species.
    uint8  AgeBucket = 0; ///< Bucket de tamaño, en [0, NumAgeBuckets).
    uint8  Variant = 0;   ///< Variante morfológica, en [0, NumLodVariants).

    FArchetypeKey() = default;
    FArchetypeKey(uint16 InSpecies, uint8 InAgeBucket, uint8 InVariant)
        : Species(InSpecies), AgeBucket(InAgeBucket), Variant(InVariant) {}

    /** Empaqueta la clave en un `uint32`: especie en los 16 bits altos, bucket y variante en
        un byte cada uno. @return Clave empaquetada, apta para `TMap` y comparación directa. */
    FORCEINLINE uint32 Pack() const
    {
        return (static_cast<uint32>(Species) << 16)
             | (static_cast<uint32>(AgeBucket) << 8)
             |  static_cast<uint32>(Variant);
    }

    /** Inversa de Pack(). */
    static FORCEINLINE FArchetypeKey Unpack(uint32 Packed)
    {
        return FArchetypeKey(
            static_cast<uint16>(Packed >> 16),
            static_cast<uint8>((Packed >> 8) & 0xFFu),
            static_cast<uint8>(Packed & 0xFFu));
    }

    FORCEINLINE bool operator==(const FArchetypeKey& Other) const { return Pack() == Other.Pack(); }
    FORCEINLINE bool operator!=(const FArchetypeKey& Other) const { return Pack() != Other.Pack(); }

    /** Forma legible para registros y diagnóstico: `SpN/BN/VN`. */
    FString ToString() const
    {
        return FString::Printf(TEXT("Sp%u/B%u/V%u"), (uint32)Species, (uint32)AgeBucket, (uint32)Variant);
    }
};

FORCEINLINE uint32 GetTypeHash(const FArchetypeKey& Key) { return ::GetTypeHash(Key.Pack()); }

/**
 * Funciones puras de la capa de presentación: solo reciben y devuelven valores simples, no
 * tocan UObjects ni componentes, y por eso se verifican sueltas.
 *
 * Determinismo por árbol: variante, giro, escala y fase estacional se derivan por hash de
 * `FTreePopulation::StableId`, nunca de `FTreePopulation::RngState`. RngState es el estado
 * vivo del flujo aleatorio del árbol y avanza en cada mortalidad y cada dispersión, así que
 * derivar de él cambiaría la malla y la rotación en cada tick. StableId se asigna al nacer y
 * no cambia, de modo que un árbol conserva su variante y su giro toda la vida. Al ser un
 * hash sin estado, tampoco consume ningún flujo de la simulación.
 */
namespace TreeArchetype
{
    /**
     * Sales de dominio del hash: un mismo StableId da flujos independientes para variante,
     * giro, escala y fase estacional. Son las constantes clásicas del hashing multiplicativo
     * (0x9E3779B9 es la razón áurea escalada a 32 bits) usadas aquí solo para separar
     * dominios, no para dispersar por sí solas.
     *
     * @see @ref bib_knuthhashing
     */
    enum : uint32
    {
        SaltVariant = 0x9E3779B9u,
        SaltYaw     = 0x85EBCA6Bu,
        SaltScale   = 0xC2B2AE35u,
        SaltPhase   = 0x27D4EB2Fu
    };

    /** Hash de 32 bits del árbol dentro de un dominio (una de las sales de arriba). */
    FORCEINLINE uint32 StableHash(uint32 StableId, uint32 Salt)
    {
        return EcoRand::Hash32(StableId ^ Salt);
    }

    /** Valor estable en [0,1) derivado del identificador del árbol. Reenvía a
        EcoRand::HashUnit, copia única del idioma hash → [0,1); aquí solo recibe el nombre
        del dominio de presentación. */
    FORCEINLINE float StableUnit(uint32 StableId, uint32 Salt)
    {
        return EcoRand::HashUnit(StableId, Salt);
    }

    /**
     * Fracción de altura adulta del árbol, en [0,1]: la entrada de todo el bucketing.
     *
     * Delega en la alometría única del proyecto en vez de reimplantarla, para que el bucket
     * sea coherente con la altura con la que la simulación proyecta sombra: si el render
     * tuviera su propia ley, un árbol podría verse de un tamaño y sombrear como otro.
     *
     * @see EcologyRules::HeightRatioFromBiomass
     */
    FORCEINLINE float HeightRatio(float Biomass, float MaxBiomass)
    {
        return EcologyRules::HeightRatioFromBiomass(Biomass, MaxBiomass);
    }

    /**
     * Borde superior del bucket, en fracción de altura adulta: @f$ (b+1)/N @f$.
     *
     * Es la talla a la que se hornea la malla del bucket, y por eso la escala de instancia
     * dentro del bucket nunca pasa de 1: la malla se encoge, jamás se estira, y los árboles
     * jóvenes no salen deformados por interpolar hacia arriba.
     */
    FORCEINLINE float BucketUpperRatio(int32 Bucket, int32 NumBuckets)
    {
        const int32 N = FMath::Max(1, NumBuckets);
        return static_cast<float>(FMath::Clamp(Bucket, 0, N - 1) + 1) / static_cast<float>(N);
    }

    /** Discretiza la fracción de altura adulta en un bucket: @f$ \mathrm{clamp}(\lfloor rN
        \rfloor, 0, N-1) @f$. */
    FORCEINLINE int32 BucketOf(float InHeightRatio, int32 NumBuckets)
    {
        const int32 N = FMath::Max(1, NumBuckets);
        return FMath::Clamp(FMath::FloorToInt32(InHeightRatio * N), 0, N - 1);
    }

    /**
     * Igual que BucketOf, pero con histéresis: cambiar de bucket exige rebasar el borde por
     * un margen, no solo tocarlo. Es un disparador de dos umbrales separados aplicado al
     * tamaño del árbol.
     *
     * Sin ese margen, un árbol detenido justo en el borde oscila entre dos buckets tick a
     * tick, y cada oscilación cuesta una baja y un alta en dos componentes instanciados
     * distintos. Con histéresis el cruce ocurre una sola vez.
     *
     * @param CurrentBucket Bucket con el que está dibujado ahora; negativo si aún no tiene.
     * @param Hysteresis    Margen en fracción de bucket; se recorta a [0, 0.49].
     * @return Bucket con el que dibujar, que puede seguir siendo CurrentBucket.
     * @see @ref bib_schmitt1938
     */
    FORCEINLINE int32 BucketWithHysteresis(float InHeightRatio, int32 CurrentBucket,
        int32 NumBuckets, float Hysteresis)
    {
        const int32 N = FMath::Max(1, NumBuckets);
        const int32 Want = BucketOf(InHeightRatio, N);
        if (CurrentBucket < 0 || Want == CurrentBucket)
        {
            return Want;
        }

        const float H = FMath::Clamp(Hysteresis, 0.f, 0.49f) / static_cast<float>(N);
        if (Want > CurrentBucket)
        {
            // Creciendo: exige superar el borde superior del bucket actual más el margen.
            return (InHeightRatio >= BucketUpperRatio(CurrentBucket, N) + H) ? Want : CurrentBucket;
        }

        // Encogiendo (posible si cae la biomasa): exige bajar del borde inferior menos el margen.
        const float LowerEdge = static_cast<float>(CurrentBucket) / static_cast<float>(N);
        return (InHeightRatio <= LowerEdge - H) ? Want : CurrentBucket;
    }

    /**
     * Escala de instancia dentro del bucket: da continuidad al crecimiento sin regenerar la
     * malla. Siempre en (0, 1], porque la malla está horneada al borde superior del bucket.
     *
     * Invariante que la sostiene:
     * @f[ h_{mundo} = Escala \cdot BucketUpperRatio \cdot MaxHeight = r \cdot MaxHeight @f]
     * es decir, el tamaño en pantalla es continuo al cruzar de bucket aunque la malla cambie
     * de golpe. Lo único que salta es la silueta.
     */
    FORCEINLINE float ScaleWithinBucket(float InHeightRatio, int32 Bucket, int32 NumBuckets)
    {
        return FMath::Clamp(InHeightRatio / BucketUpperRatio(Bucket, NumBuckets), 0.02f, 1.f);
    }

    /** Variante morfológica del árbol. Al depender solo de StableId no cambia nunca, y por
        eso la malla no parpadea entre variantes. */
    FORCEINLINE uint8 VariantOf(uint32 StableId, int32 NumVariants)
    {
        const int32 N = FMath::Clamp(NumVariants, 1, 255);
        return static_cast<uint8>(StableHash(StableId, SaltVariant) % static_cast<uint32>(N));
    }

    /** Giro estable del árbol alrededor del eje vertical, uniforme en [0, 360) grados. */
    FORCEINLINE float YawOf(uint32 StableId)
    {
        return StableUnit(StableId, SaltYaw) * 360.f;
    }

    /**
     * Factor multiplicativo de tamaño, estable y centrado en 1.
     * @param JitterAmount Semiamplitud relativa: 0.1 da el rango habitual 0.9–1.1.
     */
    FORCEINLINE float ScaleJitterOf(uint32 StableId, float JitterAmount)
    {
        return 1.f + JitterAmount * (2.f * StableUnit(StableId, SaltScale) - 1.f);
    }

    /**
     * Transformación con la que se da de alta la instancia: giro estable alrededor del eje
     * vertical, posición del árbol y escala uniforme (escala de bucket por jitter, con un
     * suelo que impide una instancia degenerada).
     */
    FORCEINLINE FTransform InstanceTransform(const FVector& Position, uint32 StableId,
        float ScaleInBucket, float JitterAmount)
    {
        const float S = FMath::Max(0.01f, ScaleInBucket * ScaleJitterOf(StableId, JitterAmount));
        return FTransform(FRotator(0.f, YawOf(StableId), 0.f), Position, FVector(S));
    }
}

/**
 * Contabilidad de índices de instancia.
 *
 * `UInstancedStaticMeshComponent::RemoveInstances` borra con semántica de `RemoveAt`: desplaza
 * hacia abajo todas las instancias de índice mayor que el borrado. Guardar un índice por árbol
 * y no volver a mapearlo tras una baja hace que, desde la primera muerte, los árboles muevan
 * la instancia de otro. Por eso cada componente mantiene un array instancia → StableId que se
 * compacta con esa misma semántica justo después del borrado por lotes.
 *
 * @see @ref bib_instancing
 */
namespace TreeInstancing
{
    /**
     * Compacta el mapeo instancia → árbol tras un borrado por lotes, reproduciendo el
     * desplazamiento que hace el componente, y notifica su nuevo índice a cada instancia
     * que se ha movido.
     *
     * @param InOutMapping   Instancia → StableId; se compacta en sitio.
     * @param RemovedIndices Índices borrados, en cualquier orden; los inválidos se ignoran.
     * @param OnRelocated    Invocado como (StableId, nuevo índice) por cada superviviente
     *                       que cambia de posición.
     * @pre El mapeo debe reflejar el estado del componente ANTES del borrado.
     */
    template <typename FnOnRelocated>
    void CompactMappingAfterRemoval(TArray<uint32>& InOutMapping,
        const TArray<int32>& RemovedIndices, FnOnRelocated&& OnRelocated)
    {
        if (RemovedIndices.Num() == 0 || InOutMapping.Num() == 0)
        {
            return;
        }

        TBitArray<> bRemoved(false, InOutMapping.Num());
        for (int32 Idx : RemovedIndices)
        {
            if (InOutMapping.IsValidIndex(Idx))
            {
                bRemoved[Idx] = true;
            }
        }

        int32 Write = 0;
        for (int32 Read = 0; Read < InOutMapping.Num(); ++Read)
        {
            if (bRemoved[Read])
            {
                continue;
            }
            if (Write != Read)
            {
                InOutMapping[Write] = InOutMapping[Read];
                OnRelocated(InOutMapping[Write], Write);
            }
            ++Write;
        }

        InOutMapping.SetNum(Write, EAllowShrinking::No);
    }
}
