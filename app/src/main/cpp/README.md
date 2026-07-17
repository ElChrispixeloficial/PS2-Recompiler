# PS2 Recompilador Nativo Android

Recompilador de PS2 para Android que traduce código MIPS del Emotion Engine
directamente a instrucciones ARM64/ARM32 nativas. Sin emulación — el código
del juego corre en el CPU real de tu dispositivo.

## Arquitectura

```
ISO de PS2
    │
    ▼
[ISO Loader]  ← lee ISO 9660, encuentra el ELF del juego
    │             carga segmentos PT_LOAD en RAM del EE (32 MB)
    ▼
[EE Core] ──────────────────────────────────────────────────
│  Emotion Engine: MIPS R5900 @ 294 MHz                    │
│  recompiler_arm64.cpp:                                    │
│    MIPS ADD  → ARM64 ADD  (instrucción nativa)           │
│    MIPS MULT → ARM64 MUL  (instrucción nativa)           │
│    MIPS LW   → ARM64 LDR  (acceso directo a RAM)         │
│    MIPS BEQ  → ARM64 B.EQ (salto condicional nativo)     │
│  CodeCache: bloques compilados en mmap RWX               │
│                                                           │
│  [VU0] ← COP2 (geometría 3D SIMD float32×4)            │
│  [VU1] ← XGKICK → GIF packets                           │
└───────────────────────────────────────────────────────────
    │
    ├──[DMA CH2: GIF]──────────────────────────────────────
    │                                                       │
    ▼                                                       ▼
[IOP Core]                                          [GS Core]
MIPS R3000 @ 36 MHz                          Graphics Synthesizer
CD-ROM, DualShock, SPU2                        ↓
    │                                   [Vulkan Translator]
    ▼                                   gs_vulkan.cpp:
[SPU2]                                   GS PRIM SPRITE → 2 triángulos Vulkan
spu2_core.cpp:                           GS PRIM TRI    → VkDrawCall
ADPCM decode → PCM                       VRAM → VkImage
    │                                        │
    ▼                                        ▼
[OpenSL ES]                           [VkQueuePresent]
    │                                        │
    ▼                                        ▼
BOCINA/AURICULARES                     PANTALLA ANDROID
   (señal directa)                      (señal directa)
```

## Instrucciones para Android-IDE

### Requisitos
- Android-IDE app instalada en tu dispositivo
- NDK descargado dentro de Android-IDE (Configuración → NDK)
- CMake 3.22+ (se descarga automáticamente con el NDK)
- Android 8.0+ (API 26) con soporte Vulkan

### Paso 1: Abrir el proyecto
1. Copia la carpeta `ps2recomp/` a tu almacenamiento interno
2. Abre Android-IDE
3. "Open Project" → selecciona la carpeta `ps2recomp`

### Paso 2: Configurar NDK en Android-IDE
1. Ve a **Configuración → SDK/NDK**
2. Instala NDK r25c o superior
3. Asegúrate de que CMake esté instalado

### Paso 3: Compilar
1. En Android-IDE, selecciona la variante **debug** o **release**
2. Presiona el botón de **Build** (martillo)
3. Espera a que CMake configure y compile el C++
4. Debería generarse `app-debug.apk` en `app/build/outputs/apk/`

### Paso 4: Ejecutar un juego
1. Instala el APK en tu dispositivo
2. Al abrir la app, te pedirá seleccionar una ISO de PS2
3. La app carga el ELF del juego, compila bloques MIPS→ARM64 y ejecuta

---

## Estado de implementación

