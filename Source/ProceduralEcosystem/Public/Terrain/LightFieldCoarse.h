#pragma once

#include "CoreMinimal.h"
#include "Core/GridMath.h"

/**
 * Grid de luz GRUESO a escala de paisaje (varios metros/voxel): sombreado
 * entre copas de distintos arboles. Es la resolucion (a) del documento de
 * diseno; la resolucion (b) -fina, local a cada hero tree- es la Fase 3.
 *
 * MECANICA: cada copa deposita AREA FOLIAR (LAI local) en los voxels de su
 * propio volumen de copa -y solo ahi-, y despues AccumulateExtinction convierte
 * esa densidad en el LAI ACUMULADO POR ENCIMA de cada voxel (suma prefija por
 * columna, de arriba abajo). La luz disponible sale entonces de Beer-Lambert:
 *
 *     Q(z) = DiffuseFloor + (FullSunlight - DiffuseFloor) * exp(-k * LAI_acum(z))
 *
 * POR QUE ASI Y NO COMO ANTES. La version anterior depositaba "sombra" en una
 * piramide invertida que decaia con la profundidad (verticalFalloff = 1 - t) y
 * recibia CanopyDepthCm = altura ENTERA del arbol, con el apice en suelo+H. El
 * resultado era una sombra que se anulaba justo en la cota del suelo: el perfil
 * de un adulto de 20 m iba de 0.72 en la copa a 0.08 a ras de suelo, o sea nueve
 * veces mas oscuro ARRIBA que abajo, exactamente al reves que un dosel real. Sin
 * gradiente de luz no hay sotobosque, y sin sotobosque el umbral de luz para
 * germinar de cada especie no filtra nada y la tolerancia a la sombra no compra
 * acceso a ningun sitio: los dos mecanismos de sucesion del modelo quedan
 * inertes. Con extincion acumulada la atenuacion PERSISTE hacia abajo, que es lo
 * que hace que exista un suelo oscuro bajo una copa densa.
 *
 * ================== REJILLA RELATIVA AL TERRENO (optimizacion C2) ==========
 * La rejilla es 3D (X, Y horizontales + Z en capas), pero la vertical NO es
 * absoluta: la capa 0 de cada columna arranca en la COTA DEL TERRENO de esa
 * columna (mas BaseZ, que suele ser un pequeno margen negativo).
 *
 * POR QUE: con Z absoluta habia que cubrir todo el relieve. Con los valores
 * por defecto (HeightScaleCm = 30.000 cm de desnivel, voxel de 400 cm) salian
 * 95 capas x 256 x 256 = 6,2 M voxels = ~25 MB que ademas se ponen a cero
 * ENTERO cada tick. Pero los arboles solo miden ~2.000 cm: cada columna tenia
 * ~5 capas utiles y ~90 de aire o de roca. Midiendo la altura RESPECTO AL
 * SUELO bastan ~10-12 capas -> ~8-10x menos memoria y un ClearShadow 8-10x mas
 * barato. Y de paso es mas correcto ecologicamente: un arbol del valle y otro
 * de la cresta ocupan las mismas capas relativas, que es lo que de verdad
 * significa "altura de copa".
 *
 * La API publica NO cambia: se sigue muestreando y depositando en COORDENADAS
 * DE MUNDO. La conversion la hace la rejilla por dentro con GroundZ, que se
 * rellena una vez con SetGroundHeights(). Si no se rellena, la rejilla se
 * comporta como antes (absoluta respecto a BaseZ), lo que mantiene validos los
 * tests y cualquier uso suelto.
 * ==========================================================================
 *
 * Muestrea en CENTROS de voxel (a diferencia de FField2D, que muestrea en
 * nodos); es medio voxel de desfase, inocuo a estas resoluciones, pero conviene
 * saberlo al combinar con agua/nutrientes.
 */
struct PROCEDURALECOSYSTEM_API FLightFieldCoarse
{
    int32 Width = 0; // voxels en X
    int32 Height = 0; // voxels en Y
    int32 Layers = 0; // voxels en Z (altura SOBRE EL SUELO, ver nota de arriba)

    double CellSizeXY = 400.0; // cm por voxel horizontal (varios metros)
    double CellSizeZ = 400.0; // cm por voxel vertical

    FVector2D Origin = FVector2D::ZeroVector; // esquina (0,0) en mundo, cm

