/**
 * @file FieldVisualizer.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del heatmap: textura transitoria, rampa de color y subida parcial
 *        a GPU.
 *
 * Contiene la creación de la textura con la configuración que necesita un heatmap
 * proyectado (sRGB, filtrado bilineal, direccionamiento por clamp, sin streaming ni
 * mipmaps), la rampa de dos tramos evaluada en espacio lineal, la normalización del campo
 * contra el rango dado o el suyo propio y la subida por regiones, cuyo buffer de origen
 * libera el render thread cuando termina de usarlo.
 *
 * @ingroup eco_debug
 * @see @ref bib_epicuetexturadinamica
 */

#include "Debug/FieldVisualizer.h"
#include "Terrain/Field2D.h" 
#include "Engine/Texture2D.h"


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
        DynamicTexture->NeverStream = true; 
#if WITH_EDITORONLY_DATA
        DynamicTexture->MipGenSettings = TMGS_NoMipmaps;
#endif
        // Única construcción del recurso RHI: los repintados posteriores van por
        // UpdateTextureRegions y no vuelven a pasar por aquí.
        DynamicTexture->UpdateResource();
    }
}

FColor UFieldVisualizer::Ramp(float T)
{
    T = FMath::Clamp(T, 0.f, 1.f);
    // Dos tramos interpolados en espacio lineal y convertidos a sRGB al final, que es la
    // codificación con la que se ha marcado la textura.
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

    float MinV, MaxV;
    FField2D::MinMax(Field, MinV, MaxV); 

    UpdateFromField(Field, MinV, MaxV);
}

void UFieldVisualizer::UploadPixels()
{
    if (!DynamicTexture) { return; }

    const int32 NumBytes = Width * Height * static_cast<int32>(sizeof(FColor));

    // Copia en heap de los píxeles y región también en heap: la subida es asíncrona, así
    // que ni 'Pixels' ni una región en pila sobrevivirían al uso desde el render thread.
    // El lambda de limpieza que se pasa abajo es el dueño de ambos.
    FColor* Buffer = static_cast<FColor*>(FMemory::Malloc(NumBytes));
    FMemory::Memcpy(Buffer, Pixels.GetData(), NumBytes);

    FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0,0,0,0,Width, Height);
         
         

    DynamicTexture->UpdateTextureRegions(
         0,
        1,
        Region,
         static_cast<uint32>(Width * sizeof(FColor)),
         static_cast<uint32>(sizeof(FColor)),
        reinterpret_cast<uint8*>(Buffer),
        [](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
        {
            FMemory::Free(SrcData);
            delete Regions;
        });
}