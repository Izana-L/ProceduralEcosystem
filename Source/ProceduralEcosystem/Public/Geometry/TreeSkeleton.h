/**
 * @file TreeSkeleton.h
 * @author Juan Luque Roldán
 * @brief Esqueleto de ramas de un árbol: el nodo plano con padre por índice y su
 *        contenedor.
 *
 * Declara FBranchNode y FTreeSkeleton, la estructura que produce la colonización del
 * espacio y consumen después el deformador de tronco, el mallador, los datos de viento y
 * el follaje. Un árbol es aquí un array plano de nodos que apuntan a su padre por
 * índice, no un grafo de punteros, y su invariante de construcción —el padre ocupa
 * siempre un índice menor que el hijo— permite recorrer la topología entera en una sola
 * pasada, hacia delante o hacia atrás, sin recursión ni ordenación previa. Fija además
 * la distinción entre el radio estructural que deja el pipe model y el radio que
 * realmente se malla.
 *
 * @ingroup eco_geometry
 */

#pragma once

#include "CoreMinimal.h"

/**
 * Bits de FBranchNode::Flags. Caben en el hueco de alineación que el struct ya tenía,
 * así que marcar el eje principal no cuesta memoria.
 */
enum EBranchNodeFlag : uint8
{
    BNF_None = 0,

    /** El nodo forma parte del EJE PRINCIPAL (tronco más el líder que atraviesa la
        copa). Lo marca la colonización del espacio al pre-construir el eje, y lo
        consumen el perfil de tronco —que solo afila y ensancha el eje— y los datos de
        viento, donde el eje continúa siempre la misma rama al bifurcar. */
    BNF_Axis = 1 << 0
};

/**
 * Un nodo del esqueleto de ramas: el extremo de un internodo.
 *
 * El esqueleto es una lista de nodos que apuntan al PADRE por índice, no un árbol de
 * punteros: así se reconstruyen las ramas y, sobre todo, se calculan los radios del pipe
 * model recorriendo de las puntas a la base sin punteros ni recursión.
 *
 * Struct plano, no USTRUCT: es dato caliente —de cientos a pocos miles por árbol, y el
 * mallador los recorre enteros—, no necesita reflexión ni exponerse a Blueprint.
 */
struct FBranchNode
{
    /** Posición de mundo del extremo del internodo (cm). */
    FVector Pos = FVector::ZeroVector;

    /** Índice del nodo padre en FTreeSkeleton::Nodes; -1 = raíz (base del tronco). */
    int32 Parent = -1;

    /** Orden de rama: 0 = tronco, aumenta al alejarse de la raíz. */
    int32 Depth = 0;

    /** Dirección del internodo (unitaria). Aporta el término de inercia a la mezcla de
        direcciones de crecimiento y orienta el anillo de sección al mallar. */
    FVector Dir = FVector::UpVector;

    /** Radio mallado de la rama en este nodo (cm): el que ve el tubo, con el perfil de
        tronco ya aplicado encima. Lo rellena el pipe model al terminar el crecimiento y
        lo reescribe después @ref SpaceColonization::ApplyTrunkProfile; vale 0 hasta
        entonces. */
    float Radius = 0.f;

    /**
     * Radio ESTRUCTURAL del pipe model, sin el ensanche de pie ni el afilado que el
     * perfil de tronco aplica después sobre Radius.
     *
     * Existe porque hay consumidores que preguntan cuán gruesa es una rama para deducir
     * RIGIDEZ, no para dibujarla: los datos de viento normalizan el balanceo contra el
     * radio de la base, y si esa referencia llevase el ensanche de raíz —que puede
     * duplicar el radio del pie— el árbol entero pasaría a considerarse fino y se
     * balancearía de más. La geometría usa Radius; la respuesta al viento, PipeRadius.
     */
    float PipeRadius = 0.f;

    /** Bits de EBranchNodeFlag (BNF_Axis). */
    uint8 Flags = 0;

