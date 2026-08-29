/**
 * @file HeightmapExport.cpp
 * @author Juan Luque Roldán
 * @brief Comando `Eco.ExportHeightmap`: vuelca el relieve de la simulación a un mapa de
 *        alturas importable como Landscape.
 *
 * El relieve del que vive la ecología es un `FHeightField` matemático e invisible; el
 * Landscape del nivel es solo su representación. Esta unidad de traducción cierra esa
 * distancia: remuestrea el campo de alturas a la resolución pedida, lo cuantiza a un PNG
 * gris de 16 bits con la convención de importación de Unreal y deja en el log la escala y la
 * posición exactas con las que el Landscape queda superpuesto al terreno que se muestrea. No
 * tiene cabecera pública: se invoca desde consola y no lo llama nadie más.
 *
 * @ingroup eco_debug
 * @see @ref bib_epicuelandscape
 */

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "Simulation/EcosystemSubsystem.h"
#include "Terrain/HeightField.h"
#include "Terrain/Field2D.h"

DEFINE_LOG_CATEGORY_STATIC(LogEcoExport, Log, All);

// =============================================================================
//  Exportación del relieve
//
//  Se exporta un PNG en lugar de construir el Landscape desde C++ porque la
//  creación por código vive en módulos de solo editor cuya API cambia entre
//  versiones del motor, mientras que el flujo de importar un mapa de alturas es
//  estable y deja un asset normal en el nivel.
// =============================================================================

namespace
{
    /**
     * Exporta el relieve a PNG y loguea los parámetros de importación.
     *
     * @param Args Resolución y nombre, ambos opcionales y en cualquier orden si el nombre no
     *             es numérico; la resolución se recorta al rango admitido.
     * @pre Requiere el subsistema del ecosistema arrancado y con el relieve ya generado.
     */
    void ExportHeightmap(const TArray<FString>& Args, UWorld* World)
    {
        UEcosystemSubsystem* Eco = World ? World->GetSubsystem<UEcosystemSubsystem>() : nullptr;
        if (!Eco || !Eco->GetHeightField().IsValid())
        {
            UE_LOG(LogEcoExport, Warning,
                TEXT("[EcoExport] No hay relieve que exportar: ejecuta el comando en PIE, con el subsistema arrancado."));
            return;
        }

        const FHeightField& HF = Eco->GetHeightField();
        const FField2D& F = HF.Field;

        // --- Argumentos: resolución del PNG y nombre del fichero ---
        // Parseo tolerante con lo que entrega la consola. Los tokens vacíos se descartan:
        // un espacio de más al teclear llega como argumento vacío, se convierte en 0 y
        // acabaría exportando un PNG del tamaño mínimo sin avisar. El nombre se admite
        // también en primera posición, de modo que un único argumento no numérico se lee
        // como nombre y no como resolución.
        TArray<FString> Clean;
        for (const FString& A : Args)
        {
            const FString T = A.TrimStartAndEnd();
            if (!T.IsEmpty()) { Clean.Add(T); }
        }

        constexpr int32 DefaultRes = 505;   // tamaño recomendado: 8x8 componentes de 63x63 quads
        constexpr int32 MinRes = 64;
        constexpr int32 MaxRes = 8192;

        int32 Res = DefaultRes;
        int32 NameIdx = 0;
        if (Clean.Num() > 0 && Clean[0].IsNumeric())
        {
            const int32 Requested = FCString::Atoi(*Clean[0]);
            Res = FMath::Clamp(Requested, MinRes, MaxRes);
            if (Requested != Res)
            {
                UE_LOG(LogEcoExport, Warning,
                    TEXT("[EcoExport] Resolucion %d fuera del rango [%d, %d]: se exporta a %d."),
                    Requested, MinRes, MaxRes, Res);
            }
            NameIdx = 1;
        }
        else if (Clean.Num() > 0)
        {
            UE_LOG(LogEcoExport, Log,
                TEXT("[EcoExport] '%s' no es un numero: se usa como nombre y la resolucion por defecto (%d)."),
                *Clean[0], DefaultRes);
        }

        const FString Name = Clean.IsValidIndex(NameIdx) ? Clean[NameIdx] : TEXT("heightmap");

        // --- Rango real del relieve, en cm; con el terreno plano se evita dividir por cero ---
        float MinH = 0.f, MaxH = 0.f;
        FField2D::MinMax(F.Data, MinH, MaxH);
        const float Range = MaxH - MinH;
        const bool bFlat = Range <= KINDA_SMALL_NUMBER;

        // --- Remuestreo bilineal a Res x Res sobre la misma extensión ---
        // Los valores de FField2D viven en los nodos (Origin + i*CellSize), igual que los
        // vértices de un Landscape, así que el píxel (i,j) del PNG es el vértice (i,j) del
        // Landscape: no hay volteos ni desplazamiento de medio texel. La extensión recorrida
        // no depende de Res, con lo que cambiar de resolución cambia el detalle y no la
        // escala del terreno exportado.
        const double SpanX = (F.Width - 1) * F.CellSize;
        const double SpanY = (F.Height - 1) * F.CellSize;

        TArray<uint16> Pixels;
        Pixels.SetNumUninitialized(Res * Res);
        for (int32 py = 0; py < Res; ++py)
        {
            const double Wy = F.Origin.Y + SpanY * py / (Res - 1);
            for (int32 px = 0; px < Res; ++px)
            {
                const double Wx = F.Origin.X + SpanX * px / (Res - 1);
                const float H = HF.SampleHeight(Wx, Wy);
                const int32 V = bFlat ? 32768
                    : FMath::RoundToInt((H - MinH) / Range * 65535.f);
                Pixels[py * Res + px] = (uint16)FMath::Clamp(V, 0, 65535);
            }
        }

        // --- PNG gris de 16 bits: el formato que espera la importación de Landscape ---
        IImageWrapperModule& ImgModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> Png = ImgModule.CreateImageWrapper(EImageFormat::PNG);
        if (!Png.IsValid() ||
            !Png->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(uint16), Res, Res, ERGBFormat::Gray, 16))
        {
            UE_LOG(LogEcoExport, Error, TEXT("[EcoExport] No se pudo codificar el PNG."));
            return;
        }
        const TArray64<uint8> Bytes = Png->GetCompressed(100);

