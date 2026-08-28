#pragma once

#include "CoreMinimal.h"

class USpeciesData;
struct FTreeSkeleton;

/**
 * Deformacion de tronco POR ARBOL: arqueado, inclinado o sinuoso (doc. Fase 3,
 * extension de §3.4 "tropismos").
 *
 * POR QUE UN PASO APARTE Y NO MAS AMPLITUD EN AxisDirection:
 * el eje del SCA ya lleva una sinuosidad sutil (TrunkSweepDeg / TrunkWobbleDeg,
 * ver AxisDirection en SpaceColonization.cpp), pero esa curva esta atrapada
 * dentro del bucle que ENCADENA el eje, y ese bucle avanza D*cos(Theta) en Z
 * con la condicion de parada tambien en Z: subir la amplitud no da un arbol
 * arqueado, da un eje que avanza cada vez menos y un tope duro
 * (MaxAxisTiltRad, ~20 grados) que existe justo para que no se dispare. Ademas
 * solo doblaria el EJE: las ramas ya colgadas se quedarian donde estaban.
 *
 * Este modulo dobla el arbol ENTERO -tronco y copa- DESPUES de que el SCA haya
 * terminado, como se dobla una vara: un re-encadenado ISOMETRICO que rota cada
 * internodo alrededor de un eje horizontal cuyo angulo crece con la altura.
 * Ninguna longitud de internodo cambia, asi que:
 *   - el pipe model (ComputeRadii) es topologico y no se entera;
 *   - el perfil de tronco (ApplyTrunkProfile) trabaja sobre LONGITUD DE ARCO
 *     (FTreeSkeleton::ComputeAlongLengths), que es invariante bajo isometria:
 *     el ensanche del pie y el afilado del fuste SIGUEN al tronco doblado sin
 *     una sola linea extra;
 *   - los datos de viento y el follaje se construyen despues, desde el
 *     esqueleto ya deformado, y salen coherentes solos.
 *
 * DETERMINISMO: Sample() consume un sub-stream DERIVADO POR HASH de la semilla
 * del arbol, nunca el stream principal (mismo patron que AxisRng en
 * SpaceColonization.cpp). Activar la deformacion no desplaza ni un valor del
 * jitter de la copa, asi que un arbol recto sigue siendo EXACTAMENTE el mismo
 * arbol que antes del cambio.
 */
namespace TrunkDeformer
{
    /**
     * Tope del doblado ACUMULADO de todas las capas en cualquier altura (~57
     * grados). Analogo a MaxAxisTiltRad, pero aqui no protege un bucle: protege
     * la lectura. Por encima de esto el arbol deja de leerse como "arqueado" y
     * pasa a "caido", y las ramas del lado interior empiezan a interpenetrarse.
     */
    constexpr float MaxTrunkBendRad = 1.0f;

    /** Una capa YA RESUELTA para un arbol concreto (la tirada ya salio a favor). */
    struct FTrunkDeformLayerState
    {
        /** Copia de ETrunkDeformType (uint8 plano: este header no depende del asset). */
        uint8 Type = 0;
        /** Angulo maximo de la capa, en radianes. */
        float AngleRad = 0.f;
        /** Azimut hacia el que se inclina el arbol (radianes). */
        float AzimuthRad = 0.f;
        /** Fase de la onda (solo SCurve). */
        float Phase = 0.f;
        /** Exponente (Arc) o nº de ondas (SCurve). */
        float Param = 1.f;
    };

    /**
     * Deformacion completa de UN arbol: las capas que le han tocado.
     *
     * TInlineAllocator: lo normal es 0-2 capas activas, y esto se construye una
     * vez por arbol generado (60 arquetipos al hornear, un hero al promocionar).
     */
    struct FTrunkDeformState
    {
        TArray<FTrunkDeformLayerState, TInlineAllocator<4>> Layers;

        /** Sin capas activas = no hay nada que hacer (camino rapido). */
        FORCEINLINE bool IsIdentity() const { return Layers.Num() == 0; }
    };

    /**
     * Resuelve que deformacion le toca a UN arbol, tirando por cada capa
     * declarada en el asset de especie.
     *
     * CONTRATO DE MUESTREO (no lo rompas al editar el asset): por cada capa se
     * extraen SIEMPRE 4 valores en orden fijo -tirada de activacion, angulo,
     * azimut, fase- y la puerta de probabilidad se aplica DESPUES. Asi, subir o
     * bajar la Probability de una capa no desplaza las muestras de las capas
     * siguientes: solo cambia cuantos arboles la reciben. Lo que SI cambia todo
     * es REORDENAR el array (o insertar una capa en medio); anadir al final es
     * seguro.
     *
     * @param DeformSeed  Semilla YA derivada (ver GrowTree / VariantDeformSeed).
     *                    Se consume una copia local: esta funcion no tiene
     *                    efectos sobre ningun stream del llamante.
     */
    PROCEDURALECOSYSTEM_API FTrunkDeformState Sample(const USpeciesData& Species, uint32 DeformSeed);

    /**
     * Cota superior del desplazamiento horizontal (cm) que la deformacion puede
     * meterle a un arbol de altura TotalHeightCm. La usa GrowTree para ensanchar
     * el padding de la rejilla de luz fina: la copa doblada muestrea AO y
     * heliotropismo en posiciones que la rejilla sin margen ya no cubriria (no
     * rompe -WorldToVoxelClamped clampa- pero el gradiente se aplana a cero
     * contra el borde, que se ve como hojas que dejan de orientarse).
     *
     * Se calcula desde los MAXIMOS del asset, no desde una tirada concreta: la
     * rejilla se dimensiona antes de saber que arbol va a salir.
     */
    PROCEDURALECOSYSTEM_API float MaxLateralReachCm(const USpeciesData& Species, float TotalHeightCm);

    /**
     * Doblega el esqueleto entero segun State. No-op si State.IsIdentity().
     *
     * ALGORITMO: una sola pasada hacia delante explotando la invariante
     * Parent < indice de FTreeSkeleton (ver su cabecera), que garantiza que el
     * padre ya esta recolocado cuando llega el turno del hijo:
     *
     *     t        = altura normalizada del PADRE en el arbol SIN deformar
     *     Bend(t)  = suma vectorial de las capas (eje horizontal x angulo)
     *     R        = rotacion de |Bend| radianes alrededor de Bend/|Bend|
     *     Pos[i]   = Pos[Parent] + R * (OldPos[i] - OldPos[Parent])
     *     Dir[i]   = R * Dir[i]
     *
     * Rotar el VECTOR AL PADRE (y no la posicion absoluta) es lo que hace que la
     * curvatura se ACUMULE a lo largo del fuste en vez de girar el arbol en
     * bloque, y a la vez conserva exactamente la longitud de cada internodo.
     *
     * OJO AL ORDEN: llamalo ANTES de ComputeRadii/ApplyTrunkProfile y antes de
     * mallar. Despues de mallar no serviria de nada, y entre radios y malla
     * dejaria las normales del tubo desalineadas con Dir.
     *
     * @param TotalHeightCm  Altura del arbol sin deformar (tronco + copa). Es el
     *                       divisor de la altura normalizada; se guarda contra 0.
     */
    PROCEDURALECOSYSTEM_API void ApplyToSkeleton(FTreeSkeleton& Skeleton, const FTrunkDeformState& State,
        const FVector& TrunkBaseWorld, float TotalHeightCm);
}