    /** El nodo pertenece al eje principal (tronco más líder). */
    FORCEINLINE bool IsAxis() const { return (Flags & BNF_Axis) != 0; }

    /**
     * Radio estructural con fallback a Radius.
     *
     * @note Los esqueletos construidos a mano (pruebas, utilidades) rellenan Radius y no
     *       PipeRadius; sin el fallback verían grosor 0 y las fórmulas que dividen por el
     *       radio de la base darían resultados absurdos.
     */
    FORCEINLINE float GetPipeRadius() const { return (PipeRadius > 0.f) ? PipeRadius : Radius; }
};

/**
 * Esqueleto de UN árbol: contenedor PASIVO de nodos.
 *
 * Es la salida de la colonización del espacio (@ref SpaceColonization::GrowTree) y la
 * entrada del mallador. Aquí solo hay datos y las operaciones básicas de gestión del
 * array: no siembra atractores, no conoce la luz ni los tropismos; eso vive en el motor
 * de crecimiento.
 *
 * INVARIANTE DE CONSTRUCCIÓN: un hijo se añade siempre después de su padre, luego
 * `Parent` < índice del hijo y `Depth` no decrece con el índice. Recorrer en índice
 * DECRECIENTE visita cada hijo antes que su padre, que es lo que piden el pipe model y
 * la pasada de monotonía del perfil de tronco; recorrer en índice CRECIENTE garantiza
 * que el padre ya está resuelto, que es lo que piden el deformador de tronco, los marcos
 * de rotación mínima del mallador y la detección de ramas. Ningún consumidor ordena ni
 * recurre.
 */
struct PROCEDURALECOSYSTEM_API FTreeSkeleton
{
    /** Nodos del árbol. El índice 0 es la raíz tras InitRoot(). */
    TArray<FBranchNode> Nodes;

    int32 Num() const { return Nodes.Num(); }

    /** Vacía el esqueleto sin liberar la capacidad reservada. */
    void Reset();

    /** Reserva espacio para evitar realojos durante el crecimiento. */
    void Reserve(int32 ExpectedNodes);

    /**
     * Reinicia el esqueleto y crea el nodo raíz en la base del tronco, marcado como eje.
     *
     * @param InitialDir Dirección de arranque del tronco, normalmente hacia arriba.
     * @return El índice de la raíz, siempre 0.
     */
    int32 InitRoot(const FVector& TrunkBaseWorld, const FVector& InitialDir = FVector::UpVector);

    /**
     * Añade un hijo de ParentIndex, derivando `Depth` del padre y preservando la
     * invariante `Parent` < índice.
     *
     * @param Dir     Dirección del nuevo internodo; debe venir normalizada.
     * @param InFlags Bits de EBranchNodeFlag; BNF_Axis al encadenar el eje principal.
     * @return El índice del nuevo nodo, o INDEX_NONE si ParentIndex no es válido.
     */
    int32 AddChild(int32 ParentIndex, const FVector& Pos, const FVector& Dir, uint8 InFlags = BNF_None);

    /**
     * Número de hijos de cada nodo, en un array de Num() elementos.
     *
     * Copia única de la pasada que necesitan tanto el mallador —los nodos sin hijos son
     * las puntas que rematan en ápice— como los datos de viento, donde un nodo abre rama
     * nueva si su padre bifurcó.
     */
    void ComputeChildCounts(TArray<int32>& OutChildCount) const;

    /**
     * Longitud de arco acumulada desde la raíz hasta cada nodo (cm), siguiendo la cadena
     * de padres en una sola pasada gracias a la invariante `Parent` < índice.
     *
     * La comparten el mallador (coordenada v de la UV de corteza), los datos de viento
     * (posición a lo largo del árbol, que gradúa el peso de balanceo) y el follaje
     * (ranuras de la espiral filotáctica).
     */
    void ComputeAlongLengths(TArray<float>& OutAlongLen) const;
};