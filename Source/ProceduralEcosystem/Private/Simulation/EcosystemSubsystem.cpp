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
#include "UObject/UnrealType.h"   // TFieldIterator sobre las UPROPERTY(config)

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

static FAutoConsoleCommandWithWorld GEcoSpeciesDump(TEXT("Eco.Especies.Volcado"),
    TEXT("Vuelca los rasgos de cada especie y sus derivados (fL a pleno sol y en sombra, "
        "anchura absoluta de campana, lluvia de semillas, radio radicular efectivo)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogSpeciesDump(); }));

static FAutoConsoleCommandWithWorld GEcoNicheMap(TEXT("Eco.Nicho.Mapa"),
    TEXT("Que especie ganaria en cada celda (argmax de vigor potencial) y que fraccion "
        "del mapa gana cada una. No simula: responde antes de gastar mil ticks."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogNicheWinnerMap(); }));

static FAutoConsoleCommand GEcoConfigAudit(TEXT("Eco.Config.Auditar"),
    TEXT("Compara las UPROPERTY(config) de UEcosystemSettings con las claves del .ini: "
        "avisa de las que corren con el default de C++ y de las huerfanas."),
    FConsoleCommandDelegate::CreateStatic(&UEcosystemSubsystem::LogConfigCoverage));

static FAutoConsoleCommandWithWorldAndArgs GEcoLightProfile(TEXT("Eco.Luz.Perfil"),
    TEXT("Perfil vertical de luz de una columna. Uso: Eco.Luz.Perfil [X] [Y] (cm; por defecto el centro)."),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                const FBox2D B = S->GetHeightField().GetWorldBounds();
                const double X = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 0.5 * (B.Min.X + B.Max.X);
                const double Y = Args.Num() > 1 ? FCString::Atod(*Args[1]) : 0.5 * (B.Min.Y + B.Max.Y);
                S->LogLightProfile(X, Y);
            }
        }));

static FAutoConsoleCommandWithWorld GEcoFieldPercentiles(TEXT("Eco.PercentilesCampos"),
    TEXT("Percentiles de los campos base de agua y nutrientes: donde colocar los "
        "WaterOptimum / NutrientOptimum de cada especie."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogFieldPercentiles(); }));

static FString EcoCsvPath(const FString& Name)
{
    return FPaths::ProjectSavedDir() / TEXT("EcoCsv") / (Name + TEXT(".csv"));
}

// El historico se toma DURANTE el tick, asi que este comando no mide nada: solo
// vuelca lo ya acumulado. Por eso se puede pedir en cualquier momento (incluso
// con la simulacion en pausa) y sale la partida entera, no el instante actual.
static FAutoConsoleCommandWithWorldAndArgs GEcoDemographicsCsv(TEXT("Eco.Demografia.CSV"),
    TEXT("Vuelca el historico demografico a Saved/EcoCsv/<nombre>.csv (por defecto 'demografia'). "
        "Uso: Eco.Demografia.CSV [nombre]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                S->SaveDemographyCsv(EcoCsvPath(Args.Num() > 0 ? Args[0] : TEXT("demografia")));
            }
        }));

static FAutoConsoleCommandWithWorld GEcoDemographicsReset(TEXT("Eco.Demografia.Reset"),
    TEXT("Vacia el historico demografico acumulado (para separar dos corridas)."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->ClearDemographyHistory(); }));

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
    Scan(LightCoarse.LeafArea, TEXT("LightLeafArea"));
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
        USpeciesData* Loaded = SoftSp.LoadSynchronous();
        if (!Loaded)
        {
            // Una entrada que no resuelve se convertia en un HUECO SILENCIOSO: la
            // especie simplemente no existia en la simulacion y nada lo decia. Es
            // facil que pase con assets binarios no descargados (punteros Git LFS).
            UE_LOG(LogEco, Error, TEXT("[Eco] La especie '%s' no se pudo cargar: no participara en la simulacion."),
                *SoftSp.ToString());
        }
        ResolvedSpecies.Add(Loaded);
    }

    SpeciesExtinctionTick.Init(-1, ResolvedSpecies.Num());

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

    // El voxel VERTICAL es independiente del horizontal: con los dos a 400 cm, toda
    // la banda de regeneracion (del suelo a los 4 m) cabia en una sola capa y las
    // plantulas no existian como estrato en el campo de luz.
    const double LightSpanZ = TallestSpeciesCm + S->LightCanopyHeadroomCm + S->LightGroundClearanceCm;
    const int32  LightLayers = FMath::Max(2, FMath::CeilToInt32(LightSpanZ / S->LightCoarseCellSizeZCm));
    LightCoarse.Init(LightW, LightH, LightLayers,
        S->LightCoarseCellSizeCm, S->LightCoarseCellSizeZCm, Bounds.Min,
        /*BaseZ = offset de la capa 0 bajo el suelo*/ -(double)S->LightGroundClearanceCm);
    LightCoarse.SetExtinctionParams(S->LightExtinctionK, S->DiffuseLightFloor);

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

// Los factores de forma de copa (CanopyRadiusFraction/CanopyDepthFraction/CanopyLeafAreaIndex) y la
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

    RefreshSpeciesResponses(*Settings);
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
        EcologyRules::ReduceScratchInto(TickContexts, WaterPool.Next.Data, NutrientPool.Next.Data,
            PendingSeeds, PendingDeaths, SpeciesFlow);
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

    // Perturbacion DESPUES de la germinacion: un claro abierto este tick deja su
    // hueco para el siguiente, igual que una muerte cualquiera. Va aqui y no en el
    // paso paralelo porque necesita elegir centros de claro de forma serial y
    // determinista, con su propio stream de RNG.
    RunDisturbance(DtYears, *Settings);

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
        // Solo LEE la poblacion y no consume RNG, asi que no altera el
        // fingerprint ni la evolucion de la partida.
        RecordDemographySample();
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
    // El radio efectivo de un adulto respeta el minimo en celdas, asi que la
    // reserva tiene que contar con el o el primer tick realojaria los deltas.
    float MaxRootRadiusCm = Settings.MinRootRadiusCells * (float)WaterBase.Field.CellSize;
    for (const TObjectPtr<USpeciesData>& Sp : ResolvedSpecies)
    {
        if (Sp) { MaxRootRadiusCm = FMath::Max(MaxRootRadiusCm, Sp->RootRadius * 100.f); }
    }
    const int32 CellsPerTree = EcologyRules::KernelCellCount(WaterBase.Field, MaxRootRadiusCm);
    const int32 TreesPerChunk = FMath::DivideAndRoundUp(PopNum, FMath::Max(1, NumChunks));

    const int32 NumSpecies = ResolvedSpecies.Num();
    TickContexts.SetNum(NumChunks);
    for (FTickScratch& Ctx : TickContexts)
    {
        Ctx.ResetForNextTick(NumSpecies);
        Ctx.ReserveForTrees(TreesPerChunk, CellsPerTree);
    }

    SpeciesFlow.SetNum(NumSpecies, EAllowShrinking::No);
    return NumChunks;
}

