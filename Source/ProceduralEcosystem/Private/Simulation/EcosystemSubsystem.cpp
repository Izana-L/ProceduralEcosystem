#include "Simulation/EcosystemSubsystem.h"
#include "Config/EcosystemSettings.h"
#include "Debug/FieldVisualizer.h"
#include "Species/SpeciesData.h"

#include "Engine/World.h"
#include "Engine/DecalActor.h"
#include "Components/DecalComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DrawDebugHelpers.h"

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

// ---------------------------------------------------------------------------
//  CVars (toggles de debug: se activan/desactivan en vivo desde la consola)
// ---------------------------------------------------------------------------
static TAutoConsoleVariable<int32> CVarDebugAgents(
    TEXT("Eco.Debug.Agents"), 1, TEXT("Dibuja los agentes de debug como esferas."));
static TAutoConsoleVariable<int32> CVarDebugTerrain(
    TEXT("Eco.Debug.Terrain"), 0, TEXT("Dibuja las normales del terreno en una rejilla de sondas."));

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
    SecondsPerTick   = S->SecondsPerSimTick;
    YearsPerTick     = S->YearsPerTick;
    MaxStepsPerFrame = S->MaxStepsPerFrame;
    bPaused          = S->bStartPaused;
}

void UEcosystemSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);

    const UEcosystemSettings* S = UEcosystemSettings::Get();

    // 1) Generar el relieve: fuente de verdad de la simulacion.
    HeightField.Origin = FVector2D::ZeroVector;
    HeightField.GenerateFractalNoise(
        S->HeightfieldResolution, S->HeightfieldResolution,
        S->HeightfieldCellSizeCm, static_cast<uint32>(S->MasterSeed),
        /*Octaves*/ 5, /*BaseFrequency*/ 0.0006, S->HeightScaleCm);

    UE_LOG(LogTemp, Log, TEXT("[Eco] Relieve %dx%d (~%.0f m de lado) generado con semilla %d"),
        HeightField.Width, HeightField.Height,
        HeightField.Width * HeightField.CellSize / 100.0, S->MasterSeed);

    // 2) Visualizador de campos (heatmap).
    FieldViz = NewObject<UFieldVisualizer>(this);
    FieldViz->Initialize(HeightField.Width, HeightField.Height);
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
    //  FASE 0: intencionadamente vacio.
    //  Aqui entrara el bucle de la Fase 2 (crecimiento, mortalidad,
    //  dispersion...) sobre la poblacion en SoA.
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
    A.Color    = Color;
    A.Radius   = Radius;
    DebugAgents.Add(A);
}

void UEcosystemSubsystem::AddRandomDebugAgent()
{
    const FBox2D B = HeightField.GetWorldBounds();
    const float u = Rng.Unit(EEcoRngStream::Colonization);
    const float v = Rng.Unit(EEcoRngStream::Colonization);
    const double x = FMath::Lerp(B.Min.X, B.Max.X, (double)u);
    const double y = FMath::Lerp(B.Min.Y, B.Max.Y, (double)v);
    const float  z = HeightField.SampleHeight(x, y);

    // Color por "especie" de prueba, si hay especies configuradas.
    FColor Color = FColor::MakeRandomColor();
    const UEcosystemSettings* S = UEcosystemSettings::Get();
    if (S->Species.Num() > 0)
    {
        const int32 Idx = Rng.U32(EEcoRngStream::Colonization) % S->Species.Num();
        if (const USpeciesData* Sp = S->Species[Idx].LoadSynchronous())
        {
            Color = Sp->DebugColor;
        }
    }

    const float R = FMath::Lerp(80.f, 300.f, Rng.Unit(EEcoRngStream::Morphology));
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

    if (CVarDebugAgents.GetValueOnGameThread() != 0)
    {
        for (const FEcoDebugAgent& A : DebugAgents)
        {
            DrawDebugSphere(World, A.Position, A.Radius, 8, A.Color, false, -1.f, 0, 2.f);
        }
    }

    if (CVarDebugTerrain.GetValueOnGameThread() != 0)
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
//  Heatmap de prueba
// ---------------------------------------------------------------------------
void UEcosystemSubsystem::PaintTestField()
{
    if (!FieldViz || !HeightField.IsValid()) return;

    // Campo de prueba = la propia altura (en Fase 1 seran agua/nutrientes/luz).
    const int32 W = HeightField.Width;
    const int32 H = HeightField.Height;

    float MinV =  TNumericLimits<float>::Max();
    float MaxV = -TNumericLimits<float>::Max();
    for (int32 i = 0; i < W * H; ++i)
    {
        MinV = FMath::Min(MinV, HeightField.Data[i]);
        MaxV = FMath::Max(MaxV, HeightField.Data[i]);
    }

    FieldViz->UpdateFromField(HeightField.Data, MinV, MaxV);
    EnsureHeatmapDecal();

    UE_LOG(LogTemp, Log, TEXT("[Eco] Campo de prueba pintado (rango %.0f..%.0f cm)."), MinV, MaxV);
}

void UEcosystemSubsystem::EnsureHeatmapDecal()
{
    UWorld* World = GetWorld();
    if (!World || !FieldViz || !FieldViz->GetTexture()) return;

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    UMaterialInterface* Base = S->HeatmapDecalMaterial.LoadSynchronous();
    if (!Base)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[Eco] Falta HeatmapDecalMaterial en Project Settings; no se puede pintar el decal."));
        return;
    }

    const FBox2D B = HeightField.GetWorldBounds();
    const FVector Center(0.5 * (B.Min.X + B.Max.X),
                         0.5 * (B.Min.Y + B.Max.Y),
                         S->HeightScaleCm * 1.5f);

    // Extensiones del decal (ajusta si la orientacion no cuadra en tu escena):
    //  X = profundidad de proyeccion, Y/Z = mitad del rectangulo proyectado.
    const float HalfX = 0.5f * (B.Max.X - B.Min.X);
    const float HalfY = 0.5f * (B.Max.Y - B.Min.Y);
    const FVector DecalSize(S->HeightScaleCm * 2.f, HalfY, HalfX);

    if (!HeatmapDecal)
    {
        // Pitch -90 hace que el eje X del decal apunte hacia abajo (-Z) y proyecte sobre el suelo.
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
