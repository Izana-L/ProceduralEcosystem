#include "Simulation/EcosystemSubsystem.h"
#include "Config/EcosystemSettings.h"
#include "Debug/FieldVisualizer.h"
#include "Species/SpeciesData.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TickScratch.h"

#include "Engine/World.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DrawDebugHelpers.h"
#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"

// Categoría de log local a este .cpp (declara y define en un solo sitio).
// Evita meter DECLARE_LOG_CATEGORY_EXTERN en una cabecera reflejada por UHT.
DEFINE_LOG_CATEGORY_STATIC(LogEco, Log, All);

// ---------------------------------------------------------------------------
//  Comandos de consola (world-safe: buscan el subsistema en el UWorld pasado)
// ---------------------------------------------------------------------------
static UEcosystemSubsystem* GetEco(UWorld* World)
{
    return World ? World->GetSubsystem<UEcosystemSubsystem>() : nullptr;
}

static FAutoConsoleCommandWithWorldAndArgs GEcoStep(
    TEXT("Eco.Step"),
    TEXT("Avanza N ticks de simulacion (por defecto 1). Uso: Eco.Step [N]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                const int32 N = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1;
                S->StepN(N);
            }
        }));

static FAutoConsoleCommandWithWorld GEcoTogglePause(
    TEXT("Eco.TogglePause"),
    TEXT("Pausa/reanuda el avance automatico de la simulacion."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->SetPaused(!S->IsPaused()); }));

static FAutoConsoleCommandWithWorld GEcoAddAgent(
    TEXT("Eco.AddAgent"),
    TEXT("Anade un agente de debug aleatorio sobre el terreno."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->AddRandomDebugAgent(); }));

static FAutoConsoleCommandWithWorld GEcoClear(
    TEXT("Eco.ClearAgents"),
    TEXT("Borra todos los agentes de debug."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->ClearDebugAgents(); }));

static FAutoConsoleCommandWithWorld GEcoPaint(
    TEXT("Eco.PaintTestField"),
    TEXT("Genera y pinta un campo de prueba como heatmap sobre el terreno."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintTestField(); }));

static FAutoConsoleCommandWithWorld GEcoPaintWater(
    TEXT("Eco.PaintWater"),
    TEXT("Pinta el heatmap del agua disponible actual (pool, no el base)."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintWaterField(); }));

static FAutoConsoleCommandWithWorld GEcoPaintNutrients(
    TEXT("Eco.PaintNutrients"),
    TEXT("Pinta el heatmap de nutrientes disponibles actuales (pool, no el base)."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintNutrientField(); }));

static FAutoConsoleCommandWithWorldAndArgs GEcoSeedForest(
    TEXT("Eco.SeedForest"),
    TEXT("Siembra N plantulas aleatorias sobre el terreno (por defecto 200). Uso: Eco.SeedForest [N]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                const int32 N = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 200;
                S->SeedInitialPopulation(N);
            }
        }));

// ---------------------------------------------------------------------------
//  CVars (toggles de debug: se activan/desactivan en vivo desde la consola)
// ---------------------------------------------------------------------------
static TAutoConsoleVariable<int32> CVarDebugAgents(
    TEXT("Eco.Debug.Agents"), 1, TEXT("Dibuja los agentes de debug (Fase 0) como esferas."));
static TAutoConsoleVariable<int32> CVarDebugPopulation(
    TEXT("Eco.Debug.Population"), 1, TEXT("Dibuja la poblacion de arboles simulada (Fase 2) como esferas."));
static TAutoConsoleVariable<int32> CVarDebugTerrain(
    TEXT("Eco.Debug.Terrain"), 0, TEXT("Dibuja las normales del terreno en una rejilla de sondas."));
static TAutoConsoleVariable<int32> CVarDebugHeatmap(
    TEXT("Eco.Debug.Heatmap"), 1, TEXT("Muestra (1) u oculta (0) el decal de heatmap."));

// ---------------------------------------------------------------------------
//  UWorldSubsystem
// ---------------------------------------------------------------------------
bool UEcosystemSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UEcosystemSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    Rng.Init(static_cast<uint32>(S->MasterSeed));
    SecondsPerTick = S->SecondsPerSimTick;
    YearsPerTick = S->YearsPerTick;
    MaxStepsPerFrame = S->MaxStepsPerFrame;
    bPaused = S->bStartPaused;
}

void UEcosystemSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // 1) Relieve: fuente de verdad de la simulacion.
    HeightField.GenerateFractalNoise(
        S->HeightfieldResolution, S->HeightfieldResolution,
        S->HeightfieldCellSizeCm, static_cast<uint32>(S->MasterSeed),
        /*Octaves*/ 5, /*BaseFrequency*/ 0.0006, S->HeightScaleCm);

    if (!HeightField.IsValid())
    {
        UE_LOG(LogEco, Error, TEXT("[Eco] Fallo al generar el relieve; el subsistema no arrancara."));
        return;
    }

    UE_LOG(LogEco, Log, TEXT("[Eco] Relieve %dx%d (~%.0f m de lado) generado con semilla %d"),
        HeightField.Field.Width, HeightField.Field.Height,
        HeightField.Field.Width * HeightField.Field.CellSize / 100.0, S->MasterSeed);

    // 2) Campos base (Fase 1): potencial del terreno, calculado una sola vez.
    //    Ambos comparten geometria con HeightField (mismo Width/Height/CellSize/Origin),
    //    asi que WaterPool y NutrientPool acaban con el mismo numero de celdas.
    WaterBase.BakeFromHeightField(HeightField);
    NutrientBase.GeneratePatchyBase(
        HeightField.Field.Width, HeightField.Field.Height, HeightField.Field.CellSize,
        HeightField.Field.Origin, static_cast<uint32>(S->MasterSeed));

    // 3) Estado runtime (Fase 2): los pools arrancan llenos al nivel del base.
    WaterPool.InitFromBase(WaterBase.Field);
    NutrientPool.InitFromBase(NutrientBase.Field);

    // 4) Grid de luz grueso: geometria derivada del relieve + settings.
    const FBox2D Bounds = HeightField.GetWorldBounds();
    const int32 LightW = FMath::Max(1, FMath::CeilToInt((Bounds.Max.X - Bounds.Min.X) / S->LightCoarseCellSizeCm));
    const int32 LightH = FMath::Max(1, FMath::CeilToInt((Bounds.Max.Y - Bounds.Min.Y) / S->LightCoarseCellSizeCm));
    LightCoarse.Init(LightW, LightH, S->LightCoarseLayers,
        S->LightCoarseCellSizeCm, S->LightCoarseCellSizeCm, Bounds.Min, /*BaseZ*/ 0.0);

    // 5) Spatial hash de agentes: geometria fijada una vez; se repuebla cada tick con Build().
    Hash.Init(Bounds, S->SpatialHashCellSizeCm);

    // 6) Cache de especies (una LoadSynchronous por especie, no por arbol/tick).
    ResolvedSpecies.Reset();
    for (const TSoftObjectPtr<USpeciesData>& SoftSp : S->Species)
    {
        ResolvedSpecies.Add(SoftSp.LoadSynchronous());
    }

    // 7) Visualizador de campos (heatmap).
    FieldViz = NewObject<UFieldVisualizer>(this);
    FieldViz->Initialize(HeightField.Field.Width, HeightField.Field.Height);

    // A partir de aqui es seguro tickear.
    bWorldReady = true;
}

void UEcosystemSubsystem::Deinitialize()
{
    if (HeatmapDecal)
    {
        HeatmapDecal->Destroy();
        HeatmapDecal = nullptr;
    }
    Super::Deinitialize();
}

// ---------------------------------------------------------------------------
//  Tick: desacopla la ecologia (por "años") del frame de render.
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::Tick(float DeltaTime)
{
    if (!bWorldReady)
    {
        return;
    }

    // Pasos manuales (Eco.Step): siempre se ejecutan, aunque este pausado.
    while (PendingSteps > 0)
    {
        SimulateTick(YearsPerTick);
        ++TickCount;
        --PendingSteps;
    }

    // Avance automatico (modo vivo).
    if (!bPaused)
    {
        Accumulator += DeltaTime;
        int32 Steps = 0;
        while (Accumulator >= SecondsPerTick && Steps < MaxStepsPerFrame)
        {
            SimulateTick(YearsPerTick);
            ++TickCount;
            Accumulator -= SecondsPerTick;
            ++Steps;
        }
    }

    DrawDebug();
}

TStatId UEcosystemSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UEcosystemSubsystem, STATGROUP_Tickables);
}

