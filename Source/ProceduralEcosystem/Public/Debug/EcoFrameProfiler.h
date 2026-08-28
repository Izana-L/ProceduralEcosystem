#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EcoFrameProfiler.generated.h"

class UEcosystemSubsystem;
class UTreeRenderSubsystem;

/** Una fila de la captura de frames (una por frame mientras dura la captura). */
struct FEcoFrameSample
{
    float  TimeSec = 0.f;     // segundos desde el inicio de la captura
    float  FrameMs = 0.f;
    int32  Population = 0;
    float  TickMs = 0.f;      // coste medio del tick de simulacion
    float  RelevelMs = 0.f;   // coste del ultimo re-nivelado de LOD
    int32  Hero = 0;
    int32  Instance = 0;
    int32  Impostor = 0;
    int32  Culled = 0;
};

/**
 * =============================================================================
 *  PRESUPUESTO DE FRAME Y CAPTURA DE ESCALABILIDAD (Fase 6, doc. 6.4)
 * =============================================================================
 *
 * El doc. 6.4 pide dos cosas que hasta ahora no existian en el proyecto:
 *
 *   1. "Presupuesto de frame: fija un objetivo (16.6 ms para 60 fps) y reparte."
 *      Eco.Profile ya media el TICK, pero no decia nada del frame en el que ese
 *      tick vive. Aqui se mide el frame entero y se dice que fraccion se lleva
 *      la simulacion y cual el gestor de LOD, contra el objetivo configurado.
 *
 *   2. Datos para las curvas de la Fase 7 ("curvas de framerate vs nº de
 *      arboles"). Eco.Frame.Capture escribe un CSV con una fila por frame
 *      -tiempo, ms, poblacion, reparto por nivel de detalle- listo para
 *      graficar. Sin eso, sacar la grafica de la memoria significa parsear el
 *      log a mano.
 *
 * QUE **NO** HACE, y es deliberado: no intenta desglosar game thread / render
 * thread / GPU. Eso ya lo dan las herramientas del motor mucho mejor de lo que
 * lo haria este codigo, y el propio documento las nombra: `stat Unit` para el
 * reparto por hilo, `stat GPU` y `ProfileGPU` para el desglose de la GPU,
 * `NaniteStats` y los cvars de VSM (r.Shadow.Virtual.*) para el follaje y las
 * sombras, y Unreal Insights para la timeline completa. La regla del doc. 6.4 es
 * "medir primero": este subsistema mide LO NUESTRO y te dice cuando el problema
 * NO es nuestro (frame alto con tick y re-nivelado bajos = el cuello esta en la
 * GPU, ve a `stat GPU`), que es justo el diagnostico que hay que hacer primero.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UEcoFrameProfiler : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Deinitialize() override;
    virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    // --- Consola ---
    /** Vuelca al log el reparto del frame frente al presupuesto. */
    void LogFrameBudget() const;

    /** Reinicia la ventana de estadisticas (tras cambiar algo, para no arrastrar). */
    void ResetWindow();

    /** Empieza a grabar una fila por frame durante Seconds; al terminar escribe
        Saved/EcoProfile/<Name>.csv. Seconds <= 0 cancela una captura en curso. */
    void StartCapture(float Seconds, const FString& Name);

    void SetShowHUD(bool bInShow) { bShowHUD = bInShow; }
    bool IsCapturing() const { return bCapturing; }

private:
    bool EnsureInitialized();
    void DrawHUD() const;
    void FinishCapture();

    /** Percentil sobre una copia ordenada de la ventana (p en [0,1]). */
    float Percentile(float P) const;

    /** Media de los frames medidos (ms), 0 si aun no hay ninguno. El bucle de
        suma estaba escrito en LogFrameBudget y otra vez en DrawHUD, que es
        exactamente la clase de duplicado que hace que el log y el HUD acaben
        diciendo numeros distintos. */
    float WindowAverageMs() const;

    /** Coste del ecosistema en el frame: tick de simulacion + re-nivelado de LOD.
        Lo leian por separado el log y el HUD, cada uno con su ternario sobre
        Render, que puede ser nulo. */
    void GetEcoCostMs(float& OutTickMs, float& OutRelevelMs) const;

    UPROPERTY(Transient) TObjectPtr<UEcosystemSubsystem> Eco = nullptr;
    UPROPERTY(Transient) TObjectPtr<UTreeRenderSubsystem> Render = nullptr;

    /** Ventana circular de tiempos de frame (ms). ~4 s a 60 fps. */
    TArray<float> Window;
    int32 WindowCursor = 0;
    int32 WindowFilled = 0;

    /** Buffer reutilizable del percentil: evita alojar en cada log/HUD. */
    mutable TArray<float> SortScratch;

    // --- Captura a CSV ---
    TArray<FEcoFrameSample> Samples;
    FString  CaptureName;
    float    CaptureSecondsLeft = 0.f;
    float    CaptureElapsed = 0.f;
    bool     bCapturing = false;

    bool bShowHUD = false;
    bool bInitialized = false;
};