void UEcosystemSubsystem::RefreshSpeciesResponses(const UEcosystemSettings& Settings)
{
    // Una vez por tick y por ESPECIE, no por arbol: las curvas son identicas para
    // todos los individuos de una especie, asi que construirlas dentro del bucle
    // era trabajo repetido decenas de miles de veces y ademas obligaba a leer el
    // UObject de especie desde dentro del ParallelFor.
    SpeciesResponses.SetNum(ResolvedSpecies.Num(), EAllowShrinking::No);
    for (int32 i = 0; i < ResolvedSpecies.Num(); ++i)
    {
        if (const USpeciesData* Sp = ResolvedSpecies[i])
        {
            SpeciesResponses[i] = EcoVigor::MakeSpeciesResponses(*Sp, Settings);
        }
    }
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

    // Radio radicular minimo de un ADULTO, en cm. Se resuelve aqui (una vez) y se
    // captura por valor: el kernel da peso EXACTAMENTE cero a los vecinos cuando el
    // radio no supera el tamano de celda, y con eso cada arbol se agota un pozo
    // privado sin tocar el de nadie -la competencia subterranea deja de existir
    // como interaccion-.
    const float MinAdultRootRadiusCm = Settings.MinRootRadiusCells * (float)WaterBase.Field.CellSize;

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
                    if (!IsAliveState(Agents_Read.State[i])) { continue; }

                    const uint16 SpeciesId = Agents_Read.SpeciesId[i];
                    const USpeciesData* Sp = ResolveSpecies(SpeciesId);
                    if (!Sp || !SpeciesResponses.IsValidIndex(SpeciesId)) { continue; }

                    const EcoVigor::FSpeciesResponses& Resp = SpeciesResponses[SpeciesId];
                    FEcoSpeciesFlow& Flow = Ctx.SpeciesFlow[SpeciesId];

                    const FVector P = Agents_Read.Position[i];
                    uint32& RngState = Agents_Write.RngState[i]; // stream propio -> determinista

                    // 2a) recursos locales
                    const float W = WaterPool.SampleCurrent(P.X, P.Y);
                    const float N = NutrientPool.SampleCurrent(P.X, P.Y);

                    // LA LUZ SE LEE EN EL TECHO DE LA COPA, no en el pie del arbol.
                    //
                    // Position.Z es la cota del terreno y no cambia nunca, asi que
                    // antes un dominante de 20 m y la plantula que tenia debajo leian
                    // EL MISMO Q: la altura entraba en el modelo solo como emisor de
                    // sombra, jamas como receptor de luz. Con eso la competencia por
                    // luz -que en un bosque real es asimetrica, el que llega arriba
                    // intercepta y el de abajo paga- quedaba plana, y con ella
                    // desaparecian la estratificacion vertical, el heredar el hueco y
                    // la sucesion entera. Leyendo en el apice, altura -> luz ->
                    // crecimiento -> altura se cierra como bucle y es lo que crea el
                    // dosel. La autoexclusion sale gratis: el LAI acumulado por
                    // ENCIMA del techo de la copa propia no contiene follaje propio.
                    //
                    // Se usa la altura del SNAPSHOT DE LECTURA (la del principio del
                    // tick): el vigor decide cuanto crece el arbol, asi que no puede
                    // depender de lo que ha crecido este mismo tick.
                    const float ReadHeight = Agents_Read.Height[i];
                    const float Q = LightCoarse.SampleLightSmooth(P + FVector(0.f, 0.f, ReadHeight));

                    // 2b) vigor. Copia UNICA de la formula (EcoVigor::EvaluateVigor):
                    //     tick, germinacion y heatmap evaluan literalmente lo mismo.
                    //     Guardar el argmin cuesta una comparacion que ya se hacia y
                    //     es lo que permite responder "por que se muere esta especie"
                    //     en vez de solo "se muere".
                    EEcoLimiter LimiterHere = EEcoLimiter::Light;
                    const float VigorValue =
                        EcoVigor::EvaluateVigor(Q, W, N, Resp, Settings.VigorCombineMode, LimiterHere)
                        * EcoCarbon::CO2Factor(Q, ReadHeight, CO2);

                    Agents_Write.Vigor[i] = VigorValue;
                    Agents_Write.Limiter[i] = static_cast<uint8>(LimiterHere);

                    // 2c) estres. Va ANTES de decidir los estados porque la supresion
                    //     se decide con el estres de ESTE tick.
                    const float NewStress = EcologyRules::UpdateStress(Agents_Read.Stress[i], VigorValue,
                        Settings.StressVigorThreshold, Settings.StressAccumulationRate,
                        Settings.StressRecoveryRate, Settings.StressDecayRate, DtYears);
                    Agents_Write.Stress[i] = NewStress;

                    // 2d) los DOS declives, que antes eran uno solo.
                    //
                    // Senescencia por EDAD: irreversible, y con razon. La puerta se
                    // hace pegajosa leyendo el estado del snapshot, que es inmutable
                    // durante todo el tick -> determinista bajo paralelismo.
                    //
                    // Supresion por ESTRES: REVERSIBLE, con histeresis. Antes las dos
                    // compartian estado, asi que el declive por estres heredaba la
                    // irreversibilidad: unos pocos anos de mala racha marcaban de por
                    // vida a una plantula -crecimiento x0.1 y mortalidad x2- aunque
                    // despues se abriera un claro justo encima. Eso prohibe el banco
                    // de plantulas suprimidas, que es el mecanismo por el que una
                    // especie tolerante hereda los huecos sin competir por semilla.
                    const float NewAge = Agents_Read.Age[i] + DtYears;
                    const ETreeState PrevState = Agents_Read.State[i];

                    const bool bSenescent = (PrevState == ETreeState::Senescent)
                        || EcologyRules::IsSenescentByAge(NewAge, Sp->Longevity, Sp->SenescenceAgeFraction);

                    const bool bSuppressed = !bSenescent && EcologyRules::UpdateSuppression(
                        PrevState == ETreeState::Suppressed, NewStress,
                        Sp->SenescenceStressThreshold, Settings.SuppressionExitStressFraction);

                    // 2e) crecimiento + altura + estado
                    const float EffGrowthRate = Sp->GrowthRate
                        * EcologyRules::DeclineGrowthFactor(bSenescent, Sp->SenescentGrowthScale)
                        * EcologyRules::DeclineGrowthFactor(bSuppressed, Sp->SuppressedGrowthScale);

                    const float NewBiomass = EcologyRules::GrowBiomassLogistic(
                        Agents_Read.Biomass[i], VigorValue, EffGrowthRate, Sp->MaxBiomass, DtYears);

                    Agents_Write.Biomass[i] = NewBiomass;
                    Agents_Write.Height[i] = EcologyRules::HeightFromBiomass(NewBiomass, Sp->MaxBiomass, Sp->MaxHeightCm);
                    Agents_Write.Age[i] = NewAge;
                    Agents_Write.State[i] = bSenescent  ? ETreeState::Senescent
                        : bSuppressed                   ? ETreeState::Suppressed
                        : (NewAge >= Sp->MaturityAge)   ? ETreeState::Mature
                                                        : ETreeState::Sapling;

                    // 2f) consumo -> SOLO al scratch de este chunk, en forma DISPERSA.
                    //     Topado contra lo disponible: sin tope, la parte de la demanda
                    //     que no existia se depositaba como cantidad NEGATIVA, se
                    //     difundia a los vecinos bajandoles recurso de verdad y luego
                    //     se destruia al recortar a cero -competencia por interferencia
                    //     gratis, y masa no conservada-.
                    const float RootRadiusCm = EcologyRules::EffectiveRootRadiusCm(
                        Sp->RootRadius, NewBiomass, Sp->MaxBiomass, MinAdultRootRadiusCm);
                    const int32 CellsInRange = EcologyRules::KernelCellCount(WaterBase.Field, RootRadiusCm);

                    const float WaterDraw = EcologyRules::ClampUptakeToAvailable(
                        NewBiomass * Sp->WaterDemand * DtYears, W, CellsInRange, Settings.MaxResourceUptakeFraction);
                    const float NutrientDraw = EcologyRules::ClampUptakeToAvailable(
                        NewBiomass * Sp->NutrientDemand * DtYears, N, CellsInRange, Settings.MaxResourceUptakeFraction);

                    EcologyRules::DepositKernelSparse(WaterBase.Field, Ctx.WaterDeltas, P, RootRadiusCm, -WaterDraw);
                    EcologyRules::DepositKernelSparse(NutrientBase.Field, Ctx.NutrientDeltas, P, RootRadiusCm, -NutrientDraw);

                    // 2g) mortalidad. Los dos canales se calculan por separado para
                    //     poder ATRIBUIR la muerte: si el de edad no aparece nunca, la
                    //     longevidad no esta comprando nada y la estrategia lenta y
                    //     longeva es inviable haga lo que haga el resto del modelo.
                    const float pAge = EcologyRules::AgeMortalityProbability(NewAge, Sp->Longevity, DtYears);

                    // Un arbol SUPRIMIDO no muere por el canal de estres general sino
                    // por el hazard propio de su especie, que puede ser MENOR. Es la
                    // mitad que faltaba del compromiso r/K: hasta ahora ningun rasgo
                    // de especie podia reducir el riesgo, solo amplificarlo.
                    const float pCondition = bSuppressed
                        ? EcologyRules::SuppressedMortalityProbability(Sp->SuppressedMortalityPerYear, DtYears)
                        : EcologyRules::StressMortalityProbability(NewStress,
                            EcologyRules::EffectiveStressMortalityWeight(Settings.StressMortalityWeight,
                                Sp->Longevity, Settings.LongevityStressRefYears, Settings.LongevityStressExponent),
                            DtYears);

                    float pDeath = EcologyRules::CombineIndependentRisks(pAge, pCondition);
                    pDeath = EcologyRules::ApplySenescentMortality(pDeath, bSenescent, Sp->SenescentMortalityMultiplier);

                    if (EcoRand::NextUnit(RngState) < pDeath)
                    {
                        Agents_Write.State[i] = ETreeState::Dead;
                        (pAge >= pCondition ? Flow.DeathsByAge : Flow.DeathsByCondition) += 1;

                        FPendingDeathPulse Pulse;
                        Pulse.Position = P;
                        Pulse.RadiusCm = RootRadiusCm;
                        Pulse.Amount = EcologyRules::DeathNutrientPulse(NewBiomass, Settings.NutrientDecompositionFactor);
                        // Fase 5: datos para la caida/tocon/hojarasca del render.
                        Pulse.SpeciesId = SpeciesId;
                        Pulse.StableId = Agents_Read.StableId[i];
                        Pulse.Biomass = NewBiomass;
                        Pulse.HeightCm = Agents_Write.Height[i];
                        Ctx.DeathPulses.Add(Pulse);
                    }
                    else if (NewAge >= Sp->MaturityAge && IsReproductiveState(Agents_Write.State[i]))
                    {
                        // 2h) semillas (solo si sigue vivo y ha alcanzado la madurez).
                        //     El senescente SIGUE reproduciendose, con menos fuerza, y
                        //     el suprimido tambien: cortarlos a cero eliminaria la
                        //     ventana de maxima fecundidad y, en el caso del suprimido,
                        //     la unica via por la que una tolerante compensa su
                        //     lentitud. La comprobacion explicita de MaturityAge hace
                        //     falta porque una PLANTULA muy estresada tambien entra en
                        //     Suppressed, y una plantula no se reproduce.
                        const float SeedScale = (Agents_Write.State[i] == ETreeState::Senescent)
                            ? FMath::Clamp(Sp->SenescentSeedScale, 0.f, 1.f)
                            : 1.f;

                        // La fecundidad SATURA con la biomasa relativa en vez de ser
                        // proporcional a ella: con proporcionalidad lineal, crecer mas
                        // rapido daba a la vez mas individuos Y mas semillas por
                        // individuo, un bucle de realimentacion que convierte una
                        // ventaja de crecimiento moderada en una diferencia de lluvia
                        // de semillas de dos ordenes de magnitud -y que de paso
                        // esteriliza al arbol suprimido, impidiendole recuperarse-.
                        const int32 NumSeeds = EcologyRules::ComputeSeedCount(
                            Settings.SeedsPerAdultPerYear * SeedScale * Sp->SeedRateScale,
                            NewBiomass, Sp->MaxBiomass, Settings.SeedBiomassHalfSaturation, DtYears, RngState);
                        Flow.SeedsEmitted += NumSeeds;

                        const float DispersalRadiusCm = Sp->SeedDispersalRadius * 100.f;
                        const uint32 ParentStableId = Agents_Read.StableId[i];

                        for (int32 seed = 0; seed < NumSeeds; ++seed)
                        {
                            const FVector2D Offset = EcologyRules::SampleSeedOffsetCm(DispersalRadiusCm, RngState);

                            FPendingSeed Seed;
                            Seed.Position = P + FVector(Offset.X, Offset.Y, 0.f);
                            Seed.SpeciesId = SpeciesId;
                            Seed.RngSeed = EcoRand::SeedForIndex(RngState, seed);
                            Seed.ParentStableId = ParentStableId;
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
    // Evita realojos de los arrays SoA durante los Add() de abajo.
    Agents_Write.Reserve(Agents_Write.Num() + PendingSeeds.Num());

    const FBox2D WorldBounds = HeightField.GetWorldBounds();
    NewbornPositions.Reset();

    // Distancia XY al cuadrado contra un radio dado. El radio ya NO es el mismo
    // para todos los vecinos: ver bSpacingScalesWithSize.
    auto IsTooClose = [](const FVector& A, const FVector& B, double RadiusCm) -> bool
        {
            const double dx = A.X - B.X;
            const double dy = A.Y - B.Y;
            return dx * dx + dy * dy < RadiusCm * RadiusCm;
        };

    // Radio de exclusion de una PLANTULA recien nacida. Con el escalado activo es
    // MinGerminationSpacingCm por su fraccion de altura adulta, o sea ~1 m para
    // una plantula del 1% de biomasa frente a los 5 m de un arbol de dosel.
    const float SeedlingHeightRatio = EcologyRules::HeightRatioFromBiomass(
        Settings.GerminationBiomassFraction, 1.f);
    const double NewbornRadiusCm = Settings.MinGerminationSpacingCm *
        (Settings.bSpacingScalesWithSize ? SeedlingHeightRatio : 1.f);

    for (const FPendingSeed& Seed : PendingSeeds)
    {
        const USpeciesData* Sp = ResolveSpecies(Seed.SpeciesId);
        if (!Sp || !SpeciesResponses.IsValidIndex(Seed.SpeciesId)) { continue; }
        FEcoSpeciesFlow& Flow = SpeciesFlow[Seed.SpeciesId];

        // Semilla dispersada fuera del terreno simulado: se descarta en vez de
        // germinar en el borde (SampleHeight/SampleLight harian clamp y
        // apelmazarian plantulas en el limite del mapa).
        if (Seed.Position.X < WorldBounds.Min.X || Seed.Position.X > WorldBounds.Max.X ||
            Seed.Position.Y < WorldBounds.Min.Y || Seed.Position.Y > WorldBounds.Max.Y)
        {
            ++Flow.RejectedOffMap;
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
                if (!IsAliveState(Agents_Write.State[NeighborIdx])) { return; }

                // El radio que impone CADA vecino depende de su tamano: un arbol de
                // dosel aparta a 5 m, una plantula a ~1 m. Con el radio fijo, unos
                // miles de adultos cubren el mapa entero de exclusiones y el
                // sotobosque deja de existir (ver bSpacingScalesWithSize).
                double NeighborRadiusCm = Settings.MinGerminationSpacingCm;
                if (Settings.bSpacingScalesWithSize)
                {
                    const USpeciesData* NeighborSp = ResolveSpecies(Agents_Read.SpeciesId[NeighborIdx]);
                    const float Ratio = NeighborSp
                        ? EcologyRules::HeightRatioFromBiomass(Agents_Read.Biomass[NeighborIdx], NeighborSp->MaxBiomass)
                        : 1.f;
                    NeighborRadiusCm *= Ratio;
                }

                if (IsTooClose(Agents_Read.Position[NeighborIdx], GerminationPos, NeighborRadiusCm))
                {
                    bTooClose = true;
                }
            });

        // El hash se construyo sobre Agents_Read y NO contiene las plantulas que han
        // germinado en este mismo bucle, asi que hay que comprobarlas aparte o dos
        // semillas del mismo tick germinan pegadas. El bucle es serial y de orden
        // fijo, asi que el resultado sigue siendo determinista.
        if (!bTooClose)
        {
            for (const FVector& NP : NewbornPositions)
            {
                // Ambas son plantulas: el radio que aplica es el de plantula.
                if (IsTooClose(NP, GerminationPos, NewbornRadiusCm)) { bTooClose = true; break; }
            }
        }
        if (bTooClose) { ++Flow.RejectedSpacing; continue; }

        // La semilla lee la luz A RAS DE SUELO, que es la cota que le corresponde
        // (a diferencia del arbol ya establecido, que la lee en el techo de su copa).
        const float LightHere = LightCoarse.SampleLightSmooth(GerminationPos);
        if (!EcologyRules::IsSafeGerminationSite(LightHere, Sp->MinLightForGermination))
        {
            // Umbral POR ESPECIE: es lo que hace del sotobosque un territorio donde
            // la pionera no puede germinar en absoluto, por muchas semillas que
            // mande, y donde la tolerante acumula el banco de plantulas que heredara
            // el hueco cuando el arbol de dosel muera. OJO: este filtro solo puede
            // funcionar si existe de verdad un gradiente de luz de suelo; con el
            // dosel transparente que producia el deposito de sombra anterior no
            // rechazaba ni un sitio (comprobalo con Eco.PercentilesCampos).
            ++Flow.RejectedLight;
            continue;
        }

        const float WHere = WaterPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);
        const float NHere = NutrientPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);

        // Fase 6: la semilla cae al SUELO, o sea altura de copa 0: es donde el
        // termino de CO2 pesa mas (aire poco mezclado bajo un dosel cerrado).
        EEcoLimiter SeedLimiter = EEcoLimiter::Light;
        const float VigorHere = EcoVigor::EvaluateVigor(LightHere, WHere, NHere,
            SpeciesResponses[Seed.SpeciesId], Settings.VigorCombineMode, SeedLimiter)
            * EcoCarbon::CO2Factor(LightHere, /*CanopyHeightCm*/ 0.f, CO2);

        // Janzen-Connell: cuenta los adultos de LA MISMA especie alrededor. Los
        // enemigos naturales especializados (patogenos de suelo, herbivoros) se
        // acumulan bajo los adultos de su hospedador, asi que una plantula rodeada
        // de los suyos arraiga mucho peor. Es un estabilizador: penaliza a quien
        // domina LOCALMENTE, de modo que la especie rara siempre recluta mejor de
        // lo que le corresponderia por numero.
        //
        // Solo cuentan los REPRODUCTIVOS: es la presencia del hospedador adulto la
        // que mantiene la carga de enemigos, no un planton vecino.
        int32 Conspecifics = 0;
        if (Settings.ConspecificHalfCount > 0.f && Settings.ConspecificInhibitionRadiusCm > 0.f)
        {
            const double InhibRadiusSq = FMath::Square((double)Settings.ConspecificInhibitionRadiusCm);
            Hash.ForEachNeighbor(GerminationPos, Settings.ConspecificInhibitionRadiusCm,
                [&](int32 NeighborIdx)
                {
                    if (Agents_Read.SpeciesId[NeighborIdx] != Seed.SpeciesId) { return; }
                    if (!IsReproductiveState(Agents_Write.State[NeighborIdx])) { return; }

                    // La MADRE no cuenta. Cuando el radio de dispersion no supera al
                    // de inhibicion, toda semilla cae dentro del circulo de su propia
                    // madre, asi que hasta el ultimo adulto de una especie al borde de
                    // la extincion pagaria la penalizacion por verse a si mismo: el
                    // rescate de la especie rara quedaba topado justo donde el
                    // mecanismo tiene que ser mas fuerte.
                    if (Settings.bExcludeMotherFromInhibition &&
                        Agents_Read.StableId[NeighborIdx] == Seed.ParentStableId)
                    {
                        return;
                    }

                    const FVector& NP = Agents_Read.Position[NeighborIdx];
                    const double dx = NP.X - GerminationPos.X;
                    const double dy = NP.Y - GerminationPos.Y;
                    if (dx * dx + dy * dy <= InhibRadiusSq) { ++Conspecifics; }
                });
        }
        const float JanzenConnell =
            EcologyRules::ConspecificInhibitionFactor(Conspecifics, Settings.ConspecificHalfCount);
        Flow.JanzenConnellSum += JanzenConnell;
        ++Flow.JanzenConnellCount;

        uint32 SeedRng = Seed.RngSeed;
        // La otra mitad del compromiso r/K: la semilla grande sale poco (SeedRateScale
        // bajo) pero arraiga mejor (GerminationRateScale alto).
        const float pGerm = EcologyRules::GerminationProbability(
            VigorHere, Settings.GerminationRate * Sp->GerminationRateScale) * JanzenConnell;
        if (EcoRand::NextUnit(SeedRng) < pGerm)
        {
            Agents_Write.Add(GerminationPos, Seed.SpeciesId, SeedRng, /*Age*/ 0.f,
                /*Biomass*/ Sp->MaxBiomass * Settings.GerminationBiomassFraction);
            NewbornPositions.Add(GerminationPos);
            ++Flow.Germinated;
        }
    }
}

