#include "Debug/EcoFrameProfiler.h"

#include "Config/EcosystemSettings.h"
#include "Core/EcoStats.h"
#include "Simulation/EcosystemSubsystem.h"
#include "Render/TreeRenderSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogEcoProfile, Log, All);

namespace
{
    /** Tamano de la ventana movil de tiempos de frame (~4 s a 60 fps). */
    constexpr int32 kFrameWindow = 240;

    /** Tope de filas de una captura (~10 min a 60 fps). Evita comerse la RAM si
        alguien lanza Eco.Frame.Capture 99999 y se va a comer. */
    constexpr int32 kMaxCaptureSamples = 36000;
}

static UEcoFrameProfiler* GetProfiler(UWorld* World)
{
    return World ? World->GetSubsystem<UEcoFrameProfiler>() : nullptr;
}

// ---------------------------------------------------------------------------
//  Comandos de consola
// ---------------------------------------------------------------------------
static FAutoConsoleCommandWithWorld GEcoFrame(TEXT("Eco.Frame"),TEXT("Reparto del frame frente al presupuesto (doc. 6.4): ms medio/p95/max, fps y coste del ecosistema."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcoFrameProfiler* P = GetProfiler(W)) P->LogFrameBudget(); }));
    

static FAutoConsoleCommandWithWorld GEcoFrameReset(TEXT("Eco.Frame.Reset"),TEXT("Reinicia la ventana de estadisticas de frame (tras cambiar ajustes)."),
    FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* W) { if (UEcoFrameProfiler* P = GetProfiler(W)) P->ResetWindow(); }));
    

static FAutoConsoleCommandWithWorldAndArgs GEcoFrameHUD(TEXT("Eco.Frame.HUD"),TEXT("Muestra (1) u oculta (0) el reparto del frame en pantalla. Uso: Eco.Frame.HUD [0|1]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* W)
        {
            if (UEcoFrameProfiler* P = GetProfiler(W))
            {
                P->SetShowHUD(Args.Num() > 0 ? (FCString::Atoi(*Args[0]) != 0) : true);
            }
        }));

static FAutoConsoleCommandWithWorldAndArgs GEcoFrameCapture(TEXT("Eco.Frame.Capture"),
    TEXT("Graba una fila por frame durante N segundos y escribe Saved/EcoProfile/<nombre>.csv. " "Uso: Eco.Frame.Capture [segundos=10] [nombre=captura]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
        [](const TArray<FString>& Args, UWorld* W)
        {
            if (UEcoFrameProfiler* P = GetProfiler(W))
            {
                const float Secs = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 10.f;
                const FString Name = Args.Num() > 1 ? Args[1] : TEXT("captura");
                P->StartCapture(Secs, Name);
            }
        }));

// ---------------------------------------------------------------------------
//  Ciclo de vida
// ---------------------------------------------------------------------------
bool UEcoFrameProfiler::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UEcoFrameProfiler::Deinitialize()
{
    if (bCapturing)
    {
        FinishCapture(); // no perder una captura por cerrar el PIE
    }
    Eco = nullptr;
    Render = nullptr;
    Window.Reset();
    Samples.Reset();
    bInitialized = false;
    Super::Deinitialize();
}

TStatId UEcoFrameProfiler::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UEcoFrameProfiler, STATGROUP_Tickables);
}

bool UEcoFrameProfiler::EnsureInitialized()
{
    if (bInitialized){ return true; }
  
    UWorld* World = GetWorld();

    if (!World){ return false; }  

    Eco = World->GetSubsystem<UEcosystemSubsystem>();
    Render = World->GetSubsystem<UTreeRenderSubsystem>();

    if (!Eco){ return false; }

    Window.SetNumZeroed(kFrameWindow);
    WindowCursor = 0;
    WindowFilled = 0;

    bShowHUD = UEcosystemSettings::Get()->bShowFrameBudgetHUD;
    bInitialized = true;
    return true;
}

