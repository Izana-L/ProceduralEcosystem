/**
 * @file EcosystemSubsystem.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del motor del ecosistema: comandos de consola, arranque del
 *        mundo, bucle de tick, persistencia del bake e instrumentación.
 *
 * Contiene el registro de los comandos `Eco.*` y de los CVars de depuración; el montaje
 * del mundo en OnWorldBeginPlay (relieve, campos base, pools, caché de especies, rejilla
 * de luz y spatial hash); el bucle de frame con acumulador de paso fijo y presupuesto en
 * milisegundos; las etapas de SimulateTick —hash, luz gruesa, paso paralelo por chunks
 * deterministas, reducción serial, regeneración, pulsos de muerte, germinación y claros—;
 * el bake `.ecobake` con validación previa al commit; el anillo de eventos de muerte que
 * consume la capa de suelo; y la batería de informes de diagnóstico (demografía,
 * percentiles de campos, volcado de especies, mapa de nicho, perfil de luz, auditoría del
 * `.ini` y heatmaps).
 *
 * @ingroup eco_simulation
 * @see @ref bib_gapmodels
 * @see @ref bib_fiedler2004
 */

#include "Simulation/EcosystemSubsystem.h"
#include "Config/EcosystemSettings.h"
#include "Core/EcoStats.h"          // stat groups, Unreal Insights y CSV profiler
#include "Debug/FieldVisualizer.h"
#include "Species/SpeciesData.h"
#include "Ecology/EcologyRules.h"
#include "Ecology/TickScratch.h"
#include "Ecology/Vigor.h"
#include "Ecology/CarbonModel.h"    // multiplicador analítico de CO2 sobre el vigor
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

// Categoría de log local a esta unidad de traducción: se declara y se define en un solo
// sitio, sin DECLARE_LOG_CATEGORY_EXTERN en una cabecera reflejada por UHT.
DEFINE_LOG_CATEGORY_STATIC(LogEco, Log, All);

// ---------------------------------------------------------------------------
//  Comandos de consola (world-safe: buscan el subsistema en el UWorld pasado)
// ---------------------------------------------------------------------------

/** Devuelve el motor del ecosistema del mundo dado, o nullptr si el mundo no lo tiene. */
static UEcosystemSubsystem* GetEco(UWorld* World)
{
    return World ? World->GetSubsystem<UEcosystemSubsystem>() : nullptr;
}
static TAutoConsoleVariable<int32> CVarForceST(
    TEXT("Eco.ForceSingleThread"), 0, TEXT("1 = tick en un solo hilo (validar determinismo)."));

// Interruptor de ablación del multiplicador de CO2: -1 toma el valor de Project Settings,
// 0 y 1 lo fuerzan. Apagado, el factor vale 1.0 exacto y el vigor queda reducido al mínimo
// de Liebig, de modo que dos corridas con la misma semilla son comparables bit a bit.
static TAutoConsoleVariable<int32> CVarCO2(TEXT("Eco.CO2.Enable"), -1,
    TEXT("Multiplicador de CO2 sobre el vigor. -1 = Project Settings, 0 = off, 1 = on."));

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

static FAutoConsoleCommandWithWorld GEcoCheckFinite(TEXT("Eco.CheckFinite"),
    TEXT("Comprueba que no hay NaN/Inf en la poblacion ni en los campos."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogFiniteCheck(); }));

static FAutoConsoleCommandWithWorld GEcoPaintLight(TEXT("Eco.PaintLight"),
    TEXT("Pinta el heatmap de luz disponible a ras de suelo."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->PaintLightField(); }));

static FAutoConsoleCommandWithWorld GEcoProfile(TEXT("Eco.Profile"),
    TEXT("Desglosa el coste del tick por etapas y la memoria de las estructuras."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogTickProfile(); }));

static FAutoConsoleCommandWithWorld GEcoLogDeaths(TEXT("Eco.Deaths.Log"),
    TEXT("Loguea el nº total de muertes y las ultimas del buffer."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->LogRecentDeaths(); }));

// La estructura demográfica por especie es lo que permite calibrar la longevidad: el
// recuento de población no distingue un bosque maduro de un vivero, porque mil plántulas
// y mil árboles de dosel son el mismo número.
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

// El histórico se muestrea DURANTE el tick, así que este comando no mide nada: solo vuelca
// lo ya acumulado. Por eso se puede pedir en cualquier momento, incluso con la simulación
// en pausa, y sale la corrida entera y no el instante actual.
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

// --- Descomposición visible sobre el terreno ---
static FAutoConsoleCommandWithWorld GEcoPaintDecomp(TEXT("Eco.PaintDecomposition"),
    TEXT("Pinta el heatmap de descomposicion (puntos de muerte recientes) sobre el terreno."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcosystemSubsystem* S = GetEco(W)) S->PaintDecompositionField(); }));

static TAutoConsoleVariable<int32> CVarDecompLive(TEXT("Eco.Decomp.Live"), 0,
    TEXT("1 = repinta el heatmap de descomposicion cada tick (ver manchas aparecer y desvanecerse)."));

// --- Bake a un año objetivo: guardar y cargar el bosque ---
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
//  Parámetros del multiplicador de CO2
// ---------------------------------------------------------------------------
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

/**
 * Desglose del coste del tick por etapas y de la memoria de las estructuras grandes.
 *
 * Las medias por etapa son exponenciales y las mantiene el propio tick; aquí solo se
 * formatean junto al contexto que hace falta para interpretarlas (presupuesto por frame,
 * ticks del último frame y estado del factor de CO2).
 *
 * @see @ref bib_epicueperfilado
 */
void UEcosystemSubsystem::LogTickProfile() const
{
    // Memoria de las estructuras cuyo tamaño crece con el mundo o con la población.
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

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const EcoCarbon::FCO2Params CO2 = GetCO2Params();
    UE_LOG(LogEco, Log, TEXT("[Eco/Profile] Presupuesto del tick: %.1f ms/frame (%d ticks el ultimo frame, tope %d) | CO2 %s (max -%.0f%%)"),
        S->TickBudgetMsPerFrame, TicksLastFrame, MaxStepsPerFrame,
        CO2.bEnabled ? TEXT("ON") : TEXT("OFF"), CO2.MaxReduction * 100.f);
    UE_LOG(LogEco, Log, TEXT("[Eco/Profile] Siguiente paso: 'Eco.Frame' para el reparto del frame, "
        "'stat EcoSim' / 'stat Unit' / 'stat GPU' en pantalla, y Unreal Insights (-trace=cpu,frame,counters) para la timeline."));
}
// ---------------------------------------------------------------------------
//  CVars de depuración (se activan y desactivan en vivo desde la consola)
// ---------------------------------------------------------------------------
static TAutoConsoleVariable<int32> CVarDebugAgents(TEXT("Eco.Debug.Agents"), 1, TEXT("Dibuja los agentes de debug como esferas."));

