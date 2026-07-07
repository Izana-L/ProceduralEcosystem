#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FieldVisualizer.generated.h"

/**
 * Convierte un campo escalar 2D (TArray<float>) en una textura dinámica con
 * rampa de color, para pintarlo como heatmap sobre el terreno (§Fase 0).
 *
 * Reutilizable en Fase 1 para agua / nutrientes / luz sin tocar el pipeline.
 */
UCLASS()
class PROCEDURALECOSYSTEM_API UFieldVisualizer : public UObject
{
    GENERATED_BODY()

public:
    void Initialize(int32 InWidth, int32 InHeight);

    /** Mapea Field[i] de [MinValue, MaxValue] a una rampa (azul->verde->rojo) y sube a GPU. */
    void UpdateFromField(const TArray<float>& Field, float MinValue, float MaxValue);

    UTexture2D* GetTexture() const { return DynamicTexture; }

private:
    static FColor Ramp(float T); // T en [0, 1]

    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> DynamicTexture = nullptr;

    int32 Width  = 0;
    int32 Height = 0;
    TArray<FColor> Pixels;
};
