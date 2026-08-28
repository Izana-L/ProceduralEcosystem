#pragma once

#include "CoreMinimal.h"

/**
 * Geometria vectorial compartida por la Fase 3 (SCA) y la Fase 6 (mallado y
 * follaje).
 *
 * POR QUE EXISTE ESTE FICHERO: la misma funcion -"dame una perpendicular
 * cualquiera pero ESTABLE a esta tangente"- estaba escrita tres veces, en tres
 * namespaces anonimos distintos y con tres nombres distintos:
 *
 *   - SpaceColonization.cpp : AnyPerpendicular (angulo de insercion de rama)
 *   - TreeMeshBuilder.cpp   : AnyPerpendicular (marco de rotacion minima)
 *   - TreeFoliage.cpp       : SideAxis         (eje lateral de la hoja)
 *
 * Las dos primeras eran COPIAS LITERALES la una de la otra y la tercera es la
 * misma idea con una reserva mas. Al vivir en namespaces anonimos ni siquiera
 * se veian entre si, asi que un arreglo en una no llegaba nunca a las otras: es
 * el caso de libro de codigo repetido que hay que sacar a un sitio unico.
 *
 * Todo es FORCEINLINE en cabecera: son dos productos vectoriales y se llaman
 * por nodo y por vertice, o sea en el bucle caliente del mallador.
 */
namespace EcoGeometry
{
    /**
     * Perpendicular unitaria a Axis, eligiendo la primera referencia que no sea
     * paralela a el.
     *
     * @param Axis        Direccion de referencia (conviene que venga normalizada).
     * @param Pref        Referencia preferida: se usa si Axis no es paralelo a ella.
     * @param Fallback    Segunda referencia, para cuando Axis SI es paralelo a Pref.
     * @param Degenerate  Valor devuelto si ni siquiera asi sale un vector valido
     *                    (Axis nulo). Nunca se devuelve un vector no normalizado.
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
            // anteriores, o sea si es degenerado. Aqui esta por completitud; el
            // GetSafeNormal de abajo es la red final.
            P = FVector::CrossProduct(Axis, FVector::ForwardVector);
        }
        return P.GetSafeNormal(SMALL_NUMBER, Degenerate);
    }

    /**
     * Una perpendicular cualquiera y ESTABLE a T (T debe venir normalizado).
     * "Estable" quiere decir que para la misma T sale siempre la misma: el
     * transporte paralelo del mallador y el angulo de insercion del SCA dependen
     * de ello para no cambiar de marco entre nodos.
     */
    FORCEINLINE FVector AnyPerpendicular(const FVector& T)
    {
        return PerpendicularTo(T, FVector::RightVector, FVector::ForwardVector, FVector::ForwardVector);
    }
}