// Apagado por defecto: dibujar una esfera por árbol vivo es trabajo O(población) por frame
// —a 20.000 árboles, decenas de miles de esferas— y además duplica lo que ya dibuja el
// render por instancias.
static TAutoConsoleVariable<int32> CVarDebugPopulation(TEXT("Eco.Debug.Population"), 0, TEXT("Dibuja la poblacion de arboles simulada como esferas. 0 = off (por defecto)."));

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

    // 1) Relieve: la geometría de la que cuelga todo lo demás. La forma (longitud de onda,
    //    persistencia, warp, crestas) y la erosión salen de Project Settings; aquí solo se
    //    convierten metros a centímetros y se lanza el bake.
    //    @see FHeightField::Generate
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

    // 2) Campos base: el potencial del terreno, calculado una sola vez y congelado. Ambos
    //    comparten geometría con HeightField (mismo Width/Height/CellSize/Origin), así que
    //    WaterPool y NutrientPool acaban con el mismo número de celdas.
    WaterBase.BakeFromHeightField(HeightField, S->WaterOutputMax, S->bFillWaterSinks,
        S->bWaterRankNormalization);

    NutrientBase.GeneratePatchyBase(HeightField.Field.Width, HeightField.Field.Height, HeightField.Field.CellSize,
        HeightField.Field.Origin, static_cast<uint32>(S->MasterSeed), S->NutrientOutputMax, S->NutrientPatchFrequency, S->NutrientOctaves);

    auto LogRange = [](const FField2D& F, const TCHAR* N) {
        float Mn, Mx;
        FField2D::MinMax(F.Data, Mn, Mx);
        UE_LOG(LogEco, Log, TEXT("[Eco] Campo %s: min=%.3f max=%.3f (%d celdas)"), N, Mn, Mx, F.Data.Num()); };
    LogRange(WaterBase.Field, TEXT("Agua"));  LogRange(NutrientBase.Field, TEXT("Nutrientes"));

    // 3) Estado runtime: los pools arrancan llenos al nivel del base.
    WaterPool.InitFromBase(WaterBase.Field);
    NutrientPool.InitFromBase(NutrientBase.Field);

    // Campo de descomposición: misma geometría que los campos de recursos y a cero,
    // porque todavía no ha muerto nadie.
    DecompositionField.Init(NutrientBase.Field.Width, NutrientBase.Field.Height, NutrientBase.Field.CellSize, NutrientBase.Field.Origin, 0.f);
    // El anillo de muertes se dimensiona UNA sola vez. Escritor (RecordDeathEvent) y lector
    // (CollectNewDeathEvents) tienen que compartir el mismo módulo: UEcosystemSettings es un
    // UDeveloperSettings editable en vivo, y releer DeathEventBufferSize en cada muerte
    // haría que ambos indexaran distinto a mitad de corrida y la capa de suelo colocaría
    // tocones en las coordenadas de otros árboles.
    RecentDeaths.Reset();
    RecentDeaths.SetNum(FMath::Max(0, S->DeathEventBufferSize));
    DeathEventCounter = 0;

    const FBox2D Bounds = HeightField.GetWorldBounds();

    // 4) Caché de especies: una LoadSynchronous por especie, no por árbol y tick. Va antes
    //    de la rejilla de luz porque ésta se dimensiona a partir de la especie más alta.
    ResolvedSpecies.Reset();
    for (const TSoftObjectPtr<USpeciesData>& SoftSp : S->Species)
    {
        USpeciesData* Loaded = SoftSp.LoadSynchronous();
        if (!Loaded)
        {
            // Una entrada que no resuelve dejaría un hueco silencioso: la especie no
            // existiría en la simulación y nada lo diría. Pasa con assets binarios no
            // descargados (punteros de Git LFS), de ahí el log de error explícito.
            UE_LOG(LogEco, Error, TEXT("[Eco] La especie '%s' no se pudo cargar: no participara en la simulacion."),
                *SoftSp.ToString());
        }
        ResolvedSpecies.Add(Loaded);
    }

    SpeciesExtinctionTick.Init(-1, ResolvedSpecies.Num());

    // 5) Rejilla de luz gruesa, RELATIVA AL TERRENO: la vertical se mide sobre la cota del
    //    suelo, no en Z absoluta, así que basta con cubrir el árbol más alto y sus dos
    //    márgenes (una veintena de capas). En Z absoluta habría que cubrir todo el desnivel
    //    del relieve —un par de centenares de capas de las que cada columna usa una decena—.
    const int32 LightW = FMath::Max(1, FMath::CeilToInt32((Bounds.Max.X - Bounds.Min.X) / S->LightCoarseCellSizeCm));
    const int32 LightH = FMath::Max(1, FMath::CeilToInt32((Bounds.Max.Y - Bounds.Min.Y) / S->LightCoarseCellSizeCm));

    float TallestSpeciesCm = 0.f;
    for (const TObjectPtr<USpeciesData>& Sp : ResolvedSpecies)
    {
        if (Sp) { TallestSpeciesCm = FMath::Max(TallestSpeciesCm, Sp->MaxHeightCm); }
    }
    if (TallestSpeciesCm <= 0.f) { TallestSpeciesCm = 2000.f; } // sin especies: 20 m de cortesia

    // El vóxel VERTICAL se configura aparte del horizontal: si ambos midieran lo mismo, la
    // banda de regeneración entera cabría en una sola capa y las plántulas dejarían de
    // existir como estrato dentro de la rejilla de luz.
    const double LightSpanZ = TallestSpeciesCm + S->LightCanopyHeadroomCm + S->LightGroundClearanceCm;
    const int32  LightLayers = FMath::Max(2, FMath::CeilToInt32(LightSpanZ / S->LightCoarseCellSizeZCm));
    LightCoarse.Init(LightW, LightH, LightLayers,
        S->LightCoarseCellSizeCm, S->LightCoarseCellSizeZCm, Bounds.Min,
        /*BaseZ = offset de la capa 0 bajo el suelo*/ -(double)S->LightGroundClearanceCm);
    LightCoarse.SetExtinctionParams(S->LightExtinctionK, S->DiffuseLightFloor);

    // Cota del terreno en el centro de cada columna: es lo que hace la rejilla relativa al
    // suelo. Se muestrea una sola vez, porque el relieve no cambia en runtime.
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

    // 6) Spatial hash de agentes: la geometría se fija una vez y el contenido se repuebla
    //    cada tick con Build().
    Hash.Init(Bounds, S->SpatialHashCellSizeCm);

    // 7) Visualizador de campos (heatmaps).
    FieldViz = NewObject<UFieldVisualizer>(this);
    FieldViz->Initialize(HeightField.Field.Width, HeightField.Field.Height);

    // A partir de aquí es seguro tickear, y es lo que esperan las capas de render y suelo.
    bWorldReady = true;
}

void UEcosystemSubsystem::Deinitialize()
{
    // Se sueltan todas las referencias, no solo el decal: el recolector se encargaría igual
    // porque son transitorias, pero conservar punteros a objetos de un mundo que ya no
    // existe es una fuente clásica de accesos tardíos.
    ClearHeroTrees();

    if (HeatmapDecal)
    {
        HeatmapDecal->Destroy();
        HeatmapDecal = nullptr;
    }
    HeatmapMID = nullptr;
    FieldViz = nullptr;

    OnStateLoaded.Clear(); // sin suscriptores de un mundo que se va

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
//  Tick de frame: desacopla el tiempo ecológico del frame de render
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::Tick(float DeltaTime)
{
    if (!bWorldReady)
    {
        return;
    }

    // ====================================================================
    // PRESUPUESTO DE TIEMPO DEL TICK DENTRO DEL FRAME
    // ====================================================================
    // MaxStepsPerFrame acota el NÚMERO de ticks, pero el número no es lo que hay que
    // repartir: un tick con 200 árboles cuesta microsegundos y con 20.000 puede costar
    // varios milisegundos. Aquí se acota el TIEMPO de pared: en cuanto los ticks del frame
    // agotan su presupuesto, el resto espera al siguiente. El efecto visible es que la
    // simulación se ralentiza en vez de hundir el framerate.
    const UEcosystemSettings* Settings = UEcosystemSettings::Get();
    const double BudgetMs = FMath::Max(0.f, Settings->TickBudgetMsPerFrame);
    const double FrameT0 = FPlatformTime::Seconds();
    auto OverBudget = [BudgetMs, FrameT0]() -> bool
        {
            return BudgetMs > 0.0 && (FPlatformTime::Seconds() - FrameT0) * 1000.0 >= BudgetMs;
        };

    TicksLastFrame = 0;

    // Pasos manuales (Eco.Step): se ejecutan aunque la simulación esté pausada, pero
    // amortizados por frames y con log de progreso. Vaciar PendingSteps de golpe congelaría
    // el editor varios segundos sin ninguna señal, que es justo lo que ocurre al pedir los
    // cientos de ticks con los que se lleva el bosque a un año objetivo.
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

            // El corte va al FINAL del cuerpo para garantizar al menos un paso por frame:
            // con un presupuesto muy apretado, Eco.Step no avanzaría nunca.
            if (OverBudget()) { break; }
        }

        if (PendingSteps > 0 && (PendingSteps % 50) < StepCap)
        {
            UE_LOG(LogEco, Log, TEXT("[Eco] Eco.Step: quedan %d ticks (tick actual %lld, %d arboles)."),
                PendingSteps, TickCount, Agents_Read.Num());
        }
    }

    // Avance automático (modo vivo).
    if (!bPaused)
    {
        Accumulator += DeltaTime;

        // Tope de deuda del acumulador. Tras un hitch —compilar shaders, hornear la
        // librería, cargar un bake— el acumulador arrastraría varios segundos y la
        // simulación correría a MaxStepsPerFrame durante minutos con el framerate hundido.
        // La deuda que no se puede pagar se descarta a propósito: se pierde un instante de
        // tiempo simulado en vez de arrastrar el problema.
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

    // Contadores para `stat EcoSim` y para el CSV profiler.
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
//  Tick de simulación
// ---------------------------------------------------------------------------

// Los factores de forma de copa (CanopyRadiusFraction, CanopyDepthFraction y
// CanopyLeafAreaIndex) y la biomasa de germinación (GerminationBiomassFraction) no son
// constantes de esta unidad de traducción: entran en la rejilla de luz y en la germinación,
// o sea que alteran el resultado ecológico, y por eso viven en UEcosystemSettings como
// parte de la configuración reproducible del proyecto.

/** Tope de tareas del ParallelFor del tick.
 *
 *  Es una constante y no un valor derivado de la máquina para que la partición en chunks
 *  —y con ella el orden en que se reducen los deltas— sea idéntica en cualquier CPU.
 *  @see PrepareTickScratch */
static constexpr int32 kMaxTickChunks = 32;

/**
 * Reparte [0, PopulationNum) en NumChunks tramos contiguos con aritmética entera.
 *
 * @param OutBegin Primer índice del tramo (inclusive).
 * @param OutEnd   Índice siguiente al último del tramo (exclusive).
 */
static void GetChunkRange(int32 ChunkIndex, int32 NumChunks, int32 PopulationNum,
    int32& OutBegin, int32& OutEnd)
{
    OutBegin = static_cast<int32>((int64)ChunkIndex * PopulationNum / NumChunks);
    OutEnd = static_cast<int32>((int64)(ChunkIndex + 1) * PopulationNum / NumChunks);
}

void UEcosystemSubsystem::SimulateTick(float DtYears)
{
    // El tick entero como un bloque nombrado: sale en `stat EcoSim` y en la timeline de
    // Unreal Insights anidado dentro del frame, así que de un vistazo se ve si es el tick
    // el que se lleva el frame o si es ruido al lado del render.
    SCOPE_CYCLE_COUNTER(STAT_EcoTickTotal);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_SimulateTick);

    const UEcosystemSettings* Settings = UEcosystemSettings::Get();
    const double TickT0 = FPlatformTime::Seconds();

    // Parámetros de CO2 resueltos una sola vez por tick y capturados por valor en el lambda
    // paralelo: dentro del ParallelFor no se tocan ni los ajustes ni las CVars.
    const EcoCarbon::FCO2Params CO2 = GetCO2Params();

    // ================================================================
    // ETAPA PREVIA (serial): estructuras derivadas del snapshot de lectura
    // ================================================================
    // Del hash tiran el espaciado mínimo de la germinación, el conteo de conespecíficos y
    // la selección de víctimas de un claro. La competencia por recursos NO pasa por él: se
    // resuelve a través de los campos compartidos (consumo de agua y nutrientes, y sombra).
    {
        SCOPE_CYCLE_COUNTER(STAT_EcoHash);
        TRACE_CPUPROFILER_EVENT_SCOPE(Eco_BuildHash);
        Hash.Build(Agents_Read.Position, Agents_Read.Num());
    }
    const double AfterHash = FPlatformTime::Seconds();

    // Cadencia de la luz gruesa: cada tick es el comportamiento exacto. Subir
    // LightRebuildEveryNTicks ahorra la pasada serial a cambio de que las copas tarden en
    // proyectar su sombra nueva.
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
    // ETAPA PARALELA: crecimiento, estrés, mortalidad y semillas por chunk
    // ================================================================
    RunGrowthParallel(DtYears, *Settings, CO2, NumChunks);

    const double AfterParallel = FPlatformTime::Seconds();

    // ================================================================
    // ETAPA SERIAL: reducción, regeneración, pulsos de muerte y germinación
    // ================================================================
    // PendingSeeds y PendingDeaths son miembros y ReduceScratchInto los vacía con Reset(),
    // así que conservan la capacidad de ticks anteriores: una oleada de germinación no
    // vuelve a pedir memoria al heap.
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

    // Este ámbito de medida cubre la última etapa entera: pulsos de muerte, germinación,
    // perturbación, compactación e intercambio de buffers.
    SCOPE_CYCLE_COUNTER(STAT_EcoGermination);
    TRACE_CPUPROFILER_EVENT_SCOPE(Eco_Germination);

    ApplyDeathPulses(DtYears, *Settings);
    RunGermination(DtYears, *Settings, CO2);

    // La perturbación va DESPUÉS de la germinación: un claro abierto en este tick deja su
    // hueco para el siguiente, igual que una muerte cualquiera. Y va aquí y no en la etapa
    // paralela porque elige los centros de claro de forma serial y determinista, con su
    // propio stream de RNG.
    RunDisturbance(DtYears, *Settings);

    // ================================================================
    // CIERRE: compactar muertos e intercambiar buffers (agentes y campos)
    // ================================================================
    Agents_Write.CompactDead();

    Swap(Agents_Read, Agents_Write);
    WaterPool.SwapBuffers();
    NutrientPool.SwapBuffers();

    // Instrumentación (Eco.Profile): media exponencial del coste de cada etapa, sin
    // histórico. @see FEcoTickProfile::Accumulate
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
        // Ambas solo LEEN la población y no consumen RNG, así que no alteran el
        // fingerprint ni la evolución de la corrida.
        RecordDemographySample();
        LogPopulationStats();
    }
}