float UEcosystemSubsystem::GetInterpolationAlpha() const
{
    return SecondsPerTick > 0.f
        ? FMath::Clamp(static_cast<float>(Accumulator) / SecondsPerTick, 0.f, 1.f)
        : 0.f;
}

// ---------------------------------------------------------------------------
//  Bucle de tick (Fase 2)
// ---------------------------------------------------------------------------

// Factores de forma de la copa hasta que la Fase 3 aporte geometria real.
// Aqui como constantes con nombre (antes eran literales sueltos en el bucle);
// si en el futuro varian por especie, muevelos a USpeciesData.
static constexpr float kCanopyRadiusFraction = 0.30f; // radio de copa = 30% de la altura
static constexpr float kCanopyShadowDensity = 0.80f; // opacidad de la copa [0,1]
static constexpr float kGerminationBiomassFraction = 0.01f; // plantula nueva = 1% de MaxBiomass

// Tope de tareas del ParallelFor del tick. Constante (no depende de la maquina)
// para que la particion en chunks -y por tanto el orden de la reduccion de
// deltas- sea identica en cualquier CPU. Ver nota de determinismo en SimulateTick.
static constexpr int32 kMaxTickChunks = 32;

/** Reparte [0, PopulationNum) en NumChunks tramos contiguos, deterministas. */
static void GetChunkRange(int32 ChunkIndex, int32 NumChunks, int32 PopulationNum,
    int32& OutBegin, int32& OutEnd)
{
    OutBegin = static_cast<int32>((int64)ChunkIndex * PopulationNum / NumChunks);
    OutEnd = static_cast<int32>((int64)(ChunkIndex + 1) * PopulationNum / NumChunks);
}

