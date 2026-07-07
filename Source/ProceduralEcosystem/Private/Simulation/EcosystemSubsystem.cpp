#include "Simulation/EcosystemSubsystem.h"
#include "Config/EcosystemSettings.h"
#include "Debug/FieldVisualizer.h"
#include "Species/SpeciesData.h"
#include "Ecology/Vigor.h"

#include "Engine/World.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DrawDebugHelpers.h"

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
    TEXT("Borra todos los agentes de debug (no toca los arboles de prueba)."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->ClearDebugAgents(); }));

static FAutoConsoleCommandWithWorld GEcoPaint(
    TEXT("Eco.PaintTestField"),
    TEXT("Genera y pinta un campo de prueba (la altura) como heatmap sobre el terreno."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->PaintTestField(); }));

static FAutoConsoleCommandWithWorldAndArgs GEcoPaintResource(
    TEXT("Eco.PaintResource"),
    TEXT("Pinta un campo de recurso: 0=agua, 1=nutrientes, 2=luz. Uso: Eco.PaintResource [0-2]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                const int32 Which = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 0;
                S->PaintResourceField(Which);
            }
        }));

static FAutoConsoleCommandWithWorldAndArgs GEcoPaintVigor(
    TEXT("Eco.PaintVigor"),
    TEXT("Pinta la idoneidad (vigor Liebig) de una especie. Uso: Eco.PaintVigor [indice]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* World)
        {
            if (UEcosystemSubsystem* S = GetEco(World))
            {
                const int32 Idx = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : -1;
                S->PaintVigor(Idx);
            }
        }));

static FAutoConsoleCommandWithWorld GEcoAddTree(
    TEXT("Eco.AddTree"),
    TEXT("Anade un arbol de prueba que proyecta sombra; el heatmap de vigor se actualiza."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->AddRandomTestTree(); }));

static FAutoConsoleCommandWithWorld GEcoClearTrees(
    TEXT("Eco.ClearTrees"),
    TEXT("Quita todos los arboles de prueba y su sombra; el heatmap de vigor se actualiza."),
    FConsoleCommandWithWorldDelegate::CreateStatic(
        [](UWorld* World) { if (UEcosystemSubsystem* S = GetEco(World)) S->ClearTestTrees(); }));

// ---------------------------------------------------------------------------
//  CVars (toggles de debug: se activan/desactivan en vivo desde la consola)
// ---------------------------------------------------------------------------
static TAutoConsoleVariable<int32> CVarDebugAgents(
    TEXT("Eco.Debug.Agents"), 1, TEXT("Dibuja los agentes de debug como esferas."));
static TAutoConsoleVariable<int32> CVarDebugTerrain(
    TEXT("Eco.Debug.Terrain"), 0, TEXT("Dibuja las normales del terreno en una rejilla de sondas."));
static TAutoConsoleVariable<int32> CVarDebugHeatmap(
    TEXT("Eco.Debug.Heatmap"), 1, TEXT("Muestra (1) u oculta (0) el decal de heatmap."));
static TAutoConsoleVariable<int32> CVarDebugTrees(
    TEXT("Eco.Debug.Trees"), 1, TEXT("Dibuja los arboles de prueba como esferas de copa."));

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
    ActiveSpeciesIndex = S->HeatmapSpeciesIndex;
}

void UEcosystemSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // 1) Generar el relieve: fuente de verdad de la simulacion.
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

    // 2) Campos de recursos de la Fase 1 (se hornean una vez).
    BakeResourceFields();
    EnsureLightGrid();

    // 3) Visualizador de campos (heatmap).
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
//  Fase 1: generación de los campos de recursos
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::BakeResourceFields()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // Agua: TWI causal derivado del relieve.
    WaterField.BakeFromHeightField(HeightField, S->WaterOutputMax, S->bFillWaterSinks);

    // Nutrientes: Perlin parcheado, con semilla desligada del relieve. Comparte
    // geometria (mismo Width/Height/CellSize/Origin) para que los tres campos se
    // muestreen en las mismas coordenadas de mundo.
    const FField2D& G = HeightField.Field;
    NutrientField.GeneratePatchyBase(
        G.Width, G.Height, G.CellSize, G.Origin,
        static_cast<uint32>(S->MasterSeed),
        S->NutrientOutputMax, S->NutrientPatchFrequency, S->NutrientOctaves);

    UE_LOG(LogEco, Log, TEXT("[Eco] Campos de recursos horneados: agua (TWI) y nutrientes (Perlin)."));
}