// ---------------------------------------------------------------------------
//  Etapas del tick (el tick queda como orquestador; el contrato está en el .h)
// ---------------------------------------------------------------------------

/**
 * @see UEcosystemSubsystem::PrepareTickScratch
 * @see @ref bib_goldberg1991
 */
int32 UEcosystemSubsystem::PrepareTickScratch(const UEcosystemSettings& Settings)
{
    // DETERMINISMO: el número de chunks se deriva SOLO de la población y de un grano fijo de
    // los ajustes, nunca del número de hilos de la máquina. Si dependiera de
    // GetNumWorkerThreads(), la partición —y con ella el orden en que ReduceScratchInto suma
    // los deltas de cada celda— cambiaría según la CPU; como la suma en coma flotante no es
    // asociativa, dos máquinas con distinto número de núcleos divergirían celda a celda y,
    // tick a tick, acabarían en bosques distintos pese a compartir semilla. Con un recuento
    // fijo, misma población implica misma partición y misma reducción bit a bit. ParallelFor
    // reparte esas NumChunks tareas entre los hilos disponibles, así que no se pierde
    // paralelismo.
    const int32 PopNum = Agents_Read.Num();
    const int32 GrainSize = FMath::Max(1, Settings.TickChunkGrainSize);
    const int32 NumChunks = FMath::Clamp(FMath::DivideAndRoundUp(PopNum, GrainSize), 1, kMaxTickChunks);

    // El scratch es PERSISTENTE (vive como miembro) y DISPERSO: cada tarea acumula pares
    // (celda, cantidad) en vez de un campo denso del tamaño del mundo, porque el trabajo
    // real es disperso y un campo denso por tarea cuesta memoria y sumas proporcionales al
    // mundo. @see FCellDelta
    //
    // Aquí solo se vacían los buffers —Reset conserva la capacidad— y se reserva de una vez
    // lo que se espera depositar, para que los Add() no realojen. El radio efectivo de un
    // adulto respeta el mínimo en celdas, así que la reserva tiene que contar con él o el
    // primer tick realojaría los deltas.
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
    // Una vez por tick y por ESPECIE, no por árbol: las curvas son idénticas para todos los
    // individuos de una especie, así que construirlas dentro del bucle repetiría el trabajo
    // decenas de miles de veces y obligaría a leer el UObject de especie desde dentro del
    // ParallelFor.
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
    // NutrientPool.Current, LightCoarse) y SOLO escribe en su tramo de Agents_Write y en su
    // propio FTickScratch: ni bloqueos ni atómicas.
    const int32 PopNum = Agents_Read.Num();

    const EParallelForFlags Flags = CVarForceST.GetValueOnGameThread()
        ? EParallelForFlags::ForceSingleThread : EParallelForFlags::None;

    // Radio radicular mínimo de un ADULTO, en cm, resuelto una vez y capturado por valor. El
    // mínimo existe porque el kernel de consumo da peso exactamente cero a los vecinos
    // cuando el radio no supera el tamaño de celda: sin él cada árbol agotaría un pozo
    // privado y la competencia subterránea dejaría de existir como interacción.
    const float MinAdultRootRadiusCm = Settings.MinRootRadiusCells * (float)WaterBase.Field.CellSize;

    // El ámbito de medida envuelve al ParallelFor ENTERO —reparto y espera incluidos—, no a
    // cada tarea: lo que interesa es el tiempo de pared que el game thread pasa bloqueado.
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
                    uint32& RngState = Agents_Write.RngState[i]; // stream propio del árbol

                    // (a) recursos locales
                    const float W = WaterPool.SampleCurrent(P.X, P.Y);
                    const float N = NutrientPool.SampleCurrent(P.X, P.Y);

                    // LA LUZ SE LEE EN EL ÁPICE DE LA COPA, no en el pie del árbol.
                    //
                    // Position.Z es la cota del terreno y no cambia nunca: leyendo ahí, un
                    // dominante de 20 m y la plántula que tiene debajo verían el mismo Q y
                    // la altura entraría en el modelo solo como emisora de sombra, nunca
                    // como receptora de luz. La competencia por luz —asimétrica en un
                    // bosque real: el que llega arriba intercepta y el de abajo paga—
                    // quedaría plana, y con ella desaparecerían la estratificación
                    // vertical, la herencia del hueco y la sucesión. Leyendo en el ápice se
                    // cierra el bucle altura -> luz -> crecimiento -> altura, que es lo que
                    // crea el dosel. La autoexclusión sale gratis: el LAI acumulado POR
                    // ENCIMA del techo de la copa propia no contiene follaje propio.
                    //
                    // Se usa la altura del SNAPSHOT de lectura, la del principio del tick:
                    // el vigor decide cuánto crece el árbol, así que no puede depender de lo
                    // que ha crecido en este mismo tick.
                    const float ReadHeight = Agents_Read.Height[i];
                    const float Q = LightCoarse.SampleLightSmooth(P + FVector(0.f, 0.f, ReadHeight));

                    // (b) vigor. Copia ÚNICA de la fórmula: tick, germinación y heatmap
                    //     evalúan literalmente lo mismo. El factor de CO2 multiplica FUERA
                    //     del mínimo de Liebig, porque no es un recurso consumible sino una
                    //     modulación de eficiencia. Guardar el argmin cuesta una comparación
                    //     que ya se hacía y es lo que permite responder por qué se muere una
                    //     especie y no solo que se muere.
                    //     @see EcoVigor::EvaluateVigor
                    //     @see EcoCarbon::CO2Factor
                    EEcoLimiter LimiterHere = EEcoLimiter::Light;
                    const float VigorValue =
                        EcoVigor::EvaluateVigor(Q, W, N, Resp, Settings.VigorCombineMode, LimiterHere)
                        * EcoCarbon::CO2Factor(Q, ReadHeight, CO2);

                    Agents_Write.Vigor[i] = VigorValue;
                    Agents_Write.Limiter[i] = static_cast<uint8>(LimiterHere);

                    // (c) estrés. Va ANTES de decidir los estados porque la supresión se
                    //     resuelve con el estrés de ESTE tick.
                    const float NewStress = EcologyRules::UpdateStress(Agents_Read.Stress[i], VigorValue,
                        Settings.StressVigorThreshold, Settings.StressAccumulationRate,
                        Settings.StressRecoveryRate, Settings.StressDecayRate, DtYears);
                    Agents_Write.Stress[i] = NewStress;

                    // (d) los DOS declives, que son independientes entre sí.
                    //
                    // Senescencia por EDAD: irreversible. La puerta se hace pegajosa
                    // leyendo el estado del snapshot, inmutable durante todo el tick, con
                    // lo que sigue siendo determinista bajo paralelismo.
                    //
                    // Supresión por ESTRÉS: reversible, con histéresis. Si compartiera
                    // estado con la senescencia heredaría su irreversibilidad y unos pocos
                    // años de mala racha marcarían de por vida a una plántula aunque
                    // después se abriera un claro justo encima; eso haría imposible el
                    // banco de plántulas, que es el mecanismo por el que una especie
                    // tolerante hereda los huecos sin competir por número de semillas.
                    const float NewAge = Agents_Read.Age[i] + DtYears;
                    const ETreeState PrevState = Agents_Read.State[i];

                    const bool bSenescent = (PrevState == ETreeState::Senescent)
                        || EcologyRules::IsSenescentByAge(NewAge, Sp->Longevity, Sp->SenescenceAgeFraction);

                    const bool bSuppressed = !bSenescent && EcologyRules::UpdateSuppression(
                        PrevState == ETreeState::Suppressed, NewStress,
                        Sp->SenescenceStressThreshold, Settings.SuppressionExitStressFraction);

                    // (e) crecimiento logístico, altura alométrica y estado resultante.
                    //     El orden de prioridad del estado es senescente, suprimido, maduro
                    //     y plántula. @see EcologyRules::HeightFromBiomass
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

                    // (f) consumo: va SOLO al scratch de este chunk y en forma DISPERSA, y
                    //     topado contra lo realmente disponible. Sin tope, la parte de la
                    //     demanda que no existe se depositaría como cantidad negativa, se
                    //     difundiría a los vecinos bajándoles recurso de verdad y se
                    //     destruiría al recortar a cero: competencia por interferencia
                    //     regalada y masa no conservada.
                    const float RootRadiusCm = EcologyRules::EffectiveRootRadiusCm(
                        Sp->RootRadius, NewBiomass, Sp->MaxBiomass, MinAdultRootRadiusCm);
                    const int32 CellsInRange = EcologyRules::KernelCellCount(WaterBase.Field, RootRadiusCm);

                    const float WaterDraw = EcologyRules::ClampUptakeToAvailable(
                        NewBiomass * Sp->WaterDemand * DtYears, W, CellsInRange, Settings.MaxResourceUptakeFraction);
                    const float NutrientDraw = EcologyRules::ClampUptakeToAvailable(
                        NewBiomass * Sp->NutrientDemand * DtYears, N, CellsInRange, Settings.MaxResourceUptakeFraction);

                    EcologyRules::DepositKernelSparse(WaterBase.Field, Ctx.WaterDeltas, P, RootRadiusCm, -WaterDraw);
                    EcologyRules::DepositKernelSparse(NutrientBase.Field, Ctx.NutrientDeltas, P, RootRadiusCm, -NutrientDraw);

                    // (g) mortalidad. Los dos canales se calculan por separado para poder
                    //     ATRIBUIR la muerte: si el de edad no aparece nunca, la longevidad
                    //     no compra nada y la estrategia lenta y longeva es inviable haga lo
                    //     que haga el resto del modelo. Después se combinan como riesgos
                    //     independientes. @see EcologyRules::CombineIndependentRisks
                    const float pAge = EcologyRules::AgeMortalityProbability(NewAge, Sp->Longevity, DtYears);

                    // Un árbol SUPRIMIDO no muere por el canal de estrés general sino por el
                    // riesgo propio de su especie, que puede ser MENOR: es la mitad del
                    // compromiso r/K que permite que un rasgo de especie reduzca el riesgo y
                    // no solo lo amplifique.
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
                        // Datos que la capa de suelo necesita para la caída, el tocón y la
                        // hojarasca.
                        Pulse.SpeciesId = SpeciesId;
                        Pulse.StableId = Agents_Read.StableId[i];
                        Pulse.Biomass = NewBiomass;
                        Pulse.HeightCm = Agents_Write.Height[i];
                        Ctx.DeathPulses.Add(Pulse);
                    }
                    else if (NewAge >= Sp->MaturityAge && IsReproductiveState(Agents_Write.State[i]))
                    {
                        // (h) semillas, solo si sigue vivo y ha alcanzado la madurez. El
                        //     senescente sigue reproduciéndose con menos fuerza, y el
                        //     suprimido también: cortarlos a cero eliminaría la ventana de
                        //     máxima fecundidad y, en el caso del suprimido, la única vía
                        //     por la que una tolerante compensa su lentitud. La
                        //     comprobación explícita de MaturityAge hace falta porque una
                        //     plántula muy estresada también entra en Suppressed.
                        const float SeedScale = (Agents_Write.State[i] == ETreeState::Senescent)
                            ? FMath::Clamp(Sp->SenescentSeedScale, 0.f, 1.f)
                            : 1.f;

                        // La fecundidad SATURA con la biomasa relativa en vez de ser
                        // proporcional a ella. Con proporcionalidad lineal, crecer más
                        // rápido daría a la vez más individuos y más semillas por
                        // individuo: un bucle de realimentación que convierte una ventaja
                        // de crecimiento moderada en dos órdenes de magnitud de diferencia
                        // en la lluvia de semillas, y que de paso esteriliza al árbol
                        // suprimido impidiéndole recuperarse.
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
    } // fin del ámbito de medida del paso paralelo
}

