/**
 * @file EcoFrameProfiler.h
 * @author Juan Luque Roldán
 * @brief Perfilador de frame: reparto del tiempo de frame frente al presupuesto y captura
 *        de escalabilidad a CSV.
 *
 * Declara el subsistema de mundo que mide el frame completo contra el objetivo configurado,
 * separa la parte atribuible al ecosistema —tick de simulación más re-nivelado— del resto y
 * graba una fila por frame para las curvas de framerate frente a población. Es el
 * complemento de `Eco.Profile`, que desglosa el tick por etapas pero no dice nada del frame
 * en el que ese tick vive. Declara también la fila de captura @ref FEcoFrameSample.
 *
 * @ingroup eco_debug
 * @see @ref bib_epicueperfilado
 */

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EcoFrameProfiler.generated.h"

class UEcosystemSubsystem;
class UTreeRenderSubsystem;

/** Una fila de la captura: el estado del frame en el instante en que se tomó la muestra. */
struct FEcoFrameSample
{
    float  TimeSec = 0.f;    ///< Segundos transcurridos desde el inicio de la captura.
    float  FrameMs = 0.f;    ///< Duración del frame, en milisegundos.
    int32  Population = 0;   ///< Árboles vivos en la simulación.
    float  TickMs = 0.f;     ///< Coste del tick de simulación, en milisegundos.
    float  RelevelMs = 0.f;  ///< Coste del último re-nivelado, en milisegundos.
    int32  Hero = 0;         ///< Árboles dibujados con geometría propia.
    int32  Instance = 0;     ///< Árboles dibujados como instancia de malla horneada.
    int32  Impostor = 0;     ///< Árboles dibujados como impostor.
    int32  Culled = 0;       ///< Árboles fuera de rango: simulados pero no dibujados.
};

/**
 * Presupuesto de frame y captura de escalabilidad.
 *
 * Mide el frame completo contra el objetivo configurado (`UEcosystemSettings::FrameBudgetMs`,
 * 16,6 ms para 60 fps) y reparte ese tiempo entre lo que cuesta el ecosistema —tick de
 * simulación más re-nivelado— y todo lo demás. Con ese reparto emite el diagnóstico que
 * precede a cualquier optimización: si el frame se pasa del objetivo pero el ecosistema
 * apenas cuenta, el cuello de botella no está en la simulación. `Eco.Frame.Capture` graba
 * además una fila por frame a CSV, material directo de las curvas de framerate frente a
 * número de árboles.
 *
 * No desglosa game thread, render thread ni GPU, y es deliberado: eso lo dan mejor las
 * herramientas del motor —`stat Unit`, `stat GPU`, `ProfileGPU`, `NaniteStats`, las cvars
 * `r.Shadow.Virtual.*` de las sombras virtuales y Unreal Insights—, a las que redirige el
 * propio diagnóstico. Es una capa de triaje sobre la instrumentación nativa, no un sustituto.
 *
 * @note Consumidor de solo lectura: no escribe en la simulación ni consume aleatoriedad.
 * @see UEcosystemSubsystem::GetTickProfile
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

    /**
     * Vuelca al log el reparto del frame frente al presupuesto y el diagnóstico asociado:
     * media, p95 y máximo de la ventana, coste del ecosistema y su porcentaje, población y
     * reparto por nivel de representación.
     */
    void LogFrameBudget() const;

    /** Descarta el histórico de la ventana, para que un cambio de ajustes no arrastre los
        frames medidos con la configuración anterior. */
    void ResetWindow();

    /**
     * Empieza a grabar una fila por frame durante Seconds y, al terminar, escribe
     * Saved/EcoProfile/<Name>.csv.
     *
     * @param Seconds Duración de la captura; con un valor <= 0 se cancela la captura en
     *                curso y se descartan sus filas.
     * @param Name    Nombre del fichero sin extensión; vacío equivale a "captura".
     * @note La captura también termina si alcanza el tope de filas, y se vuelca al cerrar
     *       el mundo para no perderla.
     */
    void StartCapture(float Seconds, const FString& Name);

    /** Muestra u oculta el reparto del frame en pantalla. */
    void SetShowHUD(bool bInShow) { bShowHUD = bInShow; }

    bool IsCapturing() const { return bCapturing; }

private:
    /** Resuelve perezosamente los subsistemas y dimensiona la ventana en el primer tick
        útil. @return false mientras no haya mundo o ecosistema con el que medir. */
    bool EnsureInitialized();

    void DrawHUD() const;
    void FinishCapture();

    /** Percentil por rango cercano sobre una copia ordenada de la ventana, con P en [0,1].
        Se usa el p95 además de la media porque la fluidez percibida la marca la cola. */
    float Percentile(float P) const;

    /** Media de los frames medidos, en milisegundos; 0 si aún no hay ninguno. Punto único
        de la suma, para que el log y el HUD no puedan dar cifras distintas. */
    float WindowAverageMs() const;

    /** Coste del ecosistema en el frame, desglosado en tick de simulación y re-nivelado.
        El subsistema de render es opcional: si falta, el re-nivelado sale como 0. */
    void GetEcoCostMs(float& OutTickMs, float& OutRelevelMs) const;

    UPROPERTY(Transient) TObjectPtr<UEcosystemSubsystem> Eco = nullptr;
    UPROPERTY(Transient) TObjectPtr<UTreeRenderSubsystem> Render = nullptr;

    /** Ventana circular de tiempos de frame en milisegundos (~4 s a 60 fps). */
    TArray<float> Window;

    /** Hueco donde se escribe el frame actual. */
    int32 WindowCursor = 0;

    /** Huecos ya escritos, saturado al tamaño de la ventana: hace válidas las estadísticas
        antes de que la ventana se llene por primera vez. */
    int32 WindowFilled = 0;

    /** Buffer reutilizable del percentil: evita alojar en cada log o cada frame de HUD. */
    mutable TArray<float> SortScratch;

    // --- Captura a CSV ---

    /** Filas acumuladas de la captura en curso, una por frame. */
    TArray<FEcoFrameSample> Samples;

    /** Nombre del fichero de destino, sin ruta ni extensión. */
    FString  CaptureName;

    /** Segundos que le quedan a la captura; al llegar a cero se vuelca el fichero. */
    float    CaptureSecondsLeft = 0.f;

    /** Segundos ya grabados: es la columna de tiempo de cada fila. */
    float    CaptureElapsed = 0.f;

    bool     bCapturing = false;

    /** Estado del HUD; su valor inicial sale de los ajustes del proyecto. */
    bool bShowHUD = false;

    /** La resolución perezosa de dependencias ya se completó. */
    bool bInitialized = false;
};
