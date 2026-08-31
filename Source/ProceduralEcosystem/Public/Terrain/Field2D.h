/**
 * @file Field2D.h
 * @author Juan Luque Roldán
 * @brief Rejilla escalar 2D con geometría de mundo, base común de todos los campos.
 *
 * Declara FField2D, la estructura sobre la que se construyen el agua, los
 * nutrientes, la descomposición, la idoneidad y los pools de recursos: Width x
 * Height nodos separados CellSize centímetros a partir de Origin, más un
 * TArray<float> plano en orden fila-mayor. Aporta el muestreo bilineal, la
 * normalización lineal min-max y el patrón de bake «buffer crudo -> ParallelFor
 * por filas -> normalización», que fija el contrato de determinismo del módulo:
 * partición siempre por filas y función generadora pura, de modo que el
 * resultado es idéntico sea cual sea el número de hilos.
 *
 * @ingroup eco_terrain
 */

#pragma once

#include "CoreMinimal.h"
#include "Math/Box2D.h"
#include "Async/ParallelFor.h"
#include "Core/GridMath.h"

/**
 * Rejilla escalar 2D genérica: «un TArray<float> con forma de mundo».
 *
 * No sabe qué magnitud representa (agua, nutrientes, luz...); solo almacena y
 * muestrea. Cada campo concreto (FWaterField, FNutrientField, FResourcePool...)
 * se compone de una de éstas en vez de reimplementar la rejilla.
 *
 * Convención de muestreo: el valor vive EN EL NODO Origin + (Ix,Iy)*CellSize, no
 * en el centro de celda, y SampleBilinear interpola entre nodos. La rejilla de luz
 * gruesa, en cambio, muestrea en centros de vóxel: media celda de desfase,
 * inocuo a estas resoluciones pero a tener en cuenta al combinar ambos en la
 * función de vigor.
 *
 * @note Coordenadas de mundo en cm (unidades de Unreal).
 */
struct PROCEDURALECOSYSTEM_API FField2D
{
    int32     Width = 0;                       ///< Nodos en X.
    int32     Height = 0;                      ///< Nodos en Y.
    double    CellSize = 100.0;                ///< Separación entre nodos contiguos, en cm.
    FVector2D Origin = FVector2D::ZeroVector;  ///< Posición de mundo (cm) del nodo (0,0).
    TArray<float> Data;                        ///< Valores fila-mayor: índice Iy*Width + Ix.

    /** Cierto si la geometría es utilizable (>= 2x2 nodos) y Data tiene el tamaño que le toca. */
    bool IsValid() const { return Width > 1 && Height > 1 && Data.Num() == Width * Height; }

    /** Número de celdas (= Width*Height cuando IsValid()). */
    FORCEINLINE int32 Num() const { return Data.Num(); }

    /**
     * Fija la geometría de la rejilla, reserva el almacenamiento y lo rellena
     * con InitialValue.
     *
     * @note Width y Height se acotan a un mínimo de 2, y CellSize a un valor
     *       estrictamente positivo: es el divisor de WorldToGrid y por tanto de
     *       todo el muestreo.
     */
    void Init(int32 InWidth, int32 InHeight, double InCellSize,
        const FVector2D& InOrigin, float InitialValue = 0.f);

    /** Pone todas las celdas al mismo valor; no toca la geometría. */
    void Fill(float Value);

    /**
     * Valor del campo en la posición de mundo (Xcm, Ycm), interpolado
     * bilinealmente entre los cuatro nodos que la rodean.
     *
     * @return El valor interpolado, o 0 si el campo no es válido.
     * @note Fuera de la rejilla el valor del borde se extiende (no se envuelve).
     */
    float SampleBilinear(double Xcm, double Ycm) const;

    /** Valor del nodo (Ix, Iy); los índices fuera de rango se acotan al borde. */
    FORCEINLINE float GetAt(int32 Ix, int32 Iy) const
    {
        Ix = FMath::Clamp(Ix, 0, Width - 1);
        Iy = FMath::Clamp(Iy, 0, Height - 1);
        return Data[Iy * Width + Ix];
    }

    /** Extensión en mundo (cm) que cubre la rejilla, del nodo (0,0) al último. */
    FBox2D GetWorldBounds() const;

    /** Coordenada de mundo (cm) del NODO (Ix, Iy), la posición donde vive su valor. */
    FORCEINLINE double NodeWorldX(int32 Ix) const { return Origin.X + Ix * CellSize; }
    FORCEINLINE double NodeWorldY(int32 Iy) const { return Origin.Y + Iy * CellSize; }