    /** Offset (cm) de la capa 0 respecto a la cota de referencia de la columna.
        Con GroundZ relleno, esa referencia es el TERRENO y este valor suele ser
        negativo (un poco de margen bajo el suelo para que las pendientes no
        clampeen); sin GroundZ es la Z de mundo absoluta de la capa 0. */
    double BaseZ = 0.0;

    /**
     * Area foliar por voxel. Width*Height*Layers.
     *
     * OJO AL DOBLE SIGNIFICADO, que es deliberado y ahorra un segundo buffer del
     * tamano de la rejilla: entre ClearShadow() y AccumulateExtinction() guarda la
     * DENSIDAD depositada por cada copa en su propio volumen; despues de
     * AccumulateExtinction() guarda el LAI ACUMULADO POR ENCIMA del centro de cada
     * voxel, que es lo que consumen los muestreadores. Los Sample* solo son
     * validos tras esa llamada (ver RebuildCoarseLight).
     */
    TArray<float> LeafArea;

    /** k de Beer-Lambert (coeficiente de extincion). ~0.5 en hoja ancha. */
    float ExtinctionK = 0.5f;

    /** Luz difusa del cielo que llega al sotobosque aunque el dosel este cerrado. */
    float DiffuseFloor = 0.04f;

    /** Cota de terreno (cm) del centro de cada columna. Vacio = rejilla absoluta. */
    TArray<float> GroundZ;

    /** C: luz plena normalizada (cielo despejado). Compartida con el grid fino. */
    static constexpr float FullSunlight = EcoGrid::FullSunlight;

    bool IsValid() const
    {
        return Width > 0 && Height > 0 && Layers > 0 && LeafArea.Num() == Width * Height * Layers;
    }

    /** true si la vertical se mide respecto al terreno (ver nota de C2). */
    bool IsTerrainRelative() const { return GroundZ.Num() == Width * Height; }

    /** Reserva la rejilla y la deja sin sombra. */
    void Init(int32 InWidth, int32 InHeight, int32 InLayers,
        double InCellSizeXY, double InCellSizeZ,
        const FVector2D& InOrigin, double InBaseZ);

    /**
     * Fija la cota de terreno de cada columna (Width*Height valores, en orden
     * fila-mayor igual que LeafArea). A partir de aqui la rejilla es relativa al
     * terreno. Pasar un array de tamano distinto la deja absoluta.
     */
    void SetGroundHeights(TArray<float>&& InGroundZ);

    /** Cota de referencia (cm) de la columna (Ix,Iy): terreno si lo hay, si no BaseZ. */
    FORCEINLINE double ReferenceZ(int32 Ix, int32 Iy) const
    {
        return IsTerrainRelative() ? static_cast<double>(GroundZ[Iy * Width + Ix]) : 0.0;
    }

    /** Pone toda el area foliar a 0 (llamar antes de re-depositar en cada tick). */
    void ClearShadow();

    /** Memoria de la rejilla en bytes (para Eco.Profile). */
    int64 MemoryBytes() const
    {
        return (int64)LeafArea.Max() * sizeof(float) + (int64)GroundZ.Max() * sizeof(float);
    }

    /** Fija los parametros de Beer-Lambert (los sirve UEcosystemSettings). */
    void SetExtinctionParams(float InExtinctionK, float InDiffuseFloor)
    {
        ExtinctionK = FMath::Max(InExtinctionK, 0.f);
        DiffuseFloor = FMath::Clamp(InDiffuseFloor, 0.f, FullSunlight);
    }

    /**
     * Indice de la primera capa que esta AL NIVEL DEL SUELO o por encima.
     *
     * BaseZ es negativo (margen bajo el terreno para que las pendientes fuertes no
     * clampeen), asi que las primeras capas de cada columna son SUBTERRANEAS: con
     * BaseZ = -800 y CellSizeZ = 400 hay dos. Nadie deposita nunca en ellas, asi
     * que valen 0 siempre; interpolar contra ellas devolvia la MITAD del LAI real
     * y falseaba toda lectura a ras de suelo -incluida la del log con el que se
     * calibran los umbrales de germinacion-. Los muestreadores clampean aqui.
     */
    FORCEINLINE int32 GroundLayerIndex() const
    {
        return FMath::Clamp(FMath::FloorToInt32(-BaseZ / CellSizeZ), 0, Layers - 1);
    }

