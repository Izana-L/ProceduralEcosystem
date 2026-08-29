/**
 * @file TreeLibrary.cpp
 * @author Juan Luque Roldán
 * @brief Implementación de la librería: fábrica de parámetros, horneado y componentes.
 *
 * Contiene la fábrica de parámetros GetArchetypeSpecies, que duplica la especie base, la
 * re-escala al bucket, comprime la copa y recorta la complejidad de los arquetipos jóvenes con
 * curvas de potencia, les aplica el jitter estable de la variante y reimpone después las
 * invariantes de la colonización del espacio; el horneado de un arquetipo, generado en el
 * origen y sin luz gruesa porque es un árbol genérico que no sabe dónde acabará; la cola FIFO
 * con cursor que reparte ese coste entre frames; la creación de los componentes de instancing
 * con su viento y su cull por distancia; y el recuento agregado de la librería.
 *
 * @ingroup eco_render
 * @see @ref bib_runions2007
 * @see @ref bib_instancing
 */

#include "Render/TreeLibrary.h"
#include "Render/TreeMeshBaker.h"
#include "Render/TreeInstanceHost.h" // fábrica común de componentes de instancing

#include "Core/EcoStats.h" // contadores del grupo EcoRender
#include "Species/SpeciesData.h"
#include "Geometry/SpaceColonization.h"
#include "Geometry/TreeSkeleton.h"
#include "Geometry/TreeLightGridFine.h"
#include "Geometry/AttractorCloud.h"
#include "Geometry/TreeMeshBuilder.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

DEFINE_LOG_CATEGORY_STATIC(LogEcoLOD, Log, All);

// ---------------------------------------------------------------------------
//  Ciclo de vida
// ---------------------------------------------------------------------------
void UTreeLibrary::Initialize(AActor* InHost, const TArray<TObjectPtr<USpeciesData>>& InSpecies,
    const FTreeLibraryConfig& InConfig)
{
    Host = InHost;
    BaseSpecies = InSpecies;
    Config = InConfig;
    Config.NumAgeBuckets = FMath::Clamp(Config.NumAgeBuckets, 1, 255);
}

void UTreeLibrary::Shutdown()
{
    for (TPair<uint32, FTreeArchetypeEntry>& It : Entries)
    {
        if (It.Value.MeshISM) { It.Value.MeshISM->DestroyComponent(); }
        if (It.Value.ImpostorISM) { It.Value.ImpostorISM->DestroyComponent(); }
    }
    Entries.Reset();
    ArchetypeSpecies.Reset();
    BakeQueue.Reset();
    BakeQueueHead = 0;
    BakeQueued.Reset();
    Host = nullptr;
}

USpeciesData* UTreeLibrary::GetBaseSpecies(uint16 SpeciesId) const
{
    return BaseSpecies.IsValidIndex(SpeciesId) ? BaseSpecies[SpeciesId].Get() : nullptr;
}

