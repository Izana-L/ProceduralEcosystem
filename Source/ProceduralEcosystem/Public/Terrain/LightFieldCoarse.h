/**
 * @file LightFieldCoarse.h
 * @author Juan Luque Roldán
 * @brief Rejilla 3D gruesa de luz: convierte las copas del bosque en sombra a escala de paisaje.
 *
 * Declara FLightFieldCoarse, el único campo del módulo que se reconstruye en cada
 * tick. Las copas depositan su área foliar en los vóxeles que ocupan, una suma
 * prefija por columna la convierte en el LAI acumulado por encima de cada vóxel y
 * los muestreadores aplican sobre él la ley de Beer-Lambert. La vertical se mide
 * sobre el terreno y no en Z absoluta, lo que recorta la memoria un orden de
 * magnitud. Es la entrada de luz de la función de vigor y del umbral de
 * germinación, y con ello el soporte de la competencia por luz y de la sucesión.
 *
 * @ingroup eco_terrain
 * @see @ref bib_monsisaeki1953
 * @see @ref bib_watson1947
 */

#pragma once

#include "CoreMinimal.h"
#include "Core/GridMath.h"

/**
 * Rejilla 3D gruesa (varios metros por vóxel) del sombreado que se hacen unas
 * copas a otras a escala de paisaje.
 *
 * Cada copa deposita área foliar en los vóxeles de su propio volumen —y solo
 * ahí—; AccumulateExtinction() convierte después esa densidad en el LAI acumulado
 * por encima de cada vóxel, y de ahí sale la luz por Beer-Lambert
 * (@ref EcoGrid::LightFromExtinction):
 *
 * @f[ Q(z) = Q_{\text{suelo}} + (Q_{\text{sol}} - Q_{\text{suelo}})\;e^{-k\,\mathrm{LAI}(z)} @f]
 *
 * Que la extinción sea acumulada y no local es lo que hace que la atenuación
 * persista hacia abajo y exista suelo oscuro bajo una copa densa. De ese gradiente
 * vertical dependen los dos mecanismos de sucesión del modelo: sin sotobosque en
 * penumbra, ni el umbral de luz para germinar ni la tolerancia a la sombra
 * discriminan entre especies.
 *
 * @par Vertical relativa al terreno
 * La capa 0 de cada columna arranca en la cota del terreno de esa columna (más
 * BaseZ), no en una Z de mundo común. Con vertical absoluta hay que cubrir todo el
 * desnivel —unas 170 capas con los valores por defecto, de las que cada columna usa
 * diez— y ponerlas a cero enteras en cada tick; medida sobre el suelo bastan veintidós,
 * con casi ocho veces menos memoria. Es además lo que significa «altura de copa»: un
 * árbol del valle y otro de la cresta ocupan las mismas capas relativas. La API
 * pública sigue siendo en coordenadas de mundo; la conversión la hace la rejilla con
 * GroundZ, y sin GroundZ se comporta como una rejilla absoluta respecto a BaseZ.
 *
 * @note Muestrea en CENTROS de vóxel, a diferencia de FField2D, que muestrea en
 *       nodos: medio vóxel de desfase, inocuo a estas resoluciones pero a tener en
 *       cuenta al combinarla con el agua y los nutrientes.
 */
struct PROCEDURALECOSYSTEM_API FLightFieldCoarse
{
    /** Vóxeles en X. */
    int32 Width = 0;

    /** Vóxeles en Y. */
    int32 Height = 0;

    /** Vóxeles en Z, es decir, capas de altura sobre el suelo de cada columna. */
    int32 Layers = 0;

    /** Lado horizontal del vóxel, en cm; del orden de varios metros. */
    double CellSizeXY = 400.0;

    /** Altura del vóxel, en cm. */
    double CellSizeZ = 400.0;

    /** Posición de mundo (cm) de la esquina de la columna (0,0). */
    FVector2D Origin = FVector2D::ZeroVector;

    /**
     * Desplazamiento (cm) de la capa 0 respecto a la cota de referencia de la
     * columna. Con GroundZ relleno esa referencia es el terreno y BaseZ suele ser
     * negativo, un margen bajo el suelo para que las pendientes fuertes no
     * clampeen; sin GroundZ es directamente la Z de mundo de la capa 0.
     */
    double BaseZ = 0.0;

