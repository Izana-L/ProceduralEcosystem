#pragma once

#include "CoreMinimal.h"

/**
 * Bits de FBranchNode::Flags. Caben en el padding que el struct ya tenia, asi
 * que marcar el eje principal no cuesta memoria (ver la nota de tamano abajo).
 */
enum EBranchNodeFlag : uint8
{
    BNF_None = 0,

    /** El nodo forma parte del EJE PRINCIPAL (tronco + lider que atraviesa la
        copa). Lo marca el SCA al pre-construir el eje y lo consumen el perfil
        de tronco (que solo afila y ensancha el eje) y los datos de viento (el
        eje continua SIEMPRE la misma rama al bifurcar, ver TreeWindData). */
    BNF_Axis = 1 << 0
};

/**
 * Un nodo del esqueleto de ramas de un arbol (doc. Fase 3, 3.2).
 *
 * El esqueleto es una lista de nodos con puntero al PADRE (indice), no un
 * arbol de punteros: asi se reconstruyen las ramas y, sobre todo, se calculan
 * los radios del pipe model recorriendo de las puntas a la base sin punteros
 * ni recursion.
 *
 * Struct PLANO (no USTRUCT): es dato caliente: el SCA genera cientos a pocos
 * miles por arbol y el mallador los recorre entero. No necesita reflexion ni
 * exponerse a Blueprint.
 */
struct FBranchNode
{
    /** Posicion de mundo del extremo del internodo (cm). */
    FVector Pos = FVector::ZeroVector;

    /** Indice del nodo padre en FTreeSkeleton::Nodes; -1 = raiz (base del tronco). */
    int32 Parent = -1;

    /** Orden de rama: 0 = tronco, aumenta al alejarse de la raiz. */
    int32 Depth = 0;

    /** Direccion del internodo (unitaria). Da la INERCIA (wPrev) a la siguiente
        iteracion del SCA y orienta el anillo de seccion al mallar. */
    FVector Dir = FVector::UpVector;

    /** Radio de la rama en este nodo (cm). Se rellena al final con el pipe
        model (doc. 3.6); vale 0 hasta entonces. Es el radio que MALLA el
        tubo, o sea el que ya lleva encima el perfil de tronco. */
    float Radius = 0.f;

    /**
     * Radio ESTRUCTURAL del pipe model, sin el perfil de tronco (ensanche de
     * base y taper) que ApplyTrunkProfile aplica despues sobre Radius.
     *
     * Existe porque hay consumidores que preguntan "que tan gruesa es esta
     * rama" para deducir RIGIDEZ, no para dibujarla: FTreeWindData normaliza
     * el balanceo contra el radio de la base, y si esa referencia llevase el
     * ensanche de raiz -que puede duplicar el radio del pie- todo el arbol
     * pasaria a considerarse "fino" y se balancearia de mas. La geometria usa
     * Radius; la fisica del viento, PipeRadius.
     */
    float PipeRadius = 0.f;

    /** Bits de EBranchNodeFlag (BNF_Axis). */
    uint8 Flags = 0;

    /** El nodo pertenece al eje principal (tronco + lider). */
    FORCEINLINE bool IsAxis() const { return (Flags & BNF_Axis) != 0; }

    /**
     * Radio estructural con FALLBACK a Radius. Los esqueletos construidos a
     * mano (tests, utilidades) rellenan Radius y no PipeRadius; sin el
     * fallback verian grosor 0 y las formulas que dividen por el radio de la
     * base darian resultados absurdos.
     */
    FORCEINLINE float GetPipeRadius() const { return (PipeRadius > 0.f) ? PipeRadius : Radius; }
};

/**
 * Esqueleto de UN arbol: contenedor PASIVO de nodos (doc. Fase 3, 3.2).
 *
 * Es la salida de la colonizacion del espacio (clase SpaceColonization) y la
 * entrada del mallador (clase TreeMeshBuilder). Igual que FTreePopulation en
 * la Fase 2, aqui SOLO hay datos y operaciones basicas de gestion del array:
 * ni siembra atractores, ni conoce la luz, ni tropismos. Eso vive en el motor
 * del SCA.
 *
 * INVARIANTE DE CONSTRUCCION (la garantiza el SCA y de ella dependen otros
 * pasos): un hijo se anade SIEMPRE despues de su padre, luego
 *   Parent < indice-del-hijo    y    Depth es no-decreciente con el indice.
 * Consecuencia util: recorrer los nodos en orden de indice DECRECIENTE visita
 * cada hijo antes que su padre, que es justo lo que pide el pipe model
 * (doc. 3.6, "NodesByDecreasingDepth") sin necesidad de ordenar nada.
 */
struct PROCEDURALECOSYSTEM_API FTreeSkeleton
{
    /** Nodos del arbol. El indice 0 es la raiz tras InitRoot(). */
    TArray<FBranchNode> Nodes;

    int32 Num() const { return Nodes.Num(); }

    /** Vacia el esqueleto sin liberar la capacidad reservada. */
    void Reset();

    /** Reserva espacio para evitar realojos durante el crecimiento. */
    void Reserve(int32 ExpectedNodes);

    /**
     * Reinicia el esqueleto y crea el nodo raiz en la base del tronco.
     * Devuelve su indice (siempre 0). InitialDir es la direccion de arranque
     * del tronco (normalmente hacia arriba).
     */
    int32 InitRoot(const FVector& TrunkBaseWorld, const FVector& InitialDir = FVector::UpVector);

    /**
     * Anade un hijo de ParentIndex en Pos con direccion Dir (debe venir
     * normalizada). Deriva Depth = Padre.Depth + 1 y preserva la invariante
     * Parent < indice. Devuelve el indice del nuevo nodo.
     *
     * InFlags son bits de EBranchNodeFlag; pasa BNF_Axis al encadenar el eje
     * principal. Por defecto 0 (rama normal), asi que las llamadas existentes
     * no cambian de significado.
     */
    int32 AddChild(int32 ParentIndex, const FVector& Pos, const FVector& Dir, uint8 InFlags = BNF_None);

    /**
     * Nº de hijos de cada nodo. UNICA copia de la pasada que necesitan tanto el
     * mallador (nodos terminales = puntas con hoja) como los datos de viento
     * (un nodo abre rama nueva si su padre bifurco). Deja OutChildCount con
     * Num() elementos.
     */
    void ComputeChildCounts(TArray<int32>& OutChildCount) const;

    /**
     * Longitud acumulada desde la raiz hasta cada nodo (cm), siguiendo la
     * cadena de padres. Explota la invariante Parent < indice (una pasada, sin
     * recursion). La comparten el mallador (UV.v de la corteza) y los datos de
     * viento (posicion a lo largo del arbol para el peso de balanceo).
     */
    void ComputeAlongLengths(TArray<float>& OutAlongLen) const;
};