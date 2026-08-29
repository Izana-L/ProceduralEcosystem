/**
 * @file FieldVisualizer.h
 * @author Juan Luque Roldán
 * @brief Conversor de campo escalar 2D a textura de color, para pintar cualquier campo
 *        del modelo como heatmap sobre el terreno.
 *
 * Declara el objeto que traduce un `TArray<float>` alineado con la rejilla de `FField2D`
 * —relieve, agua, nutrientes, luz, idoneidad o descomposición— en una `UTexture2D`
 * transitoria que el decal de diagnóstico proyecta sobre el suelo. No conoce el
 * significado de lo que pinta: recibe los valores y un rango, o lo deduce del propio
 * campo. El recurso de textura se crea una sola vez y cada repintado sube únicamente sus
 * píxeles, sin volver a construirlo.
 *
 * @ingroup eco_debug
 * @see @ref bib_epicuetexturadinamica
 */

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FieldVisualizer.generated.h"

/**
 * Convierte un campo escalar 2D en la textura de color de un heatmap.
 *
 * Normaliza cada valor a [0,1] contra el rango recibido o calculado, lo pasa por una rampa
 * azul-verde-rojo y sube el resultado al mip 0 de una textura transitoria.
 *
 * La subida usa `UpdateTextureRegions`, una copia parcial ejecutada en el render thread, y
 * no `UpdateResource()`, que recrearía el recurso RHI en cada repintado. El recurso se crea
 * una única vez en @ref Initialize.
 *
 * @warning Las actualizaciones cuyo campo no traiga exactamente Width * Height valores se
 *          descartan en silencio: la textura conserva el contenido anterior.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UFieldVisualizer : public UObject
{
    GENERATED_BODY()

public:
    /**
     * Reserva el buffer de píxeles y crea la textura transitoria.
     *
     * @note Las dimensiones se elevan a 1 como mínimo. Se toman de la rejilla del relieve,
     *       de modo que textura y campos comparten geometría y el píxel (i,j) es la celda
     *       (i,j) del campo.
     */
    void Initialize(int32 InWidth, int32 InHeight);

    /**
     * Mapea el campo desde [MinValue, MaxValue] a la rampa de color y sube el resultado.
     *
     * Rango fijo: los valores de fuera saturan en los extremos, con lo que el mismo valor
     * da siempre el mismo color y las manchas no laten entre repintados.
     */
    void UpdateFromField(const TArray<float>& Field, float MinValue, float MaxValue);

    /**
     * Igual que la sobrecarga anterior, tomando el rango del propio campo con
     * `FField2D::MinMax`.
     *
     * @note El color pasa a depender del contenido del campo, así que dos repintados de
     *       instantes distintos no son comparables entre sí.
     */
    void UpdateFromField(const TArray<float>& Field);

    /** Textura que consume el material del decal de heatmap. */
    UTexture2D* GetTexture() const { return DynamicTexture; }

private:
    /** Rampa del heatmap: azul (bajo) -> verde (medio) -> rojo (alto), con T recortada a [0,1]. */
    static FColor Ramp(float T);

    /** Sube 'Pixels' al mip 0 de la textura sin recrear el recurso. */
    void UploadPixels();

    /** Textura de destino, creada una vez en Initialize y nunca guardada en disco. */
    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> DynamicTexture = nullptr;

    /** Ancho de la textura y del campo, en píxeles y celdas a la vez. */
    int32 Width = 0;

    /** Alto de la textura y del campo, en píxeles y celdas a la vez. */
    int32 Height = 0;

    /** Buffer de color en CPU, reutilizado en cada repintado. */
    TArray<FColor> Pixels;
};