/**
 * @file GeometryMath.h
 * @author Juan Luque Roldán
 * @brief Perpendicular estable a una dirección, en el namespace EcoGeom.
 *
 * Declara EcoGeom::AnyPerpendicular, la primitiva con la que se siembra un marco de
 * sección alrededor de un eje: dada una tangente, devuelve siempre la misma
 * perpendicular unitaria. Vive en cabecera, y no en el namespace anónimo de un `.cpp`,
 * porque UnrealBuildTool compila el módulo en unity build: al concatenarse varias
 * unidades de traducción en una sola, dos namespaces anónimos con una función homónima
 * dejan de ser independientes y el compilador la ve redefinida.
 *
 * @note Ninguna unidad de traducción incluye esta cabecera. La perpendicular que
 *       consumen el crecimiento de ramas, el mallador y el follaje es la de
 *       `Core/EcoGeometry.h`, que parametriza además las direcciones de referencia.
 *
 * @ingroup eco_geometry
 * @see EcoGeometry::AnyPerpendicular
 */

#pragma once

#include "CoreMinimal.h"

/** @brief Geometría vectorial auxiliar del generador de árboles. */
namespace EcoGeom
{
    /**
     * Perpendicular unitaria a T, estable: función pura de T, sin estado ni azar.
     *
     * Cruza T con el eje Y de mundo y recurre al eje X cuando ambos son paralelos, de
     * modo que nunca devuelve el vector nulo.
     *
     * @param T Dirección de referencia; debe venir normalizada.
     * @return Vector unitario ortogonal a T.
     */
    FORCEINLINE FVector AnyPerpendicular(const FVector& T)
    {
        FVector P = FVector::CrossProduct(T, FVector::RightVector);
        if (P.IsNearlyZero())
        {
            P = FVector::CrossProduct(T, FVector::ForwardVector);
        }
        return P.GetSafeNormal(SMALL_NUMBER, FVector::ForwardVector);
    }
}