void UEcosystemSubsystem::RunDisturbance(float DtYears, const UEcosystemSettings& Settings)
{
    if (Settings.DisturbanceRatePerYear <= 0.f || !HeightField.IsValid()) { return; }

    const FBox2D B = HeightField.GetWorldBounds();
    const double MapAreaM2 = ((B.Max.X - B.Min.X) * (B.Max.Y - B.Min.Y)) / 10000.0; // cm2 -> m2
    if (MapAreaM2 <= 0.0) { return; }

    // Area a perturbar este tick, convertida a un NUMERO de claros con el area
    // media de la distribucion. La media de una ley potencia de exponente a sobre
    // [min,max] se integra analiticamente; con a=2 degenera en un logaritmo, asi
    // que se resuelve el caso aparte en vez de dividir por cero.
    const float MinA = FMath::Max(Settings.DisturbanceMinAreaM2, 1.f);
    const float MaxA = FMath::Max(Settings.DisturbanceMaxAreaM2, MinA);
    const float Alpha = FMath::Max(Settings.DisturbanceAreaExponent, 1.f);

    double MeanAreaM2;
    if (FMath::IsNearlyEqual(Alpha, 2.f, 1e-3f))
    {
        MeanAreaM2 = (FMath::Loge(MaxA) - FMath::Loge(MinA)) / (1.0 / MinA - 1.0 / MaxA);
    }
    else
    {
        const double A1 = 1.0 - Alpha, A2 = 2.0 - Alpha;
        MeanAreaM2 = (A1 / A2) * (FMath::Pow((double)MaxA, A2) - FMath::Pow((double)MinA, A2))
                              / (FMath::Pow((double)MaxA, A1) - FMath::Pow((double)MinA, A1));
    }
    if (!(MeanAreaM2 > 0.0)) { return; }

    const float Lambda = static_cast<float>(
        Settings.DisturbanceRatePerYear * DtYears * MapAreaM2 / MeanAreaM2);

    // Stream propio: activar la perturbacion NO desplaza los streams de
    // colonizacion, dispersion ni mortalidad, asi que se puede comparar una corrida
    // con claros y otra sin ellos partiendo del mismo bosque.
    uint32& Stream = Rng.State[static_cast<int32>(EEcoRngStream::Disturbance)];
    const int32 NumGaps = EcoRand::PoissonInt(Stream, Lambda);

    int32 Felled = 0;
    for (int32 g = 0; g < NumGaps; ++g)
    {
        // Tamano por ley potencia con muestreo por inversa.
        const float U = EcoRand::NextUnit(Stream);
        const double A1 = 1.0 - Alpha;
        const double AreaM2 = FMath::Pow(
            FMath::Pow((double)MinA, A1) + U * (FMath::Pow((double)MaxA, A1) - FMath::Pow((double)MinA, A1)),
            1.0 / A1);
        const double RadiusCm = FMath::Sqrt(AreaM2 / PI) * 100.0;

        const double Cx = FMath::Lerp(B.Min.X, B.Max.X, (double)EcoRand::NextUnit(Stream));
        const double Cy = FMath::Lerp(B.Min.Y, B.Max.Y, (double)EcoRand::NextUnit(Stream));
        const FVector Center(Cx, Cy, HeightField.SampleHeight(Cx, Cy));

        // El hash indexa Agents_Read, y los arboles que ya existian al empezar el
        // tick ocupan el MISMO indice en Agents_Write (que es una copia con las
        // plantulas nuevas anadidas al final). Se consulta y se marca sobre
        // Agents_Write, que es el buffer que sobrevive al swap; las plantulas
        // germinadas en este mismo tick no estan en el hash y por tanto no las
        // alcanza el claro, lo cual es razonable: aun no habian salido.
        const double RadiusSq = RadiusCm * RadiusCm;
        Hash.ForEachNeighbor(Center, (float)RadiusCm, [&](int32 Idx)
            {
                if (!Agents_Write.State.IsValidIndex(Idx)) { return; }
                if (!IsAliveState(Agents_Write.State[Idx])) { return; }

                const FVector& P = Agents_Read.Position[Idx];
                const double dx = P.X - Center.X, dy = P.Y - Center.Y;
                if (dx * dx + dy * dy > RadiusSq) { return; }

                // Mortalidad < 1 deja arboles residuales en pie, que es lo que hace
                // un temporal real y lo que da al claro su estructura irregular.
                if (EcoRand::NextUnit(Stream) >= Settings.DisturbanceMortality) { return; }

                const USpeciesData* Sp = ResolveSpecies(Agents_Write.SpeciesId[Idx]);
                if (!Sp) { return; }

                Agents_Write.State[Idx] = ETreeState::Dead;
                ++Felled;

                const float Biomass = Agents_Write.Biomass[Idx];

                FPendingDeathPulse Pulse;
                Pulse.Position = P;
                Pulse.RadiusCm = EcologyRules::EffectiveRootRadiusCm(Sp->RootRadius, Biomass, Sp->MaxBiomass,
                    Settings.MinRootRadiusCells * (float)WaterBase.Field.CellSize);
                Pulse.Amount = EcologyRules::DeathNutrientPulse(Biomass, Settings.NutrientDecompositionFactor);
                Pulse.SpeciesId = Agents_Write.SpeciesId[Idx];
                Pulse.StableId = Agents_Write.StableId[Idx];
                Pulse.Biomass = Biomass;
                Pulse.HeightCm = Agents_Write.Height[Idx];

                EcologyRules::DepositKernel(NutrientBase.Field, NutrientPool.Next.Data,
                    Pulse.Position, Pulse.RadiusCm, Pulse.Amount);
                RecordDeathEvent(Pulse);
            });
    }

    if (Felled > 0)
    {
        UE_LOG(LogEco, Verbose, TEXT("[Eco] Perturbacion | tick %lld | %d claros | %d arboles caidos"),
            TickCount, NumGaps, Felled);
    }
}

