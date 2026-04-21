# Arthur Translation Layer

A **Wayland-native VST3/ARA2 translation layer** for running Windows audio plugins on Linux, built on Wine 11 and the MIT-licensed VST3 SDK.

Arthur replaces legacy X11/XWayland bridging with direct Wayland subsurface attachment, eliminating the flickering, focus-stealing, and scaling bugs that plague existing solutions.

## Features

- **Native Wayland windowing** — plugin GUIs attach directly as `wl_subsurface` children of your DAW window
- **ARA2 path translation** — seamless Unix ↔ DOS file path conversion using Wine's internal APIs (no slow `winepath` shelling)
- **Lock-free audio IPC** — zero-allocation, atomic ring buffer for sub-1ms latency between the Linux host and Wine plugin process
- **One-click CLI scanner** — automatically discovers Windows VST3 plugins in your Wine prefix and creates bridge links

## Quick Start

### Download

Grab the latest build from the [Releases](https://github.com/danny1marshall1587-maker/arthur-translation-layer/releases) page, or download the artifact from the latest [Actions](https://github.com/danny1marshall1587-maker/arthur-translation-layer/actions) run.

### Install

```bash
tar -xzf arthur-bridge-linux-x86_64.tar.gz
cd arthur-bridge

# Scan your Wine prefix for Windows VST3 plugins and create bridge links
python3 arthur-cli.py sync
```

### CLI Commands

| Command | Description |
|---------|-------------|
| `python3 arthur-cli.py sync` | Scan Wine VST3 folder and create new bridge links |
| `python3 arthur-cli.py status` | Show currently bridged plugins |
| `python3 arthur-cli.py clean` | Remove all bridge links |
| `python3 arthur-cli.py resync` | Clean + sync (full refresh) |

### Build From Source

```bash
sudo apt install build-essential cmake libwayland-dev wayland-protocols pkg-config
git clone https://github.com/danny1marshall1587-maker/arthur-translation-layer.git
cd arthur-translation-layer
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The VST3 SDK is downloaded automatically during the CMake configure step.

## Architecture

```
┌─────────────────┐         ┌──────────────────────┐
│   Linux DAW     │         │  Wine 11 Process     │
│  (Bitwig/Reaper)│         │                      │
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