// ---------------------------------------------------------------------------
//  Fábrica de parámetros: la especie tal como la ve un arquetipo
// ---------------------------------------------------------------------------
const USpeciesData* UTreeLibrary::GetArchetypeSpecies(const FArchetypeKey& Key)
{
    if (const TObjectPtr<USpeciesData>* Found = ArchetypeSpecies.Find(Key.Pack()))
    {
        return Found->Get();
    }

    USpeciesData* Base = GetBaseSpecies(Key.Species);
    if (!Base)
    {
        return nullptr;
    }

    USpeciesData* Sp = DuplicateObject<USpeciesData>(Base, this);
    if (!Sp)
    {
        return nullptr;
    }

    // S = talla del bucket como fracción del adulto, medida en su borde superior.
    const float S = TreeArchetype::BucketUpperRatio(Key.AgeBucket, Config.NumAgeBuckets);

    // Compresión adicional de la copa en los buckets jóvenes. Con la copa encogida y la
    // fracción de tronco subida, el bucket bajo es casi todo fuste con un penacho arriba: la
    // silueta de una plántula, no la de un adulto en miniatura. Las dos palancas se compensan
    // en altura total, porque TotalH = CrownHeight / (1 - TrunkFraction).
    const float CrownSquash = FMath::Lerp(0.30f, 1.f, S);

    // --- Longitudes: escalan todas con S ---
    // Escalarlas juntas preserva las dos invariantes que valida USpeciesData::IsDataValid:
    // d_k < D < d_i, y d_i mayor que el hueco de tronco.
    Sp->CrownRadiusCm = Base->CrownRadiusCm * S * CrownSquash;
    Sp->CrownHeightCm = Base->CrownHeightCm * S * CrownSquash;
    Sp->StepLengthD = Base->StepLengthD * S;
    Sp->InfluenceRadiusDi = Base->InfluenceRadiusDi * S;
    Sp->KillRadiusDk = Base->KillRadiusDk * S;
    Sp->TipRadiusCm = Base->TipRadiusCm * S;
    Sp->FineVoxelSizeCm = FMath::Max(5.f, Base->FineVoxelSizeCm * S);
    Sp->MaxHeightCm = Base->MaxHeightCm * S;
    Sp->TrunkFlareHeightCm = FMath::Max(1.f, Base->TrunkFlareHeightCm * S);

    // --- Perfil y relieve de tronco: no son invariantes de escala ---
    // Una plántula no tiene contrafuertes ni corteza acostillada: eso lo construye el árbol
    // con las décadas. Escalar el ensanche por S a secas dejaría a los buckets bajos con un pie
    // desproporcionado, que es peor que no tenerlo.
    Sp->TrunkFlareStrength = Base->TrunkFlareStrength * FMath::Lerp(0.25f, 1.f, S);

    // El relieve cae con S^2: por debajo del umbral del mallador se apaga solo, y con él se
    // apagan también los segmentos de anillo extra. Los buckets pequeños —los que se
    // instancian a millares— se quedan así en la malla barata.
    Sp->SectionLobeAmount = Base->SectionLobeAmount * S * S;
    Sp->BarkReliefAmount = Base->BarkReliefAmount * S * S;

    // Una plántula es casi todo eje con un penacho: el líder llega arriba del todo.
    Sp->LeaderFraction = FMath::Clamp(FMath::Lerp(0.9f, Base->LeaderFraction, S), 0.f, 1.f);

    // --- Complejidad: una plántula no es un adulto a escala ---
    // La colonización del espacio es invariante de escala, así que escalarlo todo por S daría
    // los N buckets idénticos salvo en tamaño y no habría ganado nada por hornear N. Bajando
    // iteraciones y atractores, los buckets pequeños tienen menos orden de ramificación —que
    // es lo que de verdad distingue a una plántula de un adulto— y además mallas más baratas:
    // la librería hace también de LOD por edad.
    //
    // Las curvas son de potencia y no lineales porque una interpolación lineal con S = 0.2
    // aún deja la mitad de los atractores en el bucket más pequeño, y con esa densidad la
    // copa sale igual de ramificada. El suelo de cada Max() queda por debajo de lo que aporta
    // la cadena de tronco desnudo, así que ningún arquetipo baja de los dos nodos que exige
    // el mallador.
    Sp->MaxIter = FMath::Max(4, FMath::RoundToInt(Base->MaxIter * FMath::Pow(S, 0.9f)));
    Sp->NumAttractors = FMath::Max(12, FMath::RoundToInt(Base->NumAttractors * FMath::Pow(S, 2.2f)));
    Sp->RingSegments = FMath::Clamp(FMath::RoundToInt(Base->RingSegments * FMath::Lerp(0.6f, 1.f, S)), 3, 16);
    Sp->TrunkFraction = FMath::Clamp(FMath::Lerp(0.62f, Base->TrunkFraction, S), 0.f, 0.95f);
    // Hoja relativamente mayor de joven, y por tanto también más junta.
    Sp->LeafSizeCm = Base->LeafSizeCm * FMath::Lerp(0.55f, 1.f, S);
    Sp->LeafSpacingCm = Base->LeafSpacingCm * FMath::Lerp(0.55f, 1.f, S);

    // --- Un árbol joven es más flexible que uno adulto ---
    // Un tronco de 2 m se dobla con el viento; uno de 20 m con 60 cm de diámetro apenas. Como
    // la rigidez se hornea en los canales UV de la malla, basta modularla aquí por bucket y
    // cada arquetipo sale con el balanceo que le toca por su tamaño, sin coste en runtime.
    Sp->WindStiffness = FMath::Clamp(Base->WindStiffness * FMath::Lerp(0.45f, 1.f, S), 0.f, 1.f);

    // --- Variante: perturbación pequeña y estable de la morfología ---
    // La semilla ignora el bucket. Si entrase, un árbol cambiaría de radio de copa, de altura
    // y de tropismos cada vez que cruza de bucket: no ganaría estructura con la edad, se
    // convertiría en otro árbol. Atada solo a (especie, variante), los buckets son etapas de
    // la misma morfología.
    uint32 VRng = EcoRand::Hash32(FArchetypeKey(Key.Species, 0, Key.Variant).Pack() * 0x9E3779B9u + 0x51ED270Bu);
    auto Jitter = [&VRng](float Value, float Amount)
        {
            return Value * (1.f + Amount * (2.f * EcoRand::NextUnit(VRng) - 1.f));
        };
    Sp->CrownRadiusCm = Jitter(Sp->CrownRadiusCm, 0.15f);
    Sp->CrownHeightCm = Jitter(Sp->CrownHeightCm, 0.12f);
    Sp->wGrav = Jitter(Sp->wGrav, 0.25f);
    Sp->wSCA = Jitter(Sp->wSCA, 0.15f);
    Sp->wPrev = Jitter(Sp->wPrev, 0.20f);

    // Sinuosidad base del fuste, también por variante. El orden de las tiradas de VRng es
    // parte del contrato: insertar una llamada en medio desplazaría las siguientes y
    // cambiaría la copa de todos los arquetipos ya calibrados, así que las nuevas van al
    // final.
    //
    // Complementa a las capas de TrunkDeformLayers: aquéllas deciden arqueado o recto, un
    // rasgo binario por árbol, y esto reparte la ondulación sutil que llevan todos, de modo
    // que los rectos tampoco son iguales entre sí.
    Sp->TrunkSweepDeg = Jitter(Sp->TrunkSweepDeg, 0.30f);

    // --- Red de seguridad: reimponer las invariantes del algoritmo tras el jitter ---
    // Sin esto, una variante desafortunada puede dar d_i menor o igual que el hueco de
    // tronco; entonces ningún atractor cae en rango del nodo base, el crecimiento no arranca
    // y el arquetipo sale vacío.
    Sp->KillRadiusDk = FMath::Clamp(Sp->KillRadiusDk, 0.1f, Sp->StepLengthD * 0.9f);
    Sp->InfluenceRadiusDi = FMath::Max(Sp->InfluenceRadiusDi, Sp->StepLengthD * 1.2f);
    if (Sp->SubCrownFraction <= 0.f)
    {
        // Sin falda de sub-copa, el atractor más bajo está en la base de la copa y el nodo
        // raíz tiene que alcanzarlo. Con falda hay atractores repartidos por el fuste y esta
        // red de seguridad sobra: estirar d_i de más empeora el cono de percepción, porque
        // cada nodo pasa a competir por atractores mucho más lejanos.
        const float TF = FMath::Clamp(Sp->TrunkFraction, 0.f, 0.95f);
        const float TrunkGapCm = Sp->CrownHeightCm * TF / (1.f - TF);
        Sp->InfluenceRadiusDi = FMath::Max(Sp->InfluenceRadiusDi, TrunkGapCm * 1.1f + Sp->StepLengthD);
    }

    ArchetypeSpecies.Add(Key.Pack(), Sp);
    return Sp;
}

