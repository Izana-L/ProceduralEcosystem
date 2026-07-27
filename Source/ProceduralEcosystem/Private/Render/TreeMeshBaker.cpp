#include "Render/TreeMeshBaker.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Materials/MaterialInterface.h"

namespace
{
    /**
     * Vuelca una seccion (madera u hojas) en el FMeshDescription como un
     * polygon group propio -> una slot de material propia -> una seccion de
     * malla propia. Es lo que permite que la madera vaya con corteza opaca y el
     * follaje con material masked/two-sided, que es lo caro (doc. 4.6).
     *
     * Un vertex instance por vertice: los buffers de la Fase 3 ya duplican los
     * vertices donde los atributos difieren (costura del anillo, esquinas de
     * hoja), asi que no hay que partir nada aqui.
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
            Colors[VI] = FVector4f(1.f, 1.f, 1.f, 1.f);
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
                continue; // degenerado: FMeshDescription lo rechazaria con un check
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
        Attributes.GetVertexInstanceUVs().SetNumChannels(1);

        OutLocalBounds.Init(); // caja realmente vacia (IsValid = 0)

        // El ORDEN de los polygon groups tiene que casar con el de las slots de
        // material: grupo 0 -> material 0, grupo 1 -> material 1.
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
        Params.bBuildSimpleCollision = false; // el bosque no colisiona: 20k cuerpos serian inasumibles
        Params.bCommitMeshDescription = false; // runtime: no guardamos la descripcion (memoria)
        Params.bFastBuild = true;             // OBLIGATORIO fuera del editor
        Params.bMarkPackageDirty = false;

        if (!Mesh->BuildFromMeshDescriptions({ &MeshDesc }, Params))
        {
            return nullptr;
        }

        return Mesh;
    }
    // NOTA: esto es un CROSSBOARD fijo (2 tarjetas perpendiculares), no un
    // impostor octaedrico ni billboard que encare camara. Es suficiente con una
    // textura de follaje masked y es lo mas barato. Si en el pulido quieres el
    // octaedrico del doc, hace falta UNA card orientada a camara + material con
    // UV dependientes de la vista (Impostor Baker); esta geometria NO lo sirve.
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
            }

            // v = 0 arriba (convenio de UV de UE para una textura de arbol).
            Cards.UVs.Add(FVector2D(0.f, 1.f));
            Cards.UVs.Add(FVector2D(1.f, 1.f));
            Cards.UVs.Add(FVector2D(1.f, 0.f));
            Cards.UVs.Add(FVector2D(0.f, 0.f));

            Cards.Triangles.Add(Base + 0); Cards.Triangles.Add(Base + 1); Cards.Triangles.Add(Base + 2);
            Cards.Triangles.Add(Base + 0); Cards.Triangles.Add(Base + 2); Cards.Triangles.Add(Base + 3);
        };

        AddCard(FVector::RightVector, FVector::ForwardVector); // tarjeta en el plano XZ
        AddCard(FVector::ForwardVector, FVector::RightVector); // tarjeta en el plano YZ

        // Se hornea como "follaje" para que use el material de impostor (masked).
        FTreeMeshData Data;
        Data.Leaves = MoveTemp(Cards);

        FBox Ignored;
        return BuildStaticMesh(Outer, Data, FVector::ZeroVector, nullptr, ImpostorMaterial, Ignored);
    }
}
