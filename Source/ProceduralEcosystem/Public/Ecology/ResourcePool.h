/**
 * @file ResourcePool.h
 * @author Juan Luque Roldán
 * @brief Estado runtime de un recurso del suelo, agua o nutrientes, con doble buffer.
 *
 * Declara FResourcePool, el nivel de recurso que hay disponible ahora mismo en cada celda,
 * por oposición al campo BASE del terreno (FWaterField, FNutrientField), que es el
 * potencial calculado una vez y congelado. El pool es lo que los árboles agotan al
 * consumir y lo que vuelve despacio hacia el base por recarga y difusión, de modo que la
 * competencia entre vecinos se ejerce sobre un recurso común y no por un índice de
 * interferencia. Mantiene dos copias de FField2D, Current y Next: todo el tick lee de
 * Current y escribe en Next, y al cerrarlo se intercambian, con lo que ninguna decisión
 * depende del orden en que los hilos procesen a los árboles.
 *
 * @ingroup eco_ecology
 * @see @ref bib_tilman1982
 */

#pragma once

#include "CoreMinimal.h"
#include "Terrain/Field2D.h"

/**
 * Recurso consumible de una celda del terreno, con el doble buffer del tick.
 *
 * Reutiliza FField2D para los dos buffers en vez de reimplementar el muestreo bilineal:
 * cada uno es una copia de la geometría del campo base con sus propios valores.
 *
 * Ciclo de un tick: InitFromBase una sola vez al arrancar el mundo, y después
 * BeginTick, los deltas de consumo que deposita la simulación, RegenerateTowardBase y
 * SwapBuffers.
 */
struct PROCEDURALECOSYSTEM_API FResourcePool
{
    FField2D Current; ///< Nivel vigente: lo que lee el tick.
    FField2D Next;    ///< Nivel en construcción: lo que escribe el tick.

    /** Arranca el pool lleno, al nivel del campo base. Solo al inicializar el mundo. */
    void InitFromBase(const FField2D& Base)
    {
        Current = Base;
        Next = Base;
        Snapshot.Reset();
    }

    /** Recurso disponible en el punto de mundo (Xcm, Ycm), bilineal sobre el buffer de lectura. */
    float SampleCurrent(double Xcm, double Ycm) const { return Current.SampleBilinear(Xcm, Ycm); }

    /** Copia Current sobre Next: punto de partida del tick, antes de sumarle sus deltas. */
    void BeginTick() { Next = Current; }

    /**
     * Aplica sobre Next la recarga hacia el campo base y la difusión entre celdas vecinas.
     *
     * @param Base          Potencial del terreno hacia el que relaja el pool.
     * @param RechargeRate  Velocidad de la recarga, por año.
     * @param DiffusionRate Velocidad de la redistribución entre vecinos, por año.
     * @param DtYears       Paso de integración, en años.
     * @pre Next ya trae Current más los deltas de consumo del tick.
     * @note La difusión lee de una copia inmutable de Next y no del propio array: hacerlo
     *       in situ haría que cada celda viese valores ya actualizados o aún viejos según
     *       el orden de recorrido, y el resultado dejaría de ser reproducible.
     */
    void RegenerateTowardBase(const FField2D& Base, float RechargeRate, float DiffusionRate, float DtYears);

    /** Cierra el tick: el buffer de escritura pasa a ser el vigente. */
    void SwapBuffers() { Swap(Current, Next); }

    /** Bytes reservados por el buffer auxiliar persistente, para el informe de perfilado. */
    int32 ScratchBytes() const { return Snapshot.Max() * sizeof(float); }

private:
    /**
     * Copia de trabajo que RegenerateTowardBase usa como origen de la difusión, miembro
     * persistente en lugar de local.
     *
     * El campo entero ronda 1 MB a 512x512 y se copia una vez por pool y por tick;
     * conservado entre llamadas, la asignación reutiliza la capacidad y a partir del
     * segundo tick no hay ninguna llamada al heap.
     */
    TArray<float> Snapshot;
};
