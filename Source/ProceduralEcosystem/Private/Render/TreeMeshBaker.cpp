/**
 * @file TreeMeshBaker.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del horneado a malla estática y del impostor de dos tarjetas.
 *
 * Contiene el volcado de una sección —madera o follaje— a FMeshDescription como grupo de
 * polígonos propio con sus cuatro canales de UV y su color de vértice, la construcción de la
 * malla estática de dos secciones con los parámetros de build que exige el runtime, y la
 * generación del impostor: dos tarjetas perpendiculares dimensionadas desde la caja local y
 * deliberadamente sin canales de viento, para que el mismo material no las mueva.
 *
 * @ingroup eco_render
 * @see @ref bib_impostores
 * @see @ref bib_pivotpainter
 */

#include "Render/TreeMeshBaker.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Materials/MaterialInterface.h"

namespace
{
    /** Canales de UV de la malla: uno de textura y tres de viento. */
    constexpr int32 kNumUVChannels = 4;

    /**
     * Vuelca una sección —madera u hojas— en el FMeshDescription como un grupo de polígonos
     * propio, que se convierte en una ranura de material propia y en una sección de malla
     * propia. Es lo que permite que la madera vaya opaca y el follaje enmascarado y a dos
     * caras, que es la parte cara de dibujar.
     *
     * Un vertex instance por vértice: los buffers del mallador ya duplican los vértices donde
     * los atributos difieren —costura del anillo, esquinas de hoja—, así que aquí no hay que
     * partir nada.
     *
     * Además de UV0 se copian UV1..UV3 —pivote de rama, nivel jerárquico, peso de balanceo y
     * desfase— y el color de vértice, que lleva la oclusión de copa y la variación de tinte.
     * Eso es lo que hace que una misma malla instanciada se mueva con el viento y tenga su
     * autosombra horneada, sin ningún dato extra por instancia. Si un buffer no trae esos
     * canales —el impostor, que no debe moverse— se escriben ceros y blanco, y el material
     * los interpreta como ausencia de balanceo.
     *
     * @param OriginWorld      Origen al que se referencian los vértices; se resta a cada uno.
     * @param SlotName         Nombre de la ranura de material del grupo de polígonos.
     * @param InOutLocalBounds Se amplía con cada vértice volcado.
     */
    void AppendSection(FMeshDescription& MeshDesc, FStaticMeshAttributes& Attributes,
        const FTreeMeshBuffers& Buffers, const FVector& OriginWorld, FName SlotName,
        FBox& InOutLocalBounds)
    {
        const FPolygonGroupID PolyGroup = MeshDesc.CreatePolygonGroup();
        Attributes.GetPolygonGroupMaterialSlotNames()[PolyGroup] = SlotName;

        TVertexAttributesRef<FVector3f>         Positions = Attributes.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float>     BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
        TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

        const int32 NumVerts = Buffers.Vertices.Num();
        MeshDesc.ReserveNewVertices(NumVerts);
        MeshDesc.ReserveNewVertexInstances(NumVerts);
        MeshDesc.ReserveNewTriangles(Buffers.Triangles.Num() / 3);

        TArray<FVertexInstanceID> Instances;
        Instances.SetNumUninitialized(NumVerts);

        for (int32 i = 0; i < NumVerts; ++i)
        {
            const FVector Local = Buffers.Vertices[i] - OriginWorld;

            const FVertexID V = MeshDesc.CreateVertex();
            Positions[V] = FVector3f(Local);
            InOutLocalBounds += Local;

            const FVertexInstanceID VI = MeshDesc.CreateVertexInstance(V);
            Instances[i] = VI;

            Normals[VI] = Buffers.Normals.IsValidIndex(i)
                ? FVector3f(Buffers.Normals[i]) : FVector3f(0.f, 0.f, 1.f);
            Tangents[VI] = Buffers.Tangents.IsValidIndex(i)
                ? FVector3f(Buffers.Tangents[i]) : FVector3f(1.f, 0.f, 0.f);
            BinormalSigns[VI] = 1.f;

            UVs.Set(VI, 0, Buffers.UVs.IsValidIndex(i)
                ? FVector2f(Buffers.UVs[i]) : FVector2f::ZeroVector);

            // --- Canales de viento; el contrato lo fija Geometry/TreeMeshBuilder.h ---
            UVs.Set(VI, 1, Buffers.UV1.IsValidIndex(i)
                ? FVector2f(Buffers.UV1[i]) : FVector2f::ZeroVector);
            UVs.Set(VI, 2, Buffers.UV2.IsValidIndex(i)
                ? FVector2f(Buffers.UV2[i]) : FVector2f::ZeroVector);
            UVs.Set(VI, 3, Buffers.UV3.IsValidIndex(i)
                ? FVector2f(Buffers.UV3[i]) : FVector2f::ZeroVector);

            // --- Oclusión ambiental de copa y variación de tinte, en el color de vértice ---
            Colors[VI] = Buffers.Colors.IsValidIndex(i)
                ? FVector4f(Buffers.Colors[i].R, Buffers.Colors[i].G, Buffers.Colors[i].B, Buffers.Colors[i].A)
                : FVector4f(1.f, 1.f, 1.f, 1.f);
        }

        for (int32 t = 0; t + 2 < Buffers.Triangles.Num(); t += 3)
        {
            const int32 I0 = Buffers.Triangles[t];
            const int32 I1 = Buffers.Triangles[t + 1];
            const int32 I2 = Buffers.Triangles[t + 2];

            if (!Instances.IsValidIndex(I0) || !Instances.IsValidIndex(I1) || !Instances.IsValidIndex(I2))
            {
                continue;
            }
            if (I0 == I1 || I1 == I2 || I0 == I2)
            {
                continue; // degenerado: FMeshDescription lo rechazaría con un check
            }

            MeshDesc.CreateTriangle(PolyGroup, TArray<FVertexInstanceID>{ Instances[I0], Instances[I1], Instances[I2] });
        }
    }
}

