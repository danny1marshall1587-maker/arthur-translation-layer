# Arthur Translation Layer
### The Final Hurdle for Linux Audio Production.

Arthur is a **Wayland-native VST3/ARA2 translation layer** designed to make Linux a superior environment for professional audio production. By bridging Windows plugins with native performance and rock-solid stability, Arthur allows you to leave Windows behind without losing your essential tools.

---

## Why Arthur? (The "True Genius" of the Layer)

For years, Linux audio was held back by clunky bridges, flickering GUIs, and broken ARA2 support. Arthur solves these pain points at the architectural level:

### 🚀 Native DSP Performance
Wine is **not an emulator**. Arthur executes your plugin's DSP code directly on your CPU at native speeds. By using our **Lock-Free Audio IPC (`AudioIPC.h`)**, audio data is shared via atomic ring buffers in shared memory, ensuring sub-1ms latency that rivals native Windows performance.

### 🖼️ Wayland-Native UI (Zero Flickering)
Legacy bridges rely on X11/XWayland, which causes black boxes and GUI lag. Arthur binds Windows `HWND` surfaces directly to **native Wayland subsurfaces**. Your **FabFilter**, **Waves**, and **Softube** meters will be as smooth and responsive as they are on Windows.

### 🎹 Solving the "Problem Plugins"
- **ARA2 Support (Melodyne/VocAlign):** Most bridges fail here. Arthur uses deep Wine C-API hooks to translate Unix ↔ DOS paths in real-time, making ARA2 plugins "just work."
- **iLok & DRM:** Our bundled **Wine-GE** runtime is specifically patched to handle iLok, PACE, and Native Access, which typically crash standard Wine.
- **Sandboxed Stability:** Every plugin runs in its own process. If a plugin crashes, it won't take down your DAW.

### 🐧 The Linux Advantage
By moving to Linux with Arthur, you gain:
- **PipeWire & JACK:** Superior, low-latency audio routing that Windows simply cannot match.
- **Lightweight Core:** A stripped-down, high-performance OS that dedicates more CPU cycles to your music.
- **Privacy & Control:** No forced updates or telemetry interrupting your session.

---

## One-Click Installation

Arthur is now distributed as a self-contained **AppImage**. No compiling, no installing Wine, no terminal required.

1.  **[Download the Latest AppImage](https://github.com/danny1marshall1587-maker/arthur-translation-layer/releases/latest)**.
2.  Right-click -> **Properties** -> **Permissions** -> Check **"Allow executing file as program"**.
3.  Double-click to open the **Arthur Manager**.
4.  Click **"Scan & Sync Plugins"** and you're ready to produce.

---

## Quick Start (CLI)

For power users who prefer the terminal:

```bash
# Scan your Wine prefix and create bridge links
./Arthur-x86_64.AppImage sync

# Check status
./Arthur-x86_64.AppImage status
```

---

## Architecture

```
┌─────────────────┐         ┌──────────────────────┐
│   Linux Host    │         │  Wine-GE Process     │
│ (Bitwig/Reaper) │         │                      │
│                 │         │  ┌──────────────────┐ │
│  wl_surface ────┼────┐    │  │ Windows VST3     │ │
│                 │    │    │  │ Plugin (.dll)     │ │
│  AudioIPC ──────┼──┐ │    │  └────────┬─────────┘ │
│  (ring buffer)  │  │ │    │           │           │
└─────────────────┘  │ │    └───────────┼───────────┘
                     │ │                │
              ┌──────┼─┼────────────────┼──┐
              │  Arthur Translation Layer  │
              │                            │
              │  • AraPathTranslator       │
              │    (Unix ↔ DOS paths)      │
              │                            │
              │  • WaylandPluginWindow      │
              │    (HWND → wl_subsurface)  │
              │                            │
              │  • LockFreeAudioQueue      │
              │    (shared memory IPC)     │
              └────────────────────────────┘
```

## License

MIT — Built on the [MIT-licensed VST3 SDK](https://github.com/steinbergmedia/vst3sdk).
