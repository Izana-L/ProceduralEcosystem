# ProceduralEcosystem

![Unreal Engine 5.7](https://img.shields.io/badge/Unreal%20Engine-5.7-0e1128?logo=unrealengine&logoColor=white)
![C++](https://img.shields.io/badge/C%2B%2B-Runtime%20module-00599C?logo=cplusplus&logoColor=white)
![Plataforma](https://img.shields.io/badge/plataforma-Windows%20(DX12)-0078D6?logo=windows&logoColor=white)

Simulación procedural de un **ecosistema forestal** en C++ sobre Unreal Engine 5.7. Los árboles nacen, compiten por luz, agua y nutrientes, envejecen y mueren, y a la vez se dibujan con geometría generada en tiempo real.

<!-- CAPTURA PRINCIPAL: vista general del bosque. Guárdala en docs/img/ y descomenta la línea:
![Vista general del bosque](docs/img/bosque.png)
-->

## Características

- **Relieve procedural:** ruido fractal con *domain warping* y crestas, más erosión hidráulica y térmica.
- **Campos de recurso:** agua (índice topográfico de humedad), nutrientes y luz (rejilla 3D con extinción de Beer-Lambert bajo las copas).
- **Población a gran escala:** miles de árboles en arrays paralelos (SoA) con crecimiento logístico, estrés, mortalidad, dispersión de semillas y germinación.
- **Árboles generados por colonización del espacio (SCA)**, con fototropismo y autopoda.
- **Render en tres niveles de detalle:** *hero trees* con malla propia, instancias (ISM) e *impostors*, más una capa de suelo (tocones y hojarasca).
- **Especies configurables** como assets de datos. Incluye dos biomas: la laurisilva de **Garajonay** y el bosque templado de **Sierra de la Demanda**.
- **Reproducible:** generador de números aleatorios determinista, con un flujo independiente por subsistema.
- **Manejo por consola:** 42 comandos y 17 CVars (`Eco.*`), mapas de calor sobre el terreno y 31 tests de automatización.

<!-- CAPTURAS RECOMENDADAS (descomenta cuando las tengas):
![Mapa de calor de agua](docs/img/mapa-agua.png)
![Los tres niveles de detalle](docs/img/lod.png)
-->

## Requisitos

| Componente | Requisito |
|---|---|
| Sistema operativo | Windows 10 22H2 / 11 (64 bits) |
| Motor | **Unreal Engine 5.7** (exacta) |
| Compilador | Visual Studio 2022 17.14 o superior, con los componentes del fichero [`.vsconfig`](.vsconfig) |
| Control de versiones | Git 2.40+ y **Git LFS 3.x** |
| GPU | DirectX 12 con Shader Model 6. Recomendada con ray tracing por hardware (RTX 2000+ / RX 6000+) y 8 GB de VRAM |
| RAM | 16 GB mínimo, 32 GB recomendado |
| Disco | ~100 GB libres en SSD |

> [!IMPORTANT]
> **Git LFS es obligatorio.** Todos los `.uasset` y `.umap` viajan por LFS. Sin él, el clon descarga punteros de ~130 bytes en lugar de los assets reales, el proyecto abre vacío y el bosque nunca crece.

Si tu GPU no soporta ray tracing por hardware, desactívalo (ver la [guía de uso](Guia_de_Uso.pdf), §2.4).

## Instalación

```bash
git lfs install                  # una sola vez por máquina
git clone https://github.com/Izana-L/ProceduralEcosystem.git
cd ProceduralEcosystem
git lfs pull                     # fuerza la descarga de los assets
```

1. Clic derecho sobre `ProceduralEcosystem.uproject` → **Generate Visual Studio project files**.
2. Abre `ProceduralEcosystem.sln` en Visual Studio 2022, selecciona **Development Editor / Win64** y compila (`Ctrl+Mayús+B`). La primera compilación tarda entre 5 y 20 minutos.
3. Abre el editor con `F5` o con doble clic en el `.uproject`. Arranca directamente en el nivel `TestingMap`.

La primera vez, el editor compila shaders durante 20–60 minutos. Es normal y ocurre una sola vez por máquina.

> [!TIP]
> Clona el repositorio cerca de la raíz del disco (por ejemplo `C:\Dev\ProceduralEcosystem`) para evitar errores de ruta larga. No borres `Directory.Build.targets`: es un parche necesario.

## Uso rápido

Dale a **Play** (`Alt+P`), abre el *Output Log* y la consola con la tecla `` ` `` y escribe:

```
Eco.SeedForest 300     # siembra 300 plántulas por el terreno
Eco.Demografia         # cuántas hay de cada especie
Eco.Step 50            # avanza 50 años simulados
Eco.TogglePause        # deja correr el tiempo
Eco.PaintVigor         # mapa de calor de vigor sobre el terreno
Eco.GrowHeroTree 0     # árbol detallado de la especie 0
Eco.LOD.Stats          # reparto de árboles por nivel de detalle
```

- La simulación arranca **en pausa**.
- Casi todos los comandos requieren estar en Play. Fuera de él no hacen nada ni muestran error.
- Las respuestas se escriben en el *Output Log*. Filtra por `Eco` para quitar el ruido del motor.
- El relieve que usa la simulación es un array en memoria, **invisible** hasta que se exporta a un Landscape. Consulta el procedimiento en la guía de uso, §8.

La biblioteca completa de comandos, el flujo para generar el terreno y cómo configurar una especie nueva están en la [guía de uso](Guia_de_Uso.pdf).

## Tests

Los 31 tests de automatización no son comandos de consola. Se lanzan desde la caja `Cmd` del *Output Log* (sin necesidad de Play), o desde *Tools → Session Frontend → Automation*:

```
Automation RunTests Eco
```

O desde la línea de comandos, sin interfaz:

```bat
UnrealEditor-Cmd.exe "C:\Dev\ProceduralEcosystem\ProceduralEcosystem.uproject" ^
    -ExecCmds="Automation RunTests Eco; Quit" -unattended -nullrhi -nosplash
```

## Estructura del proyecto

```
ProceduralEcosystem/
├── Source/ProceduralEcosystem/     # Código C++ (un único módulo Runtime)
│   ├── Public/ y Private/
│   │   ├── Core/                   # RNG determinista, rejillas, utilidades
│   │   ├── Config/  Species/       # Ajustes del proyecto y rasgos de especie
│   │   ├── Terrain/                # Relieve, erosión, campos de agua, nutrientes y luz
│   │   ├── Ecology/                # Modelo biológico y población
│   │   ├── Simulation/             # Orquestador del tick y comandos de consola
│   │   ├── Geometry/               # Árboles procedurales (SCA), malla, follaje, viento
│   │   ├── Render/                 # LOD, instancias, impostors, capa de suelo
│   │   ├── Debug/                  # Mapas de calor, perfilador, auditoría
│   │   └── Test/                   # Tests de automatización
├── Config/                         # DefaultGame.ini (calibración) y DefaultEngine.ini
├── Content/
│   ├── Species/                    # Assets de especie (Garajonay, SierraDemanda)
│   ├── My_Levels/                  # TestingMap, nivel de arranque
│   ├── Materials*/  Textures/      # Materiales y texturas propias
│   └── Fab/  MSPresets/            # Recursos de terceros (Megascans)
├── Guia_de_Uso.pdf
└── Documentación técnica.pdf
```

La calibración de la simulación (más de 130 parámetros) se edita desde **Edit → Project Settings → Game → Procedural Ecosystem** y se guarda en `Config/DefaultGame.ini`.

## Documentación

| Documento | Contenido |
|---|---|
| [Guía de uso](Guia_de_Uso.pdf) | Instalación paso a paso, biblioteca de comandos y CVars, generación del terreno, configuración de especies, recetas y solución de problemas |
| [Documentación técnica](Documentaci%C3%B3n%20t%C3%A9cnica.pdf) | Componentes (UML 2), bibliotecas de terceros, clases, requisitos y estructura del repositorio |

## Créditos

- **Autor/a: Juan Luque Roldan


Recursos de terceros:

- [Unreal Engine 5.7](https://www.unrealengine.com/) y el plugin `ProceduralMeshComponent`.
- Texturas y materiales de **Quixel Megascans** a través de Fab (`Content/Fab`, `Content/MSPresets`), sujetos a su licencia.


## Licencia

_MIT

Los recursos de terceros de `Content/` mantienen sus licencias originales y no pueden relicenciarse.
