#include "Simulation/EcosystemSubsystem.h"
#include "Config/EcosystemSettings.h"
#include "Core/EcoStats.h"          // Fase 6 (6.4): stat groups, Insights, CSV
#include "Debug/FieldVisualizer.h"
#include "Species/SpeciesData.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TickScratch.h"
#include "Ecology/Vigor.h"
#include "Ecology/CarbonModel.h"    // Fase 6 (6.3): multiplicador de CO2
#include "Geometry/HeroTreeActor.h"

#include "Engine/World.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DrawDebugHelpers.h"
#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Serialization/MemoryWriter.h" 
#include "Serialization/MemoryReader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

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
static TAutoConsoleVariable<int32> CVarForceST(
    TEXT("Eco.ForceSingleThread"), 0, TEXT("1 = tick en un solo hilo (validar determinismo)."));

// --- Fase 6 (6.3): CO2 como capa de realismo barata ---
// -1 = usar Project Settings; 0/1 = forzar. Sirve para la ABLACION: apagarlo
// devuelve exactamente los resultados anteriores a la Fase 6 (el multiplicador
// pasa a valer 1.0 exacto), asi que dos corridas con la misma semilla se pueden
// comparar cuantitativamente en la memoria.
static TAutoConsoleVariable<int32> CVarCO2(TEXT("Eco.CO2.Enable"), -1,
    TEXT("Multiplicador de CO2 sobre el vigor (doc. 6.3). -1 = Project Settings, 0 = off, 1 = on."));

static FAutoConsoleCommandWithWorldAndArgs GEcoStep(TEXT("Eco.Step"),
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


static FAutoConsoleCommandWithWorld GEcoTogglePause(TEXT("Eco.TogglePause"),
    TEXT("Pausa/reanuda el avance automatico de la simulacion."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->SetPaused(!S->IsPaused()); }));

static FAutoConsoleCommandWithWorld GEcoAddAgent(TEXT("Eco.AddAgent"),
    TEXT("Anade un agente de debug aleatorio sobre el terreno."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->AddRandomDebugAgent(); }));

static FAutoConsoleCommandWithWorld GEcoClear(TEXT("Eco.ClearAgents"),
    TEXT("Borra todos los agentes de debug."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->ClearDebugAgents(); }));

static FAutoConsoleCommandWithWorld GEcoPaint(TEXT("Eco.PaintTestField"),
    TEXT("Genera y pinta un campo de prueba como heatmap sobre el terreno."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintTestField(); }));

static FAutoConsoleCommandWithWorld GEcoPaintWater(TEXT("Eco.PaintWater"),
    TEXT("Pinta el heatmap del agua disponible actual (pool, no el base)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintWaterField(); }));

static FAutoConsoleCommandWithWorld GEcoPaintNutrients(TEXT("Eco.PaintNutrients"),
    TEXT("Pinta el heatmap de nutrientes disponibles actuales (pool, no el base)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintNutrientField(); }));

static FAutoConsoleCommandWithWorld GEcoPaintVigor(TEXT("Eco.PaintVigor"),
    TEXT("Pinta el heatmap de idoneidad (vigor de Liebig) de la especie HeatmapSpeciesIndex."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintVigorField(); }));


static FAutoConsoleCommandWithWorldAndArgs GEcoSeedForest(TEXT("Eco.SeedForest"),
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

static FAutoConsoleCommandWithWorldAndArgs GEcoGrowHeroTree(TEXT("Eco.GrowHeroTree"),
    TEXT("Genera un hero tree (SCA) con la luz actual del ecosistema. "
        "Uso: Eco.GrowHeroTree [SpeciesIndex] [Seed] [X] [Y] (X,Y en cm; por defecto, centro del terreno)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                const int32 SpeciesIndex = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
                const uint32 Seed = Args.Num() > 1 ? (uint32)FCString::Atoi64(*Args[1]) : 12345u;

                const FBox2D B = S->GetHeightField().GetWorldBounds();
                const double X = Args.Num() > 2 ? FCString::Atod(*Args[2]) : 0.5 * (B.Min.X + B.Max.X);
                const double Y = Args.Num() > 3 ? FCString::Atod(*Args[3]) : 0.5 * (B.Min.Y + B.Max.Y);
                const float  Z = S->GetHeightField().SampleHeight(X, Y);

                S->SpawnHeroTree(FVector(X, Y, Z), SpeciesIndex, Seed);
            }
        }));


static FAutoConsoleCommandWithWorld GEcoClearHeroTrees(TEXT("Eco.ClearHeroTrees"),
    TEXT("Destruye todos los hero trees generados."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->ClearHeroTrees(); }));

static FAutoConsoleCommandWithWorld GEcoFingerprint(TEXT("Eco.Fingerprint"), TEXT("Loguea un hash del estado completo del bosque."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogStateFingerprint(); }));

// Limpieza B1: estas tres estaban IMPLEMENTADAS pero sin comando que las
// registrase, o sea codigo inalcanzable. CheckFinite y Profile son justo las que
// hacen falta para los capitulos de robustez y de rendimiento de la memoria.
static FAutoConsoleCommandWithWorld GEcoCheckFinite(TEXT("Eco.CheckFinite"),
    TEXT("Comprueba que no hay NaN/Inf en la poblacion ni en los campos."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogFiniteCheck(); }));

static FAutoConsoleCommandWithWorld GEcoPaintLight(TEXT("Eco.PaintLight"),
    TEXT("Pinta el heatmap de luz disponible a ras de suelo."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->PaintLightField(); }));

static FAutoConsoleCommandWithWorld GEcoProfile(TEXT("Eco.Profile"),
    TEXT("Desglosa el coste del tick por etapas y la memoria de las estructuras (doc. 6.4)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogTickProfile(); }));

// --- Fase 5 (Paso 1): eventos de muerte ---
static FAutoConsoleCommandWithWorld GEcoLogDeaths(TEXT("Eco.Deaths.Log"),
    TEXT("Loguea el nº total de muertes y las ultimas del buffer (Fase 5)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogRecentDeaths(); }));

// Estructura demografica por especie: es lo que hay que mirar para calibrar la
// longevidad, porque el recuento de poblacion no distingue un bosque maduro de
// un vivero (mil plantones y mil arboles de dosel son el mismo numero).
static FAutoConsoleCommandWithWorld GEcoDemographics(TEXT("Eco.Demografia"),
    TEXT("Reparto por especie y estado, edades y fraccion de arboles ya crecidos."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogDemographics(); }));

// --- Fase 5 (Paso 5): descomposicion visible en el terreno ---
static FAutoConsoleCommandWithWorld GEcoPaintDecomp(TEXT("Eco.PaintDecomposition"),
    TEXT("Pinta el heatmap de descomposicion (puntos de muerte recientes) sobre el terreno."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->PaintDecompositionField(); }));

static TAutoConsoleVariable<int32> CVarDecompLive(TEXT("Eco.Decomp.Live"), 0,
    TEXT("1 = repinta el heatmap de descomposicion cada tick (ver manchas aparecer y desvanecerse)."));

// --- Fase 5 (Paso 6): bake a un año objetivo (guardar/cargar) ---
static FString EcoBakePath(const FString& Name)
{
    return FPaths::ProjectSavedDir() / TEXT("EcoBakes") / (Name + TEXT(".ecobake"));
}

static FAutoConsoleCommandWithWorldAndArgs GEcoSave(TEXT("Eco.Save"),
    TEXT("Guarda el bosque en Saved/EcoBakes/<nombre>.ecobake (por defecto 'bake'). Uso: Eco.Save [nombre]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                S->SaveState(EcoBakePath(Args.Num() > 0 ? Args[0] : TEXT("bake")));
            }
        }));

static FAutoConsoleCommandWithWorldAndArgs GEcoLoad(TEXT("Eco.Load"),
    TEXT("Carga el bosque desde Saved/EcoBakes/<nombre>.ecobake (por defecto 'bake'). Uso: Eco.Load [nombre]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                S->LoadState(EcoBakePath(Args.Num() > 0 ? Args[0] : TEXT("bake")));
            }
        }));



void UEcosystemSubsystem::LogStateFingerprint() const
{
    uint32 H = 0;
    auto Mix = [&H](const void* D, int32 Bytes) { if (Bytes > 0) H = FCrc::MemCrc32(D, Bytes, H); };
    const FTreePopulation& P = Agents_Read;
    Mix(P.Position.GetData(), P.Position.Num() * sizeof(FVector));
    Mix(P.SpeciesId.GetData(), P.SpeciesId.Num() * sizeof(uint16));
    Mix(P.Age.GetData(), P.Age.Num() * sizeof(float));
    Mix(P.Biomass.GetData(), P.Biomass.Num() * sizeof(float));
    Mix(P.Height.GetData(), P.Height.Num() * sizeof(float));
    Mix(P.Stress.GetData(), P.Stress.Num() * sizeof(float));
    Mix(P.State.GetData(), P.State.Num() * sizeof(ETreeState));
    Mix(P.RngState.GetData(), P.RngState.Num() * sizeof(uint32));
    Mix(WaterPool.Current.Data.GetData(), WaterPool.Current.Data.Num() * sizeof(float));
    Mix(NutrientPool.Current.Data.GetData(), NutrientPool.Current.Data.Num() * sizeof(float));
    UE_LOG(LogEco, Log, TEXT("[Eco] Tick %lld | Fingerprint 0x%08X | Pop %d"), TickCount, H, Agents_Read.Num());
}
void UEcosystemSubsystem::LogFiniteCheck() const
{
    int32 Bad = 0;
    auto Scan = [&Bad](const TArray<float>& A, const TCHAR* N) {
        for (int32 i = 0; i < A.Num(); ++i) if (!FMath::IsFinite(A[i])) {
            UE_LOG(LogEco, Error, TEXT("[Eco] NO-FINITO en %s[%d]=%f"), N, i, A[i]); ++Bad; break;
        } };
    Scan(Agents_Read.Biomass, TEXT("Biomass")); Scan(Agents_Read.Height, TEXT("Height"));
    Scan(Agents_Read.Stress, TEXT("Stress"));
    Scan(WaterPool.Current.Data, TEXT("Water")); Scan(NutrientPool.Current.Data, TEXT("Nutrient"));
    Scan(LightCoarse.Shadow, TEXT("LightShadow"));
    if (!Bad) UE_LOG(LogEco, Log, TEXT("[Eco] CheckFinite OK (0 no-finitos)."));
}