void UEcosystemSubsystem::RebuildCoarseLight()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();

    LightCoarse.SetExtinctionParams(S->LightExtinctionK, S->DiffuseLightFloor);
    LightCoarse.ClearShadow();

    for (int32 i = 0; i < Agents_Read.Num(); ++i)
    {
        if (!IsAliveState(Agents_Read.State[i])) { continue; }
        const USpeciesData* Sp = ResolveSpecies(Agents_Read.SpeciesId[i]);
        if (!Sp) { continue; }

        // La copa ocupa la parte ALTA del arbol (CanopyDepthFraction), no su altura
        // entera. Pasarle la altura completa como profundidad de copa era el origen
        // del bug que dejaba el sotobosque a plena luz: la sombra se repartia por
        // todo el fuste y, con el decaimiento vertical de entonces, se desvanecia
        // justo a ras de suelo. Ahora la copa deposita area foliar en su propio
        // volumen y lo que oscurece el suelo es la extincion acumulada.
        const float H = Agents_Read.Height[i];
        const FVector Apex = Agents_Read.Position[i] + FVector(0.f, 0.f, H);
        LightCoarse.DepositCanopyLeafArea(Apex, H * S->CanopyRadiusFraction,
            H * S->CanopyDepthFraction, S->CanopyLeafAreaIndex);
    }

    // Convierte densidad depositada en LAI acumulado por encima de cada voxel.
    // Sin esta pasada los Sample* devolverian la densidad local en vez de la
    // atenuacion, o sea justo el perfil invertido que habia antes.
    LightCoarse.AccumulateExtinction();
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
    const int32 NumSpecies = ResolvedSpecies.Num();
    if (NumSpecies == 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] Demografia: no hay especies configuradas."));
        return;
    }

    struct FSpeciesDemo
    {
        int32 Saplings = 0, Mature = 0, Suppressed = 0, Senescent = 0;
        int32 Grown = 0;      // >= 70% de MaxBiomass: los que "llenan" el bosque
        double AgeSum = 0.0;
        float  AgeMax = 0.f;

        // Instrumentacion: el reparto por estado dice como esta el bosque, pero no
        // POR QUE. El vigor medio y el limitante si.
        double VigorSum = 0.0;
        double StressSum = 0.0;
        int32  Lim[3] = { 0, 0, 0 }; // luz / agua / nutrientes (indices de EEcoLimiter)

        int32 Live() const { return Saplings + Mature + Suppressed + Senescent; }
    };

    TArray<FSpeciesDemo> BySpecies;
    BySpecies.SetNum(NumSpecies);

    int32 Counted = 0;
    for (int32 i = 0; i < Agents_Read.Num(); ++i)
    {
        if (!IsAliveState(Agents_Read.State[i])) { continue; }

        const int32 sp = Agents_Read.SpeciesId[i];
        if (!BySpecies.IsValidIndex(sp)) { continue; }

        const USpeciesData* Sp = ResolveSpecies(static_cast<uint16>(sp));
        if (!Sp) { continue; }

        FSpeciesDemo& D = BySpecies[sp];
        switch (Agents_Read.State[i])
        {
        case ETreeState::Sapling:    ++D.Saplings;   break;
        case ETreeState::Mature:     ++D.Mature;     break;
        case ETreeState::Suppressed: ++D.Suppressed; break;
        case ETreeState::Senescent:  ++D.Senescent;  break;
        default: break;
        }

        const float Age = Agents_Read.Age[i];
        D.AgeSum += Age;
        D.AgeMax = FMath::Max(D.AgeMax, Age);
        D.VigorSum += Agents_Read.Vigor[i];
        D.StressSum += Agents_Read.Stress[i];
        if (Agents_Read.Limiter[i] < 3) { ++D.Lim[Agents_Read.Limiter[i]]; }
        if (Agents_Read.Biomass[i] >= 0.7f * Sp->MaxBiomass) { ++D.Grown; }
        ++Counted;
    }

    UE_LOG(LogEco, Log, TEXT("[Eco] Demografia | tick %lld | %d arboles vivos | %lld muertes acumuladas"),
        TickCount, Counted, DeathEventCounter);

    for (int32 sp = 0; sp < NumSpecies; ++sp)
    {
        const FSpeciesDemo& D = BySpecies[sp];
        const USpeciesData* Sp = ResolveSpecies(static_cast<uint16>(sp));
        const FString Name = Sp ? Sp->SpeciesName.ToString() : FString(TEXT("?"));
        const int32 Live = D.Live();

        // UNA ESPECIE EXTINTA TIENE QUE SALIR EN EL LOG. Antes se saltaba con un
        // continue, asi que desaparecia del informe sin dejar rastro y la senal mas
        // importante de todo el experimento -que una especie ha cruzado el cero, y
        // cuando- no quedaba registrada en ninguna parte.
        if (Live == 0)
        {
            const int64 Ext = SpeciesExtinctionTick.IsValidIndex(sp) ? SpeciesExtinctionTick[sp] : -1;
            if (Ext >= 0)
            {
                UE_LOG(LogEco, Warning, TEXT("  %-22s n=   0 | *** EXTINTA (detectada en el tick %lld)"), *Name, Ext);
            }
            else
            {
                UE_LOG(LogEco, Warning, TEXT("  %-22s n=   0 | *** SIN INDIVIDUOS (nunca sembrada, o asset no cargado)"), *Name);
            }
            continue;
        }

        const float InvLive = 1.f / static_cast<float>(Live);
        const float MeanAge = static_cast<float>(D.AgeSum) * InvLive;

        // DOS medianas, y la comparacion entre ellas es el diagnostico.
        //   - NOMINAL: 1.282*L^0.8, la edad a la que la mortalidad POR EDAD se lleva
        //     a la mitad de una cohorte. Es lo que la especie deberia vivir.
        //   - REALIZADA: ln2/h con h = muertes del tick / vivos, o sea lo que de
        //     verdad vive con TODOS los canales de mortalidad actuando.
        //
        // EL HAZARD SALE DE LOS FLUJOS, NO DE LA EDAD MEDIA. Derivarlo de la edad
        // media supone una poblacion en estado estacionario, y una especie que esta
        // reclutando a lo bestia tiene la piramide de edades llena de plantulas
        // recien nacidas: su edad media se hunde por el reclutamiento, no por la
        // mortalidad, y la cifra sale hasta dos veces mas pesimista de lo real. El
        // cociente muertes/vivos mide el riesgo directamente y no depende de como
        // este repartida la poblacion por edades.
        const float MedianNominal = Sp ? 1.282f * FMath::Pow(FMath::Max(Sp->Longevity, 1.f), 0.8f) : 0.f;

        float Hazard = 0.f;
        if (SpeciesFlow.IsValidIndex(sp) && YearsPerTick > KINDA_SMALL_NUMBER)
        {
            const int32 Deaths = SpeciesFlow[sp].DeathsByAge + SpeciesFlow[sp].DeathsByCondition;
            Hazard = static_cast<float>(Deaths) / (static_cast<float>(Live) * YearsPerTick);
        }
        const float MedianRealized = (Hazard > KINDA_SMALL_NUMBER) ? 0.6931472f / Hazard : 0.f;

        UE_LOG(LogEco, Log,
            TEXT("  %-22s n=%5d | plantula %4.1f%% adulto %4.1f%% suprimido %4.1f%% senescente %4.1f%% | crecidos %4.1f%%"),
            *Name, Live,
            100.f * D.Saplings * InvLive, 100.f * D.Mature * InvLive,
            100.f * D.Suppressed * InvLive, 100.f * D.Senescent * InvLive,
            100.f * D.Grown * InvLive);

        UE_LOG(LogEco, Log,
            TEXT("  %-22s edad media %6.1f max %6.1f | riesgo %5.2f%%/ano | mediana nominal %6.1f vs realizada %6.1f  (%s)"),
            TEXT(""), MeanAge, D.AgeMax, 100.f * Hazard, MedianNominal, MedianRealized,
            (MedianNominal > 0.f && MedianRealized > 0.f && MedianRealized < 0.5f * MedianNominal)
            ? TEXT("*** no llega ni a la mitad de su vida nominal")
            : TEXT("ok"));

        // Un limitante al 95-100% en todas las especies significa que solo hay UN
        // eje de competencia y que el modelo no puede repartir nicho, pase lo que
        // pase con el resto de parametros.
        UE_LOG(LogEco, Log,
            TEXT("  %-22s vigor medio %.3f | estres medio %.3f | limita: luz %4.1f%% agua %4.1f%% nutrientes %4.1f%%"),
            TEXT(""),
            static_cast<float>(D.VigorSum) * InvLive,
            static_cast<float>(D.StressSum) * InvLive,
            100.f * D.Lim[0] * InvLive, 100.f * D.Lim[1] * InvLive, 100.f * D.Lim[2] * InvLive);

        // EL EMBUDO DE RECLUTAMIENTO del ultimo tick. Sin este desglose, los cuatro
        // filtros son una caja negra: no se puede distinguir "no produce semillas"
        // de "las produce y las rechaza el espaciado" de "germinan y se mueren".
        if (SpeciesFlow.IsValidIndex(sp))
        {
            const FEcoSpeciesFlow& F = SpeciesFlow[sp];
            const float MeanJC = (F.JanzenConnellCount > 0)
                ? F.JanzenConnellSum / static_cast<float>(F.JanzenConnellCount) : 1.f;

            UE_LOG(LogEco, Log,
                TEXT("  %-22s embudo: %5d sem -> fuera %4d | espaciado %5d | luz %5d | JC medio %.3f -> %4d germinan"),
                TEXT(""), F.SeedsEmitted, F.RejectedOffMap, F.RejectedSpacing, F.RejectedLight, MeanJC, F.Germinated);

            const int32 Deaths = F.DeathsByAge + F.DeathsByCondition;
            UE_LOG(LogEco, Log,
                TEXT("  %-22s muertes del tick: %4d (edad %4.1f%% / condicion %4.1f%%)  %s"),
                TEXT(""), Deaths,
                Deaths > 0 ? 100.f * F.DeathsByAge / Deaths : 0.f,
                Deaths > 0 ? 100.f * F.DeathsByCondition / Deaths : 0.f,
                // El aviso solo tiene sentido si la poblacion ha tenido TIEMPO de
                // envejecer: en un bosque joven es normal que nadie muera de viejo,
                // y decir "la longevidad no compra nada" ahi seria ruido.
                (Deaths > 20 && F.DeathsByAge * 5 < Deaths && D.AgeMax > 0.5f * MedianNominal)
                ? TEXT("<- el canal de edad casi no participa: la longevidad no compra nada")
                : TEXT(""));
        }
    }
}

