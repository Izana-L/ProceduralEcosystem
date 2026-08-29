/**
 * @file TrunkDeformer.h
 * @author Juan Luque Roldán
 * @brief Deformación de tronco por árbol —inclinado, arqueado o sinuoso— aplicada sobre
 *        el esqueleto ya crecido.
 *
 * Declara el namespace TrunkDeformer, que da a cada individuo su curvatura propia. La
 * especie declara capas de deformación con su probabilidad y su rango de ángulos; cada
 * árbol tira los dados una sola vez (@ref TrunkDeformer::Sample) y el resultado dobla el
 * esqueleto entero, tronco y copa, como se dobla una vara
 * (@ref TrunkDeformer::ApplyToSkeleton). El doblado es un operador global parametrizado
 * por la altura normalizada, realizado como re-encadenado ISOMÉTRICO: se rota el vector
 * que va del padre al hijo, no la posición absoluta, con lo que la curvatura se acumula a
 * lo largo del fuste y ninguna longitud de internodo cambia. Esa isometría es lo que
 * permite que los pasos siguientes sigan siendo válidos sin una línea extra —el pipe
 * model es topológico y el perfil de tronco trabaja sobre longitud de arco— y que los
 * datos de viento y el follaje, construidos después, salgan coherentes solos.
 *
 * @ingroup eco_geometry
 * @see @ref bib_barr1984
 */

#pragma once

#include "CoreMinimal.h"

class USpeciesData;
struct FTreeSkeleton;

/**
 * @brief Curvatura de tronco por individuo: sorteo de las capas y doblado del esqueleto.
 *
 * Es un paso propio y posterior al crecimiento, no un aumento de la sinuosidad del eje.
 * El eje ya lleva una desviación sutil por especie, pero esa curva está atrapada en el
 * bucle que lo encadena, que avanza @f$D\cos\theta@f$ en Z y también termina por Z: subir
 * su amplitud da un eje que avanza cada vez menos contra su tope de inclinación, no un
 * árbol arqueado, y de todos modos solo doblaría el eje, dejando las ramas ya colgadas
 * donde estaban.
 *
 * @note El sorteo consume un sub-stream derivado por hash de la semilla del árbol, nunca
 *       el stream principal de la generación: activar la deformación no desplaza ningún
 *       valor del resto del proceso, y un árbol sin capas activas sale bit a bit idéntico.
 */
namespace TrunkDeformer
{
    /**
     * Tope del doblado ACUMULADO de todas las capas a cualquier altura (~57 grados).
     *
     * Es un límite de lectura, no numérico: por encima el árbol deja de leerse como
     * arqueado y pasa a caído, y las ramas del lado interior empiezan a interpenetrarse.
     */
    constexpr float MaxTrunkBendRad = 1.0f;

    /** Una capa ya resuelta para un árbol concreto: la tirada salió a favor y sus cuatro
        parámetros quedan fijados. */
    struct FTrunkDeformLayerState
    {
        /** Copia de ETrunkDeformType como uint8 plano, para que esta cabecera no dependa
            del asset de especie. */
        uint8 Type = 0;
        /** Ángulo máximo de la capa, en radianes. */
        float AngleRad = 0.f;
        /** Azimut hacia el que se inclina el árbol, en radianes. */
        float AzimuthRad = 0.f;
        /** Fase de la onda; solo la usa SCurve. */
        float Phase = 0.f;
        /** Exponente del arqueo (Arc) o número de ondas (SCurve). */
        float Param = 1.f;
    };

    /**
     * Deformación completa de UN árbol: las capas que le han tocado.
     *
     * @note El allocator en línea evita la reserva dinámica en el caso normal, de cero a
     *       dos capas activas; esto se construye una vez por árbol generado.
     */
    struct FTrunkDeformState
    {
        TArray<FTrunkDeformLayerState, TInlineAllocator<4>> Layers;