    /** Mundo -> coordenadas de rejilla (en muestras, fraccional). */
    FORCEINLINE void WorldToGrid(double Xcm, double Ycm, double& OutGx, double& OutGy) const
    {
        OutGx = EcoGrid::ToGridCoord(Xcm, Origin.X, CellSize);
        OutGy = EcoGrid::ToGridCoord(Ycm, Origin.Y, CellSize);
    }

    /**
     * Mínimo y máximo de un buffer de valores, en un barrido serial O(N).
     *
     * Es la rutina de rango que comparten los generadores de campo, el
     * visualizador de heatmaps y el volcado de heightmap.
     *
     * @warning Con Values vacío devuelve OutMin > OutMax; el llamante decide qué
     *          hacer con ese caso.
     */
    static void MinMax(const TArray<float>& Values, float& OutMin, float& OutMax);

    /**
     * Escribe en Data la normalización lineal de Raw a [0, OutputMax]:
     * @f$ t = (v - min)/(max - min) @f$ y @f$ Data[i] = t \cdot OutputMax @f$.
     *
     * @param Raw       Buffer crudo; debe tener tantas celdas como la rejilla o
     *                  la llamada no hace nada.
     * @param OutputMax Valor que toma la celda de valor máximo.
     * @note Se escribe en ParallelFor por filas: cada fila toca celdas disjuntas,
     *       así que el resultado no depende del número de hilos.
     */
    void FillNormalizedFrom(const TArray<float>& Raw, float OutputMax);

    /**
     * Escribe en Data la normalización POR RANGO de Raw a [0, OutputMax]: cada
     * celda recibe su percentil espacial —la fracción de celdas del mapa con
     * valor crudo menor— en vez de su valor reescalado linealmente.
     *
     * Existe porque la normalización lineal traslada al campo la forma de la
     * distribución del buffer crudo, y un índice con cola larga (el TWI del agua)
     * queda con el grueso del mapa comprimido abajo y unos pocos valores extremos
     * ocupando solos la mitad alta del rango: ninguna especie puede calibrar su
     * óptimo sobre esa zona sin salirse de la distribución real. Con el rango, el
     * campo resultante es uniforme por construcción —una fracción f de OutputMax
     * corresponde exactamente al f por ciento más seco del mapa— y la ordenación
     * espacial, que es lo único con significado en un índice reescalado, se
     * conserva EXACTA.
     *
     * Los empates reciben todos el rango medio de su bloque, de modo que dos
     * celdas con el mismo valor crudo salen con el mismo valor normalizado; el
     * desempate del orden interno es por índice y el resultado no depende de él.
     *
     * @param Raw       Buffer crudo; debe tener tantas celdas como la rejilla o
     *                  la llamada no hace nada.
     * @param OutputMax Valor que toma la celda de rango máximo.
     * @note Serial (ordena N celdas, O(N log N)): es una rutina de bake, no de
     *       tick, y el orden fijo con desempate por índice la hace reproducible
     *       bit a bit.
     */
    void FillRankNormalizedFrom(const TArray<float>& Raw, float OutputMax);

    /**
     * Genera el campo entero a partir de una función de rejilla y lo normaliza a
     * [0, OutputMax].
     *
     * Encadena el patrón de bake del módulo: buffer crudo evaluado en ParallelFor
     * por filas y después FillNormalizedFrom. Cada fila escribe celdas disjuntas,
     * de modo que el resultado es idéntico sea cual sea el número de hilos.
     *
     * Es plantilla y no TFunctionRef para que Gen se inline dentro del bucle, que
     * es lo que interesa en un bake de cientos de miles de celdas.
     *
     * @param Gen       Invocable Gen(int32 x, int32 y) -> float con el valor crudo
     *                  del nodo. Debe ser PURA: de ello depende el determinismo.
     * @param OutputMax Valor que toma el nodo de valor crudo máximo.
     * @pre El campo tiene que estar inicializado (IsValid()); si no, no hace nada.
     */
    template <typename FGen>
    void GenerateNormalized(FGen&& Gen, float OutputMax)
    {
        if (!IsValid())
        {
            return;
        }

        const int32 W = Width;
        TArray<float> Raw;
        Raw.SetNumUninitialized(W * Height);

        ParallelFor(Height, [&Raw, &Gen, W](int32 y)
            {
                for (int32 x = 0; x < W; ++x)
                {
                    Raw[y * W + x] = Gen(x, y);
                }
            });

        FillNormalizedFrom(Raw, OutputMax);
    }
};