// ---------------------------------------------------------------------------
//  Percentiles de los campos base (Eco.PercentilesCampos)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::LogFieldPercentiles() const
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S) { return; }

    // Copia + sort: los campos base son inmutables y esto se pide a mano una vez
    // cada varias horas de trabajo, asi que no merece la pena nada mas fino.
    auto LogOne = [](const TCHAR* Label, const TArray<float>& Data, float OutputMax)
        {
            if (Data.Num() == 0)
            {
                UE_LOG(LogEco, Warning, TEXT("[Campos] %s: vacio."), Label);
                return;
            }

            TArray<float> Sorted = Data;
            Sorted.Sort();

            auto P = [&Sorted](float Q)
                {
                    const int32 Idx = FMath::Clamp(
                        FMath::RoundToInt(Q * (Sorted.Num() - 1)), 0, Sorted.Num() - 1);
                    return Sorted[Idx];
                };

            const float Inv = (OutputMax > KINDA_SMALL_NUMBER) ? 1.f / OutputMax : 0.f;

            UE_LOG(LogEco, Log, TEXT("[Campos] %s  (rango 0..%.1f, %d celdas)"),
                Label, OutputMax, Data.Num());
            UE_LOG(LogEco, Log,
                TEXT("          p5 %.2f | p25 %.2f | p50 %.2f | p75 %.2f | p95 %.2f   (absoluto)"),
                P(0.05f), P(0.25f), P(0.50f), P(0.75f), P(0.95f));
            UE_LOG(LogEco, Log,
                TEXT("          p5 %.2f | p25 %.2f | p50 %.2f | p75 %.2f | p95 %.2f   <- FRACCIONES: usa p25/p50/p75 como los tres optimos"),
                P(0.05f) * Inv, P(0.25f) * Inv, P(0.50f) * Inv, P(0.75f) * Inv, P(0.95f) * Inv);

            // ANCHURA SUGERIDA = el HUECO MINIMO entre optimos consecutivos, no la
            // semidistancia intercuartilica.
            //
            // La media (p75-p25)/2 solo vale si el campo es SIMETRICO. El TWI del
            // agua no lo es ni de lejos -su mediana cae en torno al 17% del rango y
            // su p95 por encima del 60%-, asi que con esa formula el par p25-p50
            // queda separado menos de una anchura y la propia auditoria lo etiqueta
            // como "solape alto": las dos especies mas secas responden casi igual en
            // todas las celdas y el eje deja de repartir territorio.
            //
            // Con el hueco minimo, las tres campanas quedan separadas >= 1 anchura
            // tambien en un campo sesgado, que es la condicion que pide el bloque 5
            // de Eco.AuditarEspecies para que cada especie tenga zona propia.
            const float GapLow = (P(0.50f) - P(0.25f)) * Inv;
            const float GapHigh = (P(0.75f) - P(0.50f)) * Inv;
            const float SuggestedWidth = FMath::Min(GapLow, GapHigh);
            UE_LOG(LogEco, Log,
                TEXT("          anchura sugerida (Tolerance): %.3f   [huecos p25-p50 %.3f | p50-p75 %.3f]"),
                FMath::Max(SuggestedWidth, 0.01f), GapLow, GapHigh);
        };

    UE_LOG(LogEco, Log, TEXT("========================================================================"));
    LogOne(TEXT("AGUA (TWI) base"), WaterBase.Field.Data, S->WaterOutputMax);
    LogOne(TEXT("NUTRIENTES base"), NutrientBase.Field.Data, S->NutrientOutputMax);

    // Y AHORA LOS PERCENTILES DEL POOL, que es lo que los arboles leen de verdad.
    // El campo base es el potencial del terreno, congelado; el tick muestrea el
    // POOL, que se agota con el consumo y solo se recarga poco a poco hacia el base.
    // Colocar los optimos sobre los percentiles del base los deja sistematicamente
    // por encima del recurso realmente disponible.
    LogOne(TEXT("AGUA pool (lo que leen los arboles)"), WaterPool.Current.Data, S->WaterOutputMax);
    LogOne(TEXT("NUTRIENTES pool (lo que leen los arboles)"), NutrientPool.Current.Data, S->NutrientOutputMax);

    // LUZ A RAS DE SUELO: dice si existe un SOTOBOSQUE. Es lo que hay que mirar
    // para colocar los MinLightForGermination de cada especie: si el p25 de la luz
    // esta en 0.30, poner el umbral de la climax en 0.10 le da un cuarto del mapa
    // en exclusiva -la pionera no puede germinar ahi- y ese es el mecanismo por el
    // que hereda los huecos sin competir por numero de semillas.
    if (LightCoarse.IsValid() && HeightField.IsValid())
    {
        const FField2D& R = HeightField.Field;
        TArray<float> GroundLight;
        GroundLight.SetNumUninitialized(R.Width * R.Height);
        for (int32 y = 0; y < R.Height; ++y)
        {
            const double Yc = R.NodeWorldY(y);
            for (int32 x = 0; x < R.Width; ++x)
            {
                const double Xc = R.NodeWorldX(x);
                const double Zc = HeightField.SampleHeight(Xc, Yc);
                GroundLight[y * R.Width + x] = LightCoarse.SampleLightSmooth(FVector(Xc, Yc, Zc));
            }
        }
        LogOne(TEXT("LUZ a ras de suelo"), GroundLight, FLightFieldCoarse::FullSunlight);
        UE_LOG(LogEco, Log,
            TEXT("          (con dosel: p25-p50 marcan donde vive el banco de plantulas de las tolerantes)"));
    }
    else
    {
        UE_LOG(LogEco, Warning,
            TEXT("[Campos] LUZ: la rejilla aun no esta lista (corre unos ticks antes)."));
    }
    UE_LOG(LogEco, Log, TEXT("========================================================================"));
}

