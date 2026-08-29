/**
 * @file TreeMeshBaker.h
 * @author Juan Luque Roldán
 * @brief Horneado en runtime de los buffers de un árbol a malla estática, y su impostor.
 *
 * Es el paso que convierte la salida del mallador en un activo instanciable. La razón es de
 * API: un UProceduralMeshComponent no se puede instanciar, y un componente instanciado exige
 * un UStaticMesh; ahí está la diferencia entre 20.000 componentes y un puñado de draw calls.
 * El mallado procedural se reserva para los hero trees, que son decenas y tienen geometría
 * única. Declara además la construcción del impostor del campo lejano, dos tarjetas cruzadas
 * dimensionadas desde la caja de la malla que acaba de hornearse.
 *
 * @note La construcción rápida en runtime no puede generar datos de Nanite, que exigen un
 *       build con datos de editor. La librería horneada en vivo da instancing pero no Nanite;
 *       para tener ambos hay que hornear la librería a activos en el editor y activar allí el
 *       soporte de Nanite en cada malla.
 *
 * @ingroup eco_render
 * @see @ref bib_instancing
 * @see @ref bib_karis2021
 */

#pragma once

#include "CoreMinimal.h"
#include "Geometry/TreeMeshBuilder.h"

class UStaticMesh;
class UMaterialInterface;

/**
 * Conversión de los buffers neutros de un árbol en activos que un componente instanciado
 * puede dibujar: la malla estática de dos secciones y el impostor que la sustituye a
 * distancia. Sin estado: cada función construye un UStaticMesh y lo devuelve.
 */
namespace TreeMeshBaker
{
    /**
     * Construye una malla estática con dos secciones —0 madera, 1 follaje—, cada una con su
     * material, a partir de los buffers que deja el mallador. Cada sección va en su propio
     * grupo de polígonos para que la corteza pueda ser opaca y el follaje enmascarado y a dos
     * caras, que es la parte cara de dibujar.
     *
     * @param Outer          UObject propietario, que mantiene viva la malla frente al GC.
     * @param MeshData       Buffers de madera y hojas, con los vértices en coordenadas de mundo.
     * @param OriginWorld    Origen al que se referencia la malla, normalmente la base del
     *                       tronco; los vértices se pasan a local restándolo.
     * @param OutLocalBounds Caja envolvente local resultante; la consumen el impostor y las
     *                       estadísticas de la librería.
     * @return La malla, o nullptr si no había geometría o el build falló.
     */
    PROCEDURALECOSYSTEM_API UStaticMesh* BuildStaticMesh(
        UObject* Outer,
        const FTreeMeshData& MeshData,
        const FVector& OriginWorld,
        UMaterialInterface* BarkMaterial,
        UMaterialInterface* LeafMaterial,
        FBox& OutLocalBounds);

    /**
     * Impostor geométrico del campo lejano: dos tarjetas cruzadas, cuatro triángulos, del
     * tamaño de la copa y dimensionadas desde la caja local de la malla horneada. Sustituir
     * miles de tarjetas de hoja superpuestas por cuatro triángulos es de donde sale el
     * recorte de overdraw a distancia.
     *
     * Aquí solo se construye la geometría; la textura —el atlas del árbol visto desde fuera—
     * es un activo de arte que se hornea en el editor y se asigna en
     * USpeciesData::ImpostorMaterial.
     *
     * @param LocalBounds Caja local de la malla que sustituye; devuelve nullptr si no es válida.
     * @see @ref bib_impostores
     */
    PROCEDURALECOSYSTEM_API UStaticMesh* BuildImpostorMesh(
        UObject* Outer,
        const FBox& LocalBounds,
        UMaterialInterface* ImpostorMaterial);
}