    /**
     * Área foliar por vóxel, Width*Height*Layers valores en orden fila-mayor.
     *
     * El buffer tiene dos significados según el momento del tick, deliberadamente,
     * para no duplicar una estructura del tamaño de la rejilla: entre ClearShadow()
     * y AccumulateExtinction() guarda la densidad que cada copa deposita en su
     * propio volumen; después de AccumulateExtinction() guarda el LAI acumulado por
     * encima del centro de cada vóxel, que es lo que leen los muestreadores.
     *
     * @warning Los Sample* solo devuelven luz correcta tras AccumulateExtinction().
     */
    TArray<float> LeafArea;

    /** Coeficiente de extinción k de Beer-Lambert; ~0.5 en hoja ancha. */
    float ExtinctionK = 0.5f;

    /** Luz difusa del cielo que alcanza el sotobosque aun con el dosel cerrado. */
    float DiffuseFloor = 0.04f;

    /** Cota de terreno (cm) de cada columna. Vacío: la rejilla es absoluta. */
    TArray<float> GroundZ;

    /** Luz plena normalizada (cielo despejado), compartida con la rejilla de luz fina. */
    static constexpr float FullSunlight = EcoGrid::FullSunlight;

    /** Cierto si la geometría es utilizable y LeafArea tiene el tamaño que le toca. */
    bool IsValid() const
    {
        return Width > 0 && Height > 0 && Layers > 0 && LeafArea.Num() == Width * Height * Layers;
    }

    /** Cierto si la vertical se mide sobre el terreno, es decir, si hay GroundZ. */
    bool IsTerrainRelative() const { return GroundZ.Num() == Width * Height; }

    /**
     * Fija la geometría de la rejilla, reserva LeafArea y la deja sin sombra.
     *
     * @note Layers se acota a [1, 512] y los dos tamaños de vóxel a un mínimo de
     *       1 cm: son divisores de toda la conversión mundo -> rejilla.
     * @post La rejilla queda absoluta; para hacerla relativa al terreno hay que
     *       llamar después a SetGroundHeights().
     */
    void Init(int32 InWidth, int32 InHeight, int32 InLayers,
        double InCellSizeXY, double InCellSizeZ,
        const FVector2D& InOrigin, double InBaseZ);

    /**
     * Fija la cota de terreno de cada columna y con ello hace la rejilla relativa
     * al terreno.
     *
     * @param InGroundZ Width*Height cotas en cm, en el mismo orden fila-mayor que
     *                  LeafArea. Un array de otro tamaño deja la rejilla absoluta.
     */
    void SetGroundHeights(TArray<float>&& InGroundZ);

    /** Cota de referencia (cm) de la columna (Ix,Iy): la del terreno, o 0 si es absoluta. */
    FORCEINLINE double ReferenceZ(int32 Ix, int32 Iy) const
    {
        return IsTerrainRelative() ? static_cast<double>(GroundZ[Iy * Width + Ix]) : 0.0;
    }

    /** Pone toda el área foliar a cero; se llama antes de volver a depositar las copas del tick. */
    void ClearShadow();

    /** Memoria ocupada por la rejilla, en bytes; la publica el informe de perfilado. */
    int64 MemoryBytes() const
    {
        return (int64)LeafArea.Max() * sizeof(float) + (int64)GroundZ.Max() * sizeof(float);
    }

    /** Fija los parámetros de Beer-Lambert, que sirve UEcosystemSettings, ya acotados. */
    void SetExtinctionParams(float InExtinctionK, float InDiffuseFloor)
    {
        ExtinctionK = FMath::Max(InExtinctionK, 0.f);
        DiffuseFloor = FMath::Clamp(InDiffuseFloor, 0.f, FullSunlight);
    }

    /**
     * Índice de la primera capa que está al nivel del suelo o por encima.
     *
     * Con BaseZ negativo las primeras capas de cada columna quedan bajo tierra —dos
     * con BaseZ = -800 y CellSizeZ = 400—, nadie deposita nunca en ellas y por
     * tanto valen cero por construcción. Los muestreadores clampean aquí para no
     * interpolar contra ellas: hacerlo devolvería la mitad del LAI real en toda
     * lectura a ras de suelo, justo donde se deciden los umbrales de germinación.
     */
    FORCEINLINE int32 GroundLayerIndex() const
    {
        return FMath::Clamp(FMath::FloorToInt32(-BaseZ / CellSizeZ), 0, Layers - 1);
    }

