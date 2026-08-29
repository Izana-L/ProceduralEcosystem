/**
 * @file EcologyRules.cpp
 * @author Juan Luque Roldán
 * @brief Implementación del kernel radial de depósito y de la reducción del scratch.
 *
 * Contiene lo único de EcologyRules que no cabe inline en la cabecera: el recorrido de
 * celdas del kernel de depósito, compartido en una plantilla por la versión densa y la
 * dispersa para que ambas emitan las celdas en el mismo orden, y la reducción serial que
 * funde el scratch privado de todas las tareas del tick en el estado compartido. Las dos
 * piezas existen por el mismo motivo: que el resultado del tick no dependa del número de
 * núcleos ni del reparto de tareas entre hilos.
 *
 * @ingroup eco_ecology
 * @see @ref bib_goldberg1991
 * @see @ref bib_zonadeinfluencia
 */

#include "Ecology/EcologyRules.h"

namespace
{
    /**
     * Núcleo del kernel radial, compartido por la versión densa y la dispersa: una sola
     * copia de la matemática y, sobre todo, un único recorrido de celdas, que es lo que
     * sostiene el determinismo de la reducción.
     *
     * Dos pasadas sobre el mismo bloque de celdas:
     * @li Suma los pesos crudos de las celdas dentro del radio (kernel lineal: 1 en el
     *     centro, 0 en el borde) y los cachea en un buffer local.
     * @li Reparte TotalAmount proporcionalmente al peso normalizado por esa suma, de modo
     *     que lo depositado sume exactamente TotalAmount pese al redondeo a celdas.
     *
     * Cachear los pesos evita repetir el Sqrt por celda que exige la distancia real, en un
     * bucle que corre por cada árbol, cada campo y cada tick dentro del ParallelFor.
     *
     * Sink se invoca como Sink(int32 CellIndex, float Amount).
     */
    template <typename FSink>
    void ForEachKernelCell(const FField2D& Geometry, const FVector& WorldPos,
        float RadiusCm, float TotalAmount, FSink&& Sink)
    {
        if (RadiusCm <= 0.f || TotalAmount == 0.f || !Geometry.IsValid())
        {
            return;
        }

        double Gx, Gy;
        Geometry.WorldToGrid(WorldPos.X, WorldPos.Y, Gx, Gy);
        const int32 Cx = FMath::FloorToInt(Gx);
        const int32 Cy = FMath::FloorToInt(Gy);
        const int32 CellRadius = FMath::CeilToInt(RadiusCm / Geometry.CellSize);

        // Bloque de celdas afectado, recortado una sola vez a la rejilla. Al haber una
        // única definición del recorrido, las dos pasadas emiten las celdas en el mismo
        // orden por construcción, que es de lo que depende el determinismo de la reducción.
        const int32 X0 = FMath::Max(Cx - CellRadius, 0), X1 = FMath::Min(Cx + CellRadius, Geometry.Width - 1);
        const int32 Y0 = FMath::Max(Cy - CellRadius, 0), Y1 = FMath::Min(Cy + CellRadius, Geometry.Height - 1);

        // Las distancias se miden desde la posición fraccional del árbol (Gx,Gy) hasta
        // cada nodo, no desde el nodo que lo contiene. El campo guarda su valor EN el nodo
        // y se muestrea con bilineal sobre los cuatro vecinos, así que anclar el kernel en
        // el nodo contenedor desplazaría el depósito hasta media celda respecto de donde
        // el árbol lee: consumiría de un sitio y mediría en otro.
        auto ForEachCellInDisc = [&](auto&& CellFn)
            {
                for (int32 Iy = Y0; Iy <= Y1; ++Iy)
                {
                    const double dy = (double)Iy - Gy;
                    const int32 RowBase = Iy * Geometry.Width;
                    for (int32 Ix = X0; Ix <= X1; ++Ix)
                    {
                        CellFn((double)Ix - Gx, dy, RowBase + Ix);
                    }
                }
            };

        // Pesos de las celdas visitadas, en orden de visita. Inline porque el radio
        // efectivo típico es de ~1 celda (bloque 3x3): 64 entradas cubren hasta 7x7 sin
        // tocar el heap, y con radios mayores el array reserva como cualquier otro.
        TArray<float, TInlineAllocator<64>> Weights;

        // ==== Pasada 1: peso crudo de cada celda dentro del radio, cacheado ====
        float WeightSum = 0.f;
        ForEachCellInDisc([&](double dx, double dy, int32 /*Cell*/)
            {
                const double CellDistCm = FMath::Sqrt(dx * dx + dy * dy) * Geometry.CellSize;
                const float W = FMath::Max(0.f, 1.f - static_cast<float>(CellDistCm / RadiusCm));
                Weights.Add(W);
                WeightSum += W;
            });

        if (WeightSum <= KINDA_SMALL_NUMBER)
        {
            // El radio es menor que una celda o el punto cae justo en el borde del mundo:
            // en vez de perder masa, deposita todo en la celda más cercana.
            const int32 Ix = FMath::Clamp(Cx, 0, Geometry.Width - 1);
            const int32 Iy = FMath::Clamp(Cy, 0, Geometry.Height - 1);
            Sink(Iy * Geometry.Width + Ix, TotalAmount);
            return;
        }

        // ==== Pasada 2: repartir TotalAmount con los pesos ya calculados ====
        // Mismo recorrido, y por tanto mismo orden de emisión, sin recalcular distancias.
        const float InvWeightSum = 1.f / WeightSum;
        int32 WeightIdx = 0;
        ForEachCellInDisc([&](double /*dx*/, double /*dy*/, int32 Cell)
            {
                const float W = Weights[WeightIdx++];
                if (W <= 0.f) { return; }

                Sink(Cell, TotalAmount * (W * InvWeightSum));
            });
    }
}