        /** Sin capas activas no hay nada que hacer: camino rápido de ApplyToSkeleton. */
        FORCEINLINE bool IsIdentity() const { return Layers.Num() == 0; }
    };

    /**
     * Resuelve qué deformación le toca a UN árbol, tirando por cada capa declarada en el
     * asset de especie.
     *
     * CONTRATO DE MUESTREO: por cada capa se extraen SIEMPRE cuatro valores en orden fijo
     * —tirada de activación, ángulo, azimut y fase— y la puerta de probabilidad se aplica
     * DESPUÉS. Así, subir o bajar la probabilidad de una capa no desplaza las muestras de
     * las capas siguientes: solo cambia cuántos árboles la reciben.
     *
     * @param DeformSeed Semilla ya derivada de la del árbol. Se consume una copia local:
     *                   la función no tiene efectos sobre ningún stream del llamante.
     * @return Las capas activas de este árbol; vacío si no le ha tocado ninguna.
     * @warning Reordenar el array de capas del asset, o insertar una en medio, cambia la
     *          deformación de todos los árboles de la especie; añadir al final no.
     */
    PROCEDURALECOSYSTEM_API FTrunkDeformState Sample(const USpeciesData& Species, uint32 DeformSeed);

    /**
     * Cota superior del desplazamiento horizontal (cm) que la deformación puede meterle a
     * un árbol de altura TotalHeightCm.
     *
     * La consume el generador para ensanchar el margen de la rejilla de luz fina: la copa
     * doblada muestrea oclusión y heliotropismo en posiciones que una rejilla ajustada al
     * árbol recto ya no cubriría. No rompe nada, porque la conversión a vóxel clampa, pero
     * el gradiente se aplana contra el borde y las hojas dejan de orientarse.
     *
     * @return @f$H\sin(\min(\sum\theta_{max},\ \text{MaxTrunkBendRad}))@f$, es decir el
     *         peor caso con todas las capas activas al máximo y hacia el mismo azimut.
     * @note Se calcula desde los máximos del asset y no desde una tirada concreta: la
     *       rejilla se dimensiona antes de saber qué árbol va a salir.
     */
    PROCEDURALECOSYSTEM_API float MaxLateralReachCm(const USpeciesData& Species, float TotalHeightCm);

    /**
     * Dobla el esqueleto entero según State. No hace nada si State.IsIdentity().
     *
     * Una sola pasada en índice creciente, apoyada en la invariante `Parent` < índice de
     * FTreeSkeleton, que garantiza que el padre ya está recolocado cuando llega el turno
     * del hijo:
     *
     * @code
     * t       = altura normalizada del PADRE en el árbol SIN deformar
     * Bend(t) = suma vectorial de las capas (eje horizontal x ángulo)
     * R       = rotación de |Bend| radianes alrededor de Bend/|Bend|
     * Pos[i]  = Pos[Parent] + R * (OldPos[i] - OldPos[Parent])
     * Dir[i]  = R * Dir[i]
     * @endcode
     *
     * Rotar el vector al padre, y no la posición absoluta, es lo que hace que la curvatura
     * se acumule a lo largo del fuste en vez de girar el árbol en bloque, y a la vez
     * conserva exactamente la longitud de cada internodo.
     *
     * @param TrunkBaseWorld Base del tronco en mundo. Su Z es el origen de la altura
     *                       normalizada, y la raíz es el único nodo que no se mueve.
     * @param TotalHeightCm  Altura del árbol sin deformar (tronco más copa). Es el divisor
     *                       de la altura normalizada; se guarda contra 0.
     * @pre Se llama tras el crecimiento y antes de los radios del pipe model, del perfil
     *      de tronco y del mallado. Aplicarlo entre los radios y la malla dejaría las
     *      normales del tubo desalineadas con Dir.
     */
    PROCEDURALECOSYSTEM_API void ApplyToSkeleton(FTreeSkeleton& Skeleton, const FTrunkDeformState& State,
        const FVector& TrunkBaseWorld, float TotalHeightCm);
}