// ---------------------------------------------------------------------------
//  Horneado de arquetipos y cola amortizada
// ---------------------------------------------------------------------------
FTreeArchetypeEntry* UTreeLibrary::Find(const FArchetypeKey& Key)
{
    FTreeArchetypeEntry* E = Entries.Find(Key.Pack());
    return (E && E->bBaked) ? E : nullptr;
}

FTreeArchetypeEntry* UTreeLibrary::FindOrRequestBake(const FArchetypeKey& Key)
{
    if (FTreeArchetypeEntry* E = Find(Key))
    {
        return E;
    }

    const uint32 Packed = Key.Pack();
    if (!BakeQueued.Contains(Packed))
    {
        BakeQueued.Add(Packed);
        BakeQueue.Add(Packed);
    }
    return nullptr;
}

int32 UTreeLibrary::ProcessBakeQueue(int32 MaxThisFrame)
{
    // FIFO con cursor de lectura: avanzar BakeQueueHead evita el RemoveAt(0), que desplazaría
    // todos los elementos restantes en cada extracción.
    int32 Done = 0;
    while (BakeQueueHead < BakeQueue.Num() && Done < FMath::Max(1, MaxThisFrame))
    {
        const uint32 Packed = BakeQueue[BakeQueueHead++];
        BakeQueued.Remove(Packed);

        if (BakeArchetype(FArchetypeKey::Unpack(Packed)))
        {
            ++Done;
        }
    }

    // Cola drenada: compactar de una vez, conservando la capacidad.
    if (BakeQueueHead >= BakeQueue.Num())
    {
        BakeQueue.Reset();
        BakeQueueHead = 0;
    }
    return Done;
}

