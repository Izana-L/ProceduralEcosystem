#pragma once

#include "CoreMinimal.h"
#include "Geometry/TreeMeshBuilder.h"

class UStaticMesh;
class UMaterialInterface;

/**
 * Horneado de FTreeMeshData -> UStaticMesh en RUNTIME (doc. Fase 3, 3.7 "API de
 * UE": "para la libreria de la Fase 4, construir UStaticMesh en runtime via
 * FMeshDescription y hornearlos: asi son Nanite-ables e instanciables").
 *
 * POR QUE UStaticMesh Y NO UProceduralMeshComponent: un PMC no se puede
 * instanciar. Un ISM/HISM necesita un UStaticMesh, y es lo que convierte 20.000
 * arboles en ~120 draw calls (uno por arquetipo) en vez de 20.000 componentes.
 * El PMC se queda para los hero trees (Fase 3), que son decenas y tienen malla
 * unica.
 *
 * NANITE: la construccion rapida en runtime (bFastBuild) no puede generar los
 * datos de Nanite, que exigen build offline con datos de editor. Es decir:
 *   - Con la libreria horneada en runtime tienes instancing (la victoria gorda)
 *     pero no Nanite.
 *   - Para tener Nanite hay que hornear la libreria a assets .uasset una vez
 *     en el editor y activar "Enable Nanite Support" en cada malla (ver la guia
 *     de la fase, paso de configuracion del editor).
 * Es exactamente el benchmark que pide el doc. 4.6 (Nanite vs LOD tradicional
 * con follaje masked): con las dos vias montadas se puede medir y decidir.
 */
namespace TreeMeshBaker
{
    /**
     * Construye un UStaticMesh con DOS secciones (0 = madera, 1 = follaje), cada
     * una con su material, a partir de la salida del mallador de la Fase 3.
     *
     * @param Outer          Propietario UObject (mantiene viva la malla frente al GC).
     * @param MeshData       Buffers de madera y hojas (vertices en MUNDO, tal y como los deja la Fase 3).
     * @param OriginWorld    Origen al que se referencia la malla (la base del tronco); los vertices se pasan a local restandolo.
     * @param OutLocalBounds Caja envolvente local resultante (la usa el impostor y las estadisticas).
     * @return La malla, o nullptr si no habia geometria.
     */
    PROCEDURALECOSYSTEM_API UStaticMesh* BuildStaticMesh(
        UObject* Outer,
        const FTreeMeshData& MeshData,
        const FVector& OriginWorld,
        UMaterialInterface* BarkMaterial,
        UMaterialInterface* LeafMaterial,
        FBox& OutLocalBounds);

    /**
     * Impostor geometrico del campo lejano (doc. 4.1/4.6): dos tarjetas cruzadas
     * (4 triangulos) del tamano de la copa. Sustituye miles de leaf cards
     * translucidas superpuestas por 4 triangulos, que es de donde sale el
     * recorte brutal de overdraw del que habla el 4.6.
     *
     * La geometria la pone este codigo; la TEXTURA (el atlas del arbol visto
     * desde fuera) es un asset de arte: se hornea en el editor con el plugin
     * Impostor Baker de Epic y se asigna en USpeciesData::ImpostorMaterial.
     */
    PROCEDURALECOSYSTEM_API UStaticMesh* BuildImpostorMesh(
        UObject* Outer,
        const FBox& LocalBounds,
        UMaterialInterface* ImpostorMaterial);
}