namespace TreeMeshBaker
{
    UStaticMesh* BuildStaticMesh(UObject* Outer, const FTreeMeshData& MeshData, const FVector& OriginWorld,
        UMaterialInterface* BarkMaterial, UMaterialInterface* LeafMaterial, FBox& OutLocalBounds)
    {
        const bool bHasWood = !MeshData.Wood.IsEmpty();
        const bool bHasLeaves = !MeshData.Leaves.IsEmpty();
        if (!bHasWood && !bHasLeaves)
        {
            return nullptr;
        }

        FMeshDescription MeshDesc;
        FStaticMeshAttributes Attributes(MeshDesc);
        Attributes.Register();
        // UV0 es la textura; UV1..UV3 llevan el pivote de rama, el nivel jerárquico, el peso
        // de balanceo y el desfase, es decir la jerarquía de viento sin texturas de pivotes.
        Attributes.GetVertexInstanceUVs().SetNumChannels(kNumUVChannels);

        OutLocalBounds.Init(); // caja realmente vacía (IsValid = 0)

        // El orden de los grupos de polígonos tiene que casar con el de las ranuras de
        // material: grupo 0 con material 0, grupo 1 con material 1.
        TArray<FStaticMaterial> Materials;
        if (bHasWood)
        {
            AppendSection(MeshDesc, Attributes, MeshData.Wood, OriginWorld, TEXT("Bark"), OutLocalBounds);
            Materials.Add(FStaticMaterial(BarkMaterial, TEXT("Bark"), TEXT("Bark")));
        }
        if (bHasLeaves)
        {
            AppendSection(MeshDesc, Attributes, MeshData.Leaves, OriginWorld, TEXT("Leaf"), OutLocalBounds);
            Materials.Add(FStaticMaterial(LeafMaterial, TEXT("Leaf"), TEXT("Leaf")));
        }

        UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer ? Outer : GetTransientPackage(), NAME_None, RF_Transient);
        if (!Mesh)
        {
            return nullptr;
        }
        Mesh->SetStaticMaterials(Materials);

        UStaticMesh::FBuildMeshDescriptionsParams Params;
        Params.bBuildSimpleCollision = false;  // el bosque no colisiona: 20k cuerpos son inviables
        Params.bCommitMeshDescription = false; // en runtime la descripción no se conserva
        Params.bFastBuild = true;              // obligatorio fuera del editor; excluye Nanite
        Params.bMarkPackageDirty = false;

