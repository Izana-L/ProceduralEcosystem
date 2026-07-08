#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"

/**
 * Estado RUNTIME de un recurso (agua o nutrientes): cuanto hay disponible
 * AHORA en cada celda, tras consumo y regeneracion.
 *
 * Se distingue del campo BASE (FWaterField/FNutrientField, Fase 1): el base
 * es la fertilidad/humedad POTENCIAL del terreno, calculada una vez y
 * congelada; el pool es el nivel ACTUAL, que se agota con el consumo de los
 * arboles y se recarga hacia el base con el tiempo (doc. 2.6: "recarga
 * lenta hacia el mapa base"). Reutiliza FField2D para Current y Next en vez
 * de reinventar el muestreo bilineal: cada uno es, literalmente, una copia
 * de la geometria del base con sus propios valores.
 *
 * Doble buffer (doc. 2.4): todo el tick LEE de Current y ESCRIBE en Next;
 * al final de cada tick se intercambian.
 */
struct PROCEDURALECOSYSTEM_API FResourcePool
{
    FField2D Current;
    FField2D Next;

    /** Arranca el pool "lleno" al nivel del campo base (primera vez, en OnWorldBeginPlay). */
    void InitFromBase(const FField2D& Base)
    {
        Current = Base;
        Next = Base;
    }

    float SampleCurrent(double Xcm, double Ycm) const { return Current.SampleBilinear(Xcm, Ycm); }

    /** Copia Current -> Next: punto de partida de este tick antes de sumar deltas (ver clase 4). */
    void BeginTick() { Next = Current; }

    /**
     * Recarga lenta hacia Base + difusion (Laplaciano discreto a 4 vecinos),
     * aplicada sobre Next (que ya trae Current + los deltas de consumo del
     * tick sumados). Difunde sobre un SNAPSHOT de Next, no in-place: si
     * leyeramos y escribieramos el mismo array a la vez, el resultado
     * dependeria del orden de recorrido (la celda (1,1) veria ya el nuevo
     * valor de (0,1) pero el viejo de (1,0)) y dejaria de ser determinista
     * frente a, por ejemplo, cambiar el orden de los bucles.
     */
    void RegenerateTowardBase(const FField2D& Base, float RechargeRate, float DiffusionRate, float DtYears);

    void SwapBuffers() { Swap(Current, Next); }
};