void UEcosystemSubsystem::SimulateTick(float DtYears)
{
    const UEcosystemSettings* Settings = UEcosystemSettings::Get();

    // ================================================================
    // PRE (serial): estructuras derivadas del snapshot de lectura.
    // ================================================================
    Hash.Build(Agents_Read.Position, Agents_Read.Num());
    // NOTA: el hash queda listo para consultas de vecindad, pero esta
    // primera version de la Fase 2 resuelve la competencia enteramente a
    // traves de los campos compartidos (consumo de agua/nutrientes + sombra
    // de luz), no con consultas punto-a-punto. Se consumira directamente en
    // la Fase 3 (busqueda de puntos de atraccion del SCA) y esta disponible
    // ya para cualquier extension futura (p.ej. densidad local en germinacion).

    LightCoarse.ClearShadow();
    for (int32 i = 0; i < Agents_Read.Num(); ++i)
    {
        if (Agents_Read.State[i] == ETreeState::Dead) { continue; }
        const USpeciesData* Sp = ResolveSpecies(Agents_Read.SpeciesId[i]);
        if (!Sp) { continue; }

        const float H = Agents_Read.Height[i];
        const FVector Apex = Agents_Read.Position[i] + FVector(0.f, 0.f, H);
        // Radio/profundidad de copa: aproximacion hasta que la Fase 3 aporte
        // geometria real (ver constantes kCanopy* arriba).
        LightCoarse.DepositCanopyShadow(Apex, H * kCanopyRadiusFraction, H, kCanopyShadowDensity);
    }

    Agents_Write.CopyFrom(Agents_Read);
    WaterPool.BeginTick();
    NutrientPool.BeginTick();

    // -----------------------------------------------------------------
    // DETERMINISMO: el nº de chunks se deriva SOLO de la poblacion y de un
    // grain fijo (settings), NUNCA del nº de hilos de la maquina. Si dependiera
    // de GetNumWorkerThreads(), la particion en chunks -y con ella el ORDEN en
    // que ReduceScratchInto suma los deltas de cada celda- cambiaria segun la
    // CPU; como la suma en float NO es asociativa, dos maquinas con distinto nº
    // de nucleos divergirian celda a celda y, tick a tick, acabarian en bosques
    // distintos pese a la misma semilla. Con un recuento fijo: misma poblacion
    // -> misma particion -> misma reduccion bit a bit en cualquier maquina.
    // ParallelFor reparte estas NumChunks tareas entre los hilos disponibles,
    // asi que seguimos aprovechando todos los nucleos.
    // -----------------------------------------------------------------
    const int32 PopNum = Agents_Read.Num();
    const int32 GrainSize = FMath::Max(1, Settings->TickChunkGrainSize);
    const int32 NumChunks = FMath::Clamp(FMath::DivideAndRoundUp(PopNum, GrainSize), 1, kMaxTickChunks);
    const int32 NumCells = WaterPool.Current.Num();

    // Scratch PERSISTENTE (miembro): se dimensiona a NumChunks reutilizando la
    // memoria de ticks anteriores. Cada contexto se pone a cero (ResetForNextTick)
    // o se crea a tamano de campo la primera vez / si NumChunks crecio. Antes se
    // reasignaban NumChunks arrays del tamano del campo EN CADA tick (cientos de
    // KB-MB de churn de heap por tick); ahora es cero asignaciones en regimen.
    TickContexts.SetNum(NumChunks);
    for (FTickScratch& Ctx : TickContexts)
    {
        if (Ctx.WaterDeltas.Num() != NumCells)
        {
            Ctx.InitFromFieldSize(NumCells);
        }
        else
        {
            Ctx.ResetForNextTick();
        }
    }

    // ================================================================
    // PASO 2 (paralelo): cada chunk SOLO lee del snapshot (Agents_Read,
    // WaterPool.Current, NutrientPool.Current, LightCoarse) y SOLO escribe
    // en su porcion de Agents_Write y en su propio FTickScratch.
    // ================================================================
    ParallelFor(NumChunks, [&](int32 ChunkIndex)
        {
            int32 Begin, End;
            GetChunkRange(ChunkIndex, NumChunks, PopNum, Begin, End);
            FTickScratch& Ctx = TickContexts[ChunkIndex];

            for (int32 i = Begin; i < End; ++i)
            {
                if (Agents_Read.State[i] == ETreeState::Dead) { continue; }

                const USpeciesData* Sp = ResolveSpecies(Agents_Read.SpeciesId[i]);
                if (!Sp) { continue; }

                const FVector P = Agents_Read.Position[i];
                const float ReadHeight = Agents_Read.Height[i];
                uint32& RngState = Agents_Write.RngState[i]; // stream propio -> determinista

                // 2a) recursos locales
                const float W = WaterPool.SampleCurrent(P.X, P.Y);
                const float N = NutrientPool.SampleCurrent(P.X, P.Y);
                const float Q = LightCoarse.SampleLightSmooth(P + FVector(0, 0, ReadHeight));

                // 2b) factores + vigor (Liebig)
                const float fL = EcologyRules::LightFactor(Q, Sp->ShadeTolerance, Settings->LightHalfSaturationMax);
                const float fW = EcologyRules::DemandFactor(W, Sp->WaterDemand);
                const float fN = EcologyRules::DemandFactor(N, Sp->NutrientDemand);
                const float VigorValue = EcologyRules::Vigor(fL, fW, fN);

                // 2c) crecimiento + altura + edad + estado (Mature vs Sapling)
                const float NewBiomass = EcologyRules::GrowBiomassLogistic(
                    Agents_Read.Biomass[i], VigorValue, Sp->GrowthRate, Sp->MaxBiomass, DtYears);
                Agents_Write.Biomass[i] = NewBiomass;
                Agents_Write.Height[i] = EcologyRules::HeightFromBiomass(NewBiomass, Sp->MaxBiomass, Sp->MaxHeightCm);
                Agents_Write.Age[i] = Agents_Read.Age[i] + DtYears;
                Agents_Write.State[i] = (Agents_Write.Age[i] >= Sp->MaturityAge) ? ETreeState::Mature : ETreeState::Sapling;

                // 2d) estres
                Agents_Write.Stress[i] = EcologyRules::UpdateStress(Agents_Read.Stress[i], VigorValue,
                    Settings->StressVigorThreshold, Settings->StressAccumulationRate,
                    Settings->StressRecoveryRate, DtYears);

                // 2e) consumo -> SOLO al scratch de este chunk
                const float RootRadiusCm = EcologyRules::EffectiveRootRadiusCm(Sp->RootRadius, NewBiomass, Sp->MaxBiomass);
                EcologyRules::DepositKernel(WaterBase.Field, Ctx.WaterDeltas, P, RootRadiusCm,
                    -NewBiomass * Sp->WaterDemand * DtYears);
                EcologyRules::DepositKernel(NutrientBase.Field, Ctx.NutrientDeltas, P, RootRadiusCm,
                    -NewBiomass * Sp->NutrientDemand * DtYears);

                // 2f) mortalidad (probabilistica, con el RNG propio del arbol)
                const float pDeath = EcologyRules::MortalityProbability(Agents_Write.Age[i], Sp->Longevity,
                    Agents_Write.Stress[i], Settings->StressMortalityWeight, DtYears);

                if (EcoRand::NextUnit(RngState) < pDeath)
                {
                    Agents_Write.State[i] = ETreeState::Dead;

                    FPendingDeathPulse Pulse;
                    Pulse.Position = P;
                    Pulse.RadiusCm = RootRadiusCm;
                    Pulse.Amount = EcologyRules::DeathNutrientPulse(NewBiomass, Settings->NutrientDecompositionFactor);
                    Ctx.DeathPulses.Add(Pulse);
                }
                else if (Agents_Write.State[i] == ETreeState::Mature)
                {
                    // 2g) semillas (solo si sigue vivo y es maduro)
                    const int32 NumSeeds = EcologyRules::ComputeSeedCount(Settings->SeedRatePerBiomass, NewBiomass, DtYears, RngState);
                    const float DispersalRadiusCm = Sp->SeedDispersalRadius * 100.f;

                    for (int32 s = 0; s < NumSeeds; ++s)
                    {
                        const FVector2D Offset = EcologyRules::SampleSeedOffsetCm(DispersalRadiusCm, RngState);

                        FPendingSeed Seed;
                        Seed.Position = P + FVector(Offset.X, Offset.Y, 0.f);
                        Seed.SpeciesId = Agents_Read.SpeciesId[i];
                        Seed.RngSeed = EcoRand::SeedForIndex(RngState, s);
                        Ctx.Seeds.Add(Seed);
                    }
                }
            }
        });

    // ================================================================
    // PASO 3 (serial): reduccion -> regeneracion -> pulsos de muerte -> germinacion.
    // ================================================================
    TArray<FPendingSeed> PendingSeeds;
    TArray<FPendingDeathPulse> PendingDeaths;
    EcologyRules::ReduceScratchInto(TickContexts, WaterPool.Next.Data, NutrientPool.Next.Data, PendingSeeds, PendingDeaths);

    WaterPool.RegenerateTowardBase(WaterBase.Field, Settings->WaterRechargeRate, Settings->WaterDiffusionRate, DtYears);
    NutrientPool.RegenerateTowardBase(NutrientBase.Field, Settings->NutrientRechargeRate, Settings->NutrientDiffusionRate, DtYears);

    for (const FPendingDeathPulse& Pulse : PendingDeaths)
    {
        EcologyRules::DepositKernel(NutrientBase.Field, NutrientPool.Next.Data, Pulse.Position, Pulse.RadiusCm, Pulse.Amount);
    }

    // Reserva una sola vez: PendingSeeds.Num() es la cota superior de germinaciones.
    // Evita realojos de los 8 arrays SoA durante los Add() de abajo.
    Agents_Write.Reserve(Agents_Write.Num() + PendingSeeds.Num());

    const FBox2D WorldBounds = HeightField.GetWorldBounds();
    const double MinSpacingSq = FMath::Square((double)Settings->MinGerminationSpacingCm);

    for (const FPendingSeed& Seed : PendingSeeds)
    {
        const USpeciesData* Sp = ResolveSpecies(Seed.SpeciesId);
        if (!Sp) { continue; }

        // Semilla dispersada fuera del terreno simulado: se descarta en vez de
        // germinar en el borde (SampleHeight/SampleLight harian clamp y
        // apelmazarian plantulas en el limite del mapa).
        if (Seed.Position.X < WorldBounds.Min.X || Seed.Position.X > WorldBounds.Max.X ||
            Seed.Position.Y < WorldBounds.Min.Y || Seed.Position.Y > WorldBounds.Max.Y)
        {
            continue;
        }

        FVector GerminationPos = Seed.Position;
        GerminationPos.Z = HeightField.SampleHeight(GerminationPos.X, GerminationPos.Y);

        // Espaciado minimo: no germinar pegada a un arbol ya vivo. Aqui es donde
        // el spatial hash (que se reconstruye cada tick pero hasta ahora no tenia
        // consumidor) empieza a ganarse el coste. El hash indexa Agents_Read;
        // consultamos Agents_Write.State para NO dejar que un arbol muerto ESTE
        // tick bloquee el hueco que acaba de liberar. El booleano no depende del
        // orden de visita -> sigue siendo determinista.
        bool bTooClose = false;
        Hash.ForEachNeighbor(GerminationPos, Settings->MinGerminationSpacingCm,
            [&](int32 NeighborIdx)
            {
                if (bTooClose) { return; }
                if (Agents_Write.State[NeighborIdx] == ETreeState::Dead) { return; }
                const FVector& NP = Agents_Read.Position[NeighborIdx];
                const double dx = NP.X - GerminationPos.X;
                const double dy = NP.Y - GerminationPos.Y;
                if (dx * dx + dy * dy < MinSpacingSq)
                {
                    bTooClose = true;
                }
            });
        if (bTooClose) { continue; }

        const float LightHere = LightCoarse.SampleLightSmooth(GerminationPos);
        if (!EcologyRules::IsSafeGerminationSite(LightHere, Settings->MinLightForGermination))
        {
            continue; // sitio no seguro: la semilla no prospera, se descarta
        }

        const float WHere = WaterPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);
        const float NHere = NutrientPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);
        const float fL = EcologyRules::LightFactor(LightHere, Sp->ShadeTolerance, Settings->LightHalfSaturationMax);
        const float fW = EcologyRules::DemandFactor(WHere, Sp->WaterDemand);
        const float fN = EcologyRules::DemandFactor(NHere, Sp->NutrientDemand);
        const float VigorHere = EcologyRules::Vigor(fL, fW, fN);

        uint32 SeedRng = Seed.RngSeed;
        const float pGerm = EcologyRules::GerminationProbability(VigorHere, Settings->GerminationRate);
        if (EcoRand::NextUnit(SeedRng) < pGerm)
        {
            Agents_Write.Add(GerminationPos, Seed.SpeciesId, SeedRng, /*Age*/ 0.f, /*Biomass*/ Sp->MaxBiomass * kGerminationBiomassFraction);
        }
    }

    // ================================================================
    // PASO 4: compactar muertos e intercambiar buffers (agentes y campos).
    // ================================================================
    Agents_Write.CompactDead();

    Swap(Agents_Read, Agents_Write);
    WaterPool.SwapBuffers();
    NutrientPool.SwapBuffers();

    if ((TickCount % 20) == 0)
    {
        LogPopulationStats();
    }
}

