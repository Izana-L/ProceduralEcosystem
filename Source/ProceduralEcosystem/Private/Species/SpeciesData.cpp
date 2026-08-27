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
    if (SubCrownFraction <= 0.f && InfluenceRadiusDi <= TrunkGap)
    {
        // Solo es un error SIN falda de sub-copa: con ella hay atractores
        // repartidos por el fuste y el SCA arranca aunque d_i no llegue a la
        // base de la copa.
        Context.AddError(FText::FromString(FString::Printf(
            TEXT("InfluenceRadiusDi (%.0f) <= hueco de tronco (%.0f cm) y SubCrownFraction = 0: el SCA no arrancara (ningun atractor en rango del nodo base). Sube d_i o pon algo de SubCrownFraction."),
            InfluenceRadiusDi, TrunkGap)));
        Result = EDataValidationResult::Invalid;
    }

    // --- Perfil y relieve de tronco (troncos organicos) ---
    if (TrunkFlareHeightCm <= 0.f)
    {
        Context.AddError(FText::FromString(
            TEXT("TrunkFlareHeightCm debe ser > 0: es el divisor de la exponencial del ensanche de base.")));
        Fail();
    }

    if (SectionLobeAmount + BarkReliefAmount >= 0.95f)
    {
        Context.AddError(FText::FromString(FString::Printf(
            TEXT("SectionLobeAmount (%.2f) + BarkReliefAmount (%.2f) >= 0.95: la deformacion puede anular el radio y colapsar el tubo sobre su eje."),
            SectionLobeAmount, BarkReliefAmount)));
        Fail();
    }

    const bool bDeformsSection = (SectionLobeAmount > 0.01f) || (BarkReliefAmount > 0.01f);
    if (bDeformsSection && RingSegments < 8)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("RingSegments = %d con relieve de seccion activo: un poligono de tan pocos lados no puede tener lobulos. El mallador subira el minimo efectivo a 8, asi que el asset y la malla no coincidiran."),
            RingSegments)));
    }

    if (CrownShape == ECrownShape::Conical && LeaderFraction < 0.6f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("Copa conica con LeaderFraction = %.2f: el eje muere a media copa y las ramas volveran a concentrarse cerca de su punta (silueta de paraguas). En una conifera excurrente el valor natural es 0.9-1.0."),
            LeaderFraction)));
    }

    if (PerceptionAngleDeg < 45.f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("PerceptionAngleDeg = %.0f: un cono tan estrecho deja atractores huerfanos y el arbol puede quedarse corto. Baja desde 95 en pasos pequenos."),
            PerceptionAngleDeg)));
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