// ---------------------------------------------------------------------------
//  Instrumentacion de diagnostico (Fase 0 del plan de coexistencia)
// ---------------------------------------------------------------------------
//
// Los cuatro comandos de esta seccion existen para poder responder con NUMEROS a
// "que le pasa a esta especie". Sin ellos la calibracion es a ciegas: los assets
// de especie son binarios y sus valores no aparecen en ningun log, el embudo de
// reclutamiento es una caja negra, y una especie extinta desaparecia del informe
// sin dejar rastro. Ninguno toca el estado de la simulacion ni consume RNG de los
// streams de la simulacion, asi que llamarlos no altera el fingerprint.

void UEcosystemSubsystem::LogSpeciesDump() const
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S || ResolvedSpecies.Num() == 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Especies] No hay especies configuradas."));
        return;
    }

    // Luz de referencia para leer la DIFERENCIACION en sombra, que es donde la
    // tolerancia deberia decidir algo. Valor de lectura: no entra en la simulacion.
    constexpr float kDeepShadeQ = 0.15f;
    const float FullSun = FLightFieldCoarse::FullSunlight;
    const float CellSizeCm = (float)WaterBase.Field.CellSize;

    UE_LOG(LogEco, Log, TEXT("========================================================================"));
    UE_LOG(LogEco, Log, TEXT("[Especies] %d especies | modo de vigor %d | KlMax %.3f | umbral de estres %.2f"),
        ResolvedSpecies.Num(), (int32)S->VigorCombineMode, S->LightHalfSaturationMax, S->StressVigorThreshold);

    for (int32 i = 0; i < ResolvedSpecies.Num(); ++i)
    {
        const USpeciesData* Sp = ResolvedSpecies[i];
        if (!Sp)
        {
            UE_LOG(LogEco, Error, TEXT("  [%d] *** ASSET NO CARGADO: esta especie no existe en la simulacion."), i);
            continue;
        }

        const EcoVigor::FSpeciesResponses R = EcoVigor::MakeSpeciesResponses(*Sp, *S);
        const float fLSun = EcoVigor::LightFactor(FullSun, R.Light);
        const float fLShade = EcoVigor::LightFactor(kDeepShadeQ * FullSun, R.Light);

        UE_LOG(LogEco, Log, TEXT("------------------------------------------------------------------------"));
        UE_LOG(LogEco, Log, TEXT("  [%d] %s"), i, *Sp->SpeciesName.ToString());
        UE_LOG(LogEco, Log,
            TEXT("      vida: GrowthRate %.3f | MaxBiomass %.0f | Longevity %.0f | MaturityAge %.0f | MaxHeight %.0f cm"),
            Sp->GrowthRate, Sp->MaxBiomass, Sp->Longevity, Sp->MaturityAge, Sp->MaxHeightCm);

        // Tiempo hasta el 70% de MaxBiomass frente a la esperanza de vida: el
        // criterio duro de si una especie puede COMPLETAR su ciclo. La solucion
        // logistica desde la biomasa de plantula, con vigor 1 (cota optimista).
        const float B0 = FMath::Max(S->GerminationBiomassFraction, KINDA_SMALL_NUMBER);
        const float T70 = (Sp->GrowthRate > KINDA_SMALL_NUMBER)
            ? FMath::Loge((0.7f / 0.3f) / (B0 / (1.f - B0))) / Sp->GrowthRate : -1.f;
        UE_LOG(LogEco, Log,
            TEXT("      t70 (anos hasta el 70%% de biomasa, vigor=1): %.1f   %s"),
            T70, (T70 > 0.f && T70 > 0.5f * Sp->Longevity)
            ? TEXT("*** tarda mas de media vida en crecer: dificilmente cerrara su ciclo") : TEXT(""));

        UE_LOG(LogEco, Log,
            TEXT("      luz: ShadeTolerance %.2f -> Amax %.3f | fL pleno sol %.3f | fL sombra(Q=%.2f) %.3f  %s"),
            Sp->ShadeTolerance, R.Light.MaxAssimilation, fLSun, kDeepShadeQ, fLShade,
            (fLSun < S->StressVigorThreshold)
            ? TEXT("*** CONDENADA: se estresa a pleno sol y sin vecinos") : TEXT(""));

        // Anchuras ABSOLUTAS: es lo que hay que comparar con el rango real del
        // campo (Eco.PercentilesCampos), no la fraccion que guarda el asset.
        UE_LOG(LogEco, Log,
            TEXT("      agua:  optimo %.2f (=%.2f abs) | anchura %.3f (=%.2f abs) | exceso %.2f abs | encharca %s"),
            Sp->WaterOptimum, R.Water.OptimumAbs, Sp->WaterTolerance, R.Water.WidthAbs,
            R.Water.ExcessWidthAbs, Sp->bWaterloggingPenalty ? TEXT("si") : TEXT("no"));
        UE_LOG(LogEco, Log,
            TEXT("      nutr:  optimo %.2f (=%.2f abs) | anchura %.3f (=%.2f abs) | exceso %.2f abs | penaliza %s"),
            Sp->NutrientOptimum, R.Nutrient.OptimumAbs, Sp->NutrientTolerance, R.Nutrient.WidthAbs,
            R.Nutrient.ExcessWidthAbs, Sp->bNutrientExcessPenalty ? TEXT("si") : TEXT("no"));

        // Lluvia de semillas de un adulto YA CRECIDO: el numero que de verdad
        // compara la fecundidad entre especies, y que no se imprimia en ningun sitio.
        const float RelAdult = 1.f;
        const float SeedScale = (S->SeedBiomassHalfSaturation > 0.f)
            ? RelAdult / (RelAdult + S->SeedBiomassHalfSaturation) : RelAdult;
        UE_LOG(LogEco, Log,
            TEXT("      semilla: %.2f/ano de un adulto crecido | germinacion x%.2f | dispersion %.0f m | luz minima %.2f"),
            S->SeedsPerAdultPerYear * Sp->SeedRateScale * SeedScale,
            Sp->GerminationRateScale, Sp->SeedDispersalRadius, Sp->MinLightForGermination);

        if (Sp->SeedDispersalRadius * 100.f < S->ConspecificInhibitionRadiusCm)
        {
            UE_LOG(LogEco, Warning,
                TEXT("      -> dispersion (%.0f m) menor que el radio de Janzen-Connell (%.0f m): ninguna semilla escapa "
                    "del circulo de su madre y el rescate de la especie rara queda topado."),
                Sp->SeedDispersalRadius, S->ConspecificInhibitionRadiusCm / 100.f);
        }

        // Radio radicular efectivo: dice si el kernel de consumo llega o no a los
        // vecinos, que es lo que decide si existe competencia subterranea.
        const float MinAdult = S->MinRootRadiusCells * CellSizeCm;
        const float R30 = EcologyRules::EffectiveRootRadiusCm(Sp->RootRadius, 0.3f, 1.f, MinAdult);
        const float R100 = EcologyRules::EffectiveRootRadiusCm(Sp->RootRadius, 1.f, 1.f, MinAdult);
        UE_LOG(LogEco, Log,
            TEXT("      raiz: nominal %.1f m | efectivo a 30%% biomasa %.0f cm | a 100%% %.0f cm | celda %.0f cm  %s"),
            Sp->RootRadius, R30, R100, CellSizeCm,
            (R100 <= CellSizeCm) ? TEXT("*** no alcanza ni a la celda vecina") : TEXT(""));

        UE_LOG(LogEco, Log,
            TEXT("      declive: senescencia a %.0f anos (%.2f x longevidad) | suprimido: mortalidad %.3f/ano, crecimiento x%.2f"),
            Sp->SenescenceAgeFraction * Sp->Longevity, Sp->SenescenceAgeFraction,
            Sp->SuppressedMortalityPerYear, Sp->SuppressedGrowthScale);
    }
    UE_LOG(LogEco, Log, TEXT("========================================================================"));
}