const USpeciesData* UEcosystemSubsystem::ResolveSpecies(uint16 SpeciesId) const
{
    return ResolvedSpecies.IsValidIndex(SpeciesId) ? ResolvedSpecies[SpeciesId] : nullptr;
}

// ---------------------------------------------------------------------------
//  Poblacion (Fase 2)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::SeedInitialPopulation(int32 Count)
{
    if (!HeightField.IsValid())
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] SeedInitialPopulation: el relieve aun no esta listo."));
        return;
    }
    if (ResolvedSpecies.Num() == 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] SeedInitialPopulation: no hay especies en Project Settings -> Procedural Ecosystem."));
        return;
    }

    const FBox2D Bounds = HeightField.GetWorldBounds();
    Agents_Read.Reserve(Agents_Read.Num() + Count);

    for (int32 i = 0; i < Count; ++i)
    {
        // Stream Colonization (Fase 0): coloca el bosque inicial sin tocar
        // los streams de mortalidad/dispersion/morfologia de la simulacion.
        const double X = FMath::Lerp(Bounds.Min.X, Bounds.Max.X, (double)Rng.Unit(EEcoRngStream::Colonization));
        const double Y = FMath::Lerp(Bounds.Min.Y, Bounds.Max.Y, (double)Rng.Unit(EEcoRngStream::Colonization));
        const float  Z = HeightField.SampleHeight(X, Y);

        const int32 SpeciesIdx = Rng.RangeI(EEcoRngStream::Colonization, 0, ResolvedSpecies.Num() - 1);
        const USpeciesData* Sp = ResolvedSpecies[SpeciesIdx];
        if (!Sp) { continue; }

        const uint32 AgentSeed = Rng.U32(EEcoRngStream::Colonization);
        const float InitialBiomass = Sp->MaxBiomass * Rng.RangeF(EEcoRngStream::Colonization, 0.005f, 0.03f);

        Agents_Read.Add(FVector(X, Y, Z), static_cast<uint16>(SpeciesIdx), AgentSeed, /*Age*/ 0.f, InitialBiomass);
    }

    UE_LOG(LogEco, Log, TEXT("[Eco] Sembradas %d plantulas (poblacion total: %d)."), Count, Agents_Read.Num());
}