void UEcosystemSubsystem::EnsureLightGrid()
{
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const FBox2D B = HeightField.GetWorldBounds();

    const double SpanX = B.Max.X - B.Min.X;
    const double SpanY = B.Max.Y - B.Min.Y;

    const int32 Wv = FMath::Max(1, FMath::CeilToInt(SpanX / S->LightCellSizeXYCm) + 1);
    const int32 Hv = FMath::Max(1, FMath::CeilToInt(SpanY / S->LightCellSizeXYCm) + 1);

    // Z: desde el suelo (BaseZ=0; el relieve esta normalizado a [0, HeightScale])
    // hasta la cima del relieve mas el margen para las copas.
    const double BaseZ = 0.0;
    const double TopZ = S->HeightScaleCm + S->LightCanopyHeadroomCm;
    const int32  Lv = FMath::Max(1, FMath::CeilToInt((TopZ - BaseZ) / S->LightCellSizeZCm) + 1);

    LightField.Init(Wv, Hv, Lv,
        S->LightCellSizeXYCm, S->LightCellSizeZCm,
        B.Min, BaseZ);

    // Sin arboles todavia: cielo despejado (sombra = 0 en todo el volumen).
    LightField.ClearShadow();

    UE_LOG(LogEco, Log, TEXT("[Eco] Rejilla de luz %dx%dx%d voxels (%.0f cm XY, %.0f cm Z)."),
        Wv, Hv, Lv, S->LightCellSizeXYCm, S->LightCellSizeZCm);
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

void UEcosystemSubsystem::SimulateTick(float DtYears)
{
    // ===============================================================
    //  FASE 0/1: intencionadamente vacio.
    //  Aqui entrara el bucle de la Fase 2 (crecimiento, mortalidad,
    //  dispersion...) que CONSUME los campos y la funcion de vigor.
    // ===============================================================
    (void)DtYears;
}

// ---------------------------------------------------------------------------
//  Debug agents
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

    // Stream Debug: las herramientas de depuracion NO perturban la reproducibilidad.
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
//  Árboles de prueba: depositan sombra en el grid de luz -> cambian el vigor.
//  (Es el HITO de la Fase 1: un heatmap de idoneidad que reacciona a añadir/
//   quitar árboles.)
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::AddRandomTestTree()
{
    if (!HeightField.IsValid() || !LightField.IsValid())
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] AddRandomTestTree: relieve o rejilla de luz no listos."));
        return;
    }

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const FBox2D B = HeightField.GetWorldBounds();

    const float u = Rng.Unit(EEcoRngStream::Debug);
    const float v = Rng.Unit(EEcoRngStream::Debug);
    const double x = FMath::Lerp(B.Min.X, B.Max.X, (double)u);
    const double y = FMath::Lerp(B.Min.Y, B.Max.Y, (double)v);
    const double GroundZ = HeightField.SampleHeight(x, y);

    // Ápice de la copa por encima del suelo: colocado a CanopyDepth sobre el
    // terreno para que la piramide de sombra llegue justo hasta el suelo.
    const FVector Apex(x, y, GroundZ + S->TestTreeCanopyDepthCm);
    TestTreeApex.Add(Apex);

    // Marca visual (esfera verde a la altura de la copa).
    AddDebugAgent(Apex, FColor(30, 160, 40, 255), S->TestTreeCanopyRadiusCm * 0.5f);

    RebuildLightShadows();
    RepaintActiveHeatmap();

    UE_LOG(LogEco, Log, TEXT("[Eco] Arbol de prueba #%d en (%.0f, %.0f). Sombra re-depositada."),
        TestTreeApex.Num(), x, y);
}

void UEcosystemSubsystem::ClearTestTrees()
{
    TestTreeApex.Reset();

    // Los árboles de prueba se dibujan como agentes de debug: no distinguimos
    // aquí cuáles eran árboles, así que limpiamos también los agentes para no
    // dejar esferas huérfanas. (En Fase 2 los árboles serán agentes reales.)
    DebugAgents.Reset();

    RebuildLightShadows();
    RepaintActiveHeatmap();

    UE_LOG(LogEco, Log, TEXT("[Eco] Arboles de prueba eliminados; cielo despejado."));
}

void UEcosystemSubsystem::RebuildLightShadows()
{
    if (!LightField.IsValid()) { return; }

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // Borrón y cuenta nueva: limpiar y re-depositar TODAS las copas garantiza
    // que quitar un árbol deshaga su sombra (mismo patrón que usará la Fase 2
    // cada tick al re-depositar la sombra de toda la población).
    LightField.ClearShadow();
    for (const FVector& Apex : TestTreeApex)
    {
        LightField.DepositCanopyShadow(
            Apex, S->TestTreeCanopyRadiusCm, S->TestTreeCanopyDepthCm, S->TestTreeCanopyDensity);
    }
}

// ---------------------------------------------------------------------------
//  Heatmaps
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::PaintTestField()
{
    if (!FieldViz || !HeightField.IsValid()) { return; }

    FieldViz->UpdateFromField(HeightField.Field.Data);
    ActiveHeatmap = EActiveHeatmap::TestField;
    EnsureHeatmapDecal();

    UE_LOG(LogEco, Log, TEXT("[Eco] Campo de prueba (altura) pintado."));
}