// ---------------------------------------------------------------------------
//  Fase 6 (6.3): parametros del multiplicador de CO2
// ---------------------------------------------------------------------------
//
// UN SOLO SITIO donde se resuelven, y de ahi los leen las TRES cosas que
// evaluan vigor: el tick (crecimiento), la germinacion y el heatmap de
// idoneidad. Si cada uno los montara por su cuenta, el mapa que le ensenas al
// tribunal dejaria de representar la funcion que de verdad hace crecer al
// bosque en cuanto alguien tocase un valor.
EcoCarbon::FCO2Params UEcosystemSubsystem::GetCO2Params() const
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();

    EcoCarbon::FCO2Params P;
    const int32 Override = CVarCO2.GetValueOnGameThread();
    P.bEnabled = (Override < 0) ? S->bEnableCO2Factor : (Override != 0);
    P.MaxReduction = S->CO2MaxReduction;
    P.FullMixingHeightCm = S->CO2FullMixingHeightCm;
    P.FullSunlight = FLightFieldCoarse::FullSunlight;
    return P;
}

void UEcosystemSubsystem::LogTickProfile() const
{
    // Memoria de las estructuras que la optimizacion vigila.
    int64 ScratchBytes = 0;
    for (const FTickScratch& Ctx : TickContexts) { ScratchBytes += Ctx.DeltaBytes(); }
    const int64 LightBytes = LightCoarse.MemoryBytes();
    const int64 FieldBytes = (int64)(WaterPool.Current.Data.Num() + WaterPool.Next.Data.Num()
        + NutrientPool.Current.Data.Num() + NutrientPool.Next.Data.Num()
        + DecompositionField.Data.Num()) * sizeof(float)
        + WaterPool.ScratchBytes() + NutrientPool.ScratchBytes();

    UE_LOG(LogEco, Log, TEXT("[Eco/Profile] Tick %lld | poblacion %d | %d tareas"),
        TickCount, Agents_Read.Num(), TickContexts.Num());
    UE_LOG(LogEco, Log, TEXT("[Eco/Profile] TOTAL %.3f ms = hash %.3f + luz %.3f + paralelo %.3f + reduccion %.3f + regen %.3f + germinacion %.3f"),
        Profile.TotalMs, Profile.HashMs, Profile.LightMs, Profile.ParallelMs,
        Profile.ReduceMs, Profile.RegenMs, Profile.GerminationMs);
    UE_LOG(LogEco, Log, TEXT("[Eco/Profile] Memoria: scratch de deltas %.2f MB | grid de luz %.2f MB (%dx%dx%d) | campos 2D %.2f MB | hash %.2f MB"),
        ScratchBytes / 1048576.0, LightBytes / 1048576.0,
        LightCoarse.Width, LightCoarse.Height, LightCoarse.Layers,
        FieldBytes / 1048576.0, Hash.ScratchBytes() / 1048576.0);

    // Fase 6: contexto que hace falta para interpretar los numeros de arriba.
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const EcoCarbon::FCO2Params CO2 = GetCO2Params();
    UE_LOG(LogEco, Log, TEXT("[Eco/Profile] Presupuesto del tick: %.1f ms/frame (%d ticks el ultimo frame, tope %d) | CO2 %s (max -%.0f%%)"),
        S->TickBudgetMsPerFrame, TicksLastFrame, MaxStepsPerFrame,
        CO2.bEnabled ? TEXT("ON") : TEXT("OFF"), CO2.MaxReduction * 100.f);
    UE_LOG(LogEco, Log, TEXT("[Eco/Profile] Siguiente paso (doc. 6.4): 'Eco.Frame' para el reparto del frame, "
        "'stat EcoSim' / 'stat Unit' / 'stat GPU' en pantalla, y Unreal Insights (-trace=cpu,frame,counters) para la timeline."));
}
// ---------------------------------------------------------------------------
//  CVars (toggles de debug: se activan/desactivan en vivo desde la consola)
// ---------------------------------------------------------------------------
static TAutoConsoleVariable<int32> CVarDebugAgents(TEXT("Eco.Debug.Agents"), 1, TEXT("Dibuja los agentes de debug (Fase 0) como esferas."));

// Por defecto APAGADO: dibujar una esfera por arbol vivo es trabajo O(poblacion)
// por frame (a 20k arboles, decenas de miles de esferas) y ademas es redundante
// con el render por instancias, que ya dibuja los mismos arboles. Activalo solo
// para depurar: Eco.Debug.Population 1.
static TAutoConsoleVariable<int32> CVarDebugPopulation(TEXT("Eco.Debug.Population"), 0, TEXT("Dibuja la poblacion de arboles simulada (Fase 2) como esferas. 0 = off (por defecto)."));

static TAutoConsoleVariable<int32> CVarDebugTerrain(TEXT("Eco.Debug.Terrain"), 0, TEXT("Dibuja las normales del terreno en una rejilla de sondas."));

static TAutoConsoleVariable<int32> CVarDebugHeatmap(TEXT("Eco.Debug.Heatmap"), 1, TEXT("Muestra (1) u oculta (0) el decal de heatmap."));


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

    // 1) Relieve: fuente de verdad de la simulacion. Toda la forma (longitud
    //    de onda, persistencia, warp, crestas) y la erosion salen de Project
    //    Settings; aqui solo se convierten metros -> cm y se lanza el bake
    //    (ver FHeightField::Generate para el pipeline completo).
    FTerrainGenParams TerrainParams;
    TerrainParams.Width = S->HeightfieldResolution;
    TerrainParams.Height = S->HeightfieldResolution;
    TerrainParams.CellSizeCm = S->HeightfieldCellSizeCm;
    TerrainParams.Seed = static_cast<uint32>(S->MasterSeed);
    TerrainParams.HeightScaleCm = S->HeightScaleCm;
    TerrainParams.Noise.Octaves = S->HeightfieldOctaves;
    TerrainParams.Noise.BaseWavelengthCm = S->TerrainBaseWavelengthM * 100.0;
    TerrainParams.Noise.Persistence = S->TerrainPersistence;
    TerrainParams.Noise.Lacunarity = S->TerrainLacunarity;
    TerrainParams.Noise.WarpStrengthCm = S->TerrainWarpStrengthM * 100.0;
    TerrainParams.Noise.WarpWavelengthCm = S->TerrainWarpWavelengthM * 100.0;
    TerrainParams.Noise.RidgeWeight = S->TerrainRidgeWeight;
    TerrainParams.bErosion = S->bTerrainErosion;
    TerrainParams.Hydraulic.Droplets = S->TerrainHydraulicDroplets;
    TerrainParams.Hydraulic.Strength = S->TerrainErosionStrength;
    TerrainParams.Thermal.Iterations = S->TerrainThermalIterations;
    TerrainParams.Thermal.TalusAngleDeg = S->TerrainTalusAngleDeg;
    HeightField.Generate(TerrainParams);

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
    WaterBase.BakeFromHeightField(HeightField, S->WaterOutputMax, S->bFillWaterSinks);

    NutrientBase.GeneratePatchyBase(HeightField.Field.Width, HeightField.Field.Height, HeightField.Field.CellSize,
        HeightField.Field.Origin, static_cast<uint32>(S->MasterSeed), S->NutrientOutputMax, S->NutrientPatchFrequency, S->NutrientOctaves);

    auto LogRange = [](const FField2D& F, const TCHAR* N) {
        float Mn, Mx;
        FField2D::MinMax(F.Data, Mn, Mx);
        UE_LOG(LogEco, Log, TEXT("[Eco] Campo %s: min=%.3f max=%.3f (%d celdas)"), N, Mn, Mx, F.Data.Num()); };
    LogRange(WaterBase.Field, TEXT("Agua"));  LogRange(NutrientBase.Field, TEXT("Nutrientes"));

    // 3) Estado runtime (Fase 2): los pools arrancan llenos al nivel del base.
    WaterPool.InitFromBase(WaterBase.Field);
    NutrientPool.InitFromBase(NutrientBase.Field);

    // Fase 5 (Paso 5): campo de descomposicion, misma geometria que los campos,
    // arranca a cero (aun no ha muerto nadie).
    DecompositionField.Init(NutrientBase.Field.Width, NutrientBase.Field.Height, NutrientBase.Field.CellSize, NutrientBase.Field.Origin, 0.f);
    // Fase 5 (Paso 1): anillo de muertes dimensionado UNA sola vez. Escritor
    // (RecordDeathEvent) y lector (CollectNewDeathEvents) tienen que usar el MISMO
    // modulo. Si el escritor lo leyera de los settings en cada muerte -y
    // UEcosystemSettings es un UDeveloperSettings, editable EN VIVO- subir
    // DeathEventBufferSize a mitad de partida haria que ambos indexaran distinto y
    // la capa de suelo pondria tocones en las coordenadas de otros arboles.
    RecentDeaths.Reset();
    RecentDeaths.SetNum(FMath::Max(0, S->DeathEventBufferSize));
    DeathEventCounter = 0;

    const FBox2D Bounds = HeightField.GetWorldBounds();

    // 4) Cache de especies (una LoadSynchronous por especie, no por arbol/tick).
    //    Va ANTES del grid de luz porque este dimensiona su altura a partir de la
    //    especie mas alta (ver paso 5).
    ResolvedSpecies.Reset();
    for (const TSoftObjectPtr<USpeciesData>& SoftSp : S->Species)
    {
        ResolvedSpecies.Add(SoftSp.LoadSynchronous());
    }

    // 5) Grid de luz grueso, RELATIVO AL TERRENO (optimizacion C2).
    //    Antes cubria todo el desnivel del relieve en Z absoluta: con
    //    HeightScaleCm = 30.000 cm y voxel de 400 cm salian 95 capas (~25 MB) de
    //    las que cada columna usaba 5. Ahora la vertical se mide SOBRE EL SUELO,
    //    asi que basta con cubrir el arbol mas alto + margenes: ~10 capas.
    const int32 LightW = FMath::Max(1, FMath::CeilToInt32((Bounds.Max.X - Bounds.Min.X) / S->LightCoarseCellSizeCm));
    const int32 LightH = FMath::Max(1, FMath::CeilToInt32((Bounds.Max.Y - Bounds.Min.Y) / S->LightCoarseCellSizeCm));

    float TallestSpeciesCm = 0.f;
    for (const TObjectPtr<USpeciesData>& Sp : ResolvedSpecies)
    {
        if (Sp) { TallestSpeciesCm = FMath::Max(TallestSpeciesCm, Sp->MaxHeightCm); }
    }
    if (TallestSpeciesCm <= 0.f) { TallestSpeciesCm = 2000.f; } // sin especies: 20 m de cortesia

    const double LightSpanZ = TallestSpeciesCm + S->LightCanopyHeadroomCm + S->LightGroundClearanceCm;
    const int32  LightLayers = FMath::Max(2, FMath::CeilToInt32(LightSpanZ / S->LightCoarseCellSizeCm));
    LightCoarse.Init(LightW, LightH, LightLayers,
        S->LightCoarseCellSizeCm, S->LightCoarseCellSizeCm, Bounds.Min,
        /*BaseZ = offset de la capa 0 bajo el suelo*/ -(double)S->LightGroundClearanceCm);

    // Cota de terreno del centro de cada columna: es lo que convierte la rejilla
    // en relativa al suelo. Se muestrea UNA vez (el relieve no cambia).
    {
        TArray<float> GroundZ;
        GroundZ.SetNumUninitialized(LightW * LightH);
        for (int32 Iy = 0; Iy < LightH; ++Iy)
        {
            const double Yc = Bounds.Min.Y + (Iy + 0.5) * S->LightCoarseCellSizeCm;
            for (int32 Ix = 0; Ix < LightW; ++Ix)
            {
                const double Xc = Bounds.Min.X + (Ix + 0.5) * S->LightCoarseCellSizeCm;
                GroundZ[Iy * LightW + Ix] = HeightField.SampleHeight(Xc, Yc);
            }
        }
        LightCoarse.SetGroundHeights(MoveTemp(GroundZ));
    }

    UE_LOG(LogEco, Log, TEXT("[Eco] Grid de luz %dx%dx%d (%.1f MB, relativo al terreno; especie mas alta %.0f cm)."),
        LightW, LightH, LightLayers, LightCoarse.MemoryBytes() / 1048576.0, TallestSpeciesCm);

    // 6) Spatial hash de agentes: geometria fijada una vez; se repuebla cada tick con Build().
    Hash.Init(Bounds, S->SpatialHashCellSizeCm);

    // 7) Visualizador de campos (heatmap).
    FieldViz = NewObject<UFieldVisualizer>(this);
    FieldViz->Initialize(HeightField.Field.Width, HeightField.Field.Height);

    // A partir de aqui es seguro tickear.
    bWorldReady = true;
}