void UEcosystemSubsystem::LogPopulationStats() const
{
    if (Agents_Read.Num() == 0)
    {
        UE_LOG(LogEco, Log, TEXT("[Eco] Tick %lld | Poblacion: 0 arboles."), TickCount);
        return;
    }

    TArray<int32> CountBySpecies;
    CountBySpecies.SetNumZeroed(ResolvedSpecies.Num());
    for (int32 i = 0; i < Agents_Read.Num(); ++i)
    {
        if (Agents_Read.SpeciesId[i] < CountBySpecies.Num())
        {
            ++CountBySpecies[Agents_Read.SpeciesId[i]];
        }
    }

    FString Breakdown;
    for (int32 s = 0; s < CountBySpecies.Num(); ++s)
    {
        const USpeciesData* Sp = ResolveSpecies(static_cast<uint16>(s));
        Breakdown += FString::Printf(TEXT("%s=%d "), Sp ? *Sp->SpeciesName.ToString() : TEXT("?"), CountBySpecies[s]);
    }

    UE_LOG(LogEco, Log, TEXT("[Eco] Tick %lld | Poblacion total: %d | %s"),
        TickCount, Agents_Read.Num(), *Breakdown);
}

// ---------------------------------------------------------------------------
//  Debug agents (Fase 0)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::AddDebugAgent(const FVector& WorldPos, const FColor& Color, float Radius)
{
    FEcoDebugAgent A;
    A.Position = WorldPos;
    A.Color = Color;
    A.Radius = Radius;
    DebugAgents.Add(A);
}

