#pragma once

#include "CoreMinimal.h"

/**
 * Helpers geometricos compartidos por los generadores de esqueleto y de malla.
 *
 * Viven aqui, y no en el namespace anonimo de cada .cpp, por una razon muy
 * concreta: UBT compila el modulo en UNITY BUILD, o sea, concatena varios .cpp
 * en UNA sola unidad de traduccion. Al fusionarse, dos namespaces anonimos con
 * una funcion del mismo nombre dejan de ser independientes y el compilador la
 * ve REDEFINIDA. Una unica copia en cabecera evita la colision y, de paso, la
 * duplicacion: el criterio de EcoGrid/EcoRand aplicado a la geometria.
 */
namespace EcoGeom
{
    /** Una perpendicular cualquiera y estable a T (T debe venir normalizado). */
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