    /**
     * Deposita el área foliar de una copa en los vóxeles de su propio volumen.
     *
     * La copa es una capa de opacidad en la parte alta del árbol, no una pirámide
     * que se derrama hasta el suelo: solo se escribe entre ApexWorldPos.Z y
     * ApexWorldPos.Z - CanopyDepthCm. Lo que oscurece el sotobosque no es este
     * depósito, sino la acumulación posterior de AccumulateExtinction().
     *
     * El reparto vertical está normalizado por la profundidad de copa, de modo que
     * un rayo vertical que recorra el eje acumula exactamente LeafAreaIndex sea
     * cual sea la altura del árbol o el tamaño del vóxel. En horizontal el perfil
     * decae como @f$1-(d/R)^2@f$: denso en el eje, nulo en el borde.
     *
     * @param ApexWorldPos   Punto más alto de la copa, en coordenadas de mundo (cm).
     * @param CanopyRadiusCm Radio horizontal de la copa.
     * @param CanopyDepthCm  Espesor vertical de la copa, nunca la altura del árbol.
     * @param LeafAreaIndex  LAI de la copa medido en su eje; ~4 en un adulto de hoja ancha.
     * @see @ref bib_watson1947
     */
    void DepositCanopyLeafArea(const FVector& ApexWorldPos, float CanopyRadiusCm,
        float CanopyDepthCm, float LeafAreaIndex = 1.f);

    /**
     * Convierte la densidad depositada en el LAI acumulado por encima de cada
     * vóxel, con una suma prefija descendente por columna.
     *
     * Cada vóxel ve todo el follaje de las capas estrictamente superiores más la
     * mitad del suyo propio, porque su centro está a media capa de profundidad
     * dentro de ella; esa media capa es también lo que impide que un árbol se
     * autosombree entero al muestrearse en el techo de su copa.
     *
     * @pre Todas las copas del tick ya están depositadas.
     * @post Los Sample* devuelven luz válida hasta el siguiente ClearShadow().
     */
    void AccumulateExtinction();

    /** Luz en WorldPos por el vóxel más cercano, vía Beer-Lambert sobre el LAI acumulado. */
    float SampleLight(const FVector& WorldPos) const;

    /**
     * Luz disponible en WorldPos interpolando entre los ocho vóxeles vecinos.
     *
     * Suprime los escalones de vóxel del muestreo por vecino más cercano, a cambio
     * de siete lecturas más; es la variante que consumen la función de vigor y el
     * sembrado de la rejilla de luz fina.
     */
    float SampleLightSmooth(const FVector& WorldPos) const;

private:
    /** Índice lineal fila-mayor del vóxel (Ix,Iy,Iz) dentro de LeafArea. */
    FORCEINLINE int32 IndexOf(int32 Ix, int32 Iy, int32 Iz) const
    {
        return EcoGrid::VoxelIndex(Ix, Iy, Iz, Width, Height);
    }

    /** Posición de mundo (cm) a índice de columna (Ix,Iy), con recorte a los bordes. */
    FORCEINLINE void WorldToColumnClamped(double Xcm, double Ycm, int32& OutIx, int32& OutIy) const
    {
        OutIx = EcoGrid::WorldToCellClamped(Xcm, Origin.X, CellSizeXY, Width);
        OutIy = EcoGrid::WorldToCellClamped(Ycm, Origin.Y, CellSizeXY, Height);
    }

    /**
     * Reparte ColumnLai entre las capas de una columna que caen dentro de la copa,
     * proporcionalmente al solape vertical de cada una.
     *
     * Copia única del reparto vertical: la comparten el camino general de
     * DepositCanopyLeafArea() y su respaldo para copas menores que un vóxel.
     */
    void DepositColumnLeafArea(int32 Ix, int32 Iy, double CrownTopZ, double CrownBottomZ,
        float CanopyDepthCm, float ColumnLai);

    /**
     * Coordenada vertical continua de Zcm dentro de la columna (Ix,Iy), medida en
     * capas y referida a centros de vóxel, ya recortada al rango útil.
     *
     * Copia única de la conversión: la comparten los dos muestreadores, de modo que
     * el recorte por debajo del terreno (@ref GroundLayerIndex) no puede aplicarse
     * en uno y olvidarse en el otro.
     */
    FORCEINLINE double ColumnLayerCoord(int32 Ix, int32 Iy, double Zcm) const
    {
        const double W = (Zcm - ReferenceZ(Ix, Iy) - BaseZ) / CellSizeZ - 0.5;
        return FMath::Clamp(W, (double)GroundLayerIndex(), (double)(Layers - 1));
    }
};