void UEcosystemSubsystem::Deinitialize()
{
    // Limpieza B13: se sueltan TODAS las referencias, no solo el decal. El GC se
    // encargaria igual (son Transient), pero dejar punteros a objetos de un mundo
    // que ya no existe es una fuente clasica de accesos tardios, y ademas el
    // mismo patron de ReleaseEverything() del subsistema de render.
    ClearHeroTrees();

    if (HeatmapDecal)
    {
        HeatmapDecal->Destroy();
        HeatmapDecal = nullptr;
    }
    HeatmapMID = nullptr;
    FieldViz = nullptr;

    OnStateLoaded.Clear(); // no dejar suscriptores de un mundo que se va

    TickContexts.Reset();
    NewbornPositions.Reset();
    PendingSeeds.Reset();
    PendingDeaths.Reset();
    RecentDeaths.Reset();
    ResolvedSpecies.Reset();
    DebugAgents.Reset();
    bWorldReady = false;

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

    // =====================================================================
    // FASE 6 (doc. 6.4): PRESUPUESTO DE TIEMPO DEL TICK DENTRO DEL FRAME
    // =====================================================================
    // "Fija un objetivo (16.6 ms para 60 fps) y reparte; amortiza los ticks (no
    //  corras un tick por frame si es caro: cadencia + interpolacion de 5.2)."
    //
    // MaxStepsPerFrame ya acotaba el NUMERO de ticks, pero el numero no es lo que
    // hay que repartir: un tick con 200 arboles cuesta microsegundos y con 20.000
    // puede costar varios milisegundos. Aqui se acota el TIEMPO: en cuanto los
    // ticks de este frame se comen su presupuesto, el resto espera al siguiente.
    // El efecto visible es que la simulacion se ralentiza un poco en vez de tirar
    // el framerate al suelo, que es exactamente el comportamiento que se quiere
    // durante una demo o una captura.
    const UEcosystemSettings* Settings = UEcosystemSettings::Get();
    const double BudgetMs = FMath::Max(0.f, Settings->TickBudgetMsPerFrame);
    const double FrameT0 = FPlatformTime::Seconds();
    auto OverBudget = [BudgetMs, FrameT0]() -> bool
        {
            return BudgetMs > 0.0 && (FPlatformTime::Seconds() - FrameT0) * 1000.0 >= BudgetMs;
        };

    TicksLastFrame = 0;

    // Pasos manuales (Eco.Step): se ejecutan aunque este pausado, pero AMORTIZADOS
    // (correccion B10). Antes se vaciaba PendingSteps entero en un frame mientras
    // que el avance automatico si estaba capado: un `Eco.Step 500` -que es
    // exactamente el flujo de "bake a un año objetivo" del Paso 6- congelaba el
    // editor varios segundos sin ningun feedback. Ahora se reparten por frames y
    // se loguea el progreso.
    if (PendingSteps > 0)
    {
        const int32 StepCap = FMath::Max(1, MaxStepsPerFrame);
        int32 Done = 0;
        while (PendingSteps > 0 && Done < StepCap)
        {
            SimulateTick(YearsPerTick);
            ++TickCount;
            --PendingSteps;
            ++Done;
            ++TicksLastFrame;

            // Al menos UNO por frame siempre: si no, con un presupuesto muy
            // apretado un Eco.Step no avanzaria nunca.
            if (OverBudget()) { break; }
        }

        if (PendingSteps > 0 && (PendingSteps % 50) < StepCap)
        {
            UE_LOG(LogEco, Log, TEXT("[Eco] Eco.Step: quedan %d ticks (tick actual %lld, %d arboles)."),
                PendingSteps, TickCount, Agents_Read.Num());
        }
    }

    // Avance automatico (modo vivo).
    if (!bPaused)
    {
        Accumulator += DeltaTime;

        // Tope del acumulador (Fase 6): tras un hitch -compilar shaders, hornear
        // la libreria, cargar un bake- el acumulador podia quedarse con varios
        // segundos de deuda y hacer que la simulacion corriese a MaxStepsPerFrame
        // durante minutos, con el framerate hundido, sin que nadie entendiera por
        // que. La deuda que no se puede pagar se descarta: el tiempo simulado se
        // ralentiza un instante, que es preferible a arrastrar el problema.
        const double MaxDebt = static_cast<double>(SecondsPerTick) * FMath::Max(1, MaxStepsPerFrame);
        if (Accumulator > MaxDebt)
        {
            Accumulator = MaxDebt;
        }

        int32 Steps = 0;
        while (Accumulator >= SecondsPerTick && Steps < MaxStepsPerFrame)
        {
            SimulateTick(YearsPerTick);
            ++TickCount;
            Accumulator -= SecondsPerTick;
            ++Steps;
            ++TicksLastFrame;

            if (OverBudget()) { break; }
        }
    }

    // Fase 6 (6.4): contadores para `stat EcoSim` y para el CSV profiler.
    SET_DWORD_STAT(STAT_EcoPopulation, Agents_Read.Num());
    SET_DWORD_STAT(STAT_EcoTicksThisFrame, TicksLastFrame);
    CSV_CUSTOM_STAT(Eco, TickMs, (float)Profile.TotalMs, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, TicksPerFrame, TicksLastFrame, ECsvCustomStatOp::Set);

    DrawDebug();
}

TStatId UEcosystemSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UEcosystemSubsystem, STATGROUP_Tickables);
}

float UEcosystemSubsystem::GetInterpolationAlpha() const
{
    return SecondsPerTick > 0.f ? FMath::Clamp(static_cast<float>(Accumulator) / SecondsPerTick, 0.f, 1.f) : 0.f;
}

// ---------------------------------------------------------------------------
//  Bucle de tick (Fase 2)
// ---------------------------------------------------------------------------