        const FString Dir = FPaths::ProjectSavedDir() / TEXT("EcoExport");
        const FString Path = Dir / (Name + TEXT(".png"));
        IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
        if (!FFileHelper::SaveArrayToFile(MakeArrayView(Bytes.GetData(), (int32)Bytes.Num()), *Path))
        {
            UE_LOG(LogEcoExport, Error, TEXT("[EcoExport] No se pudo escribir %s"), *Path);
            return;
        }

        // --- Parámetros de importación ----------------------------------------
        // Se despejan de la fórmula de altura del Landscape imponiendo que el valor mínimo
        // del PNG caiga en la cota mínima del relieve y el máximo en la máxima:
        //   Zmundo = ActorZ + (pixel - 32768) / 128 * EscalaZ
        //   pixel 0     -> MinH  =>  ActorZ  = MinH + 256 * EscalaZ
        //   pixel 65535 -> MaxH  =>  EscalaZ = (MaxH - MinH) * 128 / 65535
        // En XY, con escala E cada quad mide E cm: E = Span / (Res - 1).
        const double ScaleX = SpanX / (Res - 1);
        const double ScaleY = SpanY / (Res - 1);
        const double ScaleZ = bFlat ? 1.0 : (double)Range * 128.0 / 65535.0;
        const double ActorZ = bFlat ? (double)MinH : (double)MinH + 256.0 * ScaleZ;
        const double CenterX = F.Origin.X + 0.5 * SpanX;
        const double CenterY = F.Origin.Y + 0.5 * SpanY;

        const bool bRecommended =
            Res == 127 || Res == 253 || Res == 505 || Res == 1009 ||
            Res == 2017 || Res == 4033 || Res == 8129;

        UE_LOG(LogEcoExport, Log, TEXT("[EcoExport] Heightmap %dx%d escrito en: %s"),
            Res, Res, *FPaths::ConvertRelativePathToFull(Path));
        UE_LOG(LogEcoExport, Log, TEXT("[EcoExport] Relieve: min=%.1f cm, max=%.1f cm (pico-valle %.1f cm)"),
            MinH, MaxH, Range);
        UE_LOG(LogEcoExport, Log, TEXT("[EcoExport] --- Importar: modo Landscape -> New -> 'Import from File' ---"));
        UE_LOG(LogEcoExport, Log, TEXT("[EcoExport]   Scale:    X=%.4f  Y=%.4f  Z=%.4f"), ScaleX, ScaleY, ScaleZ);
        UE_LOG(LogEcoExport, Log, TEXT("[EcoExport]   Location: X=%.1f  Y=%.1f  Z=%.1f (el panel centra el Landscape en XY)"),
            CenterX, CenterY, ActorZ);
        UE_LOG(LogEcoExport, Log, TEXT("[EcoExport]   Equivalente: el vertice (0,0) del Landscape debe caer en (%.1f, %.1f, %.1f)."),
            F.Origin.X, F.Origin.Y, ActorZ);
        if (!bRecommended)
        {
            UE_LOG(LogEcoExport, Warning,
                TEXT("[EcoExport] %d no es un tamano recomendado de Landscape (127, 253, 505, 1009, 2017, 4033, 8129): "
                    "Unreal recortara o rellenara al importar."), Res);
        }
        UE_LOG(LogEcoExport, Log,
            TEXT("[EcoExport] Verificacion en PIE: 'Eco.Debug.Terrain 1' -> las sondas cian deben apoyarse en el Landscape."));
    }

    FAutoConsoleCommandWithWorldAndArgs GEcoExportHeightmap(
        TEXT("Eco.ExportHeightmap"),
        TEXT("Exporta el relieve de la simulacion a Saved/EcoExport/<nombre>.png (PNG gris de 16 bits) y "
            "loguea la escala y posicion exactas para importarlo como Landscape. "
            "Uso: Eco.ExportHeightmap [Resolucion=505] [nombre=heightmap]"),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExportHeightmap));
}