int32 UTreeLibrary::BakeAll()
{
    const double T0 = FPlatformTime::Seconds();
    int32 Baked = 0;

    for (int32 SpeciesIdx = 0; SpeciesIdx < BaseSpecies.Num(); ++SpeciesIdx)
    {
        const USpeciesData* Base = BaseSpecies[SpeciesIdx].Get();
        if (!Base) { continue; }

        const int32 NumVariants = FMath::Clamp(Base->NumLodVariants, 1, 255);
        for (int32 Bucket = 0; Bucket < Config.NumAgeBuckets; ++Bucket)
        {
            for (int32 Variant = 0; Variant < NumVariants; ++Variant)
            {
                const FArchetypeKey Key(static_cast<uint16>(SpeciesIdx),
                    static_cast<uint8>(Bucket), static_cast<uint8>(Variant));
                if (!Find(Key) && BakeArchetype(Key))
                {
                    ++Baked;
                }
            }
        }
    }

    BakeQueue.Reset();
    BakeQueueHead = 0;
    BakeQueued.Reset();

    UE_LOG(LogEcoLOD, Log, TEXT("[Eco/LOD] Libreria horneada: %d arquetipos en %.0f ms."),
        Baked, (FPlatformTime::Seconds() - T0) * 1000.0);
    return Baked;
}

