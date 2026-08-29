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

    // =====================================================================
    //  COMPROMISOS ENTRE RASGOS
    // =====================================================================
    // Estos avisos no comprueban que el asset sea "correcto": comprueban que la
    // especie PAGA por sus ventajas. El modelo no impone ningun coste por si
    // solo -subir tolerancia, crecimiento, longevidad o fecundidad sale gratis-
    // y sin compromisos existe una estrategia estrictamente dominante, con lo
    // que la exclusion competitiva deja de ser un resultado y pasa a ser una
    // certeza. Son avisos, no errores: si sabes por que lo haces, ignoralos.
    //
    // Para la comparacion ENTRE especies (que es la que de verdad decide si hay
    // una dominante) usa el comando de consola Eco.AuditarEspecies: un asset no
    // puede ver a los demas desde aqui.

    if (ShadeTolerance > 0.7f && GrowthRate > 0.30f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("ShadeTolerance %.2f con GrowthRate %.2f: tolerante a la sombra Y de crecimiento rapido a la vez. Ese arbol no existe (la tolerancia se paga con madera densa y crecimiento lento) y en el modelo es una estrategia dominante: gana en el claro y ademas bajo el dosel."),
            ShadeTolerance, GrowthRate)));
    }

    if (GrowthRate > 0.35f && Longevity > 600.f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("GrowthRate %.2f con Longevity %.0f: crecimiento de pionera y vida de arbol de dosel. El compromiso real es el contrario (crecer rapido cuesta madera barata y vida corta)."),
            GrowthRate, Longevity)));
    }

    // El umbral de luz para germinar y la tolerancia a la sombra describen LO
    // MISMO desde dos lados (donde puedo instalarme / donde puedo vivir). Si se
    // contradicen, la especie germina en sitios donde luego se muere, o al reves
    // desaprovecha el sotobosque que si podria ocupar -que es donde una climax se
    // juega la partida-.
    if (ShadeTolerance > 0.7f && MinLightForGermination > 0.3f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("ShadeTolerance %.2f (muy tolerante) con MinLightForGermination %.2f (exige claro): esta especie puede VIVIR en penumbra pero no puede INSTALARSE alli, asi que renuncia al banco de plantulas del sotobosque, que es como una climax hereda los huecos sin competir por numero de semillas. Prueba 0.05-0.15."),
            ShadeTolerance, MinLightForGermination)));
    }
    if (ShadeTolerance < 0.35f && MinLightForGermination < 0.35f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("ShadeTolerance %.2f (heliofila) con MinLightForGermination %.2f (germina en penumbra): sembrara bajo dosel y esas plantulas se moriran. Sube el umbral a 0.5-0.6."),
            ShadeTolerance, MinLightForGermination)));
    }

    if (SeedRateScale > 1.5f && GerminationRateScale > 1.2f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("SeedRateScale %.2f con GerminationRateScale %.2f: mucha semilla Y que ademas arraiga bien. Son las dos mitades del mismo compromiso r/K y deben ir en sentidos opuestos."),
            SeedRateScale, GerminationRateScale)));
    }

    // --- Nicho de recurso (respuesta unimodal) ---
    // La campana solo reparte nicho si es lo bastante ESTRECHA. Una anchura del
    // orden del rango entero del campo deja la respuesta casi plana: la especie
    // responde igual en todo el mapa y vuelve a competir solo por el ranking
    // global, que es justo lo que el nicho venia a evitar.
    if (WaterTolerance >= 1.f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("WaterTolerance (%.2f) cubre el rango entero del campo de agua: la respuesta queda casi plana y esta especie no se reparte el gradiente con nadie. Prueba 0.2-0.4."),
            WaterTolerance)));
    }
    if (NutrientTolerance >= 1.f)
    {
        Context.AddWarning(FText::FromString(FString::Printf(
            TEXT("NutrientTolerance (%.2f) cubre el rango entero del campo de nutrientes: respuesta casi plana, sin reparto de nicho. Prueba 0.25-0.45."),
            NutrientTolerance)));
    }
    if (!bWaterloggingPenalty && WaterOptimum > 0.5f)
    {
        // Sin penalizacion por exceso la campana satura en 1 por encima del
        // optimo, o sea que por arriba vuelve a ser monotona: la especie es
        // igual de buena en su optimo que en todo lo mas humedo, y la de
        // vaguada acaba ganando tambien donde no le toca.
        Context.AddWarning(FText::FromString(
            TEXT("bWaterloggingPenalty desactivado con un WaterOptimum alto: por encima del optimo la respuesta al agua vuelve a ser monotona y se pierde la mitad humeda del reparto de nicho.")));
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

    // --- Deformacion de tronco por arbol (arqueado / torcido) ---
    {
        float SumMaxDeg = 0.f;
        for (int32 i = 0; i < TrunkDeformLayers.Num(); ++i)
        {
            const FTrunkDeformLayerSpec& L = TrunkDeformLayers[i];

            if (L.MaxAngleDeg < L.MinAngleDeg)
            {
                Context.AddWarning(FText::FromString(FString::Printf(
                    TEXT("TrunkDeformLayers[%d]: MaxAngleDeg (%.0f) < MinAngleDeg (%.0f). El deformador los ordena solo, pero el asset se lee al reves de lo que hace."),
                    i, L.MaxAngleDeg, L.MinAngleDeg)));
            }

            if (L.Type == ETrunkDeformType::Lean && FMath::Max(L.MinAngleDeg, L.MaxAngleDeg) > 12.f)
            {
                Context.AddWarning(FText::FromString(FString::Printf(
                    TEXT("TrunkDeformLayers[%d]: Lean a %.0f grados inclina el arbol ENTERO en bloque y se lee como que se cae, no como arqueado. Para angulos grandes usa Arc."),
                    i, FMath::Max(L.MinAngleDeg, L.MaxAngleDeg))));
            }

            if (L.Probability > 0.f)
            {
                SumMaxDeg += FMath::Max(L.MinAngleDeg, L.MaxAngleDeg);
            }
        }

        // 57 grados = MaxTrunkBendRad (1 rad), el tope que aplica TrunkDeformer.
        if (SumMaxDeg > 57.f)
        {
            Context.AddWarning(FText::FromString(FString::Printf(
                TEXT("La suma de angulos maximos de TrunkDeformLayers (%.0f grados) supera el tope de doblado acumulado (57). Los arboles con varias capas a la vez saldran recortados: baja los angulos o las probabilidades."),
                SumMaxDeg)));
        }
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