// Los factores de forma de copa (CanopyRadiusFraction/CanopyShadowDensity) y la
// biomasa de germinacion (GerminationBiomassFraction) ya NO son constantes de
// este .cpp: entran en el bucle de luz y en la germinacion -alteran el resultado
// ecologico-, asi que viven en UEcosystemSettings como parte de la configuracion
// reproducible del proyecto.

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
    // Fase 6 (6.4): el tick entero como un bloque nombrado. Sale en `stat EcoSim`
    // y como un bloque en la timeline de Unreal Insights, anidado dentro del
    // frame: asi se ve de un vistazo si el tick es el que se lleva el frame o si
    // es ruido al lado del render (que es lo que el doc. 6.4 dice que sera).
    SCOPE_CYCLE_COUNTER(STAT_EcoTickTotal);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_SimulateTick);

    const UEcosystemSettings* Settings = UEcosystemSettings::Get();
    const double TickT0 = FPlatformTime::Seconds();

    // Fase 6 (6.3): parametros de CO2, resueltos UNA vez por tick y capturados
    // por valor en el lambda paralelo (nada de tocar settings ni cvars desde
    // dentro del ParallelFor).
    const EcoCarbon::FCO2Params CO2 = GetCO2Params();

    // ================================================================
    // PRE (serial): estructuras derivadas del snapshot de lectura.
    // ================================================================
    // El hash lo consume el espaciado minimo de germinacion del paso 3 y, a
    // partir de la Fase 3, las consultas por rango del SCA. La competencia por
    // recursos NO pasa por el: se resuelve a traves de los campos compartidos
    // (consumo de agua/nutrientes + sombra de luz).
    {
        SCOPE_CYCLE_COUNTER(STAT_EcoHash);
        TRACE_CPUPROFILER_EVENT_SCOPE(Eco_BuildHash);
        Hash.Build(Agents_Read.Position, Agents_Read.Num());
    }
    const double AfterHash = FPlatformTime::Seconds();

    // Cadencia de la luz gruesa (optimizacion C6): por defecto cada tick, que es
    // el comportamiento exacto. Subir LightRebuildEveryNTicks ahorra la pasada
    // serial a cambio de que las copas tarden en proyectar su sombra nueva.
    const int32 LightEvery = FMath::Max(1, Settings->LightRebuildEveryNTicks);
    if ((TickCount % LightEvery) == 0)
    {
        SCOPE_CYCLE_COUNTER(STAT_EcoLight);
        TRACE_CPUPROFILER_EVENT_SCOPE(Eco_CoarseLight);
        RebuildCoarseLight();
    }
    const double AfterLight = FPlatformTime::Seconds();

    Agents_Write.CopyFrom(Agents_Read);
    WaterPool.BeginTick();
    NutrientPool.BeginTick();

    const int32 NumChunks = PrepareTickScratch(*Settings);

    // ================================================================
    // PASO 2 (paralelo): crecimiento/estres/mortalidad/semillas por chunk.
    // ================================================================
    RunGrowthParallel(DtYears, *Settings, CO2, NumChunks);

    const double AfterParallel = FPlatformTime::Seconds();

    // ================================================================
    // PASO 3 (serial): reduccion -> regeneracion -> pulsos de muerte -> germinacion.
    // ================================================================
    // PendingSeeds/PendingDeaths son MIEMBROS (C5): ReduceScratchInto los hace
    // Reset(), asi que conservan la capacidad de ticks anteriores y una oleada de
    // germinacion no vuelve a pedir memoria al heap.
    {
        SCOPE_CYCLE_COUNTER(STAT_EcoReduce);
        TRACE_CPUPROFILER_EVENT_SCOPE(Eco_Reduce);
        EcologyRules::ReduceScratchInto(TickContexts, WaterPool.Next.Data, NutrientPool.Next.Data, PendingSeeds, PendingDeaths);
    }
    const double AfterReduce = FPlatformTime::Seconds();

    {
        SCOPE_CYCLE_COUNTER(STAT_EcoRegen);
        TRACE_CPUPROFILER_EVENT_SCOPE(Eco_Regen);
        WaterPool.RegenerateTowardBase(WaterBase.Field, Settings->WaterRechargeRate, Settings->WaterDiffusionRate, DtYears);
        NutrientPool.RegenerateTowardBase(NutrientBase.Field, Settings->NutrientRechargeRate, Settings->NutrientDiffusionRate, DtYears);
    }
    const double AfterRegen = FPlatformTime::Seconds();

    // Fase 6 (6.4): pulsos de muerte + germinacion, la ultima etapa del tick.
    SCOPE_CYCLE_COUNTER(STAT_EcoGermination);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_Germination);

    ApplyDeathPulses(DtYears, *Settings);
    RunGermination(DtYears, *Settings, CO2);

    // ================================================================
    // PASO 4: compactar muertos e intercambiar buffers (agentes y campos).
    // ================================================================
    Agents_Write.CompactDead();

    Swap(Agents_Read, Agents_Write);
    WaterPool.SwapBuffers();
    NutrientPool.SwapBuffers();

    // Instrumentacion (Eco.Profile): media exponencial del coste de cada etapa.
    // Es lo que el doc. 6.4 exige tener ANTES de decidir que se optimiza.
    const double TickT1 = FPlatformTime::Seconds();
    FEcoTickProfile::Accumulate(Profile.HashMs, (AfterHash - TickT0) * 1000.0);
    FEcoTickProfile::Accumulate(Profile.LightMs, (AfterLight - AfterHash) * 1000.0);
    FEcoTickProfile::Accumulate(Profile.ParallelMs, (AfterParallel - AfterLight) * 1000.0);
    FEcoTickProfile::Accumulate(Profile.ReduceMs, (AfterReduce - AfterParallel) * 1000.0);
    FEcoTickProfile::Accumulate(Profile.RegenMs, (AfterRegen - AfterReduce) * 1000.0);
    FEcoTickProfile::Accumulate(Profile.GerminationMs, (TickT1 - AfterRegen) * 1000.0);
    FEcoTickProfile::Accumulate(Profile.TotalMs, (TickT1 - TickT0) * 1000.0);

    if ((TickCount % 20) == 0)
    {
        LogPopulationStats();
    }
}

// ---------------------------------------------------------------------------
//  Etapas de SimulateTick (el tick queda como orquestador; ver el .h)
// ---------------------------------------------------------------------------
int32 UEcosystemSubsystem::PrepareTickScratch(const UEcosystemSettings& Settings)
{
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
    const int32 GrainSize = FMath::Max(1, Settings.TickChunkGrainSize);
    const int32 NumChunks = FMath::Clamp(FMath::DivideAndRoundUp(PopNum, GrainSize), 1, kMaxTickChunks);

    // Scratch PERSISTENTE (miembro) y DISPERSO (optimizacion C1): cada tarea
    // acumula pares (celda, cantidad) en vez de un campo denso del tamano del
    // mundo. Ver la nota larga en FCellDelta: la version densa gastaba ~64 MB y
    // 16,8 M sumas seriales por tick para mover ~360.000 valores reales.
    // Aqui solo se vacian los buffers (Reset conserva la capacidad) y se reserva
    // de una vez lo que se espera depositar, para que los Add() no realojen.
    float MaxRootRadiusCm = 0.f;
    for (const TObjectPtr<USpeciesData>& Sp : ResolvedSpecies)
    {
        if (Sp) { MaxRootRadiusCm = FMath::Max(MaxRootRadiusCm, Sp->RootRadius * 100.f); }
    }
    const int32 CellsPerTree = EcologyRules::KernelCellCount(WaterBase.Field, MaxRootRadiusCm);
    const int32 TreesPerChunk = FMath::DivideAndRoundUp(PopNum, FMath::Max(1, NumChunks));

    TickContexts.SetNum(NumChunks);
    for (FTickScratch& Ctx : TickContexts)
    {
        Ctx.ResetForNextTick();
        Ctx.ReserveForTrees(TreesPerChunk, CellsPerTree);
    }
    return NumChunks;
}