void UEcosystemSubsystem::ApplyDeathPulses(float DtYears, const UEcosystemSettings& Settings)
{
    // Las manchas de descomposición existentes envejecen —decaimiento exponencial— antes de
    // sumar las de este tick.
    if (DecompositionField.IsValid())
    {
        const float Decay = FMath::Exp(-Settings.DecompositionDecayPerYear * DtYears);
        for (float& V : DecompositionField.Data) { V *= Decay; }
    }

    for (const FPendingDeathPulse& Pulse : PendingDeaths)
    {
        EcologyRules::DepositKernel(NutrientBase.Field, NutrientPool.Next.Data, Pulse.Position, Pulse.RadiusCm, Pulse.Amount);
        RecordDeathEvent(Pulse); // alimenta la capa de suelo
        // Mancha de descomposición visible sobre el terreno: solo visualización, no entra
        // en el vigor.
        if (DecompositionField.IsValid())
        {
            EcologyRules::DepositKernel(DecompositionField, DecompositionField.Data,
                Pulse.Position, Pulse.RadiusCm, Pulse.Amount * Settings.DecompositionPulseScale);
        }
    }

}

/**
 * @see UEcosystemSubsystem::RunGermination
 * @see @ref bib_janzenconnell
 */
void UEcosystemSubsystem::RunGermination(float DtYears, const UEcosystemSettings& Settings,
    const EcoCarbon::FCO2Params& CO2)
{
    // Una sola reserva: PendingSeeds.Num() es la cota superior de germinaciones, y con ella
    // los Add() de abajo no realojan los arrays del SoA.
    Agents_Write.Reserve(Agents_Write.Num() + PendingSeeds.Num());

    const FBox2D WorldBounds = HeightField.GetWorldBounds();
    NewbornPositions.Reset();

    /** Distancia XY al cuadrado contra un radio dado; el radio no es el mismo para todos
        los vecinos cuando bSpacingScalesWithSize está activo. */
    auto IsTooClose = [](const FVector& A, const FVector& B, double RadiusCm) -> bool
        {
            const double dx = A.X - B.X;
            const double dy = A.Y - B.Y;
            return dx * dx + dy * dy < RadiusCm * RadiusCm;
        };

    // Radio de exclusión de una PLÁNTULA recién nacida. Con el escalado activo es
    // MinGerminationSpacingCm multiplicado por su fracción de altura adulta, o sea un orden
    // de magnitud menos que el radio que impone un árbol de dosel.
    const float SeedlingHeightRatio = EcologyRules::HeightRatioFromBiomass(
        Settings.GerminationBiomassFraction, 1.f);
    const double NewbornRadiusCm = Settings.MinGerminationSpacingCm *
        (Settings.bSpacingScalesWithSize ? SeedlingHeightRatio : 1.f);

    for (const FPendingSeed& Seed : PendingSeeds)
    {
        const USpeciesData* Sp = ResolveSpecies(Seed.SpeciesId);
        if (!Sp || !SpeciesResponses.IsValidIndex(Seed.SpeciesId)) { continue; }
        FEcoSpeciesFlow& Flow = SpeciesFlow[Seed.SpeciesId];

        // Filtro 1: semilla dispersada fuera del terreno simulado. Se descarta en vez de
        // germinar en el borde, porque SampleHeight y SampleLight recortarían a los límites
        // y apelmazarían plántulas contra el borde del mapa.
        if (Seed.Position.X < WorldBounds.Min.X || Seed.Position.X > WorldBounds.Max.X ||
            Seed.Position.Y < WorldBounds.Min.Y || Seed.Position.Y > WorldBounds.Max.Y)
        {
            ++Flow.RejectedOffMap;
            continue;
        }

        FVector GerminationPos = Seed.Position;
        GerminationPos.Z = HeightField.SampleHeight(GerminationPos.X, GerminationPos.Y);

        // Filtro 2: espaciado mínimo, o sea no germinar pegada a un árbol ya vivo. Es el
        // consumidor principal del spatial hash. El hash indexa Agents_Read, pero el estado
        // se consulta en Agents_Write para no dejar que un árbol muerto en ESTE tick bloquee
        // el hueco que acaba de liberar. El booleano no depende del orden de visita, así que
        // el resultado sigue siendo determinista.
        bool bTooClose = false;
        Hash.ForEachNeighbor(GerminationPos, Settings.MinGerminationSpacingCm,
            [&](int32 NeighborIdx)
            {
                if (bTooClose) { return; }
                if (!IsAliveState(Agents_Write.State[NeighborIdx])) { return; }

                // El radio que impone CADA vecino depende de su tamaño. Con un radio fijo
                // para todos, unos miles de adultos cubren el mapa entero de exclusiones y
                // el sotobosque deja de existir.
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

        // El hash se construyó sobre Agents_Read y NO contiene las plántulas nacidas en este
        // mismo bucle, así que hay que comprobarlas aparte o dos semillas del mismo tick
        // germinan pegadas. El bucle es serial y de orden fijo, de modo que el resultado
        // sigue siendo determinista.
        if (!bTooClose)
        {
            for (const FVector& NP : NewbornPositions)
            {
                // Ambas son plántulas, así que el radio que aplica es el de plántula.
                if (IsTooClose(NP, GerminationPos, NewbornRadiusCm)) { bTooClose = true; break; }
            }
        }
        if (bTooClose) { ++Flow.RejectedSpacing; continue; }

        // Filtro 3: sitio seguro. La semilla lee la luz A RAS DE SUELO, que es la cota que
        // le corresponde, a diferencia del árbol establecido, que la lee en el ápice.
        const float LightHere = LightCoarse.SampleLightSmooth(GerminationPos);
        if (!EcologyRules::IsSafeGerminationSite(LightHere, Sp->MinLightForGermination))
        {
            // El umbral es POR ESPECIE: es lo que hace del sotobosque un territorio donde la
            // pionera no puede germinar por muchas semillas que mande, y donde la tolerante
            // acumula el banco de plántulas que hereda el hueco al morir el dominante. El
            // filtro solo muerde si existe de verdad un gradiente de luz a ras de suelo;
            // Eco.PercentilesCampos dice si lo hay.
            ++Flow.RejectedLight;
            continue;
        }

        const float WHere = WaterPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);
        const float NHere = NutrientPool.Next.SampleBilinear(GerminationPos.X, GerminationPos.Y);

        // La semilla cae al suelo, o sea con altura de copa cero: es donde el término de CO2
        // pesa más, porque bajo un dosel cerrado el aire se mezcla peor.
        EEcoLimiter SeedLimiter = EEcoLimiter::Light;
        const float VigorHere = EcoVigor::EvaluateVigor(LightHere, WHere, NHere,
            SpeciesResponses[Seed.SpeciesId], Settings.VigorCombineMode, SeedLimiter)
            * EcoCarbon::CO2Factor(LightHere, /*CanopyHeightCm*/ 0.f, CO2);

        // Filtro 4, inhibición de Janzen-Connell: cuenta los adultos de LA MISMA especie
        // alrededor. Los enemigos naturales especializados —patógenos de suelo, herbívoros—
        // se acumulan bajo los adultos de su hospedador, así que una plántula rodeada de los
        // suyos arraiga mucho peor. Es un estabilizador: penaliza a quien domina localmente,
        // de modo que la especie rara recluta mejor de lo que le tocaría por número.
        //
        // Solo cuentan los REPRODUCTIVOS: la carga de enemigos la mantiene el hospedador
        // adulto, no una plántula vecina.
        int32 Conspecifics = 0;
        if (Settings.ConspecificHalfCount > 0.f && Settings.ConspecificInhibitionRadiusCm > 0.f)
        {
            const double InhibRadiusSq = FMath::Square((double)Settings.ConspecificInhibitionRadiusCm);
            Hash.ForEachNeighbor(GerminationPos, Settings.ConspecificInhibitionRadiusCm,
                [&](int32 NeighborIdx)
                {
                    if (Agents_Read.SpeciesId[NeighborIdx] != Seed.SpeciesId) { return; }
                    if (!IsReproductiveState(Agents_Write.State[NeighborIdx])) { return; }

                    // La MADRE no cuenta. Cuando el radio de dispersión no supera al de
                    // inhibición, toda semilla cae dentro del círculo de su propia madre, de
                    // modo que hasta el último adulto de una especie al borde de la
                    // extinción pagaría la penalización por verse a sí mismo: el rescate de
                    // la especie rara quedaría topado justo donde el mecanismo tiene que ser
                    // más fuerte.
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
        // La otra mitad del compromiso r/K: la semilla grande sale poco —SeedRateScale
        // bajo— pero arraiga mejor —GerminationRateScale alto—.
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

/**
 * @note Los árboles derribados por un claro depositan sus nutrientes y emiten su evento de
 *       muerte aquí mismo, sin pasar por PendingDeaths: no manchan el campo de
 *       descomposición ni entran en los contadores por especie del tick.
 * @see UEcosystemSubsystem::RunDisturbance
 * @see @ref bib_dinamicadeclaros
 * @see @ref bib_leypotenciaclaros
 */
void UEcosystemSubsystem::RunDisturbance(float DtYears, const UEcosystemSettings& Settings)
{
    if (Settings.DisturbanceRatePerYear <= 0.f || !HeightField.IsValid()) { return; }

    const FBox2D B = HeightField.GetWorldBounds();
    const double MapAreaM2 = ((B.Max.X - B.Min.X) * (B.Max.Y - B.Min.Y)) / 10000.0; // cm2 -> m2
    if (MapAreaM2 <= 0.0) { return; }

    // El área a perturbar este tick se convierte en un NÚMERO de claros dividiendo por el
    // área media de la distribución. Para una ley potencia de exponente @f$a@f$ truncada a
    // [min, max] esa media se integra en forma cerrada:
    // @f[ E[A] = \frac{1-a}{2-a}\,\frac{max^{2-a} - min^{2-a}}{max^{1-a} - min^{1-a}} @f]
    // El caso @f$a = 2@f$ degenera en un logaritmo y se resuelve aparte en vez de dividir
    // por cero.
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

    // Stream propio: activar la perturbación NO desplaza los de colonización, dispersión ni
    // mortalidad, así que una corrida con claros y otra sin ellos parten del mismo bosque y
    // son comparables.
    uint32& Stream = Rng.State[static_cast<int32>(EEcoRngStream::Disturbance)];
    const int32 NumGaps = EcoRand::PoissonInt(Stream, Lambda);

    int32 Felled = 0;
    for (int32 g = 0; g < NumGaps; ++g)
    {
        // Área del claro por transformada inversa de la ley potencia truncada.
        const float U = EcoRand::NextUnit(Stream);
        const double A1 = 1.0 - Alpha;
        const double AreaM2 = FMath::Pow(
            FMath::Pow((double)MinA, A1) + U * (FMath::Pow((double)MaxA, A1) - FMath::Pow((double)MinA, A1)),
            1.0 / A1);
        const double RadiusCm = FMath::Sqrt(AreaM2 / PI) * 100.0;

        const double Cx = FMath::Lerp(B.Min.X, B.Max.X, (double)EcoRand::NextUnit(Stream));
        const double Cy = FMath::Lerp(B.Min.Y, B.Max.Y, (double)EcoRand::NextUnit(Stream));
        const FVector Center(Cx, Cy, HeightField.SampleHeight(Cx, Cy));

        // El hash indexa Agents_Read, y los árboles que ya existían al empezar el tick
        // ocupan el MISMO índice en Agents_Write, que es una copia con las plántulas nuevas
        // añadidas al final. Se consulta y se marca sobre Agents_Write, que es el buffer que
        // sobrevive al intercambio. Las plántulas germinadas en este mismo tick no están en
        // el hash y por tanto el claro no las alcanza: todavía no habían salido.
        const double RadiusSq = RadiusCm * RadiusCm;
        Hash.ForEachNeighbor(Center, (float)RadiusCm, [&](int32 Idx)
            {
                if (!Agents_Write.State.IsValidIndex(Idx)) { return; }
                if (!IsAliveState(Agents_Write.State[Idx])) { return; }

                const FVector& P = Agents_Read.Position[Idx];
                const double dx = P.X - Center.X, dy = P.Y - Center.Y;
                if (dx * dx + dy * dy > RadiusSq) { return; }

                // Una mortalidad menor que 1 deja árboles residuales en pie, que es lo que
                // hace un temporal real y lo que da al claro su estructura irregular.
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

/**
 * @see UEcosystemSubsystem::RebuildCoarseLight
 * @see @ref bib_monsisaeki1953
 */
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

        // La copa ocupa solo la parte ALTA del árbol (CanopyDepthFraction), no su altura
        // entera: repartir el área foliar por todo el fuste diluiría la sombra justo donde
        // tiene que ser densa y dejaría el sotobosque a plena luz. Cada copa deposita área
        // foliar en su propio volumen y lo que oscurece el suelo es la extinción acumulada.
        const float H = Agents_Read.Height[i];
        const FVector Apex = Agents_Read.Position[i] + FVector(0.f, 0.f, H);
        LightCoarse.DepositCanopyLeafArea(Apex, H * S->CanopyRadiusFraction,
            H * S->CanopyDepthFraction, S->CanopyLeafAreaIndex);
    }

    // Convierte la densidad depositada en LAI acumulado por encima de cada vóxel. Sin esta
    // pasada los Sample* devolverían la densidad local en vez de la atenuación, o sea el
    // perfil invertido.
    LightCoarse.AccumulateExtinction();
}

const USpeciesData* UEcosystemSubsystem::ResolveSpecies(uint16 SpeciesId) const
{
    return ResolvedSpecies.IsValidIndex(SpeciesId) ? ResolvedSpecies[SpeciesId] : nullptr;
}

// ---------------------------------------------------------------------------
//  Población
// ---------------------------------------------------------------------------
/** @see UEcosystemSubsystem::RandomPointOnTerrain */
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
        // Toda la siembra consume el stream de colonización: coloca el bosque inicial sin
        // desplazar los de mortalidad, dispersión ni morfología.
        const FVector Site = RandomPointOnTerrain(EEcoRngStream::Colonization);

        const int32 SpeciesIdx = Rng.RangeI(EEcoRngStream::Colonization, 0, ResolvedSpecies.Num() - 1);
        const USpeciesData* Sp = ResolvedSpecies[SpeciesIdx];
        if (!Sp) { continue; }

        const uint32 AgentSeed = Rng.U32(EEcoRngStream::Colonization);

        // EDAD ESCALONADA, no toda la cohorte a cero. Con Age = 0 para todos, la población
        // fundadora es una única cohorte que envejece y muere en bloque: alrededor de la
        // mediana del canal de edad (@f$1{,}282\,L^{0,8}@f$ años) se abriría un claro
        // simultáneo en todo el mapa y el bosque se reiniciaría solo.
        //
        // El tope de 0,35 reparte a los fundadores por la mitad joven de la curva: hay
        // adultos desde el primer momento y ninguno arranca senescente, porque el sorteo es
        // semiabierto y no llega a alcanzar el 0,35 de longevidad en que entra la
        // senescencia por defecto (USpeciesData::SenescenceAgeFraction).
        const float InitialAge = Rng.RangeF(EEcoRngStream::Colonization, 0.f, 0.35f * Sp->Longevity);

        // La biomasa acompaña a la edad. Un fundador viejo con biomasa de plántula sería un
        // árbol anciano del tamaño de un arbusto durante décadas y además sombrearía como
        // tal, porque la rejilla de luz lee Height y Height sale de Biomass.
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
//  Hero trees
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

    // La rejilla de luz gruesa es el contexto de sombra de vecinos con el que el algoritmo
    // de colonización del espacio decide hacia dónde crecen las ramas.
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

/**
 * @see UEcosystemSubsystem::LogDemographics
 * @see @ref bib_weibull1951
 */
void UEcosystemSubsystem::LogDemographics() const
{
    const int32 NumSpecies = ResolvedSpecies.Num();
    if (NumSpecies == 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] Demografia: no hay especies configuradas."));
        return;
    }

    /** Acumuladores por especie de una sola pasada sobre la población. */
    struct FSpeciesDemo
    {
        int32 Saplings = 0, Mature = 0, Suppressed = 0, Senescent = 0;
        int32 Grown = 0;      ///< Con al menos el 70% de MaxBiomass: los que llenan el bosque
        double AgeSum = 0.0;  ///< Suma de edades, en años
        float  AgeMax = 0.f;  ///< Edad del individuo más viejo, en años

        // El reparto por estado dice cómo está el bosque; el vigor medio y el limitante,
        // por qué está así.
        double VigorSum = 0.0;
        double StressSum = 0.0;
        int32  Lim[3] = { 0, 0, 0 }; ///< Recuento por limitante: luz, agua y nutrientes

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

        // Una especie extinta sale igualmente en el informe: que haya cruzado el cero, y en
        // qué tick, es la señal más importante del experimento, y saltársela la haría
        // desaparecer del log sin dejar rastro.
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

        // DOS medianas, y el diagnóstico es la comparación entre ellas.
        //
        //   - NOMINAL: la edad a la que el canal de EDAD se lleva a media cohorte. Con
        //     riesgo @f$h(t) = (t/L)^4@f$ el riesgo acumulado es @f$H(t) = t^5/(5L^4)@f$, e
        //     imponer @f$H = \ln 2@f$ da @f$t = (5\ln 2)^{1/5} L^{0,8} = 1{,}282\,L^{0,8}@f$.
        //   - REALIZADA: @f$\ln 2 / h@f$, con todos los canales de mortalidad actuando.
        //
        // El riesgo sale de los FLUJOS del tick, no de la edad media. Derivarlo de la edad
        // media supone una población en estado estacionario: una especie que recluta mucho
        // tiene la pirámide llena de plántulas y su edad media se hunde por el
        // reclutamiento, no por la mortalidad, con lo que la cifra sale hasta dos veces más
        // pesimista. El cociente muertes/vivos mide el riesgo directamente.
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

        // Un mismo limitante al 95-100% en todas las especies significa que solo hay un eje
        // de competencia y que el modelo no puede repartir nicho, hagan lo que hagan el
        // resto de parámetros.
        UE_LOG(LogEco, Log,
            TEXT("  %-22s vigor medio %.3f | estres medio %.3f | limita: luz %4.1f%% agua %4.1f%% nutrientes %4.1f%%"),
            TEXT(""),
            static_cast<float>(D.VigorSum) * InvLive,
            static_cast<float>(D.StressSum) * InvLive,
            100.f * D.Lim[0] * InvLive, 100.f * D.Lim[1] * InvLive, 100.f * D.Lim[2] * InvLive);

        // El EMBUDO DE RECLUTAMIENTO del último tick. Sin este desglose los cuatro filtros
        // son una caja negra y no se puede distinguir «no produce semillas» de «las produce
        // y las rechaza el espaciado» de «germinan y se mueren».
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
                // El aviso solo tiene sentido si la población ha tenido tiempo de envejecer:
                // en un bosque joven es normal que nadie muera de viejo.
                (Deaths > 20 && F.DeathsByAge * 5 < Deaths && D.AgeMax > 0.5f * MedianNominal)
                ? TEXT("<- el canal de edad casi no participa: la longevidad no compra nada")
                : TEXT(""));
        }
    }
}

// ---------------------------------------------------------------------------
//  Percentiles de los campos (Eco.PercentilesCampos)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::LogFieldPercentiles() const
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S) { return; }

    /** Percentiles de un campo, en absoluto y en fracción de su máximo de salida. Copia y
        ordenación completas: es un comando manual, no hace falta nada más fino. */
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

            // La ANCHURA SUGERIDA es el HUECO MÍNIMO entre óptimos consecutivos, no la
            // semidistancia intercuartílica. La media (p75-p25)/2 solo vale si el campo es
            // simétrico, y el TWI del agua no lo es ni de lejos: con esa fórmula el par
            // p25-p50 queda separado menos de una anchura, las dos especies más secas
            // responden casi igual en todas las celdas y el eje deja de repartir territorio.
            // Con el hueco mínimo las campanas quedan separadas al menos una anchura también
            // en un campo sesgado.
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

    // Percentiles del POOL, que es lo que los árboles leen de verdad. El campo base es el
    // potencial del terreno, congelado; el pool se agota con el consumo y solo se recarga
    // poco a poco hacia el base, así que colocar los óptimos sobre los percentiles del base
    // los deja sistemáticamente por encima del recurso disponible.
    LogOne(TEXT("AGUA pool (lo que leen los arboles)"), WaterPool.Current.Data, S->WaterOutputMax);
    LogOne(TEXT("NUTRIENTES pool (lo que leen los arboles)"), NutrientPool.Current.Data, S->NutrientOutputMax);

    // La LUZ A RAS DE SUELO dice si existe sotobosque, y es lo que hay que mirar para
    // colocar los MinLightForGermination de cada especie: un umbral por debajo del p25 da a
    // la tolerante esa fracción del mapa en exclusiva, porque la pionera no puede germinar
    // ahí, y ése es el mecanismo por el que hereda los huecos sin competir por número de
    // semillas.
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
//  Instrumentación de diagnóstico
// ---------------------------------------------------------------------------
//
// Los comandos de esta sección responden con números a «qué le pasa a esta especie». Sin
// ellos la calibración va a ciegas: los assets de especie son binarios y sus valores no
// aparecen en ningún log, y el embudo de reclutamiento es una caja negra. Ninguno modifica
// el estado de la simulación ni consume los streams de RNG del bosque, así que llamarlos no
// altera el fingerprint.

void UEcosystemSubsystem::LogSpeciesDump() const
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (!S || ResolvedSpecies.Num() == 0)
    {
        UE_LOG(LogEco, Warning, TEXT("[Especies] No hay especies configuradas."));
        return;
    }

    // Luz de referencia para leer la diferenciación en sombra, que es donde la tolerancia
    // tiene que decidir algo. Es un valor de lectura del informe: no entra en la simulación.
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

        // Tiempo hasta el 70% de MaxBiomass frente a la esperanza de vida: el criterio duro
        // de si una especie puede COMPLETAR su ciclo. Sale de la solución cerrada de la
        // logística desde la biomasa de plántula con vigor 1, o sea una cota optimista.
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

        // Anchuras ABSOLUTAS: es lo que se compara con el rango real del campo que imprime
        // Eco.PercentilesCampos, no la fracción que guarda el asset.
        UE_LOG(LogEco, Log,
            TEXT("      agua:  optimo %.2f (=%.2f abs) | anchura %.3f (=%.2f abs) | exceso %.2f abs | encharca %s"),
            Sp->WaterOptimum, R.Water.OptimumAbs, Sp->WaterTolerance, R.Water.WidthAbs,
            R.Water.ExcessWidthAbs, Sp->bWaterloggingPenalty ? TEXT("si") : TEXT("no"));
        UE_LOG(LogEco, Log,
            TEXT("      nutr:  optimo %.2f (=%.2f abs) | anchura %.3f (=%.2f abs) | exceso %.2f abs | penaliza %s"),
            Sp->NutrientOptimum, R.Nutrient.OptimumAbs, Sp->NutrientTolerance, R.Nutrient.WidthAbs,
            R.Nutrient.ExcessWidthAbs, Sp->bNutrientExcessPenalty ? TEXT("si") : TEXT("no"));

        // Lluvia de semillas de un adulto ya crecido, con la saturación aplicada: es el
        // número que de verdad compara la fecundidad entre especies.
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

        // Radio radicular efectivo: dice si el kernel de consumo llega o no a los vecinos,
        // que es lo que decide si existe competencia subterránea.
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

    // La prueba es DIRECTA y no simula un solo tick: si una especie es la mejor en
    // prácticamente todo el mapa, la exclusión competitiva ya está escrita en la forma de
    // las curvas y no hace falta gastar mil ticks para verla.
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

    // Se pinta además como heatmap, para ver dónde cae la frontera entre nichos.
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

    // Se recorre de arriba abajo: bajo un dosel real Q tiene que DECRECER hacia el suelo. Un
    // perfil invertido —claro abajo y oscuro arriba— significa que el depósito de copa está
    // invirtiendo la sombra.
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

    // El .ini se lee como TEXTO en vez de consultar FConfigCacheIni: aquí interesa qué
    // claves hay escritas en el fichero del proyecto, no el valor efectivo tras fusionar la
    // cadena de .ini del motor, que es justo lo que enmascara una propiedad sin fijar.
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
            // '+Clave' y '-Clave' son la sintaxis de array de Unreal.
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

    // Una propiedad sin clave no es un detalle de higiene: significa que el .ini ha dejado
    // de ser el registro reproducible de la corrida, porque un cambio del valor por defecto
    // en C++ altera la ecología sin que nada visible cambie, y así se acaba con una
    // calibración híbrida entre dos versiones del modelo.
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
//  Histórico demográfico (Eco.Demografia.CSV)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::RecordDemographySample()
{
    const int32 NumSpecies = ResolvedSpecies.Num();
    if (NumSpecies == 0) { return; }

    TArray<FEcoDemoSample> Row;
    Row.SetNum(NumSpecies);

    // Las sumas van en double: una corrida larga acumula cientos de miles de términos y en
    // float el redondeo se come justo las medias pequeñas, que son las que interesan, porque
    // una especie en declive tiene el vigor bajo.
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

        // Biomasa RELATIVA a la máxima de su especie: en valor absoluto no se pueden
        // comparar un árbol de dosel y un arbusto de sotobosque.
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

        // Los flujos del último tick son lo que convierte una serie de recuentos en una
        // tabla de vida: con nacimientos y muertes por especie, R0 sale de una división. Sin
        // ellos dn/dt es observable pero no se puede descomponer en reclutamiento menos
        // mortalidad, que es justo lo que dice dónde hay que tocar.
        if (SpeciesFlow.IsValidIndex(s)) { Row[s].Flow = SpeciesFlow[s]; }

        // Primer tick en que se observa a cero: sin este registro una extinción solo se
        // detecta por ausencia y su momento, el dato más informativo del experimento, no
        // queda anotado en ninguna parte.
        if (SpeciesExtinctionTick.IsValidIndex(s) && Row[s].Count == 0 && SpeciesExtinctionTick[s] < 0)
        {
            SpeciesExtinctionTick[s] = TickCount;
            UE_LOG(LogEco, Warning, TEXT("[Eco] *** La especie '%s' se ha EXTINGUIDO (tick %lld)."),
                ResolvedSpecies[s] ? *ResolvedSpecies[s]->SpeciesName.ToString() : TEXT("?"), TickCount);
        }

        // La fila se guarda AUNQUE Count sea 0: una especie extinguida tiene que aparecer
        // como una línea que baja a cero en la gráfica y no desaparecer del CSV, que se
        // leería como ausencia de datos.
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

    // Separador ';' y punto decimal: es lo que abre directamente una hoja de cálculo en
    // español sin pasar por el asistente de importación. Las columnas de flujo —semillas,
    // rechazos del embudo, nacimientos y muertes por canal— son lo que convierte la serie de
    // recuentos en una tabla de vida.
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
//  Eventos de muerte: anillo circular que consume la capa de suelo
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
    const int32 Cap = RecentDeaths.Num();   // el mismo módulo que usa RecordDeathEvent
    if (Cap == 0) { InOutCursor = DeathEventCounter; return; }

    // Solo están disponibles las últimas Cap muertes, porque el anillo pisa las viejas. From
    // nunca baja de DeathEventCounter - Cap, así que jamás se lee una ranura todavía sin
    // escribir: el array está predimensionado con eventos vacíos.
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
    // El anillo está PREDIMENSIONADO: Num() es la capacidad, no cuántas muertes hay.
    const int64 Available = FMath::Min<int64>(DeathEventCounter, Cap);
    UE_LOG(LogEco, Log, TEXT("[Eco/Muertes] Muertes totales: %lld | disponibles en el anillo: %lld/%d"),
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
//  Bake a un año objetivo: guardar y cargar el estado completo
// ---------------------------------------------------------------------------
//
// Se serializan la única fuente de verdad —la población—, el estado runtime de los campos
// —pools de agua y nutrientes y campo de descomposición—, los streams de RNG y el contador
// de ticks. Los campos BASE (relieve, potenciales de agua y nutrientes, luz) no se guardan:
// son deterministas a partir de la semilla maestra y se regeneran idénticos en
// OnWorldBeginPlay. Por eso un bake solo cuadra con la misma semilla y los mismos ajustes de
// relieve, y por eso hay que comprobarlo al cargar.
//
// El anillo de muertes (RecentDeaths y DeathEventCounter) no se serializa a propósito: es un
// buffer de eventos para la capa de vista, no estado del bosque. Tras cargar, la capa de
// suelo se vacía al recibir OnStateLoaded y recoloca su cursor.

/** Firma de fichero de un `.ecobake`. */
static constexpr uint32 kEcoBakeMagic = 0x4F434501u;

/**
 * Versión del formato de bake. Un fichero de otra versión se rechaza al cargar.
 *
 * @li v2: el SoA incorpora los arrays de instrumentación Vigor y Limiter. Como el bake
 *         enumera los campos con FTreePopulation::ForEachArray, el formato cambia solo.
 * @li v3: dos cambios incompatibles a la vez. ETreeState gana el estado Suppressed antes de
 *         Dead, con lo que los valores serializados en v2 significan otra cosa, y
 *         FEcosystemRng gana el stream de perturbación y se serializa como bloque plano, con
 *         lo que su tamaño cambia. Leer un v2 como v3 daría árboles muertos resucitados y
 *         streams desplazados.
 */
static constexpr int32  kEcoBakeVersion = 3;

/** Contenido completo de un bake. @see UEcosystemSubsystem::SerializeState */
struct FEcoBakePayload
{
    int64           TickCount = 0;                  ///< Ticks simulados hasta el instante horneado
    FEcosystemRng   Rng;                            ///< Estado de todos los streams de RNG
    FTreePopulation Population;                     ///< Población de árboles en SoA
    FField2D        Water, Nutrient, Decomposition; ///< Estado runtime de los tres campos
};

/**
 * Comprueba que dos rejillas describen el mismo trozo de mundo con la misma resolución.
 *
 * @return true si coinciden dimensiones, tamaño de celda y origen —con 1 cm de tolerancia—
 *         y el número de celdas de A es coherente con sus dimensiones.
 */
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

    /** Serializa un array de datos planos, validando su tamaño al cargar. */
    auto PODArray = [&Ar](auto& Arr)
        {
            int32 N = Arr.Num();
            Ar << N;
            if (Ar.IsLoading())
            {
                // Un N corrupto, o simplemente un fichero truncado, provocaría un
                // SetNumUninitialized gigantesco y con él un fallo de memoria. Se valida
                // contra lo que de verdad queda por leer en el archivo.
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
    /** Serializa una rejilla 2D: geometría primero y datos después. */
    auto FieldSer = [&Ar, &PODArray](FField2D& F)
        {
            Ar << F.Width; Ar << F.Height; Ar << F.CellSize; Ar << F.Origin;
            PODArray(F.Data);
        };

    // Población en SoA: posición, especie, edad, tamaño y estado. Los arrays no se enumeran
    // aquí sino con el visitor de la propia FTreePopulation, que es el único sitio donde
    // vive la lista: así un campo nuevo entra en el bake solo y no se puede escribir un
    // fichero al que le falte un array, que cargaría sin error y descuadraría después.
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
        UE_LOG(LogEco, Log, TEXT("[Eco/Bake] Bake guardado: %s (tick %lld, %d arboles, %d KB)."),
            *FilePath, TickCount, Agents_Read.Num(), Bytes.Num() / 1024);
    }
    else
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/Bake] No se pudo escribir el bake: %s"), *FilePath);
    }
}

bool UEcosystemSubsystem::LoadState(const FString& FilePath)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco/Bake] No existe el bake: %s"), *FilePath);
        return false;
    }

    FMemoryReader Ar(Bytes, /*bIsPersistent*/ true);
    uint32 Magic = 0; int32 Version = 0; uint32 Seed = 0;
    Ar << Magic << Version << Seed;
    if (Magic != kEcoBakeMagic)
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/Bake] '%s' no es un bake valido."), *FilePath);
        return false;
    }
    if (Version != kEcoBakeVersion)
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/Bake] '%s' es version %d y esta build lee la %d: no se carga."),
            *FilePath, Version, kEcoBakeVersion);
        return false;
    }
    if (Seed != Rng.MasterSeed)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco/Bake] El bake se hizo con semilla %u pero la actual es %u: "
            "los campos base pueden no cuadrar (se carga de todas formas)."), Seed, Rng.MasterSeed);
    }

    // --- Deserializar APARTE y validar: el estado vivo no se toca todavía ---
    FEcoBakePayload P;
    SerializeState(Ar, P);
    if (Ar.IsError())
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/Bake] '%s' esta corrupto o truncado: no se carga."), *FilePath);
        return false;
    }

    // Geometría de los campos: si no cuadra con la de los campos BASE actuales, el tick
    // indexaría Base.Data[] fuera de rango en RegenerateTowardBase y rompería el check()
    // de ReduceScratchInto. Es un fallo duro, no un artefacto visual.
    if (!EcoFieldGeometryMatches(P.Water, WaterBase.Field) ||
        !EcoFieldGeometryMatches(P.Nutrient, NutrientBase.Field) ||
        !EcoFieldGeometryMatches(P.Decomposition, NutrientBase.Field))
    {
        UE_LOG(LogEco, Error, TEXT("[Eco/Bake] El bake usa una geometria de relieve distinta "
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
        UE_LOG(LogEco, Error, TEXT("[Eco/Bake] El bake tiene los arrays SoA descuadrados: no se carga."));
        return false;
    }

    // Especies: un bake horneado con más especies de las configuradas ahora dejaría árboles
    // cuyo ResolveSpecies devuelve null, y ésos ni crecen, ni mueren, ni se dibujan.
    for (int32 i = 0; i < N; ++i)
    {
        if (!ResolvedSpecies.IsValidIndex(NewPop.SpeciesId[i]))
        {
            UE_LOG(LogEco, Error, TEXT("[Eco/Bake] El bake referencia la especie %d y solo hay %d "
                "configuradas en Project Settings: no se carga."),
                (int32)NewPop.SpeciesId[i], ResolvedSpecies.Num());
            return false;
        }
    }

    // --- Commit: a partir de aquí ya no puede fallar ---
    TickCount = P.TickCount;
    Rng = P.Rng;
    Agents_Read.CopyFrom(NewPop);
    WaterPool.Current = P.Water;
    NutrientPool.Current = P.Nutrient;
    DecompositionField = P.Decomposition;

    // Los buffers Next parten del Current recién cargado, que es la invariante con la que
    // BeginTick espera encontrarse.
    WaterPool.Next = WaterPool.Current;
    NutrientPool.Next = NutrientPool.Current;

    // La luz gruesa es estado DERIVADO de la población y solo se refresca al principio de
    // SimulateTick. Como la carga deja la simulación en pausa, hay que rehacerla aquí o el
    // bosque recién cargado conservaría la sombra del anterior.
    RebuildCoarseLight();
    ClearHeroTrees();

    bPaused = true; // un bake es un instante objetivo: se muestra congelado
    OnStateLoaded.Broadcast();

    UE_LOG(LogEco, Log, TEXT("[Eco/Bake] Bake cargado: %s (tick %lld, %d arboles). "
        "Simulacion en pausa; Eco.TogglePause para continuar."), *FilePath, TickCount, Agents_Read.Num());
    return true;
}

// ---------------------------------------------------------------------------
//  Agentes de depuración: sondas manuales, ajenas a la simulación
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

    // Todo lo aleatorio de esta función sale del stream de depuración, nunca de los de la
    // simulación: colocar sondas no puede desplazar la secuencia del bosque ni cambiar su
    // fingerprint. @see EEcoRngStream
    const FVector Site = RandomPointOnTerrain(EEcoRngStream::Debug);

    // El color se toma de la caché de especies resuelta en OnWorldBeginPlay: aquí no se
    // resuelve ningún soft pointer.
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
//  Dibujo de depuración (cada frame, gobernado por las CVars Eco.Debug.*)
// ---------------------------------------------------------------------------
/**
 * Dibujo de diagnóstico del frame: sondas, población, normales del relieve y visibilidad
 * del heatmap, cada bloque tras su CVar.
 *
 * Solo lee el estado ya calculado y no consume RNG, así que activar cualquiera de las
 * vistas no altera la corrida.
 */
void UEcosystemSubsystem::DrawDebug()
{
    UWorld* World = GetWorld();
    if (!World) return;

    if (HeatmapDecal)
    {
        HeatmapDecal->SetActorHiddenInGame(CVarDebugHeatmap.GetValueOnGameThread() == 0);
    }

    // Modo continuo: repinta la descomposición una vez por tick —no una vez por frame—, con
    // lo que las manchas de muerte se ven aparecer y desvanecerse sobre el terreno.
    if (CVarDecompLive.GetValueOnGameThread() != 0 && LastDecompPaintTick != TickCount)
    {
        PaintDecompositionField(/*bLogResult*/ false); // sin una línea de log por tick
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
            // El senescente se dibuja apagado: distingue el declive del árbol sano sin
            // tener que consultar el estado en un log.
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
//  Heatmaps: campos 2D proyectados sobre el terreno con un decal
// ---------------------------------------------------------------------------
//
// Los comandos Eco.Paint* se diferencian solo en el buffer que suben y en la escala; todos
// desembocan en PaintField, de modo que el camino de pintado —textura, decal y log— existe
// una sola vez. @see UFieldVisualizer

/** @see UEcosystemSubsystem::PaintField */
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

/** @see UEcosystemSubsystem::SuitabilityWaterField */
const FField2D& UEcosystemSubsystem::SuitabilityWaterField() const
{
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
    // Las MISMAS respuestas, el MISMO modo de combinación y los MISMOS parámetros de CO2
    // que usa el tick: si el mapa de idoneidad y el crecimiento real evaluaran modelos
    // distintos, el heatmap dejaría de explicar por qué el bosque crece donde crece.
    // @see EcoVigor::BakeSuitabilityField
    const EcoCarbon::FCO2Params CO2 = GetCO2Params();
    EcoVigor::BakeSuitabilityField(HeightField, SuitabilityWaterField(), SuitabilityNutrientField(),
        LightCoarse, EcoVigor::MakeSpeciesResponses(*Sp, *S), S->VigorCombineMode, Suit, nullptr, &CO2);

    PaintField(Suit.Data, *FString::Printf(TEXT("Vigor (%s)"), *Sp->SpeciesName.ToString()),
        /*bAutoRange*/ false, 0.f, 1.f);
}

void UEcosystemSubsystem::PaintLightField()
{
    if (!HeightField.IsValid()) { return; }

    // Luz a ras de suelo en cada NODO del relieve: comparte geometría con el resto de
    // campos, así que el buffer encaja tal cual en el visualizador. Es el heatmap que dice
    // si existe sotobosque y dónde puede germinar una especie tolerante.
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

    // Rango FIJO [0, max]: con auto-rango la intensidad de una mancha dependería del máximo
    // del campo en ese tick, y el heatmap latiría a cada muerte en vez de mostrar cuánta
    // materia hay en descomposición.
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    PaintField(DecompositionField.Data, TEXT("descomposicion (puntos de muerte)"),
        /*bAutoRange*/ false, 0.f, FMath::Max(0.001f, S->DecompositionPaintMax), bLogResult);
}

/**
 * Garantiza el decal sobre el que se proyecta el heatmap: lo crea la primera vez —material,
 * instancia dinámica y tamaño derivado de los límites del relieve— y le reasigna la textura
 * del visualizador en cada repintado.
 */
void UEcosystemSubsystem::EnsureHeatmapDecal()
{
    UWorld* World = GetWorld();
    if (!World || !FieldViz || !FieldViz->GetTexture()) return;

    // Camino rápido con el decal y su material ya montados: basta con reasignar la textura,
    // porque el decal cubre el mapa entero y no se mueve. Evita repetir el LoadSynchronous
    // del material y el SpawnActor en cada repintado, que con Eco.Decomp.Live activo se
    // producen una vez por tick.
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