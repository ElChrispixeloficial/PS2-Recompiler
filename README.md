<div align="center">

<!-- Banner SVG -->
<svg width="800" height="200" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:#0f0c29"/>
      <stop offset="50%" style="stop-color:#302b63"/>
      <stop offset="100%" style="stop-color:#24243e"/>
    </linearGradient>
    <linearGradient id="accent" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" style="stop-color:#00d2ff"/>
      <stop offset="100%" style="stop-color:#7b2ff7"/>
    </linearGradient>
    <filter id="glow">
      <feGaussianBlur stdDeviation="3" result="blur"/>
      <feMerge>
        <feMergeNode in="blur"/>
        <feMergeNode in="SourceGraphic"/>
      </feMerge>
    </filter>
  </defs>
  <rect width="800" height="200" rx="16" fill="url(#bg)"/>
  <circle cx="120" cy="100" r="60" fill="none" stroke="url(#accent)" stroke-width="3" opacity="0.3"/>
  <circle cx="120" cy="100" r="45" fill="none" stroke="url(#accent)" stroke-width="2" opacity="0.5"/>
  <circle cx="120" cy="100" r="30" fill="url(#accent)" opacity="0.15"/>
  <text x="120" y="108" text-anchor="middle" font-family="monospace" font-size="28" font-weight="bold" fill="url(#accent)" filter="url(#glow)">PS2</text>
  <text x="400" y="80" text-anchor="middle" font-family="Arial, sans-serif" font-size="42" font-weight="bold" fill="white">PS2-Recompiler</text>
  <text x="400" y="115" text-anchor="middle" font-family="Arial, sans-serif" font-size="18" fill="#a0a0c0">MIPS → ARM64 JIT Recompiler for Android</text>
  <text x="400" y="148" text-anchor="middle" font-family="monospace" font-size="14" fill="#7b2ff7">Native code execution. Zero emulation overhead.</text>
  <line x1="200" y1="170" x2="600" y2="170" stroke="url(#accent)" stroke-width="1" opacity="0.4"/>
</svg>

<br/>