void UEcosystemSubsystem::RunGrowthParallel(float DtYears, const UEcosystemSettings& Settings,
    const EcoCarbon::FCO2Params& CO2, int32 NumChunks)
{
    // Cada chunk SOLO lee del snapshot (Agents_Read, WaterPool.Current,
    // NutrientPool.Current, LightCoarse) y SOLO escribe en su porcion de
    // Agents_Write y en su propio FTickScratch.
    const int32 PopNum = Agents_Read.Num();

    const EParallelForFlags Flags = CVarForceST.GetValueOnGameThread()
        ? EParallelForFlags::ForceSingleThread : EParallelForFlags::None;

    // Fase 6 (6.4): el ambito envuelve al ParallelFor ENTERO (el reparto y la
    // espera), no a cada tarea. Es lo que interesa medir: el tiempo de pared que
    // el game thread se queda aqui bloqueado.
    {
        SCOPE_CYCLE_COUNTER(STAT_EcoParallel);
        TRACE_CPUPROFILER_EVENT_SCOPE(Eco_TickParallel);
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
                    uint32& RngState = Agents_Write.RngState[i]; // stream propio -> determinista

                    // 2a) recursos locales
                    const float W = WaterPool.SampleCurrent(P.X, P.Y);
                    const float N = NutrientPool.SampleCurrent(P.X, P.Y);
                    const float Q = LightCoarse.SampleLightSmooth(P);

                    // 2b) factores + vigor (Liebig)
                    const float fL = EcologyRules::LightFactor(Q, Sp->ShadeTolerance, Settings.LightHalfSaturationMax);
                    const float fW = EcologyRules::DemandFactor(W, Sp->WaterDemand);
                    const float fN = EcologyRules::DemandFactor(N, Sp->NutrientDemand);

                    // FASE 6 (doc. 6.3): CO2 como multiplicador analitico del vigor.
                    // Coste: dos clamps y una multiplicacion, con dos valores que ya
                    // estaban en registros (Q y la altura cacheada). Ni un campo nuevo
                    // ni un muestreo extra, que es exactamente lo que pide el
                    // documento ("mismo sabor, coste ~0").
                    //
                    // La altura se toma del SNAPSHOT DE LECTURA (la del principio del
                    // tick), no de la que se acaba de calcular: el vigor decide cuanto
                    // crece el arbol, asi que no puede depender de lo que ha crecido.
                    // Ademas mantiene la regla de la Fase 2 de leer solo del snapshot.
                    const float CO2Factor = EcoCarbon::CO2Factor(Q, Agents_Read.Height[i], CO2);
                    const float VigorValue = EcologyRules::Vigor(fL, fW, fN) * CO2Factor;

                    // 2c) crecimiento + altura + edad + estado (Sapling/Mature/Senescent, Fase 5 Paso 1)
                    const float NewAge = Agents_Read.Age[i] + DtYears;

                    // Senescencia: se decide con el estres del tick ANTERIOR
                    // (Agents_Read.Stress) para no depender del estres que aun no se
                    // ha calculado este tick -> el orden sigue siendo determinista.
                    //
                    // Y es una puerta de UN SOLO SENTIDO: IsSenescent es funcion pura del
                    // estres ACTUAL, asi que por si sola haria que un arbol que se recupera
                    // volviese de Senescent a Mature, cosa que biologicamente no ocurre
                    // (el declive no se revierte). Se hace "pegajosa" leyendo el estado del
                    // snapshot de lectura, que es inmutable durante todo el tick: sigue
                    // siendo determinista bajo paralelismo.
                    const bool bSenescent =
                        (Agents_Read.State[i] == ETreeState::Senescent) ||
                        EcologyRules::IsSenescent(NewAge, Sp->Longevity, Sp->SenescenceAgeFraction,
                            Agents_Read.Stress[i], Sp->SenescenceStressThreshold);

                    // En declive el arbol casi deja de crecer.
                    const float EffGrowthRate = Sp->GrowthRate *
                        EcologyRules::SenescentGrowthFactor(bSenescent, Sp->SenescentGrowthScale);
                    const float NewBiomass = EcologyRules::GrowBiomassLogistic(
                        Agents_Read.Biomass[i], VigorValue, EffGrowthRate, Sp->MaxBiomass, DtYears);

                    Agents_Write.Biomass[i] = NewBiomass;
                    Agents_Write.Height[i] = EcologyRules::HeightFromBiomass(NewBiomass, Sp->MaxBiomass, Sp->MaxHeightCm);
                    Agents_Write.Age[i] = NewAge;
                    Agents_Write.State[i] = bSenescent ? ETreeState::Senescent
                        : (NewAge >= Sp->MaturityAge ? ETreeState::Mature : ETreeState::Sapling);

                    // 2d) estres
                    Agents_Write.Stress[i] = EcologyRules::UpdateStress(Agents_Read.Stress[i], VigorValue,
                        Settings.StressVigorThreshold, Settings.StressAccumulationRate,
                        Settings.StressRecoveryRate, DtYears);

                    // 2e) consumo -> SOLO al scratch de este chunk, en forma DISPERSA
                    //     (lista de (celda, cantidad), ver C1 en FCellDelta).
                    const float RootRadiusCm = EcologyRules::EffectiveRootRadiusCm(Sp->RootRadius, NewBiomass, Sp->MaxBiomass);
                    EcologyRules::DepositKernelSparse(WaterBase.Field, Ctx.WaterDeltas, P, RootRadiusCm,
                        -NewBiomass * Sp->WaterDemand * DtYears);
                    EcologyRules::DepositKernelSparse(NutrientBase.Field, Ctx.NutrientDeltas, P, RootRadiusCm,
                        -NewBiomass * Sp->NutrientDemand * DtYears);

                    // 2f) mortalidad (probabilistica, con el RNG propio del arbol).
                    //     La senescencia multiplica la probabilidad de morir (Fase 5 Paso 1).
                    float pDeath = EcologyRules::MortalityProbability(Agents_Write.Age[i], Sp->Longevity,
                        Agents_Write.Stress[i], Settings.StressMortalityWeight, DtYears);
                    pDeath = EcologyRules::ApplySenescentMortality(pDeath, bSenescent, Sp->SenescentMortalityMultiplier);

                    if (EcoRand::NextUnit(RngState) < pDeath)
                    {
                        Agents_Write.State[i] = ETreeState::Dead;

                        FPendingDeathPulse Pulse;
                        Pulse.Position = P;
                        Pulse.RadiusCm = RootRadiusCm;
                        Pulse.Amount = EcologyRules::DeathNutrientPulse(NewBiomass, Settings.NutrientDecompositionFactor);
                        // Fase 5: datos para la caida/tocon/hojarasca del render.
                        Pulse.SpeciesId = Agents_Read.SpeciesId[i];
                        Pulse.StableId = Agents_Read.StableId[i];
                        Pulse.Biomass = NewBiomass;
                        Pulse.HeightCm = Agents_Write.Height[i];
                        Ctx.DeathPulses.Add(Pulse);
                    }
                    else if (NewAge >= Sp->MaturityAge &&
                        (Agents_Write.State[i] == ETreeState::Mature ||
                            Agents_Write.State[i] == ETreeState::Senescent))
                    {
                        // 2g) semillas (solo si sigue vivo y ha alcanzado la madurez).
                        //     El senescente SIGUE reproduciendose, con menos fuerza: cortarlo
                        //     a cero eliminaba la ventana de maxima fecundidad y cambiaba en
                        //     silencio el balance de sucesion que ajusto la Fase 2.
                        //     La comprobacion explicita de MaturityAge hace falta ahora porque
                        //     un SAPLING muy estresado tambien entra en Senescent, y una
                        //     plantula no se reproduce.
                        const float SeedScale = (Agents_Write.State[i] == ETreeState::Senescent)
                            ? FMath::Clamp(Sp->SenescentSeedScale, 0.f, 1.f)
                            : 1.f;
                        const int32 NumSeeds = EcologyRules::ComputeSeedCount(
                            Settings.SeedRatePerBiomass * SeedScale, NewBiomass, DtYears, RngState);
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
            }, Flags);
    } // fin del ambito de STAT_EcoParallel
}

void UEcosystemSubsystem::ApplyDeathPulses(float DtYears, const UEcosystemSettings& Settings)
{
    // Fase 5 (Paso 5): envejece las manchas de descomposicion existentes
    // (decaimiento exponencial) antes de sumar las de este tick.
    if (DecompositionField.IsValid())
    {
        const float Decay = FMath::Exp(-Settings.DecompositionDecayPerYear * DtYears);
        for (float& V : DecompositionField.Data) { V *= Decay; }
    }

    for (const FPendingDeathPulse& Pulse : PendingDeaths)
    {
        EcologyRules::DepositKernel(NutrientBase.Field, NutrientPool.Next.Data, Pulse.Position, Pulse.RadiusCm, Pulse.Amount);
        RecordDeathEvent(Pulse); // Fase 5 (Paso 1/4): alimenta la capa de suelo
        // Fase 5 (Paso 5): mancha de descomposicion visible en el terreno.
        if (DecompositionField.IsValid())
        {
            EcologyRules::DepositKernel(DecompositionField, DecompositionField.Data,
                Pulse.Position, Pulse.RadiusCm, Pulse.Amount * Settings.DecompositionPulseScale);
        }
    }

}

void UEcosystemSubsystem::RunGermination(float DtYears, const UEcosystemSettings& Settings,
    const EcoCarbon::FCO2Params& CO2)
{
    // Reserva una sola vez: PendingSeeds.Num() es la cota superior de germinaciones.
    // Evita realojos de los 8 arrays SoA durante los Add() de abajo.
    Agents_Write.Reserve(Agents_Write.Num() + PendingSeeds.Num());

    const FBox2D WorldBounds = HeightField.GetWorldBounds();
    const double MinSpacingSq = FMath::Square((double)Settings.MinGerminationSpacingCm);
    NewbornPositions.Reset();

    // Distancia XY al cuadrado contra el umbral de espaciado. Estaba escrita dos
    // veces con las mismas cuatro lineas (una contra los vecinos del hash y otra
    // contra las plantulas nacidas en este mismo tick), asi que el criterio podia
    // divergir entre ambas comprobaciones.
    auto IsTooClose = [MinSpacingSq](const FVector& A, const FVector& B) -> bool
        {
            const double dx = A.X - B.X;
            const double dy = A.Y - B.Y;
            return dx * dx + dy * dy < MinSpacingSq;
        };

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

        // Espaciado minimo: no germinar pegada a un arbol ya vivo. Es el consumidor
        // principal del spatial hash. El hash indexa Agents_Read; consultamos
        // Agents_Write.State para NO dejar que un arbol muerto ESTE tick bloquee el
        // hueco que acaba de liberar. El booleano no depende del orden de visita ->
        // sigue siendo determinista.
        bool bTooClose = false;
        Hash.ForEachNeighbor(GerminationPos, Settings.MinGerminationSpacingCm,
            [&](int32 NeighborIdx)
            {
                if (bTooClose) { return; }
                if (Agents_Write.State[NeighborIdx] == ETreeState::Dead) { return; }
                if (IsTooClose(Agents_Read.Position[NeighborIdx], GerminationPos))
                {
                    bTooClose = true;
                }
            });

        // El hash se construyo sobre Agents_Read y NO contiene las plantulas que han
        // germinado en este mismo bucle, asi que hay que comprobarlas aparte o dos
        // semillas del mismo tick germinan pegadas. El bucle es serial y de orden
        // fijo, asi que el resultado sigue siendo determinista.
        // (Escaneo lineal: son unas pocas decenas-cientos por tick; si algun dia
        //  crece, mete estas posiciones en un mini-hash.)
        if (!bTooClose)
        {
            for (const FVector& NP : NewbornPositions)
            {
                if (IsTooClose(NP, GerminationPos)) { bTooClose = true; break; }
            }
        }
        if (bTooClose) { continue; }

        const float LightHere = LightCoarse.SampleLightSmooth(GerminationPos);
        if (!EcologyRules::IsSafeGerminationSite(LightHere, Settings.MinLightForGermination))
        {
            continue; // sitio no seguro: la semilla no prospera, se descarta
        }

        const float WHere = WaterPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);
        const float NHere = NutrientPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);
        const float fL = EcologyRules::LightFactor(LightHere, Sp->ShadeTolerance, Settings.LightHalfSaturationMax);
        const float fW = EcologyRules::DemandFactor(WHere, Sp->WaterDemand);
        const float fN = EcologyRules::DemandFactor(NHere, Sp->NutrientDemand);
        // Fase 6: la semilla cae al SUELO, o sea altura de copa 0: es donde el
        // termino de CO2 pesa mas (aire poco mezclado bajo un dosel cerrado).
        // Aplicarlo tambien aqui es lo que hace que el efecto tenga consecuencias
        // ecologicas -menos reclutamiento bajo dosel denso- y no sea solo un
        // adorno sobre el crecimiento.
        const float VigorHere = EcologyRules::Vigor(fL, fW, fN)
            * EcoCarbon::CO2Factor(LightHere, /*CanopyHeightCm*/ 0.f, CO2);

        uint32 SeedRng = Seed.RngSeed;
        const float pGerm = EcologyRules::GerminationProbability(VigorHere, Settings.GerminationRate);
        if (EcoRand::NextUnit(SeedRng) < pGerm)
        {
            Agents_Write.Add(GerminationPos, Seed.SpeciesId, SeedRng, /*Age*/ 0.f, /*Biomass*/ Sp->MaxBiomass * Settings.GerminationBiomassFraction);
            NewbornPositions.Add(GerminationPos);
        }
    }

}

