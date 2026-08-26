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
    if (!(KillRadiusDk < StepLengthD && StepLengthD < InfluenceRadiusDi))
    {
        Context.AddError(FText::FromString(
            TEXT("Debe cumplirse d_k < D < d_i (KillRadiusDk < StepLengthD < InfluenceRadiusDi); "
                "si no, el SCA no ramifica o los atractores no se consumen (doc. §3.1).")));
        Fail();
    }
    const float TrunkGap = CrownHeightCm * FMath::Clamp(TrunkFraction, 0.f, 0.95f) / (1.f - FMath::Clamp(TrunkFraction, 0.f, 0.95f));
    if (InfluenceRadiusDi <= TrunkGap)
    {
        Context.AddError(FText::FromString(FString::Printf(
            TEXT("InfluenceRadiusDi (%.0f) <= hueco de tronco (%.0f cm): el SCA no arrancara (ningun atractor en rango del nodo base)."),
            InfluenceRadiusDi, TrunkGap)));
        Result = EDataValidationResult::Invalid;
    }
    if (CrownRadiusCm <= 0.f || CrownHeightCm <= 0.f)
    {
        Context.AddError(FText::FromString(
            TEXT("CrownRadiusCm y CrownHeightCm deben ser > 0: definen la envolvente donde se siembran los atractores.")));
        Fail();
    }

    if (StepLengthD > CrownHeightCm)
    {
        Context.AddWarning(FText::FromString(
            TEXT("StepLengthD > CrownHeightCm: cada paso de crecimiento supera la copa entera; sube MaxIter o baja D.")));
    }

    if (LeafSpacingCm <= 0.f)
    {
        Context.AddError(FText::FromString(
            TEXT("LeafSpacingCm debe ser > 0: es el paso de la espiral filotáctica (divisor en TreeFoliage).")));
        Fail();
    }
    else if (LeafSpacingCm < LeafSizeCm * 0.25f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("LeafSpacingCm (%.1f) muy por debajo de LeafSizeCm (%.1f): las hojas se solaparán mucho y el coste de overdraw se dispara."),
            LeafSpacingCm, LeafSizeCm)));
    }

    if (LeafBearingRadiusScale < 1.f)
    {
        Context.AddWarning(FText::FromString(
            TEXT("LeafBearingRadiusScale < 1: ni las ramillas terminales llegarían a llevar hoja.")));
    }

    if (FMath::IsNearlyZero(FMath::Fmod(PhyllotaxisAngleDeg, 360.f)))
    {
        Context.AddWarning(FText::FromString(
            TEXT("PhyllotaxisAngleDeg = 0: todas las hojas de una ramilla saldrían en el mismo azimut.")));
    }

    return Result;
}
#endif // WITH_EDITOR