[![License](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Android](https://img.shields.io/badge/Android-8.0%2B-brightgreen.svg)](https://developer.android.com)
[![C++](https://img.shields.io/badge/C%2B%2B-17-orange.svg)](https://isocpp.org)
[![Vulkan](https://img.shields.io/badge/Graphics-Vulkan-red.svg)](https://www.vulkan.org)
[![Kotlin](https://img.shields.io/badge/Kotlin-1.9-purple.svg)](https://kotlinlang.org)

</div>

---

## What is PS2-Recompiler?

A PlayStation 2 emulator for Android that **recompiles** MIPS R5900/IOP R3000 machine code directly to ARM64 native instructions at runtime. No interpretation overhead — your PS2 games run as native ARM code on your phone.

<br/>

<!-- Features SVG -->
<svg width="800" height="340" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="card-bg" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" style="stop-color:#1a1a2e"/>
      <stop offset="100%" style="stop-color:#16213e"/>
    </linearGradient>
  </defs>
  
  <!-- Card 1: JIT -->
  <rect x="20" y="20" width="240" height="140" rx="12" fill="url(#card-bg)" stroke="#7b2ff7" stroke-width="1.5"/>
  <text x="140" y="55" text-anchor="middle" font-family="Arial" font-size="13" fill="#7b2ff7" font-weight="bold">JIT RECOMPILATION</text>
  <text x="140" y="80" text-anchor="middle" font-family="monospace" font-size="11" fill="#00d2ff">MIPS → ARM64</text>
  <text x="140" y="100" text-anchor="middle" font-family="monospace" font-size="11" fill="#888">ADD, MULT, LW, BEQ...</text>
  <text x="140" y="125" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">Runtime recompilation with</text>
  <text x="140" y="140" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">executable code cache (mmap RWX)</text>

  <!-- Card 2: Vulkan -->
  <rect x="280" y="20" width="240" height="140" rx="12" fill="url(#card-bg)" stroke="#e74c3c" stroke-width="1.5"/>
  <text x="400" y="55" text-anchor="middle" font-family="Arial" font-size="13" fill="#e74c3c" font-weight="bold">VULKAN RENDERING</text>
  <text x="400" y="80" text-anchor="middle" font-family="monospace" font-size="11" fill="#00d2ff">GS → Vulkan Pipeline</text>
  <text x="400" y="100" text-anchor="middle" font-family="monospace" font-size="11" fill="#888">SPRITE, TRI, GIF packets</text>
  <text x="400" y="125" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">Hardware-accelerated graphics</text>
  <text x="400" y="140" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">with SPIR-V shaders</text>

  <!-- Card 3: Audio -->
  <rect x="540" y="20" width="240" height="140" rx="12" fill="url(#card-bg)" stroke="#2ecc71" stroke-width="1.5"/>
  <text x="660" y="55" text-anchor="middle" font-family="Arial" font-size="13" fill="#2ecc71" font-weight="bold">SPU2 AUDIO</text>
  <text x="660" y="80" text-anchor="middle" font-family="monospace" font-size="11" fill="#00d2ff">ADPCM → PCM</text>
  <text x="660" y="100" text-anchor="middle" font-family="monospace" font-size="11" fill="#888">24 voices, reverb, pitch</text>
  <text x="660" y="125" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">OpenSL ES output with</text>
  <text x="660" y="140" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">low-latency audio mixing</text>

  <!-- Card 4: Multi-core -->
  <rect x="20" y="180" width="240" height="140" rx="12" fill="url(#card-bg)" stroke="#f39c12" stroke-width="1.5"/>
  <text x="140" y="215" text-anchor="middle" font-family="Arial" font-size="13" fill="#f39c12" font-weight="bold">PS2 ARCHITECTURE</text>
  <text x="140" y="240" text-anchor="middle" font-family="monospace" font-size="11" fill="#00d2ff">EE + IOP + VU + GS</text>
  <text x="140" y="260" text-anchor="middle" font-family="monospace" font-size="11" fill="#888">DMA channels, memory map</text>
  <text x="140" y="285" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">Full system emulation with</text>
  <text x="140" y="300" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">accurate hardware timing</text>

  <!-- Card 5: Native -->
  <rect x="280" y="180" width="240" height="140" rx="12" fill="url(#card-bg)" stroke="#9b59b6" stroke-width="1.5"/>
  <text x="400" y="215" text-anchor="middle" font-family="Arial" font-size="13" fill="#9b59b6" font-weight="bold">ARM64 OPTIMIZED</text>
  <text x="400" y="240" text-anchor="middle" font-family="monospace" font-size="11" fill="#00d2ff">NEON SIMD, SVE</text>
  <text x="400" y="260" text-anchor="middle" font-family="monospace" font-size="11" fill="#888">armv8-a+simd, O3 -ffast-math</text>
  <text x="400" y="285" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">ARM32 fallback with NEON</text>
  <text x="400" y="300" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">support for older devices</text>

  <!-- Card 6: Android -->
  <rect x="540" y="180" width="240" height="140" rx="12" fill="url(#card-bg)" stroke="#3498db" stroke-width="1.5"/>
  <text x="660" y="215" text-anchor="middle" font-family="Arial" font-size="13" fill="#3498db" font-weight="bold">ANDROID NATIVE</text>
  <text x="660" y="240" text-anchor="middle" font-family="monospace" font-size="11" fill="#00d2ff">Kotlin + JNI + NDK</text>
  <text x="660" y="260" text-anchor="middle" font-family="monospace" font-size="11" fill="#888">API 26+, Landscape mode</text>
  <text x="660" y="285" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">Material Design UI with</text>
  <text x="660" y="300" text-anchor="middle" font-family="Arial" font-size="10" fill="#666">game library management</text>
</svg>

---

## Architecture

<!-- Architecture Diagram SVG -->
<svg width="800" height="520" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="module-bg" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" style="stop-color:#1e1e3f"/>
      <stop offset="100%" style="stop-color:#12122a"/>
    </linearGradient>
    <marker id="arrow" markerWidth="10" markerHeight="7" refX="9" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#00d2ff"/>
    </marker>
    <marker id="arrow-gif" markerWidth="10" markerHeight="7" refX="9" refY="3.5" orient="auto">
      <polygon points="0 0, 10 3.5, 0 7" fill="#f39c12"/>
    </marker>
  </defs>

  <!-- ISO Loader -->
  <rect x="20" y="20" width="180" height="70" rx="10" fill="url(#module-bg)" stroke="#7b2ff7" stroke-width="2"/>
  <text x="110" y="45" text-anchor="middle" font-family="monospace" font-size="12" fill="#7b2ff7" font-weight="bold">ISO Loader</text>
  <text x="110" y="65" text-anchor="middle" font-family="Arial" font-size="10" fill="#888">ISO 9660 → ELF</text>

  <!-- Arrow: ISO → EE -->
  <line x1="110" y1="90" x2="110" y2="140" stroke="#00d2ff" stroke-width="2" marker-end="url(#arrow)"/>

  <!-- EE Core (big block) -->
  <rect x="20" y="140" width="360" height="200" rx="14" fill="url(#module-bg)" stroke="#00d2ff" stroke-width="2.5"/>
  <text x="200" y="170" text-anchor="middle" font-family="Arial" font-size="16" fill="#00d2ff" font-weight="bold">Emotion Engine (R5900)</text>
  <text x="200" y="192" text-anchor="middle" font-family="Arial" font-size="11" fill="#666">294 MHz — MIPS → ARM64 JIT</text>

  <!-- Sub-blocks inside EE -->
  <rect x="40" y="210" width="150" height="50" rx="6" fill="#0d0d20" stroke="#333" stroke-width="1"/>
  <text x="115" y="230" text-anchor="middle" font-family="monospace" font-size="10" fill="#2ecc71">recompiler_arm64.cpp</text>
  <text x="115" y="248" text-anchor="middle" font-family="Arial" font-size="9" fill="#666">MIPS → ARM64</text>

  <rect x="210" y="210" width="150" height="50" rx="6" fill="#0d0d20" stroke="#333" stroke-width="1"/>
  <text x="285" y="230" text-anchor="middle" font-family="monospace" font-size="10" fill="#2ecc71">code_cache.cpp</text>
  <text x="285" y="248" text-anchor="middle" font-family="Arial" font-size="9" fill="#666">mmap RWX blocks</text>

  <rect x="40" y="275" width="150" height="50" rx="6" fill="#0d0d20" stroke="#333" stroke-width="1"/>
  <text x="115" y="295" text-anchor="middle" font-family="monospace" font-size="10" fill="#f39c12">VU0 / VU1</text>
  <text x="115" y="313" text-anchor="middle" font-family="Arial" font-size="9" fill="#666">Vector Units (SIMD)</text>

  <rect x="210" y="275" width="150" height="50" rx="6" fill="#0d0d20" stroke="#333" stroke-width="1"/>
  <text x="285" y="295" text-anchor="middle" font-family="monospace" font-size="10" fill="#f39c12">DMA Controller</text>
  <text x="285" y="313" text-anchor="middle" font-family="Arial" font-size="9" fill="#666">GIF, VIF channels</text>

  <!-- Arrow: EE → IOP -->
  <line x1="200" y1="340" x2="200" y2="380" stroke="#00d2ff" stroke-width="2" marker-end="url(#arrow)"/>

  <!-- IOP Core -->
  <rect x="80" y="380" width="240" height="60" rx="10" fill="url(#module-bg)" stroke="#e67e22" stroke-width="2"/>
  <text x="200" y="405" text-anchor="middle" font-family="monospace" font-size="12" fill="#e67e22" font-weight="bold">IOP Core (R3000A)</text>
  <text x="200" y="422" text-anchor="middle" font-family="Arial" font-size="10" fill="#888">36 MHz — Interpreter</text>

  <!-- Arrow: IOP → SPU2 -->
  <line x1="200" y1="440" x2="200" y2="470" stroke="#00d2ff" stroke-width="2" marker-end="url(#arrow)"/>

  <!-- SPU2 -->
  <rect x="100" y="470" width="200" height="45" rx="8" fill="url(#module-bg)" stroke="#2ecc71" stroke-width="2"/>
  <text x="200" y="493" text-anchor="middle" font-family="monospace" font-size="11" fill="#2ecc71">SPU2 → OpenSL ES</text>
  <text x="200" y="508" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">ADPCM decode + audio</text>

  <!-- GIF Arrow to GS -->
  <line x1="380" y1="240" x2="460" y2="240" stroke="#f39c12" stroke-width="2" stroke-dasharray="6,3" marker-end="url(#arrow-gif)"/>
  <text x="420" y="232" text-anchor="middle" font-family="monospace" font-size="9" fill="#f39c12">GIF</text>

  <!-- GS Core -->
  <rect x="460" y="170" width="320" height="100" rx="14" fill="url(#module-bg)" stroke="#e74c3c" stroke-width="2.5"/>
  <text x="620" y="200" text-anchor="middle" font-family="Arial" font-size="14" fill="#e74c3c" font-weight="bold">Graphics Synthesizer</text>

  <rect x="480" y="215" width="130" height="40" rx="6" fill="#0d0d20" stroke="#333" stroke-width="1"/>
  <text x="545" y="235" text-anchor="middle" font-family="monospace" font-size="9" fill="#e74c3c">gs_core.cpp</text>
  <text x="545" y="248" text-anchor="middle" font-family="Arial" font-size="8" fill="#666">GIF parser</text>

  <rect x="630" y="215" width="130" height="40" rx="6" fill="#0d0d20" stroke="#333" stroke-width="1"/>
  <text x="695" y="235" text-anchor="middle" font-family="monospace" font-size="9" fill="#e74c3c">gs_vulkan.cpp</text>
  <text x="695" y="248" text-anchor="middle" font-family="Arial" font-size="8" fill="#666">SPIR-V shaders</text>

  <!-- Arrow: GS → Screen -->
  <line x1="620" y1="270" x2="620" y2="310" stroke="#e74c3c" stroke-width="2" marker-end="url(#arrow)"/>

  <!-- Screen output -->
  <rect x="530" y="310" width="180" height="50" rx="10" fill="url(#module-bg)" stroke="#9b59b6" stroke-width="2"/>
  <text x="620" y="335" text-anchor="middle" font-family="Arial" font-size="12" fill="#9b59b6" font-weight="bold">VkQueue → Screen</text>
  <text x="620" y="352" text-anchor="middle" font-family="Arial" font-size="10" fill="#888">Android SurfaceView</text>

  <!-- Legend -->
  <rect x="500" y="390" width="280" height="110" rx="8" fill="none" stroke="#333" stroke-width="1"/>
  <text x="640" y="412" text-anchor="middle" font-family="Arial" font-size="11" fill="#888">Legend</text>
  <line x1="520" y1="430" x2="550" y2="430" stroke="#00d2ff" stroke-width="2"/>
  <text x="560" y="434" font-family="Arial" font-size="10" fill="#888">Data flow</text>
  <line x1="520" y1="455" x2="550" y2="455" stroke="#f39c12" stroke-width="2" stroke-dasharray="6,3"/>
  <text x="560" y="459" font-family="Arial" font-size="10" fill="#888">GIF packets</text>
  <rect x="520" y="472" width="20" height="12" rx="3" fill="#0d0d20" stroke="#333" stroke-width="1"/>
  <text x="560" y="483" font-family="Arial" font-size="10" fill="#888">Internal module</text>
</svg>

---

## Requirements

| Requirement | Details |
|-------------|---------|
| **Android** | 8.0+ (API 26) with Vulkan 1.1 |
| **Architecture** | ARM64 (arm64-v8a) or ARM32 (armeabi-v7a) |
| **NDK** | r25c or newer |
| **CMake** | 3.22+ |
| **Disk Space** | ~50MB APK + game ISOs |
| **RAM** | 2GB+ recommended |

---

## Building

### Option 1: Android Studio / Android-IDE
```bash
# Clone the repository
git clone https://github.com/ElChrispixeloficial/PS2-Recompiler.git

# Open in Android Studio and build
# Or use command line:
./gradlew assembleDebug
```

### Option 2: Termux / CLI
```bash
# Install prerequisites
pkg install git cmake

# Clone and build
git clone https://github.com/ElChrispixeloficial/PS2-Recompiler.git
cd PS2-Recompiler
./gradlew assembleDebug

# APK output
ls app/build/outputs/apk/debug/app-debug.apk
```

---

## Usage

1. **Install** the APK on your Android device
2. **Select BIOS** — Tap "Selecciona BIOS oficial" and pick your `scph-XXXX.bin` file
3. **Add Games** — Tap the `+` button and select a PS2 ISO file
4. **Play** — Tap on any game in your library to launch

> **Note:** You need a legitimate PS2 BIOS file (`scph-10000.bin` through `scph-39001.bin`). This software does not include BIOS files.

---

## Project Structure

```
PS2-Recompiler/
├── app/
│   ├── src/main/
│   │   ├── java/com/chrispixel/ps2recompiler/
│   │   │   ├── MainActivity.kt           # Game library UI
│   │   │   ├── RuntimeActivity.kt        # Emulator screen
│   │   │   ├── TestActivity.kt           # Debug/test activity
│   │   │   ├── GameLibrary.kt            # Persistent game list
│   │   │   └── GameLibraryAdapter.kt     # RecyclerView adapter
│   │   └── cpp/
│   │       ├── CMakeLists.txt            # Build configuration
│   │       ├── jni_bridge.cpp            # JNI entry point
│   │       ├── ee/                       # Emotion Engine
│   │       │   ├── ee_core.cpp           # JIT dispatcher
│   │       │   ├── recompiler_arm64.cpp  # ★ MIPS → ARM64
│   │       │   ├── recompiler_arm32.cpp  # MIPS → ARM32
│   │       │   ├── code_cache.cpp        # Executable mmap cache
│   │       │   └── ee_memory.cpp         # Memory map
│   │       ├── gs/                       # Graphics Synthesizer
│   │       │   ├── gs_core.cpp           # GIF packet parser
│   │       │   ├── gs_vulkan.cpp         # ★ GS → Vulkan
│   │       │   └── vulkan_context.cpp    # Vulkan setup
│   │       ├── spu2/                     # Sound Processing
│   │       │   └── spu2_core.cpp         # ADPCM → PCM
│   │       ├── iop/                      # I/O Processor
│   │       │   └── iop_core.cpp          # R3000 interpreter
│   │       ├── vu/                       # Vector Units
│   │       │   └── vu_core.cpp           # VU0/VU1
│   │       └── bus/                      # DMA & Memory
│   │           └── dma_controller.cpp    # DMA channels
├── build.gradle.kts
└── gradlew
```

---

## Implementation Status

<!-- Status SVG -->
<svg width="800" height="300" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="done" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" style="stop-color:#1a3a1a"/>
      <stop offset="100%" style="stop-color:#0d1f0d"/>
    </linearGradient>
    <linearGradient id="wip" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" style="stop-color:#3a2a0a"/>
      <stop offset="100%" style="stop-color:#1f1505"/>
    </linearGradient>
    <linearGradient id="todo" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" style="stop-color:#2a1a1a"/>
      <stop offset="100%" style="stop-color:#1f0d0d"/>
    </linearGradient>
  </defs>

  <!-- Header -->
  <text x="400" y="30" text-anchor="middle" font-family="Arial" font-size="18" fill="white" font-weight="bold">Implementation Status</text>

  <!-- Done items -->
  <rect x="20" y="50" width="380" height="200" rx="10" fill="url(#done)" stroke="#2ecc71" stroke-width="1.5"/>
  <text x="210" y="75" text-anchor="middle" font-family="Arial" font-size="13" fill="#2ecc71" font-weight="bold">✓ COMPLETED</text>
  
  <circle cx="45" cy="100" r="6" fill="#2ecc71"/>
  <text x="60" y="104" font-family="monospace" font-size="11" fill="#aaa">ISO Loader (ISO 9660 + ELF)</text>

  <circle cx="45" cy="125" r="6" fill="#2ecc71"/>
  <text x="60" y="129" font-family="monospace" font-size="11" fill="#aaa">ARM64 Code Emitter</text>

  <circle cx="45" cy="150" r="6" fill="#2ecc71"/>
  <text x="60" y="154" font-family="monospace" font-size="11" fill="#aaa">MIPS R5900 Definitions</text>

  <circle cx="45" cy="175" r="6" fill="#2ecc71"/>
  <text x="60" y="179" font-family="monospace" font-size="11" fill="#aaa">Code Cache (mmap RWX)</text>

  <circle cx="45" cy="200" r="6" fill="#2ecc71"/>
  <text x="60" y="204" font-family="monospace" font-size="11" fill="#aaa">EE Core (JIT Dispatcher)</text>

  <circle cx="45" cy="225" r="6" fill="#2ecc71"/>
  <text x="60" y="229" font-family="monospace" font-size="11" fill="#aaa">GS Core + Vulkan Structure</text>

  <!-- WIP items -->
  <rect x="420" y="50" width="370" height="100" rx="10" fill="url(#wip)" stroke="#f39c12" stroke-width="1.5"/>
  <text x="605" y="75" text-anchor="middle" font-family="Arial" font-size="13" fill="#f39c12" font-weight="bold">⚙ IN PROGRESS</text>

  <circle cx="445" cy="100" r="6" fill="#f39c12"/>
  <text x="460" y="104" font-family="monospace" font-size="11" fill="#aaa">EE Recompiler (~80% instructions)</text>

  <circle cx="445" cy="125" r="6" fill="#f39c12"/>
  <text x="460" y="129" font-family="monospace" font-size="11" fill="#aaa">SPU2 (ADPCM + mixing done)</text>

  <!-- Todo items -->
  <rect x="420" y="170" width="370" height="80" rx="10" fill="url(#todo)" stroke="#e74c3c" stroke-width="1.5"/>
  <text x="605" y="195" text-anchor="middle" font-family="Arial" font-size="13" fill="#e74c3c" font-weight="bold">⏳ TODO</text>

  <circle cx="445" cy="215" r="6" fill="#e74c3c"/>
  <text x="460" y="219" font-family="monospace" font-size="11" fill="#aaa">COP1/FPU, MMI, VIF, DualShock</text>

  <circle cx="445" cy="240" r="6" fill="#e74c3c"/>
  <text x="460" y="244" font-family="monospace" font-size="11" fill="#aaa">SPIR-V shader compilation</text>

  <!-- Legend -->
  <circle cx="100" cy="275" r="5" fill="#2ecc71"/>
  <text x="115" y="279" font-family="Arial" font-size="10" fill="#666">Done</text>
  <circle cx="180" cy="275" r="5" fill="#f39c12"/>
  <text x="195" y="279" font-family="Arial" font-size="10" fill="#666">In Progress</text>
  <circle cx="300" cy="275" r="5" fill="#e74c3c"/>
  <text x="315" y="279" font-family="Arial" font-size="10" fill="#666">Todo</text>
</svg>

| Component | Status | Notes |
|-----------|--------|-------|
| ISO Loader | ✅ Done | ISO 9660 + ELF parsing |
| ARM64 Emitter | ✅ Done | Full instruction set |
| EE Recompiler | ⚙️ ~80% | Core MIPS instructions |
| Code Cache | ✅ Done | mmap RWX blocks |
| GS Vulkan | ⚙️ Partial | Structure complete, shaders needed |
| SPU2 | ⚙️ Partial | ADPCM + mixing done |
| IOP Core | ✅ Done | Basic interpreter functional |
| VU Core | ⚙️ Partial | Main instructions only |
| DMA Controller | ✅ Done | GIF channel functional |

### Next Steps (Priority Order)
1. **SPIR-V Shaders** — Compile with `glslangValidator`
2. **COP1/FPU** — Floating point instructions (used by many games)
3. **MMI Instructions** — EE-specific extensions (PMULT, PADDW, etc.)
4. **VIF Unpacker** — Geometry data decompression VIF1 → VU1
5. **DualShock Input** — Connect Android controller to IOP
6. **BIOS Boot** — Full BIOS ROM support (4MB)

---

## Tech Stack

<!-- Tech Stack SVG -->
<svg width="800" height="120" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="tech-bg" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" style="stop-color:#1a1a2e"/>
      <stop offset="100%" style="stop-color:#16213e"/>
    </linearGradient>
  </defs>
  
  <rect x="20" y="10" width="100" height="100" rx="10" fill="url(#tech-bg)" stroke="#4285F4" stroke-width="1.5"/>
  <text x="70" y="55" text-anchor="middle" font-family="monospace" font-size="12" fill="#4285F4" font-weight="bold">C++17</text>
  <text x="70" y="75" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">Core engine</text>

  <rect x="140" y="10" width="100" height="100" rx="10" fill="url(#tech-bg)" stroke="#7F52FF" stroke-width="1.5"/>
  <text x="190" y="55" text-anchor="middle" font-family="monospace" font-size="12" fill="#7F52FF" font-weight="bold">Kotlin</text>
  <text x="190" y="75" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">Android UI</text>

  <rect x="260" y="10" width="100" height="100" rx="10" fill="url(#tech-bg)" stroke="#E74C3C" stroke-width="1.5"/>
  <text x="310" y="55" text-anchor="middle" font-family="monospace" font-size="12" fill="#E74C3C" font-weight="bold">Vulkan</text>
  <text x="310" y="75" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">GPU rendering</text>

  <rect x="380" y="10" width="100" height="100" rx="10" fill="url(#tech-bg)" stroke="#2ECC71" stroke-width="1.5"/>
  <text x="430" y="55" text-anchor="middle" font-family="monospace" font-size="12" fill="#2ECC71" font-weight="bold">JNI/NDK</text>
  <text x="430" y="75" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">Native bridge</text>

  <rect x="500" y="10" width="100" height="100" rx="10" fill="url(#tech-bg)" stroke="#F39C12" stroke-width="1.5"/>
  <text x="550" y="55" text-anchor="middle" font-family="monospace" font-size="12" fill="#F39C12" font-weight="bold">ARM64</text>
  <text x="550" y="75" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">JIT target</text>

  <rect x="620" y="10" width="100" height="100" rx="10" fill="url(#tech-bg)" stroke="#9B59B6" stroke-width="1.5"/>
  <text x="670" y="55" text-anchor="middle" font-family="monospace" font-size="12" fill="#9B59B6" font-weight="bold">OpenSL</text>
  <text x="670" y="75" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">Audio output</text>

  <rect x="740" y="10" width="100" height="100" rx="10" fill="url(#tech-bg)" stroke="#3498DB" stroke-width="1.5"/>
  <text x="790" y="55" text-anchor="middle" font-family="monospace" font-size="12" fill="#3498DB" font-weight="bold">Gradle</text>
  <text x="790" y="75" text-anchor="middle" font-family="Arial" font-size="9" fill="#888">Build system</text>
</svg>

---

## Contributing

Contributions are welcome! Here's how to get started:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

### Development Areas
- **Recompiler** — Add missing MIPS instructions in `recompiler_arm64.cpp`
- **Graphics** — Implement GS→Vulkan translation in `gs_vulkan.cpp`
- **Audio** — Improve SPU2 mixing and effects in `spu2_core.cpp`
- **Input** — Add DualShock controller support

---

## License

This project is licensed under the GNU General Public License v3.0 — see the [LICENSE](LICENSE) file for details.

---

<div align="center">

<!-- Footer SVG -->
<svg width="800" height="80" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="footer-bg" x1="0%" y1="0%" x2="100%" y2="0%">
      <stop offset="0%" style="stop-color:#0f0c29"/>
      <stop offset="50%" style="stop-color:#302b63"/>
      <stop offset="100%" style="stop-color:#24243e"/>
    </linearGradient>
  </defs>
  <rect width="800" height="80" rx="12" fill="url(#footer-bg)"/>
  <text x="400" y="35" text-anchor="middle" font-family="Arial" font-size="14" fill="#888">Built with passion for PS2 preservation</text>
  <text x="400" y="55" text-anchor="middle" font-family="monospace" font-size="11" fill="#7b2ff7">ElChrispixeloficial/PS2-Recompiler</text>
  <line x1="250" y1="70" x2="550" y2="70" stroke="#7b2ff7" stroke-width="1" opacity="0.3"/>
</svg>

</div>