    /**
     * Deposita el AREA FOLIAR de UNA copa en los voxels de su propio volumen.
     *
     * La copa es una capa de opacidad en la parte alta del arbol, no una piramide
     * que se derrama hasta el suelo: solo se escribe entre ApexWorldPos.Z y
     * ApexWorldPos.Z - CanopyDepthCm. Lo que oscurece el sotobosque no es este
     * deposito sino la ACUMULACION posterior (AccumulateExtinction).
     *
     * El reparto vertical esta normalizado por la profundidad, asi que un rayo
     * vertical por el eje de la copa acumula exactamente LeafAreaIndex sea cual
     * sea la altura del arbol o el tamano del voxel. Radialmente decae como
     * 1-(d/R)^2: densa en el eje, nula en el borde.
     *
     * @param ApexWorldPos     Punto mas alto de la copa (mundo, cm).
     * @param CanopyRadiusCm   Radio horizontal de la copa.
     * @param CanopyDepthCm    Espesor vertical de la copa (NO la altura del arbol).
     * @param LeafAreaIndex    LAI de la copa completa medido en su eje (~4 en un adulto de hoja ancha).
     */
    void DepositCanopyLeafArea(const FVector& ApexWorldPos, float CanopyRadiusCm,
        float CanopyDepthCm, float LeafAreaIndex = 1.f);

    /**
     * Convierte la densidad depositada en LAI ACUMULADO POR ENCIMA de cada voxel,
     * con una suma prefija descendente por columna. Hay que llamarla UNA vez tras
     * depositar todas las copas y ANTES de cualquier Sample*.
     *
     * Un voxel ve todo el follaje de las capas estrictamente superiores mas la
     * MITAD del suyo propio: su centro esta a media capa de profundidad dentro de
     * ella. Esa media capa es tambien lo que evita que un arbol se autoexcluya mal
     * cuando se muestrea en el techo de su propia copa.
     */
    void AccumulateExtinction();

    /** Luz disponible (vecino mas cercano), via Beer-Lambert sobre el LAI acumulado. */
    float SampleLight(const FVector& WorldPos) const;

    /**
     * Igual que SampleLight pero con interpolacion TRILINEAL entre los 8
     * voxels vecinos. Mas suave (sin bloques); util para la funcion de vigor
     * y para sembrar el grid fino del hero tree en la Fase 3. Un pelin mas
     * caro que el vecino mas cercano.
     */
    float SampleLightSmooth(const FVector& WorldPos) const;

private:
    FORCEINLINE int32 IndexOf(int32 Ix, int32 Iy, int32 Iz) const
    {
        return EcoGrid::VoxelIndex(Ix, Iy, Iz, Width, Height);
    }

    /** Columna de mundo -> indice (Ix,Iy) con clamp a los bordes. */
    FORCEINLINE void WorldToColumnClamped(double Xcm, double Ycm, int32& OutIx, int32& OutIy) const
    {
        OutIx = EcoGrid::WorldToCellClamped(Xcm, Origin.X, CellSizeXY, Width);
        OutIy = EcoGrid::WorldToCellClamped(Ycm, Origin.Y, CellSizeXY, Height);
    }

    /**
     * Reparte ColumnLai a lo largo de las capas de UNA columna que caen dentro de la
     * copa, proporcionalmente a cuanto solapa cada una. Copia unica del reparto
     * vertical: lo usan tanto el camino normal como el de copa sub-voxel.
     */
    void DepositColumnLeafArea(int32 Ix, int32 Iy, double CrownTopZ, double CrownBottomZ,
        float CanopyDepthCm, float ColumnLai);

    /**
     * Coordenada vertical CONTINUA (en capas, referida a centros de voxel) de Zcm
     * dentro de la columna (Ix,Iy), ya recortada al rango util de la rejilla.
     *
     * Copia unica de la conversion: la comparten los dos muestreadores, asi que el
     * recorte por debajo del terreno no puede aplicarse en uno y olvidarse en otro.
     */
    FORCEINLINE double ColumnLayerCoord(int32 Ix, int32 Iy, double Zcm) const
    {
        const double W = (Zcm - ReferenceZ(Ix, Iy) - BaseZ) / CellSizeZ - 0.5;
        return FMath::Clamp(W, (double)GroundLayerIndex(), (double)(Layers - 1));
    }
};