void UEcosystemSubsystem::AddRandomDebugAgent()
{
    if (!HeightField.IsValid())
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] AddRandomDebugAgent: el relieve aun no esta listo."));
        return;
    }

    // IMPORTANTE: usamos el stream Debug, no los de la simulacion, para que las
    // herramientas de depuracion NO perturben la reproducibilidad del bosque.
    const FBox2D B = HeightField.GetWorldBounds();
    const float u = Rng.Unit(EEcoRngStream::Debug);
    const float v = Rng.Unit(EEcoRngStream::Debug);
    const double x = FMath::Lerp(B.Min.X, B.Max.X, (double)u);
    const double y = FMath::Lerp(B.Min.Y, B.Max.Y, (double)v);
    const float  z = HeightField.SampleHeight(x, y);

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const USpeciesData* Sp = nullptr;
    if (S->Species.Num() > 0)
    {
        const int32 Idx = Rng.RangeI(EEcoRngStream::Debug, 0, S->Species.Num() - 1);
        Sp = S->Species[Idx].LoadSynchronous();
    }

    FColor Color;
    if (Sp)
    {
        Color = Sp->DebugColor;
    }
    else
    {
        Color = FColor(
            static_cast<uint8>(Rng.RangeI(EEcoRngStream::Debug, 40, 255)),
            static_cast<uint8>(Rng.RangeI(EEcoRngStream::Debug, 40, 255)),
            static_cast<uint8>(Rng.RangeI(EEcoRngStream::Debug, 40, 255)),
            255);
    }

    const float R = Rng.RangeF(EEcoRngStream::Debug, 80.f, 300.f);
    AddDebugAgent(FVector(x, y, z), Color, R);
}

void UEcosystemSubsystem::ClearDebugAgents()
{
    DebugAgents.Reset();
}

