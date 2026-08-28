#pragma once

#include "CoreMinimal.h"
#include "Core/EcoCore.h"
#include "Ecology/EcologyRules.h" // HeightRatioFromBiomass: alometria compartida con el tick

/**
 * Clave de ARQUETIPO (doc. Fase 4, 4.2).
 *
 * No se pueden hornear 20.000 mallas unicas ni generarlas al vuelo, asi que se
 * discretiza el espacio continuo (especie, tamano) en un punado de arquetipos:
 * especie x bucket de edad/tamano x variante morfologica. Con 3 especies x 5
 * buckets x 8 variantes son 120 mallas, cada una horneada UNA vez por la
 * Fase 3 con semilla fija.
 *
 * El truco para que 120 mallas parezcan 20.000 arboles distintos es que la
 * variedad continua NO esta en la malla, sino en la transformacion de
 * instancia (variante estable + escala interpolada dentro del bucket + yaw +
 * jitter), todo derivado de un valor estable por-arbol.
 *
 * Struct PLANO de 4 bytes utiles: se empaqueta en un uint32 que hace de clave
 * de TMap y de identificador legible en logs.
 */
struct FArchetypeKey
{
    uint16 Species = 0;   // indice en UEcosystemSettings::Species
    uint8  AgeBucket = 0; // bucket de tamano [0, NumAgeBuckets)
    uint8  Variant = 0;   // variante morfologica [0, NumLodVariants)

    FArchetypeKey() = default;
    FArchetypeKey(uint16 InSpecies, uint8 InAgeBucket, uint8 InVariant)
        : Species(InSpecies), AgeBucket(InAgeBucket), Variant(InVariant) {}

    FORCEINLINE uint32 Pack() const
    {
        return (static_cast<uint32>(Species) << 16)
             | (static_cast<uint32>(AgeBucket) << 8)
             |  static_cast<uint32>(Variant);
    }

    static FORCEINLINE FArchetypeKey Unpack(uint32 Packed)
    {
        return FArchetypeKey(
            static_cast<uint16>(Packed >> 16),
            static_cast<uint8>((Packed >> 8) & 0xFFu),
            static_cast<uint8>(Packed & 0xFFu));
    }

    FORCEINLINE bool operator==(const FArchetypeKey& Other) const { return Pack() == Other.Pack(); }
    FORCEINLINE bool operator!=(const FArchetypeKey& Other) const { return Pack() != Other.Pack(); }

    FString ToString() const
    {
        return FString::Printf(TEXT("Sp%u/B%u/V%u"), (uint32)Species, (uint32)AgeBucket, (uint32)Variant);
    }
};

FORCEINLINE uint32 GetTypeHash(const FArchetypeKey& Key) { return ::GetTypeHash(Key.Pack()); }

/**
 * Funciones PURAS de la capa de render (doc. 4.2). Igual que EcologyRules en la
 * Fase 2: solo reciben y devuelven valores simples, no tocan UObjects ni
 * componentes, y por eso se pueden testear sueltas (ver Eco.LOD.* en EcoTests).
 *
 * DETERMINISMO (doc. 0, "Determinismo por-arbol"): variante, yaw y jitter se
 * derivan por hash de un identificador ESTABLE del arbol
 * (FTreePopulation::StableId), NO de FTreePopulation::RngState.
 *
 *   OJO, desviacion consciente del pseudocodigo del documento: alli se escribe
 *   variant = Hash(Pop.RngState[i]) % NVariants asumiendo que RngState es la
 *   semilla inmutable del arbol. En nuestra Fase 2, RngState es el ESTADO VIVO
 *   del stream (avanza en cada mortalidad/semilla de cada tick), asi que
 *   hashearlo haria que el arbol cambiase de variante y de yaw en cada tick:
 *   parpadeo de malla y rotacion. StableId se asigna una vez al nacer y no
 *   cambia nunca -> misma variante toda la vida del arbol.
 */
namespace TreeArchetype
{
    // Sales de hash: un mismo StableId debe dar flujos INDEPENDIENTES para
    // variante, giro, escala y fase (esta ultima la usara la Fase 5 para
    // desincronizar el cambio estacional por instancia).
    enum : uint32
    {
        SaltVariant = 0x9E3779B9u,
        SaltYaw     = 0x85EBCA6Bu,
        SaltScale   = 0xC2B2AE35u,
        SaltPhase   = 0x27D4EB2Fu
    };

    FORCEINLINE uint32 StableHash(uint32 StableId, uint32 Salt)
    {
        return EcoRand::Hash32(StableId ^ Salt);
    }

    /** Valor estable en [0,1) derivado del id del arbol. Es EcoRand::HashUnit
        (copia unica del idioma hash -> [0,1)); aqui solo se le pone el nombre
        del dominio de render. */
    FORCEINLINE float StableUnit(uint32 StableId, uint32 Salt)
    {
        return EcoRand::HashUnit(StableId, Salt);
    }

    /**
     * Fraccion de altura adulta que tiene el arbol, en [0,1]. DELEGA en la
     * alometria unica de EcologyRules (raiz cubica de la biomasa) para que el
     * bucket sea coherente con la altura que la Fase 2 usa en el grid de luz:
     * si no, un arbol podria "verse" de un tamano y sombrear como otro.
     */
    FORCEINLINE float HeightRatio(float Biomass, float MaxBiomass)
    {
        return EcologyRules::HeightRatioFromBiomass(Biomass, MaxBiomass);
    }