### ✅ Implementado
| Componente | Archivo | Estado |
|---|---|---|
| ISO Loader (ISO 9660 + ELF) | `iso/iso_loader.cpp` | Completo |
| ARM64 Code Emitter | `include/arm64/arm64_emitter.h` | Completo |
| MIPS R5900 definiciones | `include/ee/mips_defs.h` | Completo |
| Recompilador EE ARM64 | `ee/recompiler_arm64.cpp` | ~80% de instrucciones |
| Code Cache (mmap RWX) | `ee/code_cache.h` | Completo |
| EE Core (dispatcher JIT) | `ee/ee_core.cpp` | Completo |
| GS Core (parser GIF) | `gs/gs_core.cpp` | Completo |
| GS Vulkan Translator | `gs/gs_vulkan.cpp` | Estructura completa |
| SPU2 → OpenSL ES | `spu2/spu2_core.cpp` | ADPCM + mezcla |
| IOP Core (intérprete) | `iop/iop_core.cpp` | Básico funcional |
| VU Core (intérprete) | `vu/vu_core.cpp` | Instrucciones principales |
| DMA Controller | `bus/dma_controller.cpp` | Canal GIF funcional |
| JNI Bridge | `jni_bridge.cpp` | Completo |

### 🔧 Próximos pasos (en orden de prioridad)
1. **Shaders SPIR-V** — compilar los vertex/fragment shaders con glslangValidator
2. **COP1/FPU** — instrucciones de punto flotante del EE (muchos juegos las usan)
3. **Instrucciones MMI** — extensiones exclusivas del EE (PMULT, PADDW, etc.)
4. **VIF unpacker** — descompresión de datos de geometría VIF1 → VU1
5. **DualShock input** — conectar el controller de Android a IOP
6. **BIOS** — para arrancar completamente necesitas la BIOS de PS2 (archivo aparte)

---

## ¿Por qué no es 100% funcional todavía?

### Lo que falta más trabajo:
- **Shaders Vulkan**: los archivos `.spv` se deben generar en el build con `glslangValidator`
- **BIOS de PS2**: el sistema arranca en la BIOS (ROM de 4MB) antes del juego
- **Instrucciones FPU/COP1**: God of War las usa intensivamente
- **Sincronización de timing**: algunos juegos dependen de ciclos exactos entre EE e IOP

### God of War específicamente necesita:
- COP1 FPU completo (transformaciones de cámara y física)
- VU1 microprogramas (toda la geometría 3D)
- GS texture sampling (texturas de los modelos)
- SPU2 streaming (la música del juego)

Todo está estructurado y parcialmente implementado — es cuestión de continuar
implementando instrucción por instrucción y probando con ROMs reales.

---

## Estructura de archivos

```
ps2recomp/
├── app/
│   ├── build.gradle
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── java/com/ps2recomp/
│       │   └── MainActivity.java          ← UI Android, carga ISO
│       └── cpp/
│           ├── CMakeLists.txt             ← configuración de build C++
│           ├── jni_bridge.cpp             ← Java ↔ C++ (entrada principal)
│           ├── include/
│           │   ├── ee/mips_defs.h         ← instrucciones MIPS R5900
│           │   └── arm64/arm64_emitter.h  ← emisor de código ARM64
│           ├── ee/                        ← Emotion Engine
│           │   ├── ee_core.{h,cpp}        ← dispatcher principal
│           │   ├── recompiler_arm64.cpp   ← ★ MIPS → ARM64
│           │   ├── recompiler_arm32.cpp   ← MIPS → ARM32 (stub)
│           │   ├── code_cache.h           ← buffer ejecutable mmap
│           │   ├── ee_memory.h            ← mapa de memoria
│           │   └── mips_disasm.cpp        ← desensamblador (debug)
│           ├── gs/                        ← Graphics Synthesizer
│           │   ├── gs_core.{h,cpp}        ← parser GIF packets
│           │   └── gs_vulkan.{h,cpp}      ← ★ GS → Vulkan
│           ├── spu2/                      ← Audio
│           │   ├── spu2_core.{h,cpp}      ← ★ ADPCM → OpenSL ES
│           │   └── audio_output.cpp
│           ├── iop/                       ← I/O Processor
│           │   ├── iop_core.{h,cpp}       ← intérprete MIPS R3000
│           │   └── iop_recompiler.cpp
│           ├── vu/                        ← Vector Units
│           │   ├── vu_core.{h,cpp}        ← VU0/VU1
│           │   └── vu_recompiler.cpp
│           └── bus/                       ← Bus / DMA
│               ├── dma_controller.{h,cpp} ← canales DMA
│               └── memory_map.cpp
├── build.gradle
└── settings.gradle
```