void UEcosystemSubsystem::PaintResourceField(int32 Which)
{
    if (!FieldViz || !HeightField.IsValid()) { return; }

    const FField2D& G = HeightField.Field;

    switch (Which)
    {
    case 0: // agua
        if (!WaterField.IsValid()) { return; }
        FieldViz->UpdateFromField(WaterField.Field.Data);
        ActiveHeatmap = EActiveHeatmap::Water;
        UE_LOG(LogEco, Log, TEXT("[Eco] Heatmap: AGUA (TWI)."));
        break;

    case 1: // nutrientes
        if (!NutrientField.IsValid()) { return; }
        FieldViz->UpdateFromField(NutrientField.Field.Data);
        ActiveHeatmap = EActiveHeatmap::Nutrient;
        UE_LOG(LogEco, Log, TEXT("[Eco] Heatmap: NUTRIENTES."));
        break;

    case 2: // luz a ras de suelo (Q en [0,1]); refleja la sombra de los árboles
    {
        TArray<float> Q;
        Q.SetNumUninitialized(G.Width * G.Height);
        for (int32 y = 0; y < G.Height; ++y)
        {
            for (int32 x = 0; x < G.Width; ++x)
            {
                const double Xcm = G.Origin.X + x * G.CellSize;
                const double Ycm = G.Origin.Y + y * G.CellSize;
                const double Zcm = HeightField.SampleHeight(Xcm, Ycm);
                Q[y * G.Width + x] = LightField.IsValid()
                    ? LightField.SampleLightSmooth(FVector(Xcm, Ycm, Zcm))
                    : FLightFieldCoarse::FullSunlight;
            }
        }
        FieldViz->UpdateFromField(Q, 0.f, 1.f); // rango fijo: comparables entre repintados
        ActiveHeatmap = EActiveHeatmap::Light;
        UE_LOG(LogEco, Log, TEXT("[Eco] Heatmap: LUZ a ras de suelo."));
        break;
    }

    default:
        UE_LOG(LogEco, Warning, TEXT("[Eco] PaintResource: indice %d fuera de rango (0=agua,1=nutri,2=luz)."), Which);
        return;
    }

    EnsureHeatmapDecal();
}

void UEcosystemSubsystem::PaintVigor(int32 SpeciesIndex)
{
    if (!FieldViz || !HeightField.IsValid()) { return; }

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    if (S->Species.Num() == 0)
    {
        UE_LOG(LogEco, Warning,
            TEXT("[Eco] PaintVigor: no hay especies en Project Settings; anade al menos una."));
        return;
    }

    const int32 Idx = (SpeciesIndex >= 0) ? SpeciesIndex : ActiveSpeciesIndex;
    const int32 Clamped = FMath::Clamp(Idx, 0, S->Species.Num() - 1);
    if (Clamped != Idx)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] PaintVigor: indice %d fuera de rango; usando %d."), Idx, Clamped);
    }

    const USpeciesData* Sp = S->Species[Clamped].LoadSynchronous();
    if (!Sp)
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] PaintVigor: no se pudo cargar la especie %d."), Clamped);
        return;
    }

    // El corazón de la Fase 1: combinar los tres campos con Liebig, por nodo.
    EcoVigor::BakeSuitabilityField(
        HeightField, WaterField, NutrientField, LightField,
        *Sp, S->KlMax, VigorField);

    if (!VigorField.IsValid())
    {
        UE_LOG(LogEco, Warning, TEXT("[Eco] PaintVigor: el campo de vigor salio invalido."));
        return;
    }

    // Rango FIJO [0,1]: así el color significa lo mismo entre especies y entre
    // repintados (añadir un árbol baja el vigor a la sombra y se ve al instante).
    FieldViz->UpdateFromField(VigorField.Data, 0.f, 1.f);
    ActiveHeatmap = EActiveHeatmap::Vigor;
    ActiveSpeciesIndex = Clamped;
    EnsureHeatmapDecal();

    UE_LOG(LogEco, Log, TEXT("[Eco] Heatmap: VIGOR (idoneidad) de '%s' [especie %d]."),
        *Sp->SpeciesName.ToString(), Clamped);
}

void UEcosystemSubsystem::RepaintActiveHeatmap()
{
    switch (ActiveHeatmap)
    {
    case EActiveHeatmap::TestField: PaintTestField();          break;
    case EActiveHeatmap::Water:     PaintResourceField(0);     break;
    case EActiveHeatmap::Nutrient:  PaintResourceField(1);     break;
    case EActiveHeatmap::Light:     PaintResourceField(2);     break;
    case EActiveHeatmap::Vigor:     PaintVigor(ActiveSpeciesIndex); break;
    default: break; // None: nada que repintar todavía
    }
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

    if (CVarDebugTrees.GetValueOnGameThread() != 0)
    {
        const UEcosystemSettings* S = UEcosystemSettings::Get();
        for (const FVector& Apex : TestTreeApex)
        {
            DrawDebugSphere(World, Apex, S->TestTreeCanopyRadiusCm, 12,
                FColor(20, 120, 30, 255), false, -1.f, 0, 2.f);
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
//  Decal del heatmap
// ---------------------------------------------------------------------------
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