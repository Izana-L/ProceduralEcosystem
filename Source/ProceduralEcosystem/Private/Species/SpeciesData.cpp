#include "Species/SpeciesData.h"

// En Fase 0 los parámetros se editan como asset; no hay lógica de runtime.
// Lo único que añadimos es validación en tiempo de editor.

#if WITH_EDITOR
#include "Misc/DataValidation.h"

EDataValidationResult USpeciesData::IsDataValid(FDataValidationContext& Context) const
{
    EDataValidationResult Result = Super::IsDataValid(Context);

    auto Fail = [&Result]() { Result = EDataValidationResult::Invalid; };

    if (MaxBiomass <= 0.f)
    {
        Context.AddError(FText::FromString(
            TEXT("MaxBiomass debe ser > 0 (se usa como divisor en el crecimiento logístico).")));
        Fail();
    }

    if (Longevity <= 0.f)
    {
        Context.AddError(FText::FromString(
            TEXT("Longevity debe ser > 0 (se usa como divisor en la mortalidad por edad).")));
        Fail();
    }

    if (WaterDemand <= 0.f || NutrientDemand <= 0.f)
    {
        Context.AddError(FText::FromString(
            TEXT("WaterDemand y NutrientDemand deben ser > 0 (son divisores en los factores de recurso).")));
        Fail();
    }

    if (MaturityAge >= Longevity)
    {
        Context.AddWarning(FText::FromString(
            TEXT("MaturityAge >= Longevity: la especie moriría (casi) antes de poder reproducirse.")));
        // Es un aviso, no un error: puede ser intencionado para pruebas.
    }

    return Result;
}
#endif // WITH_EDITOR