void UEcosystemSubsystem::LogNicheWinnerMap()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S || !HeightField.IsValid()) { return; }

    const int32 NumSpecies = ResolvedSpecies.Num();
    if (NumSpecies == 0) { return; }

    const EcoCarbon::FCO2Params CO2 = GetCO2Params();
    const int32 NumCells = HeightField.Field.Data.Num();

    TArray<float> BestVigor;   BestVigor.Init(-1.f, NumCells);
    TArray<int32> BestSpecies; BestSpecies.Init(INDEX_NONE, NumCells);

    FField2D Suit;
    for (int32 i = 0; i < NumSpecies; ++i)
    {
        const USpeciesData* Sp = ResolvedSpecies[i];
        if (!Sp) { continue; }

        EcoVigor::BakeSuitabilityField(HeightField, SuitabilityWaterField(), SuitabilityNutrientField(),
            LightCoarse, EcoVigor::MakeSpeciesResponses(*Sp, *S), S->VigorCombineMode, Suit, nullptr, &CO2);

        if (Suit.Data.Num() != NumCells) { continue; }
        for (int32 c = 0; c < NumCells; ++c)
        {
            if (Suit.Data[c] > BestVigor[c]) { BestVigor[c] = Suit.Data[c]; BestSpecies[c] = i; }
        }
    }

    TArray<int32> Wins; Wins.SetNumZeroed(NumSpecies);
    for (int32 c = 0; c < NumCells; ++c)
    {
        if (BestSpecies[c] != INDEX_NONE) { ++Wins[BestSpecies[c]]; }
    }

    UE_LOG(LogEco, Log, TEXT("========================================================================"));
    UE_LOG(LogEco, Log, TEXT("[Nicho] Fraccion del mapa donde cada especie tendria MAS vigor (%d celdas)"), NumCells);
    UE_LOG(LogEco, Log, TEXT("[Nicho] Muestreado sobre %s"),
        WaterPool.Current.IsValid() ? TEXT("el POOL (lo que los arboles leen de verdad)")
                                    : TEXT("el campo BASE (la simulacion aun no ha corrido)"));

    float MaxShare = 0.f;
    float MinShare = 1.f;
    for (int32 i = 0; i < NumSpecies; ++i)
    {
        const float Share = (NumCells > 0) ? (float)Wins[i] / (float)NumCells : 0.f;
        MaxShare = FMath::Max(MaxShare, Share);
        MinShare = FMath::Min(MinShare, Share);
        UE_LOG(LogEco, Log, TEXT("   %-22s gana el %5.1f%% del mapa"),
            ResolvedSpecies[i] ? *ResolvedSpecies[i]->SpeciesName.ToString() : TEXT("?"), 100.f * Share);
    }

    // Es la prueba DIRECTA, y se calcula sin simular un solo tick: si una especie
    // es la mejor en practicamente todo el mapa, la exclusion competitiva ya esta
    // escrita en la forma de las curvas y no hace falta gastar mil ticks para verla.
    if (MaxShare > 0.70f)
    {
        UE_LOG(LogEco, Warning,
            TEXT("   *** Una especie gana en el %.0f%% del mapa: no hay reparto de nicho posible. "
                "Separa los optimos, estrecha las campanas o revisa los ejes monotonos (Eco.AuditarEspecies)."),
            100.f * MaxShare);
    }
    else if (MinShare < 0.10f)
    {
        UE_LOG(LogEco, Warning,
            TEXT("   *** Alguna especie gana en menos del 10%% del mapa: tiene refugio, pero muy pequeno."));
    }
    else
    {
        UE_LOG(LogEco, Log, TEXT("   Cada especie tiene una zona propia apreciable. Bien."));
    }
    UE_LOG(LogEco, Log, TEXT("========================================================================"));

    // Y se pinta, para poder VER donde esta la frontera entre nichos.
    TArray<float> Paint;
    Paint.SetNumUninitialized(NumCells);
    for (int32 c = 0; c < NumCells; ++c)
    {
        Paint[c] = (BestSpecies[c] == INDEX_NONE) ? 0.f
            : (float)(BestSpecies[c] + 1) / (float)NumSpecies;
    }
    PaintField(Paint, TEXT("Nicho ganador"), /*bAutoRange*/ false, 0.f, 1.f);
}

void UEcosystemSubsystem::LogLightProfile(double Xcm, double Ycm) const
{
    if (!LightCoarse.IsValid() || !HeightField.IsValid())
    {
        UE_LOG(LogEco, Warning, TEXT("[Luz] La rejilla aun no esta lista (corre un tick antes)."));
        return;
    }

    const double GroundZ = HeightField.SampleHeight(Xcm, Ycm);

    UE_LOG(LogEco, Log, TEXT("========================================================================"));
    UE_LOG(LogEco, Log, TEXT("[Luz] Perfil en (%.0f, %.0f), suelo a Z=%.0f | k=%.2f, piso difuso=%.3f"),
        Xcm, Ycm, GroundZ, LightCoarse.ExtinctionK, LightCoarse.DiffuseFloor);
    UE_LOG(LogEco, Log, TEXT("      altura sobre el suelo (cm) ->  Q"));

    // De arriba abajo: en un dosel real Q tiene que DECRECER hacia el suelo. Si el
    // perfil sale al reves -claro abajo y oscuro arriba- el deposito de copa esta
    // invirtiendo la sombra, que es el bug que dejaba el sotobosque a plena luz.
    const int32 TopLayer = LightCoarse.Layers - 1;
    for (int32 iz = TopLayer; iz >= LightCoarse.GroundLayerIndex(); --iz)
    {
        const double HeightAboveGround = (iz + 0.5) * LightCoarse.CellSizeZ + LightCoarse.BaseZ;
        const float Q = LightCoarse.SampleLight(FVector(Xcm, Ycm, GroundZ + HeightAboveGround));
        UE_LOG(LogEco, Log, TEXT("      %8.0f  ->  %.4f"), HeightAboveGround, Q);
    }

    const float QGround = LightCoarse.SampleLightSmooth(FVector(Xcm, Ycm, GroundZ));
    UE_LOG(LogEco, Log, TEXT("      a ras de suelo (interpolado): Q = %.4f  %s"),
        QGround, (QGround > 0.85f)
        ? TEXT("<- practicamente pleno sol: NO hay sotobosque en este punto")
        : TEXT(""));
    UE_LOG(LogEco, Log, TEXT("========================================================================"));
}

void UEcosystemSubsystem::LogConfigCoverage()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S) { return; }

    const UClass* Cls = S->GetClass();
    const FString SectionHeader = FString::Printf(TEXT("[%s]"), *Cls->GetPathName());
    const FString IniPath = S->GetDefaultConfigFilename();

    // Se lee el .ini como TEXTO en vez de consultar FConfigCacheIni: aqui interesa
    // que claves hay ESCRITAS en el fichero del proyecto, no el valor efectivo tras
    // fusionar la cadena de .ini del motor -que es justo lo que enmascara que una
    // propiedad no este fijada-.
    FString Text;
    if (!FFileHelper::LoadFileToString(Text, *IniPath))
    {
        UE_LOG(LogEco, Warning, TEXT("[Config] No se pudo leer %s"), *IniPath);
        return;
    }

    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines);

    TSet<FName> InIni;
    bool bInSection = false;
    for (const FString& Raw : Lines)
    {
        const FString Line = Raw.TrimStartAndEnd();
        if (Line.StartsWith(TEXT("[")))
        {
            bInSection = Line.Equals(SectionHeader, ESearchCase::IgnoreCase);
            continue;
        }
        if (!bInSection || Line.IsEmpty() || Line.StartsWith(TEXT(";"))) { continue; }

        FString Key, Value;
        if (Line.Split(TEXT("="), &Key, &Value))
        {
            // '+Clave' / '-Clave' son la sintaxis de array de UE.
            Key = Key.TrimStartAndEnd();
            if (Key.StartsWith(TEXT("+")) || Key.StartsWith(TEXT("-"))) { Key.RightChopInline(1); }
            InIni.Add(FName(*Key));
        }
    }

    TArray<FString> Missing;
    TSet<FName> Declared;
    for (TFieldIterator<FProperty> It(Cls); It; ++It)
    {
        const FProperty* Prop = *It;
        if (!Prop->HasAnyPropertyFlags(CPF_Config)) { continue; }

        Declared.Add(Prop->GetFName());
        if (!InIni.Contains(Prop->GetFName())) { Missing.Add(Prop->GetName()); }
    }

    TArray<FString> Orphan;
    for (const FName& Key : InIni)
    {
        if (!Declared.Contains(Key)) { Orphan.Add(Key.ToString()); }
    }

    Missing.Sort();
    Orphan.Sort();

    UE_LOG(LogEco, Log, TEXT("========================================================================"));
    UE_LOG(LogEco, Log, TEXT("[Config] %s"), *IniPath);
    UE_LOG(LogEco, Log, TEXT("[Config] %d propiedades config declaradas | %d claves escritas en el .ini"),
        Declared.Num(), InIni.Num());

    // Una propiedad sin clave no es un detalle de higiene: significa que el .ini ha
    // dejado de ser el registro reproducible de la corrida -un cambio de default en
    // C++ altera la ecologia sin que nada visible cambie- y es como se acaba con una
    // calibracion hibrida, con valores ajustados a mano para un modelo conviviendo
    // con defaults de otro.
    if (Missing.Num() > 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Config] %d propiedades NO estan en el .ini (corren con el default de C++):"), Missing.Num());
        for (const FString& Name : Missing) { UE_LOG(LogEco, Warning, TEXT("           %s"), *Name); }
    }
    if (Orphan.Num() > 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Config] %d claves HUERFANAS en el .ini (ya no mapean a ninguna propiedad):"), Orphan.Num());
        for (const FString& Name : Orphan) { UE_LOG(LogEco, Warning, TEXT("           %s"), *Name); }
    }
    if (Missing.Num() == 0 && Orphan.Num() == 0)
    {
        UE_LOG(LogEco, Log, TEXT("[Config] El .ini fija todas las propiedades y no tiene huerfanas. Bien."));
    }
    UE_LOG(LogEco, Log, TEXT("========================================================================"));
}