void UEcosystemSubsystem::RebuildCoarseLight()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();

    LightCoarse.ClearShadow();
    for (int32 i = 0; i < Agents_Read.Num(); ++i)
    {
        if (Agents_Read.State[i] == ETreeState::Dead) { continue; }
        const USpeciesData* Sp = ResolveSpecies(Agents_Read.SpeciesId[i]);
        if (!Sp) { continue; }

        const float H = Agents_Read.Height[i];
        const FVector Apex = Agents_Read.Position[i] + FVector(0.f, 0.f, H);
        // Radio/profundidad de copa: aproximacion hasta que la Fase 3 aporte
        // geometria real. Config (no literales): altera el resultado ecologico.
        LightCoarse.DepositCanopyShadow(Apex, H * S->CanopyRadiusFraction, H, S->CanopyShadowDensity);
    }
}
const USpeciesData* UEcosystemSubsystem::ResolveSpecies(uint16 SpeciesId) const
{
    return ResolvedSpecies.IsValidIndex(SpeciesId) ? ResolvedSpecies[SpeciesId] : nullptr;
}

// ---------------------------------------------------------------------------
//  Poblacion (Fase 2)
// ---------------------------------------------------------------------------
/**
 * Punto aleatorio sobre el terreno: XY uniforme dentro de los limites del
 * relieve y Z muestreada del propio relieve.
 *
 * Lo hacian por su cuenta -con las mismas dos Lerp y el mismo SampleHeight-
 * SeedInitialPopulation y AddRandomDebugAgent. El stream se pasa por parametro
 * precisamente porque NO es el mismo en los dos: la siembra gasta Colonization y
 * las herramientas de debug gastan Debug, para que depurar no altere el bosque.
 *
 * Consume DOS valores del stream, en orden X e Y: es el orden que tenian las dos
 * copias, y cambiarlo cambiaria el bosque de una semilla dada.
 */
FVector UEcosystemSubsystem::RandomPointOnTerrain(EEcoRngStream Stream)
{
    const FBox2D B = HeightField.GetWorldBounds();
    const double X = FMath::Lerp(B.Min.X, B.Max.X, (double)Rng.Unit(Stream));
    const double Y = FMath::Lerp(B.Min.Y, B.Max.Y, (double)Rng.Unit(Stream));
    return FVector(X, Y, HeightField.SampleHeight(X, Y));
}

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

    Agents_Read.Reserve(Agents_Read.Num() + Count);

    for (int32 i = 0; i < Count; ++i)
    {
        // Stream Colonization (Fase 0): coloca el bosque inicial sin tocar
        // los streams de mortalidad/dispersion/morfologia de la simulacion.
        const FVector Site = RandomPointOnTerrain(EEcoRngStream::Colonization);

        const int32 SpeciesIdx = Rng.RangeI(EEcoRngStream::Colonization, 0, ResolvedSpecies.Num() - 1);
        const USpeciesData* Sp = ResolvedSpecies[SpeciesIdx];
        if (!Sp) { continue; }

        const uint32 AgentSeed = Rng.U32(EEcoRngStream::Colonization);

        // EDAD ESCALONADA, no toda la cohorte a cero.
        //
        // Con Age = 0 para todos, la poblacion fundadora es una unica cohorte que
        // envejece en bloque y muere en bloque: no se nota al arrancar, pero
        // alrededor de la mediana de muerte (~1.28*Longevity^0.8 años) se abre un
        // claro simultaneo en todo el mapa y el bosque se reinicia solo. Con la
        // longevidad recalibrada eso pasaria a los ~200 años simulados, o sea
        // justo cuando uno se pone a mirar el bosque maduro.
        //
        // El tope 0.35 deja a los fundadores repartidos por la mitad joven de la
        // curva: hay adultos desde el primer momento, pero ninguno arranca ya
        // senescente (la senescencia entra en 0.40*Longevity).
        const float InitialAge = Rng.RangeF(EEcoRngStream::Colonization, 0.f, 0.35f * Sp->Longevity);

        // La biomasa acompaña a la edad: un fundador de 150 años con biomasa de
        // plantula seria un arbol viejo del tamano de un arbusto durante las
        // primeras decadas, y ademas sombrearia como tal (el grid de luz lee
        // Height, que sale de Biomass).
        const float AgeRatio = FMath::Clamp(InitialAge / FMath::Max(Sp->Longevity, KINDA_SMALL_NUMBER), 0.f, 1.f);
        const float InitialBiomass = FMath::Min(
            Sp->MaxBiomass * (Rng.RangeF(EEcoRngStream::Colonization, 0.005f, 0.03f) + 1.6f * AgeRatio),
            Sp->MaxBiomass);

        Agents_Read.Add(Site, static_cast<uint16>(SpeciesIdx), AgentSeed, InitialAge, InitialBiomass);
    }

    UE_LOG(LogEco, Log, TEXT("[Eco] Sembrados %d arboles con edades escalonadas (poblacion total: %d)."),
        Count, Agents_Read.Num());
}
// ---------------------------------------------------------------------------
//  Hero trees (Fase 3)
// ---------------------------------------------------------------------------
AHeroTreeActor* UEcosystemSubsystem::SpawnHeroTree(const FVector& WorldPos, int32 SpeciesIndex, uint32 Seed)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }
    if (!HeightField.IsValid())
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] GrowHeroTree: el relieve aun no esta listo."));
        return nullptr;
    }
    if (ResolvedSpecies.Num() == 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] GrowHeroTree: no hay especies en Project Settings -> Procedural Ecosystem."));
        return nullptr;
    }

    const int32 Idx = FMath::Clamp(SpeciesIndex, 0, ResolvedSpecies.Num() - 1);
    const USpeciesData* Sp = ResolvedSpecies[Idx];
    if (!Sp)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] GrowHeroTree: la especie %d es nula."), Idx);
        return nullptr;
    }

    AHeroTreeActor* Actor = World->SpawnActor<AHeroTreeActor>(WorldPos, FRotator::ZeroRotator);
    if (!Actor)
    {
        return nullptr;
    }

    // La luz gruesa da el contexto de vecinos (sombra) para el SCA.
    Actor->Generate(Sp, Seed, &LightCoarse, WorldPos);
    HeroTrees.Add(Actor);

    UE_LOG(LogEco, Log, TEXT("[Eco] Hero tree '%s' generado en (%.0f, %.0f) con semilla %u | %d nodos."),
        *Sp->SpeciesName.ToString(), WorldPos.X, WorldPos.Y, Seed, Actor->GetNodeCount());
    return Actor;
}

void UEcosystemSubsystem::ClearHeroTrees()
{
    for (const TObjectPtr<AHeroTreeActor>& A : HeroTrees)
    {
        if (A)
        {
            A->Destroy();
        }
    }
    HeroTrees.Reset();
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
        TickCount, GetLivePopulationCount(), *Breakdown);
}

void UEcosystemSubsystem::LogDemographics() const
{
    if (Agents_Read.Num() == 0)
    {
        UE_LOG(LogEco, Log, TEXT("[Eco] Demografia | tick %lld: no hay arboles."), TickCount);
        return;
    }

    struct FSpeciesDemo
    {
        int32 Saplings = 0, Mature = 0, Senescent = 0;
        int32 Grown = 0;      // >= 70% de MaxBiomass: los que "llenan" el bosque
        double AgeSum = 0.0;
        float  AgeMax = 0.f;
    };

    TArray<FSpeciesDemo> BySpecies;
    BySpecies.SetNum(ResolvedSpecies.Num());

    int32 Counted = 0;
    for (int32 i = 0; i < Agents_Read.Num(); ++i)
    {
        if (Agents_Read.State[i] == ETreeState::Dead) { continue; }

        const int32 s = Agents_Read.SpeciesId[i];
        if (!BySpecies.IsValidIndex(s)) { continue; }

        const USpeciesData* Sp = ResolveSpecies(static_cast<uint16>(s));
        if (!Sp) { continue; }

        FSpeciesDemo& D = BySpecies[s];
        switch (Agents_Read.State[i])
        {
        case ETreeState::Sapling:   ++D.Saplings;  break;
        case ETreeState::Mature:    ++D.Mature;    break;
        case ETreeState::Senescent: ++D.Senescent; break;
        default: break;
        }

        const float Age = Agents_Read.Age[i];
        D.AgeSum += Age;
        D.AgeMax = FMath::Max(D.AgeMax, Age);
        if (Agents_Read.Biomass[i] >= 0.7f * Sp->MaxBiomass) { ++D.Grown; }
        ++Counted;
    }

    UE_LOG(LogEco, Log, TEXT("[Eco] Demografia | tick %lld | %d arboles vivos | %lld muertes acumuladas"),
        TickCount, Counted, DeathEventCounter);

    for (int32 s = 0; s < BySpecies.Num(); ++s)
    {
        const FSpeciesDemo& D = BySpecies[s];
        const int32 Live = D.Saplings + D.Mature + D.Senescent;
        if (Live == 0) { continue; }

        const USpeciesData* Sp = ResolveSpecies(static_cast<uint16>(s));
        const float InvLive = 1.f / static_cast<float>(Live);

        // "Mediana teorica" = 1.282*L^0.8, la edad a la que la mortalidad por edad
        // se lleva a la mitad de una cohorte (ver USpeciesData::Longevity). Se
        // imprime al lado de la edad maxima observada porque juntas dicen de un
        // vistazo si el bosque esta llegando donde deberia.
        const float MedianTarget = Sp ? 1.282f * FMath::Pow(FMath::Max(Sp->Longevity, 1.f), 0.8f) : 0.f;

        UE_LOG(LogEco, Log,
            TEXT("  %-16s n=%4d | plantula %4.1f%% adulto %4.1f%% senescente %4.1f%% | crecidos %4.1f%% | edad media %6.1f max %6.1f (mediana esperada %6.1f)"),
            Sp ? *Sp->SpeciesName.ToString() : TEXT("?"),
            Live,
            100.f * D.Saplings * InvLive,
            100.f * D.Mature * InvLive,
            100.f * D.Senescent * InvLive,
            100.f * D.Grown * InvLive,
            static_cast<float>(D.AgeSum) * InvLive,
            D.AgeMax,
            MedianTarget);
    }
}

