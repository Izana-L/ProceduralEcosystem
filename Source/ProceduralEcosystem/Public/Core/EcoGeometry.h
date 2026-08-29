/**
 * @file EcoGeometry.h
 * @author Juan Luque Roldán
 * @brief Perpendicular estable a una tangente: la primitiva geométrica que comparten el
 *        SCA, el mallador y el follaje.
 *
 * Resuelve un único problema, «dame una perpendicular cualquiera pero ESTABLE a esta
 * tangente», y lo resuelve en un solo sitio para que las tres piezas que lo necesitan
 * no puedan divergir: el ángulo de inserción de rama del SCA, la semilla del marco de
 * rotación mínima del mallador y el eje lateral de la hoja. Estable significa función
 * pura de la tangente, con degradación controlada hasta un valor no nulo; de ello
 * depende que el mismo árbol con la misma semilla produzca la misma malla en cualquier
 * ejecución y en cualquier hilo. Todo es FORCEINLINE en cabecera porque se llama por
 * nodo y por vértice, dentro del bucle caliente del mallador.
 *
 * @ingroup eco_core
 * @see @ref bib_marcorotacionminima
 * @see @ref bib_runions2007
 */

#pragma once

#include "CoreMinimal.h"

/** @brief Geometría vectorial compartida por el crecimiento de ramas, el mallado y el
 *         follaje. */
namespace EcoGeometry
{
    /**
     * Perpendicular unitaria a Axis, eligiendo la primera referencia que no sea
     * paralela a el.
     *
     * @param Axis        Dirección de referencia; conviene que venga normalizada.
     * @param Pref        Referencia preferida: se usa si Axis no es paralelo a ella.
     * @param Fallback    Segunda referencia, para cuando Axis sí es paralelo a Pref.
     * @param Degenerate  Valor devuelto si ni aun así sale un vector válido, es decir
     *                    con Axis nulo.
     * @return Un vector unitario ortogonal a Axis, o Degenerate. Nunca devuelve un
     *         vector sin normalizar.
     */
    FORCEINLINE FVector PerpendicularTo(const FVector& Axis, const FVector& Pref,
        const FVector& Fallback, const FVector& Degenerate)
    {
        FVector P = FVector::CrossProduct(Axis, Pref);
        if (P.IsNearlyZero())
        {
            P = FVector::CrossProduct(Axis, Fallback);
        }
        if (P.IsNearlyZero())
        {
            // Tercera referencia: solo se alcanza si Axis es paralelo a las dos
            // anteriores, es decir si es degenerado. Está por completitud; la red
            // final es el GetSafeNormal de abajo.
            P = FVector::CrossProduct(Axis, FVector::ForwardVector);
        }
        return P.GetSafeNormal(SMALL_NUMBER, Degenerate);
    }

    /**
     * Una perpendicular cualquiera y estable a la tangente T, con la política del
     * proyecto: primero el eje derecho, y el eje adelante como reserva y como valor
     * degenerado.
     *
     * @pre T viene normalizado.
     * @note Para la misma T devuelve siempre la misma perpendicular. De ello dependen
     *       el transporte paralelo del marco del mallador, que si no cambiaría de marco
     *       entre nodos, y el ángulo de inserción de rama del SCA.
     */
    FORCEINLINE FVector AnyPerpendicular(const FVector& T)
    {
        return PerpendicularTo(T, FVector::RightVector, FVector::ForwardVector, FVector::ForwardVector);
    }
}
