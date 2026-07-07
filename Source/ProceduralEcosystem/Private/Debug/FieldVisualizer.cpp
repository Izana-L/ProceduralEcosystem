#include "Debug/FieldVisualizer.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

void UFieldVisualizer::Initialize(int32 InWidth, int32 InHeight)
{
    Width  = FMath::Max(1, InWidth);
    Height = FMath::Max(1, InHeight);
    Pixels.SetNumUninitialized(Width * Height);

    DynamicTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
    if (DynamicTexture)
    {
        DynamicTexture->SRGB     = true;
        DynamicTexture->Filter   = TF_Bilinear;
        DynamicTexture->AddressX = TA_Clamp;
        DynamicTexture->AddressY = TA_Clamp;
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
    return C.ToFColor(true);
}

void UFieldVisualizer::UpdateFromField(const TArray<float>& Field, float MinValue, float MaxValue)
{
    if (!DynamicTexture || Field.Num() != Width * Height) return;

    const float Range = FMath::Max(MaxValue - MinValue, KINDA_SMALL_NUMBER);
    for (int32 i = 0; i < Field.Num(); ++i)
    {
        const float t = (Field[i] - MinValue) / Range;
        Pixels[i] = Ramp(t);
    }

    // Nota: en UE 5.x el acceso es GetPlatformData(); en versiones antiguas era
    // el miembro PlatformData. Si tu 5.7.x difiere, ajusta esta línea.
    FTexture2DMipMap& Mip = DynamicTexture->GetPlatformData()->Mips[0];
    void* Dst = Mip.BulkData.Lock(LOCK_READ_WRITE);
    FMemory::Memcpy(Dst, Pixels.GetData(), Pixels.Num() * sizeof(FColor));
    Mip.BulkData.Unlock();
    DynamicTexture->UpdateResource();
}