bool UTreeLibrary::BakeArchetype(const FArchetypeKey& Key)
{
    // El horneado es el pico de coste puntual del hilo de juego. Instrumentarlo lo hace
    // aparecer como bloque propio en el perfilado y en `stat EcoRender`, que es donde se ve
    // si el presupuesto de horneados por frame está bien puesto.
    SCOPE_CYCLE_COUNTER(STAT_EcoBake);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_BakeArchetype);

    const USpeciesData* Base = GetBaseSpecies(Key.Species);
    const USpeciesData* Sp = GetArchetypeSpecies(Key);
    if (!Base || !Sp)
    {
        return false;
    }

    // Semilla fija por arquetipo: la librería sale idéntica en cada arranque.
    uint32 Rng = ArchetypeSeed(Key);

    // El árbol de librería se genera en el origen y sin luz gruesa: es un árbol genérico que
    // no sabe dónde acabará. Recibir el contexto real de vecinos es justo lo que distingue a
    // un hero tree de una instancia.
    FTreeSkeleton Skeleton;
    FTreeLightGridFine FineLight;
    FAttractorCloud Attractors;

    FSpaceColonizationConfig Cfg;
    // La curvatura del tronco se ata a (especie, variante) ignorando el bucket: los buckets
    // de una variante son las etapas de un mismo árbol, y sin esto un individuo pasaría de
    // recto a arqueado al crecer. Es el mismo motivo por el que el jitter de morfología de
    // GetArchetypeSpecies tampoco mira el bucket.
    Cfg.DeformSeedOverride = static_cast<int64>(VariantDeformSeed(Key.Species, Key.Variant));

    SpaceColonization::GrowTree(*Sp, Rng, FVector::ZeroVector, /*CoarseLight*/ nullptr, Cfg,
        Skeleton, FineLight, Attractors);

    // Aunque el arquetipo no conozca a sus vecinos, sí conoce su propia autosombra —la
    // rejilla fina que dejó el crecimiento—, así que la oclusión ambiental de copa por
    // vértice se hornea igual. Es la parte de la oclusión que no depende del sitio: el
    // interior de la copa está oscuro en cualquier árbol. La que sí depende del sitio, estar
    // bajo el dosel de un vecino, viaja por instancia en PerInstanceCustomData[2].
    FTreeMeshData MeshData;
    TreeMeshBuilder::BuildMesh(Skeleton, *Sp, Rng, MeshData, &FineLight);

    FBox LocalBounds;
    UStaticMesh* Mesh = TreeMeshBaker::BuildStaticMesh(this, MeshData, FVector::ZeroVector,
        Base->BarkMaterial, Base->LeafMaterial, LocalBounds);

    if (!Mesh)
    {
        UE_LOG(LogEcoLOD, Warning, TEXT("[Eco/LOD] Arquetipo %s: el SCA no produjo geometria (revisa d_k < D < d_i y NumAttractors)."),
            *Key.ToString());
        return false;
    }

    FTreeArchetypeEntry& Entry = Entries.FindOrAdd(Key.Pack());
    Entry.Mesh = Mesh;
    Entry.BakedHeightCm = LocalBounds.IsValid ? static_cast<float>(LocalBounds.Max.Z) : 0.f;
    Entry.WoodTriangles = MeshData.Wood.Triangles.Num() / 3;
    Entry.LeafTriangles = MeshData.Leaves.Triangles.Num() / 3;
    Entry.ImpostorMesh = TreeMeshBaker::BuildImpostorMesh(this, LocalBounds,
        Base->ImpostorMaterial ? Base->ImpostorMaterial.Get() : Base->LeafMaterial.Get());
    Entry.bBaked = true;

    UE_LOG(LogEcoLOD, Verbose, TEXT("[Eco/LOD] Arquetipo %s horneado: %d nodos, %d tri madera + %d tri hoja, alto %.0f cm."),
        *Key.ToString(), Skeleton.Num(), Entry.WoodTriangles, Entry.LeafTriangles, Entry.BakedHeightCm);

    return true;
}

// ---------------------------------------------------------------------------
//  Componentes de instancing
// ---------------------------------------------------------------------------

/**
 * Ajustes de viento de un componente. Las tres llamadas acotan el coste de mover vértices en
 * el material:
 *
 * @li `SetEvaluateWorldPositionOffset` es el interruptor duro: con false el componente ni
 *     siquiera ejecuta la parte de desplazamiento del vertex shader, y es lo que deja los
 *     impostores del campo lejano completamente estáticos.
 * @li `SetWorldPositionOffsetDisableDistance` corta el balanceo dentro del propio componente
 *     más allá del radio dado, de modo que solo se mueven los árboles cercanos.
 * @li `SetBoundsScale` compensa que el culling trabaja con la caja envolvente sin mover; sin
 *     margen, un árbol en el borde del encuadre parpadea.
 *
 * @note `SetWorldPositionOffsetDisableDistance` existe desde UE 5.1. Contra una versión
 *       anterior, esa línea sobra y solo se pierde el corte por distancia.
 * @see @ref bib_vientovegetacion
 */
void UTreeLibrary::ConfigureWind(UHierarchicalInstancedStaticMeshComponent* Comp, bool bImpostor) const
{
    if (!Comp) { return; }

    const bool bWind = bImpostor ? Config.bWindOnImpostors : Config.bWindOnInstances;

    Comp->SetEvaluateWorldPositionOffset(bWind);
    Comp->SetWorldPositionOffsetDisableDistance(
        (bWind && Config.WindWpoCutoffCm > 0.f) ? FMath::RoundToInt(Config.WindWpoCutoffCm) : 0);
    Comp->SetBoundsScale(bWind ? FMath::Max(1.f, Config.WindBoundsScale) : 1.f);
}