        if (!Mesh->BuildFromMeshDescriptions({ &MeshDesc }, Params))
        {
            return nullptr;
        }

        // El material de viento desplaza vértices, así que la caja envolvente geométrica de
        // la malla se queda corta y el culling puede descartar un árbol cuyas ramas todavía
        // asoman en pantalla. El margen no se pone aquí sino en el componente, con
        // SetBoundsScale: es una llamada por componente en vez de por malla, y sirve igual
        // para el mallado procedural de un hero tree.
        // Ver UTreeLibrary::GetOrCreateComponent y AHeroTreeActor.

        return Mesh;
    }

    // Es un crossboard fijo de dos tarjetas perpendiculares, no un impostor octaédrico ni un
    // billboard que encare a la cámara: con una textura de follaje enmascarada basta y es lo
    // más barato. Un impostor octaédrico exigiría una tarjeta orientada a cámara y un
    // material con UV dependientes de la vista, que esta geometría no sirve.
    //
    // El impostor no recibe canales de viento deliberadamente: al no traer UV1..UV3 se
    // escriben ceros, el peso de balanceo queda a 0 y el mismo material no produce ningún
    // desplazamiento aunque se reutilice. Es lo que deja estático el campo lejano.
    UStaticMesh* BuildImpostorMesh(UObject* Outer, const FBox& LocalBounds, UMaterialInterface* ImpostorMaterial)
    {
        if (!LocalBounds.IsValid)
        {
            return nullptr;
        }

        const FVector Center = LocalBounds.GetCenter();
        const FVector Extent = LocalBounds.GetExtent();
        const float HalfWidth = FMath::Max(static_cast<float>(FMath::Max(Extent.X, Extent.Y)), 1.f);
        const float Z0 = static_cast<float>(LocalBounds.Min.Z);
        const float Z1 = static_cast<float>(LocalBounds.Max.Z);

        FTreeMeshBuffers Cards;

        auto AddCard = [&Cards, Center, HalfWidth, Z0, Z1](const FVector& Right, const FVector& Normal)
            {
                const int32 Base = Cards.Vertices.Num();
                const FVector CenterXY(Center.X, Center.Y, 0.0);

                Cards.Vertices.Add(CenterXY + FVector(0, 0, Z0) - Right * HalfWidth);
                Cards.Vertices.Add(CenterXY + FVector(0, 0, Z0) + Right * HalfWidth);
                Cards.Vertices.Add(CenterXY + FVector(0, 0, Z1) + Right * HalfWidth);
                Cards.Vertices.Add(CenterXY + FVector(0, 0, Z1) - Right * HalfWidth);

                for (int32 i = 0; i < 4; ++i)
                {
                    Cards.Normals.Add(Normal);
                    Cards.Tangents.Add(Right);
                    // Blanco: sin oclusión horneada, el impostor ya es una silueta plana.
                    Cards.Colors.Add(FLinearColor::White);
                }

                // v = 0 arriba, el convenio de UV del motor para una textura de árbol.
                Cards.UVs.Add(FVector2D(0.f, 1.f));
                Cards.UVs.Add(FVector2D(1.f, 1.f));
                Cards.UVs.Add(FVector2D(1.f, 0.f));
                Cards.UVs.Add(FVector2D(0.f, 0.f));

                Cards.Triangles.Add(Base + 0); Cards.Triangles.Add(Base + 1); Cards.Triangles.Add(Base + 2);
                Cards.Triangles.Add(Base + 0); Cards.Triangles.Add(Base + 2); Cards.Triangles.Add(Base + 3);
            };

        AddCard(FVector::RightVector, FVector::ForwardVector); // tarjeta en el plano XZ
        AddCard(FVector::ForwardVector, FVector::RightVector); // tarjeta en el plano YZ

        // Se hornea como sección de follaje para que tome el material de impostor enmascarado.
        FTreeMeshData Data;
        Data.Leaves = MoveTemp(Cards);

        FBox Ignored;
        return BuildStaticMesh(Outer, Data, FVector::ZeroVector, nullptr, ImpostorMaterial, Ignored);
    }
}
