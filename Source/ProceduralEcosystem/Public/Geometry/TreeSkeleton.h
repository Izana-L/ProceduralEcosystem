#pragma once

#include "CoreMinimal.h"

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
        model (doc. 3.6); vale 0 hasta entonces. */
    float Radius = 0.f;
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
     */
    int32 AddChild(int32 ParentIndex, const FVector& Pos, const FVector& Dir);

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