void UTreeLibrary::ApplyWindSettings(const FTreeLibraryConfig& InConfig)
{
    Config.bWindOnInstances = InConfig.bWindOnInstances;
    Config.bWindOnImpostors = InConfig.bWindOnImpostors;
    Config.WindWpoCutoffCm = InConfig.WindWpoCutoffCm;
    Config.WindBoundsScale = InConfig.WindBoundsScale;

    for (TPair<uint32, FTreeArchetypeEntry>& It : Entries)
    {
        ConfigureWind(It.Value.MeshISM.Get(), /*bImpostor*/ false);
        ConfigureWind(It.Value.ImpostorISM.Get(), /*bImpostor*/ true);
    }
}

UHierarchicalInstancedStaticMeshComponent* UTreeLibrary::GetOrCreateComponent(const FArchetypeKey& Key, bool bImpostor)
{
    FTreeArchetypeEntry* Entry = Find(Key);
    if (!Entry || !Host)
    {
        return nullptr;
    }

    TObjectPtr<UHierarchicalInstancedStaticMeshComponent>& Slot = bImpostor ? Entry->ImpostorISM : Entry->MeshISM;
    if (Slot)
    {
        return Slot.Get();
    }

    UStaticMesh* Mesh = bImpostor ? Entry->ImpostorMesh.Get() : Entry->Mesh.Get();
    if (!Mesh)
    {
        return nullptr;
    }

    // Configuración común a todo componente de instancing del proyecto —movilidad, colisión,
    // navegación y sombra—, compartida con la capa de suelo.
    const FName CompName(*FString::Printf(TEXT("ISM_%s%s"),
        *Key.ToString(), bImpostor ? TEXT("_Imp") : TEXT("")));
    UHierarchicalInstancedStaticMeshComponent* Comp = ATreeInstanceHost::CreateInstancedComponent(
        Host, Mesh, CompName,
        bImpostor ? Config.bImpostorsCastShadow : Config.bInstancesCastShadow,
        Config.NumInstanceCustomDataFloats);
    if (!Comp)
    {
        return nullptr;
    }

    // Y lo específico de los árboles: viento, margen de la caja envolvente y cull.
    ConfigureWind(Comp, bImpostor);

    const float EndCull = bImpostor ? Config.ImpostorEndCullDistanceCm : Config.InstanceEndCullDistanceCm;
    if (EndCull > 0.f)
    {
        // El desvanecido del componente es una red de seguridad estrecha justo antes del
        // corte, no un degradado repartido por todo el rango: con la distancia de inicio a 0
        // las instancias se difuminan desde el observador y el bosque ralea. Arrancarlo en el
        // 90 % del cull evita además que compita con la conmutación de nivel.
        const int32 End = FMath::RoundToInt(EndCull);
        const int32 Start = FMath::RoundToInt(EndCull * 0.9f);
        Comp->SetCullDistances(Start, End);
    }

    Slot = Comp;
    return Comp;
}

void UTreeLibrary::ClearAllInstances()
{
    for (TPair<uint32, FTreeArchetypeEntry>& It : Entries)
    {
        FTreeArchetypeEntry& E = It.Value;
        if (E.MeshISM) { E.MeshISM->ClearInstances(); }
        if (E.ImpostorISM) { E.ImpostorISM->ClearInstances(); }
        E.MeshMapping.Reset();
        E.ImpostorMapping.Reset();
    }
}

void UTreeLibrary::GetStats(int32& OutMeshes, int32& OutComponents, int32& OutInstances, int32& OutTriangles) const
{
    OutMeshes = 0; OutComponents = 0; OutInstances = 0; OutTriangles = 0;

    for (const TPair<uint32, FTreeArchetypeEntry>& It : Entries)
    {
        const FTreeArchetypeEntry& E = It.Value;
        if (!E.bBaked) { continue; }

        ++OutMeshes;
        OutTriangles += E.WoodTriangles + E.LeafTriangles;

        if (E.MeshISM) { ++OutComponents; OutInstances += E.MeshISM->GetInstanceCount(); }
        if (E.ImpostorISM) { ++OutComponents; OutInstances += E.ImpostorISM->GetInstanceCount(); }
    }
}