// ---------------------------------------------------------------------------
//  Dibujo de debug (cada frame, gobernado por CVars)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::DrawDebug()
{
    UWorld* World = GetWorld();
    if (!World) return;

    if (HeatmapDecal)
    {
        HeatmapDecal->SetActorHiddenInGame(CVarDebugHeatmap.GetValueOnGameThread() == 0);
    }

    if (CVarDebugAgents.GetValueOnGameThread() != 0)
    {
        for (const FEcoDebugAgent& A : DebugAgents)
        {
            DrawDebugSphere(World, A.Position, A.Radius, 8, A.Color, false, -1.f, 0, 2.f);
        }
    }

    if (CVarDebugPopulation.GetValueOnGameThread() != 0)
    {
        for (int32 i = 0; i < Agents_Read.Num(); ++i)
        {
            if (Agents_Read.State[i] == ETreeState::Dead) { continue; }

            const USpeciesData* Sp = ResolveSpecies(Agents_Read.SpeciesId[i]);
            const FColor Color = Sp ? Sp->DebugColor : FColor::White;
            const float  H = Agents_Read.Height[i];
            const FVector Center = Agents_Read.Position[i] + FVector(0.f, 0.f, H * 0.5f);
            const float  Radius = FMath::Max(30.f, H * 0.3f);

            DrawDebugSphere(World, Center, Radius, 8, Color, false, -1.f, 0, 2.f);
        }
    }

    if (CVarDebugTerrain.GetValueOnGameThread() != 0 && HeightField.IsValid())
    {
        const FBox2D B = HeightField.GetWorldBounds();
        const int32 N = 24;
        for (int32 j = 0; j <= N; ++j)
        {
            for (int32 i = 0; i <= N; ++i)
            {
                const double x = FMath::Lerp(B.Min.X, B.Max.X, (double)i / N);
                const double y = FMath::Lerp(B.Min.Y, B.Max.Y, (double)j / N);
                const float  z = HeightField.SampleHeight(x, y);
                const FVector P(x, y, z);
                const FVector Nn = HeightField.SampleNormal(x, y);
                DrawDebugLine(World, P, P + Nn * 300.f, FColor::Cyan, false, -1.f, 0, 3.f);
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  Heatmaps
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::PaintTestField()
{
    if (!FieldViz || !HeightField.IsValid()) return;
    FieldViz->UpdateFromField(HeightField.Field.Data);
    EnsureHeatmapDecal();
    UE_LOG(LogEco, Log, TEXT("[Eco] Campo de prueba pintado."));
}

void UEcosystemSubsystem::PaintWaterField()
{
    if (!FieldViz || !WaterPool.Current.IsValid()) return;
    FieldViz->UpdateFromField(WaterPool.Current.Data);
    EnsureHeatmapDecal();
    UE_LOG(LogEco, Log, TEXT("[Eco] Heatmap de agua (pool actual) pintado."));
}

void UEcosystemSubsystem::PaintNutrientField()
{
    if (!FieldViz || !NutrientPool.Current.IsValid()) return;
    FieldViz->UpdateFromField(NutrientPool.Current.Data);
    EnsureHeatmapDecal();
    UE_LOG(LogEco, Log, TEXT("[Eco] Heatmap de nutrientes (pool actual) pintado."));
}

void UEcosystemSubsystem::EnsureHeatmapDecal()
{
    UWorld* World = GetWorld();
    if (!World || !FieldViz || !FieldViz->GetTexture()) return;

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UMaterialInterface* Base = S->HeatmapDecalMaterial.LoadSynchronous();
    if (!Base)
    {
        UE_LOG(LogEco, Warning,
            TEXT("[Eco] Falta HeatmapDecalMaterial en Project Settings; no se puede pintar el decal."));
        return;
    }

    const FBox2D B = HeightField.GetWorldBounds();
    const FVector Center(0.5 * (B.Min.X + B.Max.X),
        0.5 * (B.Min.Y + B.Max.Y),
        S->HeightScaleCm * 1.5f);

    const float HalfX = 0.5f * (B.Max.X - B.Min.X);
    const float HalfY = 0.5f * (B.Max.Y - B.Min.Y);
    const FVector DecalSize(S->HeightScaleCm * 2.f, HalfY, HalfX);

    if (!HeatmapDecal)
    {
        const FRotator DownRot(-90.f, 0.f, 0.f);
        HeatmapDecal = World->SpawnActor<ADecalActor>(Center, DownRot);
    }

    if (HeatmapDecal)
    {
        if (!HeatmapMID)
        {
            HeatmapMID = UMaterialInstanceDynamic::Create(Base, this);
            HeatmapDecal->SetDecalMaterial(HeatmapMID);
        }
        if (UDecalComponent* DC = HeatmapDecal->GetDecal())
        {
            DC->DecalSize = DecalSize;
        }
        HeatmapDecal->SetActorLocation(Center);
        if (HeatmapMID)
        {
            HeatmapMID->SetTextureParameterValue(TEXT("FieldTex"), FieldViz->GetTexture());
        }
    }
}