// ---------------------------------------------------------------------------
//  Eventos de muerte (Fase 5, Paso 1): anillo que consume la capa de suelo
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::RecordDeathEvent(const FPendingDeathPulse& Pulse)
{
    const int32 Cap = RecentDeaths.Num();
    if (Cap == 0) { return; }

    FTreeDeathEvent Ev;
    Ev.Position = Pulse.Position;
    Ev.SpeciesId = Pulse.SpeciesId;
    Ev.StableId = Pulse.StableId;
    Ev.Biomass = Pulse.Biomass;
    Ev.HeightCm = Pulse.HeightCm;
    Ev.Tick = TickCount;

    RecentDeaths[static_cast<int32>(DeathEventCounter % Cap)] = Ev;
    ++DeathEventCounter;
}

void UEcosystemSubsystem::CollectNewDeathEvents(int64& InOutCursor, TArray<FTreeDeathEvent>& Out) const
{
    const int32 Cap = RecentDeaths.Num();   // mismo modulo que RecordDeathEvent
    if (Cap == 0) { InOutCursor = DeathEventCounter; return; }

    // Solo estan disponibles las ultimas Cap muertes (el anillo pisa las viejas).
    // From nunca baja de DeathEventCounter-Cap, asi que jamas se lee una ranura
    // que aun no se ha escrito (el array esta predimensionado con eventos vacios).
    const int64 From = FMath::Max<int64>(InOutCursor, DeathEventCounter - Cap);
    for (int64 g = From; g < DeathEventCounter; ++g)
    {
        Out.Add(RecentDeaths[static_cast<int32>(g % Cap)]);
    }
    InOutCursor = DeathEventCounter;
}

void UEcosystemSubsystem::LogRecentDeaths() const
{
    const int32 Cap = RecentDeaths.Num();
    // El anillo esta PREDIMENSIONADO: Num() es la capacidad, no cuantas muertes hay.
    const int64 Available = FMath::Min<int64>(DeathEventCounter, Cap);
    UE_LOG(LogEco, Log, TEXT("[Eco/F5] Muertes totales: %lld | disponibles en el anillo: %lld/%d"),
        DeathEventCounter, Available, Cap);
    const int32 Show = static_cast<int32>(FMath::Min<int64>(5, Available));
    for (int32 k = 0; k < Show; ++k)
    {
        const int64 g = DeathEventCounter - 1 - k;
        if (g < 0) { break; }
        const FTreeDeathEvent& Ev = RecentDeaths[static_cast<int32>(g % Cap)];
        const USpeciesData* Sp = ResolveSpecies(Ev.SpeciesId);
        UE_LOG(LogEco, Log, TEXT("  #%lld %s en (%.0f, %.0f) biomasa=%.1f altura=%.0f tick=%lld"),
            g, Sp ? *Sp->SpeciesName.ToString() : TEXT("?"),
            Ev.Position.X, Ev.Position.Y, Ev.Biomass, Ev.HeightCm, Ev.Tick);
    }
}

// ---------------------------------------------------------------------------
//  Bake a un año objetivo (Fase 5, Paso 6): guardar / cargar el estado completo
// ---------------------------------------------------------------------------
//
// Se serializa la UNICA fuente de verdad (la poblacion) + el estado runtime de
// los campos (pools de agua/nutrientes y descomposicion) + los streams de RNG +
// el contador de ticks. Los campos BASE (relieve, agua/nutrientes potenciales,
// luz) NO se guardan: son deterministas a partir de la semilla maestra y se
// regeneran identicos en OnWorldBeginPlay. Por eso un bake solo cuadra con la
// misma MasterSeed/ajustes de relieve -- y por eso hay que COMPROBARLO al cargar.
//
// El anillo de muertes (RecentDeaths/DeathEventCounter) NO se serializa a
// proposito: es un buffer de eventos para la capa de vista, no estado del bosque.
// Tras cargar, la capa de suelo se vacia (OnStateLoaded -> Clear) y recoloca su
// cursor, asi que no hay nada que restaurar.

static constexpr uint32 kEcoBakeMagic = 0x4F434501u;
static constexpr int32  kEcoBakeVersion = 1;

/** Todo lo que vive en un bake. Ver SerializeState. */
struct FEcoBakePayload
{
    int64           TickCount = 0;
    FEcosystemRng   Rng;
    FTreePopulation Population;
    FField2D        Water, Nutrient, Decomposition;
};

/** Dos rejillas describen el MISMO trozo de mundo con la MISMA resolucion. */
static bool EcoFieldGeometryMatches(const FField2D& A, const FField2D& B)
{
    return A.Width == B.Width
        && A.Height == B.Height
        && FMath::IsNearlyEqual(A.CellSize, B.CellSize, 1e-6)
        && A.Origin.Equals(B.Origin, 1.0)   // 1 cm de tolerancia
        && A.Data.Num() == A.Width * A.Height;
}

void UEcosystemSubsystem::SerializeState(FArchive& Ar, FEcoBakePayload& P)
{
    Ar << P.TickCount;
    Ar.Serialize(&P.Rng, sizeof(FEcosystemRng)); // streams de RNG (struct plano)

    auto PODArray = [&Ar](auto& Arr)
        {
            int32 N = Arr.Num();
            Ar << N;
            if (Ar.IsLoading())
            {
                // Un N corrupto (o simplemente un fichero truncado) haria un
                // SetNumUninitialized gigantesco -> OOM. Se valida contra lo que
                // realmente queda por leer en el archivo.
                const int64 Bytes = (int64)N * (int64)sizeof(Arr[0]);
                if (N < 0 || Bytes > Ar.TotalSize() - Ar.Tell())
                {
                    Ar.SetError();
                    return;
                }
                Arr.SetNumUninitialized(N);
            }
            if (N > 0) { Ar.Serialize(Arr.GetData(), (int64)N * sizeof(Arr[0])); }
        };
    auto FieldSer = [&Ar, &PODArray](FField2D& F)
        {
            Ar << F.Width; Ar << F.Height; Ar << F.CellSize; Ar << F.Origin;
            PODArray(F.Data);
        };

    // Poblacion (SoA): la fuente de verdad de posicion/especie/edad/tamano/estado.
    // Los campos NO se enumeran aqui: se recorren con el visitor de la propia
    // FTreePopulation, que es el unico sitio donde vive la lista. Asi un campo
    // nuevo entra en el bake solo, y no se puede escribir un fichero al que le
    // falte un array (que es un bake que carga "bien" y luego descuadra).
    FTreePopulation& Pop = P.Population;
    Pop.ForEachArray([&PODArray](auto& Array) { PODArray(Array); });
    Ar << Pop.NextStableId;

    // Estado runtime de los campos.
    FieldSer(P.Water);
    FieldSer(P.Nutrient);
    FieldSer(P.Decomposition);
}

void UEcosystemSubsystem::SaveState(const FString& FilePath)
{
    FEcoBakePayload Payload;
    Payload.TickCount = TickCount;
    Payload.Rng = Rng;
    Payload.Population.CopyFrom(Agents_Read);
    Payload.Water = WaterPool.Current;
    Payload.Nutrient = NutrientPool.Current;
    Payload.Decomposition = DecompositionField;

    TArray<uint8> Bytes;
    FMemoryWriter Ar(Bytes, /*bIsPersistent*/ true);

    uint32 Magic = kEcoBakeMagic;
    int32  Version = kEcoBakeVersion;
    uint32 Seed = Rng.MasterSeed;
    Ar << Magic << Version << Seed;
    SerializeState(Ar, Payload);

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree*/ true);
    if (FFileHelper::SaveArrayToFile(Bytes, *FilePath))
    {
        UE_LOG(LogEco, Log, TEXT("[Eco/F5] Bake guardado: %s (tick %lld, %d arboles, %d KB)."),
            *FilePath, TickCount, Agents_Read.Num(), Bytes.Num() / 1024);
    }
    else
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/F5] No se pudo escribir el bake: %s"), *FilePath);
    }
}