void EcologyRules::DepositKernel(const FField2D& Geometry, TArray<float>& Deltas,
    const FVector& WorldPos, float RadiusCm, float TotalAmount)
{
    ForEachKernelCell(Geometry, WorldPos, RadiusCm, TotalAmount,
        [&Deltas](int32 Cell, float Amount)
        {
            if (Deltas.IsValidIndex(Cell)) { Deltas[Cell] += Amount; }
        });
}

void EcologyRules::DepositKernelSparse(const FField2D& Geometry, TArray<FCellDelta>& OutDeltas,
    const FVector& WorldPos, float RadiusCm, float TotalAmount)
{
    ForEachKernelCell(Geometry, WorldPos, RadiusCm, TotalAmount,
        [&OutDeltas](int32 Cell, float Amount)
        {
            OutDeltas.Add(FCellDelta{ Cell, Amount });
        });
}

void EcologyRules::ReduceScratchInto(const TArray<FTickScratch>& Contexts,
    TArray<float>& DestWater, TArray<float>& DestNutrient,
    TArray<FPendingSeed>& OutSeeds, TArray<FPendingDeathPulse>& OutDeathPulses,
    TArray<FEcoSpeciesFlow>& OutFlow)
{
    OutSeeds.Reset();
    OutDeathPulses.Reset();
    for (FEcoSpeciesFlow& F : OutFlow) { F.Reset(); }

    // Orden de índice creciente: fijo, no depende de qué hilo físico ejecutó cada tarea ni
    // de cuántos núcleos tiene la máquina. Y dentro de cada tarea, orden de inserción
    // (árbol por índice creciente, celda por el orden del kernel). Suma en coma flotante
    // no asociativa más orden fijo dan un resultado reproducible bit a bit.
    for (const FTickScratch& Ctx : Contexts)
    {
        for (const FCellDelta& D : Ctx.WaterDeltas)
        {
            checkSlow(DestWater.IsValidIndex(D.Cell));
            DestWater[D.Cell] += D.Amount;
        }
        for (const FCellDelta& D : Ctx.NutrientDeltas)
        {
            checkSlow(DestNutrient.IsValidIndex(D.Cell));
            DestNutrient[D.Cell] += D.Amount;
        }

        OutSeeds.Append(Ctx.Seeds);
        OutDeathPulses.Append(Ctx.DeathPulses);

        // El embudo se reduce aquí por el mismo motivo que los deltas: es el punto donde
        // el scratch privado de cada tarea converge en un único estado, y hacerlo con
        // atómicas dentro del ParallelFor rompería la reproducibilidad.
        const int32 Num = FMath::Min(OutFlow.Num(), Ctx.SpeciesFlow.Num());
        for (int32 i = 0; i < Num; ++i) { OutFlow[i] += Ctx.SpeciesFlow[i]; }
    }
}