// ---------------------------------------------------------------------------
//  Historico demografico (Eco.Demografia.CSV)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::RecordDemographySample()
{
    const int32 NumSpecies = ResolvedSpecies.Num();
    if (NumSpecies == 0) { return; }

    TArray<FEcoDemoSample> Row;
    Row.SetNum(NumSpecies);

    // Sumas en double: una corrida larga acumula cientos de miles de terminos, y
    // en float el redondeo se come justo las medias pequeñas, que son las que
    // interesan (una especie en declive tiene el vigor bajo).
    TArray<double> BiomassFracSum, VigorSum, StressSum;
    BiomassFracSum.SetNumZeroed(NumSpecies);
    VigorSum.SetNumZeroed(NumSpecies);
    StressSum.SetNumZeroed(NumSpecies);

    for (int32 i = 0; i < Agents_Read.Num(); ++i)
    {
        if (Agents_Read.State[i] == ETreeState::Dead) { continue; }

        const int32 s = Agents_Read.SpeciesId[i];
        if (!Row.IsValidIndex(s)) { continue; }

        const USpeciesData* Sp = ResolveSpecies(static_cast<uint16>(s));
        if (!Sp) { continue; }

        ++Row[s].Count;

        switch (Agents_Read.State[i])
        {
        case ETreeState::Sapling:    ++Row[s].Saplings;   break;
        case ETreeState::Suppressed: ++Row[s].Suppressed; break;
        case ETreeState::Senescent:  ++Row[s].Senescent;  break;
        default: break;
        }

        // Biomasa RELATIVA a la maxima de su especie: en absoluto no se pueden
        // comparar un til de 150 y un brezo de 40.
        BiomassFracSum[s] += Agents_Read.Biomass[i] / FMath::Max(Sp->MaxBiomass, KINDA_SMALL_NUMBER);
        VigorSum[s] += Agents_Read.Vigor[i];
        StressSum[s] += Agents_Read.Stress[i];

        switch (static_cast<EEcoLimiter>(Agents_Read.Limiter[i]))
        {
        case EEcoLimiter::Light:    ++Row[s].LimitedByLight;    break;
        case EEcoLimiter::Water:    ++Row[s].LimitedByWater;    break;
        case EEcoLimiter::Nutrient: ++Row[s].LimitedByNutrient; break;
        default: break;
        }
    }

    for (int32 s = 0; s < NumSpecies; ++s)
    {
        Row[s].Tick = TickCount;
        Row[s].SpeciesIndex = s;

        const double Inv = (Row[s].Count > 0) ? 1.0 / static_cast<double>(Row[s].Count) : 0.0;
        Row[s].MeanBiomass = static_cast<float>(BiomassFracSum[s] * Inv);
        Row[s].MeanVigor = static_cast<float>(VigorSum[s] * Inv);
        Row[s].MeanStress = static_cast<float>(StressSum[s] * Inv);

        // Los flujos del ultimo tick: son lo que convierte una serie de recuentos en
        // una tabla de vida. Con nacimientos y muertes por especie, R0 sale de una
        // division; sin ellos, dn/dt es observable pero no se puede descomponer en
        // reclutamiento menos mortalidad, que es justo lo que decide donde tocar.
        if (SpeciesFlow.IsValidIndex(s)) { Row[s].Flow = SpeciesFlow[s]; }

        // Primer tick en que se observa a cero. Sin esto una extincion solo se
        // detecta por ausencia y su momento -el dato mas informativo de todo el
        // experimento- no queda registrado en ninguna parte.
        if (SpeciesExtinctionTick.IsValidIndex(s) && Row[s].Count == 0 && SpeciesExtinctionTick[s] < 0)
        {
            SpeciesExtinctionTick[s] = TickCount;
            UE_LOG(LogEco, Warning, TEXT("[Eco] *** La especie '%s' se ha EXTINGUIDO (tick %lld)."),
                ResolvedSpecies[s] ? *ResolvedSpecies[s]->SpeciesName.ToString() : TEXT("?"), TickCount);
        }

        // Se guarda la fila AUNQUE Count sea 0: una especie extinguida tiene que
        // aparecer como una linea que baja a cero en la grafica, no desaparecer
        // del CSV (que se leeria como "no habia datos").
        DemoHistory.Add(Row[s]);
    }
}

void UEcosystemSubsystem::SaveDemographyCsv(const FString& FullPath) const
{
    if (DemoHistory.Num() == 0)
    {
        UE_LOG(LogEco, Warning,
            TEXT("[Eco] No hay historico demografico todavia: corre la simulacion unos ticks."));
        return;
    }

    // Separador ';' y punto decimal: es lo que abre directamente un Excel en
    // español sin pasar por el asistente de importacion.
    // Los FLUJOS (nacimientos, muertes por canal y el embudo de rechazos) son lo
    // que convierte una serie de recuentos en una tabla de vida: con ellos R0 por
    // especie sale de una division en la hoja de calculo, y se puede distinguir
    // "recluta poco" de "se le mueren las plantulas" de "no llega a madurez".
    FString Csv = TEXT("tick;especie;n;plantulas;suprimidos;senescentes;biomasa_rel;vigor;estres;"
        "lim_luz;lim_agua;lim_nutrientes;"
        "semillas;rech_fuera;rech_espaciado;rech_luz;jc_medio;nacimientos;muertes_edad;muertes_condicion\n");
    Csv.Reserve(Csv.Len() + DemoHistory.Num() * 128);

    for (const FEcoDemoSample& Sample : DemoHistory)
    {
        const USpeciesData* Sp = ResolveSpecies(static_cast<uint16>(Sample.SpeciesIndex));
        const FEcoSpeciesFlow& F = Sample.Flow;
        const float MeanJC = (F.JanzenConnellCount > 0)
            ? F.JanzenConnellSum / static_cast<float>(F.JanzenConnellCount) : 1.f;

        Csv += FString::Printf(TEXT("%lld;%s;%d;%d;%d;%d;%.4f;%.4f;%.4f;%d;%d;%d;%d;%d;%d;%d;%.4f;%d;%d;%d\n"),
            Sample.Tick,
            Sp ? *Sp->SpeciesName.ToString() : TEXT("?"),
            Sample.Count, Sample.Saplings, Sample.Suppressed, Sample.Senescent,
            Sample.MeanBiomass, Sample.MeanVigor, Sample.MeanStress,
            Sample.LimitedByLight, Sample.LimitedByWater, Sample.LimitedByNutrient,
            F.SeedsEmitted, F.RejectedOffMap, F.RejectedSpacing, F.RejectedLight, MeanJC,
            F.Germinated, F.DeathsByAge, F.DeathsByCondition);
    }

    if (FFileHelper::SaveStringToFile(Csv, *FullPath))
    {
        UE_LOG(LogEco, Log, TEXT("[Eco] Historico demografico (%d filas) escrito en %s"),
            DemoHistory.Num(), *FullPath);
    }
    else
    {
        UE_LOG(LogEco, Error, TEXT("[Eco] No se pudo escribir %s"), *FullPath);
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
// v2: el SoA lleva dos arrays mas (Vigor y Limiter, instrumentacion). El bake
// enumera los campos con FTreePopulation::ForEachArray, asi que el formato cambia
// solo: un .ecobake v1 ya no se puede leer y LoadState lo rechaza limpiamente.
//
// v3: DOS cambios incompatibles a la vez. (a) ETreeState gana el estado Suppressed
// ANTES de Dead, asi que los valores serializados de v2 significan otra cosa.
// (b) FEcosystemRng gana un stream (Disturbance) y se serializa como bloque plano,
// asi que su sizeof cambia. Un bake v2 leido como v3 daria arboles muertos
// resucitados y streams desplazados; la version lo rechaza limpiamente.
static constexpr int32  kEcoBakeVersion = 3;

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

const FField2D& UEcosystemSubsystem::SuitabilityWaterField() const
{
    // El POOL si la simulacion ya ha corrido, el potencial del terreno si no.
    // Lo que los arboles leen es el pool, y puede estar bastante por debajo del
    // base: horneando la idoneidad sobre el base, el mapa de nicho declara un
    // reparto que en el recurso realmente disponible ya no existe.
    return WaterPool.Current.IsValid() ? WaterPool.Current : WaterBase.Field;
}

const FField2D& UEcosystemSubsystem::SuitabilityNutrientField() const
{
    return NutrientPool.Current.IsValid() ? NutrientPool.Current : NutrientBase.Field;
}

void UEcosystemSubsystem::PaintVigorField()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const int32 Index = FMath::Max(0, S->HeatmapSpeciesIndex);
    const USpeciesData* Sp = ResolveSpecies((uint16)Index);
    if (!Sp || !HeightField.IsValid()) { return; }

    FField2D Suit;
    // Se le pasan las MISMAS respuestas, el MISMO modo de combinacion y los MISMOS
    // parametros de CO2 que usa el tick: si el mapa de idoneidad y el crecimiento
    // real evaluaran modelos distintos, el heatmap dejaria de servir para explicar
    // por que el bosque crece donde crece.
    const EcoCarbon::FCO2Params CO2 = GetCO2Params();
    EcoVigor::BakeSuitabilityField(HeightField, SuitabilityWaterField(), SuitabilityNutrientField(),
        LightCoarse, EcoVigor::MakeSpeciesResponses(*Sp, *S), S->VigorCombineMode, Suit, nullptr, &CO2);

    PaintField(Suit.Data, *FString::Printf(TEXT("Vigor (%s)"), *Sp->SpeciesName.ToString()),
        /*bAutoRange*/ false, 0.f, 1.f);
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