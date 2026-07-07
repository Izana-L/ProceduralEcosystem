#include "Debug/FieldVisualizer.h"
#include "Engine/Texture2D.h"
// Si tu versión no encuentra FUpdateTextureRegion2D con estos includes,
// añade #include "RHI.h".

void UFieldVisualizer::Initialize(int32 InWidth, int32 InHeight)
{
    Width = FMath::Max(1, InWidth);
    Height = FMath::Max(1, InHeight);
    Pixels.SetNumUninitialized(Width * Height);

    DynamicTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
    if (DynamicTexture)
    {
        DynamicTexture->SRGB = true;
        DynamicTexture->Filter = TF_Bilinear;
        DynamicTexture->AddressX = TA_Clamp;
        DynamicTexture->AddressY = TA_Clamp;
        DynamicTexture->NeverStream = true; // textura dinámica: no queremos streaming
#if WITH_EDITORONLY_DATA
        DynamicTexture->MipGenSettings = TMGS_NoMipmaps;
#endif
        // Crea el recurso RHI UNA sola vez. A partir de aquí actualizamos con
        // UpdateTextureRegions (barato) en vez de recrearlo.
        DynamicTexture->UpdateResource();
    }
}

FColor UFieldVisualizer::Ramp(float T)
{
    T = FMath::Clamp(T, 0.f, 1.f);
    // azul (bajo) -> verde (medio) -> rojo (alto)
    FLinearColor C;
    if (T < 0.5f)
    {
        const float k = T / 0.5f;
        C = FMath::Lerp(FLinearColor(0.05f, 0.10f, 0.60f), FLinearColor(0.10f, 0.70f, 0.20f), k);
    }
    else
    {
        const float k = (T - 0.5f) / 0.5f;
        C = FMath::Lerp(FLinearColor(0.10f, 0.70f, 0.20f), FLinearColor(0.85f, 0.15f, 0.10f), k);
    }
    // ToFColor(true): codifica a sRGB en 8 bits; con la textura marcada SRGB=true
    // el sampler la vuelve a linealizar al leerla -> el color se conserva.
    return C.ToFColor(true);
}

void UFieldVisualizer::UpdateFromField(const TArray<float>& Field, float MinValue, float MaxValue)
{
    if (!DynamicTexture || Field.Num() != Width * Height) { return; }

    const float Range = FMath::Max(MaxValue - MinValue, KINDA_SMALL_NUMBER);
    for (int32 i = 0; i < Field.Num(); ++i)
    {
        const float t = (Field[i] - MinValue) / Range;
        Pixels[i] = Ramp(t);
    }

    UploadPixels();
}

void UFieldVisualizer::UpdateFromField(const TArray<float>& Field)
{
    if (Field.Num() != Width * Height) { return; }

    float MinV = TNumericLimits<float>::Max();
    float MaxV = TNumericLimits<float>::Lowest();
    for (const float V : Field)
    {
        MinV = FMath::Min(MinV, V);
        MaxV = FMath::Max(MaxV, V);
    }

    UpdateFromField(Field, MinV, MaxV);
}

void UFieldVisualizer::UploadPixels()
{
    if (!DynamicTexture) { return; }

    const int32 NumBytes = Width * Height * static_cast<int32>(sizeof(FColor));

    // Copia efímera: UpdateTextureRegions lee la fuente de forma ASÍNCRONA en el
    // render thread, así que no podemos pasarle 'Pixels' directamente (se
    // sobrescribiría en la siguiente actualización). Ambos punteros (búfer y
    // región) se liberan en el callback, ya en el render thread.
    FColor* Buffer = static_cast<FColor*>(FMemory::Malloc(NumBytes));
    FMemory::Memcpy(Buffer, Pixels.GetData(), NumBytes);

    FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(
        /*DestX*/ 0, /*DestY*/ 0, /*SrcX*/ 0, /*SrcY*/ 0,
        /*Width*/ Width, /*Height*/ Height);

    DynamicTexture->UpdateTextureRegions(
        /*MipIndex*/ 0,
        /*NumRegions*/ 1,
        Region,
        /*SrcPitch*/ static_cast<uint32>(Width * sizeof(FColor)),
        /*SrcBpp*/   static_cast<uint32>(sizeof(FColor)),
        reinterpret_cast<uint8*>(Buffer),
        [](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
        {
            FMemory::Free(SrcData);
            delete Regions;
        });
}