    /**
     * Borde SUPERIOR del bucket (fraccion de altura adulta). Es la referencia a
     * la que se hornea la malla del bucket: asi la escala de instancia dentro
     * del bucket es siempre <= 1 (la malla se encoge, nunca se estira) y los
     * arboles jovenes no salen "estirados".
     */
    FORCEINLINE float BucketUpperRatio(int32 Bucket, int32 NumBuckets)
    {
        const int32 N = FMath::Max(1, NumBuckets);
        return static_cast<float>(FMath::Clamp(Bucket, 0, N - 1) + 1) / static_cast<float>(N);
    }

    /** Discretiza el tamano continuo en un bucket. */
    FORCEINLINE int32 BucketOf(float InHeightRatio, int32 NumBuckets)
    {
        const int32 N = FMath::Max(1, NumBuckets);
        return FMath::Clamp(FMath::FloorToInt32(InHeightRatio * N), 0, N - 1);
    }

    /**
     * Igual que BucketOf pero con HISTERESIS: para cambiar de bucket hay que
     * rebasar el borde por Hysteresis (en fraccion de bucket).
     *
     * POR QUE: un arbol parado justo en el borde oscila entre dos buckets tick a
     * tick. Cada oscilacion es un RemoveInstance + AddInstance en dos HISM
     * distintos, que es EXACTAMENTE el trap de rendimiento del doc. 4.4 y el
     * riesgo (Alto) del Apendice B. Con histeresis el cruce ocurre una vez.
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
            // Creciendo: exige superar el borde superior del bucket actual + H.
            return (InHeightRatio >= BucketUpperRatio(CurrentBucket, N) + H) ? Want : CurrentBucket;
        }

        // Encogiendo (posible si baja la biomasa): exige bajar del borde inferior - H.
        const float LowerEdge = static_cast<float>(CurrentBucket) / static_cast<float>(N);
        return (InHeightRatio <= LowerEdge - H) ? Want : CurrentBucket;
    }

    /**
     * Escala de instancia DENTRO del bucket: da continuidad al crecimiento sin
     * regenerar la malla (doc. 4.2 "escala que interpola dentro del bucket").
     *
     * Invariante util: altura_en_mundo = Escala * BucketUpperRatio * MaxHeight
     *                                  = HeightRatio * MaxHeight,
     * o sea, el tamano en pantalla es CONTINUO al cruzar de bucket aunque la
     * malla cambie de golpe. Lo unico que salta es la silueta, y eso es lo que
     * disimula la transicion con dither (doc. 4.4).
     */
    FORCEINLINE float ScaleWithinBucket(float InHeightRatio, int32 Bucket, int32 NumBuckets)
    {
        return FMath::Clamp(InHeightRatio / BucketUpperRatio(Bucket, NumBuckets), 0.02f, 1.f);
    }

    /** Variante ESTABLE por-arbol (no cambia nunca -> no hay parpadeo de malla). */
    FORCEINLINE uint8 VariantOf(uint32 StableId, int32 NumVariants)
    {
        const int32 N = FMath::Clamp(NumVariants, 1, 255);
        return static_cast<uint8>(StableHash(StableId, SaltVariant) % static_cast<uint32>(N));
    }

    /** Giro en yaw estable, en grados. */
    FORCEINLINE float YawOf(uint32 StableId)
    {
        return StableUnit(StableId, SaltYaw) * 360.f;
    }

    /** Jitter multiplicativo de tamano, estable (Apendice A: 0.9-1.1). */
    FORCEINLINE float ScaleJitterOf(uint32 StableId, float JitterAmount)
    {
        return 1.f + JitterAmount * (2.f * StableUnit(StableId, SaltScale) - 1.f);
    }

    /** Transformacion final de la instancia (doc. 4.2, InstanceXform). */
    FORCEINLINE FTransform InstanceTransform(const FVector& Position, uint32 StableId,
        float ScaleInBucket, float JitterAmount)
    {
        const float S = FMath::Max(0.01f, ScaleInBucket * ScaleJitterOf(StableId, JitterAmount));
        return FTransform(FRotator(0.f, YawOf(StableId), 0.f), Position, FVector(S));
    }
}

/**
 * Utilidades de bookkeeping del instancing (doc. 4.4).
 *
 * CAVEAT REAL DE LA API que el pseudocodigo del documento no cubre:
 * UInstancedStaticMeshComponent::RemoveInstances() borra con RemoveAt, o sea
 * DESPLAZA hacia abajo todas las instancias con indice mayor que el borrado.
 * Guardar un InstanceId por arbol y no re-mapear tras una baja hace que, a
 * partir de la primera muerte, los arboles muevan la instancia de OTRO arbol.
 * Por eso mantenemos, por componente, un array instancia -> StableId y lo
 * compactamos con la misma semantica justo despues del borrado por lotes.
 */
namespace TreeInstancing
{
    /**
     * Compacta el mapeo instancia->arbol despues de un RemoveInstances por lote,
     * reproduciendo el desplazamiento que hace el componente, y notifica el
     * nuevo indice de cada instancia que se ha movido.
     *
     * @param InOutMapping     instancia -> StableId (se compacta en sitio).
     * @param RemovedIndices   indices borrados (en cualquier orden).
     * @param OnRelocated      callback(StableId, NuevoIndice) por cada superviviente que se mueve.
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