bool UEcosystemSubsystem::LoadState(const FString& FilePath)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco/F5] No existe el bake: %s"), *FilePath);
        return false;
    }

    FMemoryReader Ar(Bytes, /*bIsPersistent*/ true);
    uint32 Magic = 0; int32 Version = 0; uint32 Seed = 0;
    Ar << Magic << Version << Seed;
    if (Magic != kEcoBakeMagic)
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/F5] '%s' no es un bake valido."), *FilePath);
        return false;
    }
    if (Version != kEcoBakeVersion)
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/F5] '%s' es version %d y esta build lee la %d: no se carga."),
            *FilePath, Version, kEcoBakeVersion);
        return false;
    }
    if (Seed != Rng.MasterSeed)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco/F5] El bake se hizo con semilla %u pero la actual es %u: "
            "los campos base pueden no cuadrar (se carga de todas formas)."), Seed, Rng.MasterSeed);
    }

    // --- Deserializar APARTE y validar: nada de pisar el estado vivo todavia ---
    FEcoBakePayload P;
    SerializeState(Ar, P);
    if (Ar.IsError())
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/F5] '%s' esta corrupto o truncado: no se carga."), *FilePath);
        return false;
    }

    // Geometria de los campos: si no cuadra con los campos BASE actuales, el tick
    // indexaria Base.Data[] fuera de rango en RegenerateTowardBase y saltaria el
    // check() de ReduceScratchInto. Es un crash, no un artefacto visual.
    if (!EcoFieldGeometryMatches(P.Water, WaterBase.Field) ||
        !EcoFieldGeometryMatches(P.Nutrient, NutrientBase.Field) ||
        !EcoFieldGeometryMatches(P.Decomposition, NutrientBase.Field))
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/F5] El bake usa una geometria de relieve distinta "
            "(%dx%d @ %.0f cm) de la actual (%dx%d @ %.0f cm): no se carga. Ajusta "
            "HeightfieldResolution/HeightfieldCellSizeCm o rehaz el bake."),
            P.Water.Width, P.Water.Height, P.Water.CellSize,
            WaterBase.Field.Width, WaterBase.Field.Height, WaterBase.Field.CellSize);
        return false;
    }

    // Coherencia interna del SoA: todos los arrays paralelos con el mismo largo.
    const FTreePopulation& NewPop = P.Population;
    const int32 N = NewPop.Num();
    if (!NewPop.AllArraysHaveNum(N))
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/F5] El bake tiene los arrays SoA descuadrados: no se carga."));
        return false;
    }

    // Especies: un bake de un proyecto con mas especies dejaria arboles cuyo
    // ResolveSpecies devuelve null -> nunca crecen, nunca mueren, no se dibujan.
    for (int32 i = 0; i < N; ++i)
    {
        if (!ResolvedSpecies.IsValidIndex(NewPop.SpeciesId[i]))
        {
            UE_LOG(LogEco, Error, TEXT("[Eco/F5] El bake referencia la especie %d y solo hay %d "
                "configuradas en Project Settings: no se carga."),
                (int32)NewPop.SpeciesId[i], ResolvedSpecies.Num());
            return false;
        }
    }

    // --- Commit: a partir de aqui ya no puede fallar ---
    TickCount = P.TickCount;
    Rng = P.Rng;
    Agents_Read.CopyFrom(NewPop);
    WaterPool.Current = P.Water;
    NutrientPool.Current = P.Nutrient;
    DecompositionField = P.Decomposition;

    // Post-carga: los buffers Next parten del Current recien cargado.
    WaterPool.Next = WaterPool.Current;
    NutrientPool.Next = NutrientPool.Current;

    // La luz gruesa es estado DERIVADO de la poblacion y solo se refresca al inicio
    // de SimulateTick; como aqui dejamos la sim en pausa, hay que rehacerla a mano o
    // se queda la del bosque anterior (ver A7).
    RebuildCoarseLight();
    ClearHeroTrees();

    bPaused = true; // un bake es un instante objetivo: se muestra congelado
    OnStateLoaded.Broadcast();

    UE_LOG(LogEco, Log, TEXT("[Eco/F5] Bake cargado: %s (tick %lld, %d arboles). "
        "Simulacion en pausa; Eco.TogglePause para continuar."), *FilePath, TickCount, Agents_Read.Num());
    return true;
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
    const FVector Site = RandomPointOnTerrain(EEcoRngStream::Debug);

    // ResolvedSpecies ya esta cargada y cacheada en OnWorldBeginPlay: no hay que
    // volver a resolver el soft pointer aqui (era el unico sitio que lo hacia).
    const USpeciesData* Sp = nullptr;
    if (ResolvedSpecies.Num() > 0)
    {
        const int32 Idx = Rng.RangeI(EEcoRngStream::Debug, 0, ResolvedSpecies.Num() - 1);
        Sp = ResolvedSpecies[Idx];
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
    AddDebugAgent(Site, Color, R);
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

    // Fase 5 (Paso 5): en modo live, repinta la descomposicion una vez por tick
    // para ver las manchas de muerte aparecer y desvanecerse en el terreno.
    if (CVarDecompLive.GetValueOnGameThread() != 0 && LastDecompPaintTick != TickCount)
    {
        PaintDecompositionField(/*bLogResult*/ false); // B9: sin spam de log por tick
        LastDecompPaintTick = TickCount;
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
            FColor Color = Sp ? Sp->DebugColor : FColor::White;
            // Fase 5 (Paso 1): los arboles en declive se ven apagados/marrones.
            if (Agents_Read.State[i] == ETreeState::Senescent)
            {
                Color = FColor(150, 90, 40);
            }
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
/**
 * Camino UNICO de todos los heatmaps: sube el buffer al visualizador, se asegura
 * de que el decal existe y loguea.
 *
 * Los seis comandos Eco.Paint* tenian el mismo cuerpo copiado seis veces
 * (guarda -> UpdateFromField -> EnsureHeatmapDecal -> UE_LOG) y solo se
 * diferenciaban en el buffer y en el texto. Con una copia, cualquier arreglo del
 * pintado -por ejemplo, dejar de repintar si el decal no se pudo crear- llega a
 * los seis a la vez.
 *
 * bAutoRange = true usa el min/max del propio buffer; false usa [MinValue,
 * MaxValue] fijos, que es lo que necesita la descomposicion para que las manchas
 * no "laten" al cambiar el maximo entre ticks.
 */
void UEcosystemSubsystem::PaintField(const TArray<float>& Values, const TCHAR* LogLabel,
    bool bAutoRange, float MinValue, float MaxValue, bool bLogResult)
{
    if (!FieldViz || Values.Num() == 0)
    {
        return;
    }

    if (bAutoRange)
    {
        FieldViz->UpdateFromField(Values);
    }
    else
    {
        FieldViz->UpdateFromField(Values, MinValue, MaxValue);
    }

    EnsureHeatmapDecal();

    if (bLogResult && LogLabel)
    {
        UE_LOG(LogEco, Log, TEXT("[Eco] Heatmap pintado: %s."), LogLabel);
    }
}

void UEcosystemSubsystem::PaintTestField()
{
    if (!HeightField.IsValid()) { return; }
    PaintField(HeightField.Field.Data, TEXT("campo de prueba (relieve)"));
}

void UEcosystemSubsystem::PaintWaterField()
{
    if (!WaterPool.Current.IsValid()) { return; }
    PaintField(WaterPool.Current.Data, TEXT("agua (pool actual)"));
}

void UEcosystemSubsystem::PaintNutrientField()
{
    if (!NutrientPool.Current.IsValid()) { return; }
    PaintField(NutrientPool.Current.Data, TEXT("nutrientes (pool actual)"));
}

void UEcosystemSubsystem::PaintVigorField()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const USpeciesData* Sp = ResolveSpecies((uint16)FMath::Max(0, S->HeatmapSpeciesIndex));
    if (!Sp || !HeightField.IsValid()) { return; }

    FField2D Suit;
    // Fase 6: se le pasan los MISMOS parametros de CO2 que usa el tick, para que
    // el mapa de idoneidad siga representando exactamente la funcion que hace
    // crecer al bosque (ver GetCO2Params).
    const EcoCarbon::FCO2Params CO2 = GetCO2Params();
    EcoVigor::BakeSuitabilityField(HeightField, WaterBase, NutrientBase, LightCoarse,
        *Sp, S->LightHalfSaturationMax /* el mismo Kl que el tick */, Suit,
        /*OutLimiter*/ nullptr, &CO2);

    PaintField(Suit.Data, *FString::Printf(TEXT("vigor de %s%s"),
        *Sp->SpeciesName.ToString(), CO2.bEnabled ? TEXT(" (con CO2)") : TEXT(" (sin CO2)")));
}

void UEcosystemSubsystem::PaintLightField()
{
    if (!HeightField.IsValid()) { return; }

    // Luz a ras de suelo en cada NODO del relieve (misma geometria que el resto
    // de campos, asi el buffer encaja tal cual en el visualizador).
    const FField2D& R = HeightField.Field;
    TArray<float> L;
    L.SetNumUninitialized(R.Width * R.Height);
    for (int32 y = 0; y < R.Height; ++y)
    {
        const double Yc = R.NodeWorldY(y);
        for (int32 x = 0; x < R.Width; ++x)
        {
            const double Xc = R.NodeWorldX(x);
            const float  Z = HeightField.SampleHeight(Xc, Yc);
            L[y * R.Width + x] = LightCoarse.SampleLightSmooth(FVector(Xc, Yc, Z));
        }
    }

    PaintField(L, TEXT("luz disponible a ras de suelo"));
}

void UEcosystemSubsystem::PaintDecompositionField(bool bLogResult)
{
    if (!DecompositionField.IsValid()) { return; }

    // Rango FIJO [0, max]: las manchas no cambian de intensidad al variar el
    // maximo del campo entre ticks (el auto-rango haria "latir" el heatmap).
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    PaintField(DecompositionField.Data, TEXT("descomposicion (puntos de muerte)"),
        /*bAutoRange*/ false, 0.f, FMath::Max(0.001f, S->DecompositionPaintMax), bLogResult);
}

void UEcosystemSubsystem::EnsureHeatmapDecal()
{
    UWorld* World = GetWorld();
    if (!World || !FieldViz || !FieldViz->GetTexture()) return;

    // Camino rapido: ya esta todo montado y el decal no se mueve. Evita repetir el
    // LoadSynchronous del material y el SpawnActor en cada repintado, que en modo
    // live (Eco.Decomp.Live 1) es una vez por tick.
    if (HeatmapDecal && HeatmapMID)
    {
        HeatmapMID->SetTextureParameterValue(TEXT("FieldTex"), FieldViz->GetTexture());
        return;
    }

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