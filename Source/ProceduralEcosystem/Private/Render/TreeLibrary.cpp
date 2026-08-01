#include "Render/TreeLibrary.h"
#include "Render/TreeMeshBaker.h"

#include "Core/EcoStats.h" // Fase 6: instrumentacion (stat EcoRender / Insights)
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
    BakeQueued.Reset();
    Host = nullptr;
}

const USpeciesData* UTreeLibrary::GetBaseSpecies(uint16 SpeciesId) const
{
    return BaseSpecies.IsValidIndex(SpeciesId) ? BaseSpecies[SpeciesId].Get() : nullptr;
}

// ---------------------------------------------------------------------------
//  Fabrica de parametros: la especie "vista" por un arquetipo
// ---------------------------------------------------------------------------
const USpeciesData* UTreeLibrary::GetArchetypeSpecies(const FArchetypeKey& Key)
{
    if (const TObjectPtr<USpeciesData>* Found = ArchetypeSpecies.Find(Key.Pack()))
    {
        return Found->Get();
    }

    const USpeciesData* Base = GetBaseSpecies(Key.Species);
    if (!Base)
    {
        return nullptr;
    }

    USpeciesData* Sp = DuplicateObject<USpeciesData>(const_cast<USpeciesData*>(Base), this);
    if (!Sp)
    {
        return nullptr;
    }

    // S = tamano del bucket como fraccion del adulto (borde superior del bucket).
    const float S = TreeArchetype::BucketUpperRatio(Key.AgeBucket, Config.NumAgeBuckets);

    // --- Longitudes: escalan TODAS con S ---
    // Escalarlas juntas preserva las dos invariantes que valida
    // USpeciesData::IsDataValid: d_k < D < d_i y d_i > hueco de tronco.
    Sp->CrownRadiusCm = Base->CrownRadiusCm * S;
    Sp->CrownHeightCm = Base->CrownHeightCm * S;
    Sp->StepLengthD = Base->StepLengthD * S;
    Sp->InfluenceRadiusDi = Base->InfluenceRadiusDi * S;
    Sp->KillRadiusDk = Base->KillRadiusDk * S;
    Sp->TipRadiusCm = Base->TipRadiusCm * S;
    Sp->FineVoxelSizeCm = FMath::Max(5.f, Base->FineVoxelSizeCm * S);
    Sp->MaxHeightCm = Base->MaxHeightCm * S;

    // --- Complejidad: un plantón NO es un adulto a escala ---
    // Si escalasemos TODO por S, el SCA es invariante de escala y las 5 mallas
    // del bucket saldrian identicas salvo tamano: no habria ganado nada por
    // hornear 5. Reduciendo iteraciones y atractores, los buckets pequenos
    // tienen menos orden de ramificacion (que es lo que de verdad distingue a un
    // plantón de un adulto) y ademas mallas mas baratas: la libreria hace
    // tambien de LOD por edad.
    Sp->MaxIter = FMath::Max(6, FMath::RoundToInt(Base->MaxIter * FMath::Lerp(0.35f, 1.f, S)));
    Sp->NumAttractors = FMath::Max(24, FMath::RoundToInt(Base->NumAttractors * FMath::Lerp(0.15f, 1.f, S)));
    Sp->RingSegments = FMath::Clamp(FMath::RoundToInt(Base->RingSegments * FMath::Lerp(0.6f, 1.f, S)), 3, 16);
    Sp->TrunkFraction = Base->TrunkFraction * FMath::Lerp(0.35f, 1.f, S); // copa mas baja de joven
    Sp->LeafSizeCm = Base->LeafSizeCm * FMath::Lerp(0.55f, 1.f, S);       // hoja relativamente MAYOR de joven

    // --- Fase 6: un arbol joven es MAS flexible que uno adulto ---
    // Un tronco de 2 m se dobla con el viento; uno de 20 m con 60 cm de diametro
    // apenas. Como la rigidez se hornea en los canales UV de la malla (ver
    // TreeWindData.h), basta con modularla aqui por bucket y cada arquetipo sale
    // ya con el balanceo que le toca por su tamano, sin coste en runtime.
    Sp->WindStiffness = FMath::Clamp(Base->WindStiffness * FMath::Lerp(0.45f, 1.f, S), 0.f, 1.f);

    // --- Variante: perturbacion pequena y ESTABLE de la morfologia ---
    // (doc. 4.2: "variacion parametrizada ... para que no haya dos identicos").
    uint32 VRng = EcoRand::Hash32(Key.Pack() * 0x9E3779B9u + 0x51ED270Bu);
    auto Jitter = [&VRng](float Value, float Amount)
        {
            return Value * (1.f + Amount * (2.f * EcoRand::NextUnit(VRng) - 1.f));
        };
    Sp->CrownRadiusCm = Jitter(Sp->CrownRadiusCm, 0.15f);
    Sp->CrownHeightCm = Jitter(Sp->CrownHeightCm, 0.12f);
    Sp->wGrav = Jitter(Sp->wGrav, 0.25f);
    Sp->wSCA = Jitter(Sp->wSCA, 0.15f);
    Sp->wPrev = Jitter(Sp->wPrev, 0.20f);

    // --- Red de seguridad: reimponer las invariantes del SCA tras el jitter ---
    // Sin esto, una variante desafortunada puede dar d_i <= hueco de tronco y el
    // SCA no arranca (ningun atractor en rango del nodo base): arquetipo vacio.
    Sp->KillRadiusDk = FMath::Clamp(Sp->KillRadiusDk, 0.1f, Sp->StepLengthD * 0.9f);
    Sp->InfluenceRadiusDi = FMath::Max(Sp->InfluenceRadiusDi, Sp->StepLengthD * 1.2f);
    {
        const float TF = FMath::Clamp(Sp->TrunkFraction, 0.f, 0.95f);
        const float TrunkGapCm = Sp->CrownHeightCm * TF / (1.f - TF);
        Sp->InfluenceRadiusDi = FMath::Max(Sp->InfluenceRadiusDi, TrunkGapCm * 1.1f + Sp->StepLengthD);
    }

    ArchetypeSpecies.Add(Key.Pack(), Sp);
    return Sp;
}