void UEcoFrameProfiler::ResetWindow()
{
    WindowCursor = 0;
    WindowFilled = 0;
    for (float& V : Window) { V = 0.f; }
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------
void UEcoFrameProfiler::Tick(float DeltaTime)
{
    if (!EnsureInitialized() || DeltaTime <= 0.f){ return; }

    const float FrameMs = DeltaTime * 1000.f;

    // Ventana circular: coste O(1) por frame y sin allocations.
    Window[WindowCursor] = FrameMs;
    WindowCursor = (WindowCursor + 1) % Window.Num();
    WindowFilled = FMath::Min(WindowFilled + 1, Window.Num());

    // --- Metricas al CSV profiler del motor (categoria "Eco") ---
    // Con esto, una captura hecha con `csvprofile start` ya trae las columnas del
    // ecosistema junto a las del motor, sin nada mas que hacer.
    const int32 PopNow = Eco->GetLivePopulationCount();
    CSV_CUSTOM_STAT(Eco, Population, PopNow, ECsvCustomStatOp::Set);
    CSV_CUSTOM_STAT(Eco, FrameMs, FrameMs, ECsvCustomStatOp::Set);

    // --- Captura propia a CSV  ---
    if (bCapturing)
    {
        CaptureElapsed += DeltaTime;
        CaptureSecondsLeft -= DeltaTime;

        if (Samples.Num() < kMaxCaptureSamples)
        {
            FEcoFrameSample Row;
            Row.TimeSec = CaptureElapsed;
            Row.FrameMs = FrameMs;
            Row.Population = PopNow;
            GetEcoCostMs(Row.TickMs, Row.RelevelMs);
            if (Render)
            {
                Render->GetTierCounts(Row.Hero, Row.Instance, Row.Impostor, Row.Culled);
            }
            Samples.Add(Row);
        }

        if (CaptureSecondsLeft <= 0.f || Samples.Num() >= kMaxCaptureSamples)
        {
            FinishCapture();
        }
    }

    if (bShowHUD)
    {
        DrawHUD();
    }
}

// ---------------------------------------------------------------------------
//  Estadisticas
// ---------------------------------------------------------------------------
float UEcoFrameProfiler::Percentile(float P) const
{
    if (WindowFilled == 0)
    {
        return 0.f;
    }

    SortScratch.Reset(WindowFilled);
    for (int32 i = 0; i < WindowFilled; ++i)
    {
        SortScratch.Add(Window[i]);
    }
    SortScratch.Sort();

    const int32 Idx = FMath::Clamp(FMath::RoundToInt(P * (SortScratch.Num() - 1)), 0, SortScratch.Num() - 1);
    return SortScratch[Idx];
}

float UEcoFrameProfiler::WindowAverageMs() const
{
    if (WindowFilled == 0)
    {
        return 0.f;
    }

    float Sum = 0.f;
    for (int32 i = 0; i < WindowFilled; ++i)
    {
        Sum += Window[i];
    }
    return Sum / WindowFilled;
}

void UEcoFrameProfiler::GetEcoCostMs(float& OutTickMs, float& OutRelevelMs) const
{
    OutTickMs = Eco ? static_cast<float>(Eco->GetTickProfile().TotalMs) : 0.f;
    OutRelevelMs = Render ? static_cast<float>(Render->GetLastRelevelMs()) : 0.f;
}

void UEcoFrameProfiler::LogFrameBudget() const
{
    if (!bInitialized || WindowFilled == 0)
    {
        UE_LOG(LogEcoProfile, Warning, TEXT("[Eco/F6] Aun no hay frames medidos."));
        return;
    }

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const float Budget = FMath::Max(1.f, S->FrameBudgetMs);

    float Max = 0.f;
    for (int32 i = 0; i < WindowFilled; ++i)
    {
        Max = FMath::Max(Max, Window[i]);
    }
    const float Avg = WindowAverageMs();
    const float P95 = Percentile(0.95f);

    float TickMs, RelevelMs;
    GetEcoCostMs(TickMs, RelevelMs);
    const float OursMs = TickMs + RelevelMs;

    UE_LOG(LogEcoProfile, Log, TEXT("=== [Eco/F6] Presupuesto de frame (objetivo %.1f ms = %.0f fps) ==="), Budget, 1000.f / Budget);
    UE_LOG(LogEcoProfile, Log, TEXT("  Frame: medio %.2f ms (%.0f fps) | p95 %.2f ms | max %.2f ms | ventana %d frames"), Avg, 1000.f / FMath::Max(Avg, 0.001f), P95, Max, WindowFilled);  
    UE_LOG(LogEcoProfile, Log, TEXT("  Ecosistema: tick %.2f ms + re-nivelado LOD %.2f ms = %.2f ms (%.1f%% del frame medio)"),TickMs, RelevelMs, OursMs, 100.f * OursMs / FMath::Max(Avg, 0.001f));   
    UE_LOG(LogEcoProfile, Log, TEXT("  Poblacion: %d arboles vivos"), Eco->GetLivePopulationCount());

    if (Render)
    {
        int32 H = 0, I = 0, Imp = 0, C = 0;
        Render->GetTierCounts(H, I, Imp, C);
        UE_LOG(LogEcoProfile, Log, TEXT("  Reparto: hero %d | instancia %d | impostor %d | fuera de rango %d"), H, I, Imp, C);
    }

    // El DIAGNOSTICO, que es lo que de verdad pide el doc. 6.4 ("primero
    // determinar si el cuello es CPU o GPU").
    if (Avg <= Budget)
    {
        UE_LOG(LogEcoProfile, Log, TEXT("  -> Dentro de presupuesto."));
    }
    else if (OursMs > 0.35f * Avg)
    {
        UE_LOG(LogEcoProfile, Warning, TEXT("  -> Fuera de presupuesto y el ecosistema se lleva una parte grande del frame: "
                                            "mira Eco.Profile para ver que etapa del tick domina, y sube TickBudgetMsPerFrame / "
                                            "RelevelEveryNFrames / LightRebuildEveryNTicks."));
    }
    else
    {
        UE_LOG(LogEcoProfile, Warning, TEXT("  -> Fuera de presupuesto PERO el ecosistema apenas cuenta (%.1f%%): "
                                            "el cuello NO es la simulacion. Sigue por `stat Unit` (game vs render vs GPU), "
                                            "`stat GPU` / ProfileGPU y `stat RHI`; lo mas probable es overdraw de follaje, "
                                            "coste de VSM o material masked en Nanite (doc. 4.6 / 6.4)."),
                                            100.f * OursMs / FMath::Max(Avg, 0.001f));
    }
}

void UEcoFrameProfiler::DrawHUD() const
{
    if (!GEngine || WindowFilled == 0)
    {
        return;
    }

    const UEcosystemSettings* S = UEcosystemSettings::Get();
    const float Budget = FMath::Max(1.f, S->FrameBudgetMs);

    const float Avg = WindowAverageMs();

    float TickMs, RelevelMs;
    GetEcoCostMs(TickMs, RelevelMs);

    const FColor Color = (Avg <= Budget) ? FColor::Green : (Avg <= Budget * 1.5f ? FColor::Yellow : FColor::Red);

    int32 H = 0, I = 0, Imp = 0, C = 0;
    if (Render) { Render->GetTierCounts(H, I, Imp, C); }

    // Claves fijas (0..3): sobrescriben el mensaje anterior en vez de acumular.
    GEngine->AddOnScreenDebugMessage(0, 0.f, Color,FString::Printf(TEXT("Frame %.2f ms (%.0f fps)  | objetivo %.1f ms"), Avg, 1000.f / FMath::Max(Avg, 0.001f), Budget));
        
    GEngine->AddOnScreenDebugMessage(1, 0.f, FColor::White, FString::Printf(TEXT("Eco: tick %.2f ms + LOD %.2f ms"), TickMs, RelevelMs));
       
    GEngine->AddOnScreenDebugMessage(2, 0.f, FColor::White,FString::Printf(TEXT("Arboles %d | hero %d  inst %d  imp %d  off %d"), Eco->GetLivePopulationCount(), H, I, Imp, C));

    if (bCapturing)
    {
        GEngine->AddOnScreenDebugMessage(3, 0.f, FColor::Cyan,
            FString::Printf(TEXT("Capturando... %.1f s restantes (%d filas)"), CaptureSecondsLeft, Samples.Num()));
    }
}

// ---------------------------------------------------------------------------
//  Captura a CSV 
// ---------------------------------------------------------------------------
void UEcoFrameProfiler::StartCapture(float Seconds, const FString& Name)
{
    if (!EnsureInitialized()){ return; }

    if (Seconds <= 0.f)
    {
        if (bCapturing)
        {
            UE_LOG(LogEcoProfile, Log, TEXT("[Eco/F6] Captura cancelada (%d filas descartadas)."), Samples.Num());
            Samples.Reset();
            bCapturing = false;
        }
        return;
    }

    Samples.Reset();
    Samples.Reserve(FMath::Min(kMaxCaptureSamples, FMath::CeilToInt(Seconds * 70.f)));
    CaptureName = Name.IsEmpty() ? TEXT("captura") : Name;
    CaptureSecondsLeft = Seconds;
    CaptureElapsed = 0.f;
    bCapturing = true;

    UE_LOG(LogEcoProfile, Log, TEXT("[Eco/F6] Capturando %.1f s -> Saved/EcoProfile/%s.csv"), Seconds, *CaptureName);
}

void UEcoFrameProfiler::FinishCapture()
{
    bCapturing = false;
    if (Samples.Num() == 0)
    {
        return;
    }

    // Cabecera pensada para abrirse tal cual en una hoja de calculo o en pandas.
    FString Csv = TEXT("Frame,TimeSec,FrameMs,FPS,Population,TickMs,RelevelMs,Hero,Instance,Impostor,Culled\n");
    Csv.Reserve(Samples.Num() * 72);

    for (int32 i = 0; i < Samples.Num(); ++i)
    {
        const FEcoFrameSample& R = Samples[i];
        Csv += FString::Printf(TEXT("%d,%.4f,%.4f,%.2f,%d,%.4f,%.4f,%d,%d,%d,%d\n"),
            i, R.TimeSec, R.FrameMs, 1000.f / FMath::Max(R.FrameMs, 0.001f),
            R.Population, R.TickMs, R.RelevelMs, R.Hero, R.Instance, R.Impostor, R.Culled);
    }

    const FString Dir = FPaths::ProjectSavedDir() / TEXT("EcoProfile");
    const FString Path = Dir / (CaptureName + TEXT(".csv"));
    IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);

    if (FFileHelper::SaveStringToFile(Csv, *Path))
    {
        UE_LOG(LogEcoProfile, Log, TEXT("[Eco/F6] Captura guardada: %s (%d frames)."), *Path, Samples.Num());
    }
    else
    {
        UE_LOG(LogEcoProfile, Error, TEXT("[Eco/F6] No se pudo escribir la captura: %s"), *Path);
    }

    Samples.Reset();
}