// ---------------------------------------------------------------------------
//  Horneado
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
    int32 Done = 0;
    while (BakeQueue.Num() > 0 && Done < FMath::Max(1, MaxThisFrame))
    {
        const uint32 Packed = BakeQueue[0];
        BakeQueue.RemoveAt(0, 1, EAllowShrinking::No);
        BakeQueued.Remove(Packed);

        if (BakeArchetype(FArchetypeKey::Unpack(Packed)))
        {
            ++Done;
        }
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
    BakeQueued.Reset();

    UE_LOG(LogEcoLOD, Log, TEXT("[Eco/LOD] Libreria horneada: %d arquetipos en %.0f ms."),
        Baked, (FPlatformTime::Seconds() - T0) * 1000.0);
    return Baked;
}

bool UTreeLibrary::BakeArchetype(const FArchetypeKey& Key)
{
    // Fase 6 (6.4): el horneado es EL pico de coste puntual del game thread.
    // Marcarlo hace que salga como bloque propio en Unreal Insights y en
    // `stat EcoRender`, que es donde se ve si MaxBakesPerFrame esta bien puesto.
    SCOPE_CYCLE_COUNTER(STAT_EcoBake);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_BakeArchetype);

    const USpeciesData* Base = GetBaseSpecies(Key.Species);
    const USpeciesData* Sp = GetArchetypeSpecies(Key);
    if (!Base || !Sp)
    {
        return false;
    }

    // Semilla FIJA por arquetipo: la libreria es la misma en cada arranque
    // (doc. 3.8: "para la libreria se generan K variantes con K semillas").
    uint32 Rng = ArchetypeSeed(Key);

    // El arbol de libreria se genera en el ORIGEN y SIN luz gruesa: es un arbol
    // generico, no sabe donde acabara. El contexto real de vecinos (micro<-macro,
    // doc. 3.5) es justo lo que distingue a un hero tree de una instancia.
    FTreeSkeleton Skeleton;
    FTreeLightGridFine FineLight;
    FAttractorCloud Attractors;
    const FSpaceColonizationConfig Cfg;

    SpaceColonization::GrowTree(*Sp, Rng, FVector::ZeroVector, /*CoarseLight*/ nullptr, Cfg,
        Skeleton, FineLight, Attractors);

    // Fase 6 (6.2): aunque el arquetipo no conozca a sus vecinos, SI conoce su
    // propia autosombra -la rejilla fina que dejo el SCA-, asi que el AO de copa
    // por vertice se hornea igual. Es la parte del AO que no depende del sitio:
    // el interior de la copa esta oscuro en cualquier arbol. La parte que SI
    // depende del sitio (estar bajo el dosel de un vecino) viaja aparte, por
    // instancia, en PerInstanceCustomData[2].
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
 * Fase 6 (doc. 6.1): ajustes de viento de UN componente.
 *
 * Las tres llamadas son el caveat de rendimiento del documento hecho codigo:
 *
 *   - SetEvaluateWorldPositionOffset: el interruptor duro. Con false el
 *     componente ni siquiera ejecuta la parte de WPO del vertex shader; es lo
 *     que deja los impostors del campo lejano completamente estaticos.
 *
 *   - SetWorldPositionOffsetDisableDistance: el corte por distancia dentro del
 *     propio componente. Las instancias mas alla del radio dejan de moverse
 *     solas, que es exactamente "solo se mueven los arboles cercanos".
 *
 *   - SetBoundsScale: el precio de mover vertices en el material es que el
 *     culling trabaja con la caja SIN mover. Sin margen, un arbol en el borde de
 *     la pantalla parpadea.
 *
 * NOTA DE COMPATIBILIDAD: SetWorldPositionOffsetDisableDistance existe desde
 * UE 5.1. Si compilas contra una version anterior, borra esa linea (perderas el
 * corte por distancia, no el resto).
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

    UHierarchicalInstancedStaticMeshComponent* Comp =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(Host,
            *FString::Printf(TEXT("ISM_%s%s"), *Key.ToString(), bImpostor ? TEXT("_Imp") : TEXT("")));
    if (!Comp)
    {
        return nullptr;
    }

    // Movable: se anaden y quitan instancias en runtime. Con Static, UE asume
    // que el buffer de instancias no cambia tras registrar el componente.
    Comp->SetMobility(EComponentMobility::Movable);
    Comp->SetStaticMesh(Mesh);
    Comp->NumCustomDataFloats = FMath::Max(0, Config.NumInstanceCustomDataFloats);
    Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Comp->SetCanEverAffectNavigation(false); // si no, cada alta/baja pide rebuild de navmesh
    Comp->SetCastShadow(bImpostor ? Config.bImpostorsCastShadow : Config.bInstancesCastShadow);

    // Fase 6: viento + margen de bounds (ver ConfigureWind).
    ConfigureWind(Comp, bImpostor);

    const float EndCull = bImpostor ? Config.ImpostorEndCullDistanceCm : Config.InstanceEndCullDistanceCm;
    if (EndCull > 0.f)
    {
        // El fade del ISM debe ser un backstop ESTRECHO justo antes del corte, no
        // repartirse por todo el rango: con StartCull=0 las instancias se
        // difuminan desde la distancia 0 (el bosque "ralea"). Arrancamos el fade
        // en el 90% del cull para que no compita con la conmutacion de nivel.
        const int32 End = FMath::RoundToInt(EndCull);
        const int32 Start = FMath::RoundToInt(EndCull * 0.9f);
        Comp->SetCullDistances(Start, End);
    }

    Comp->SetupAttachment(Host->GetRootComponent());
    Comp->RegisterComponent();
    Host->AddInstanceComponent(Comp);

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
