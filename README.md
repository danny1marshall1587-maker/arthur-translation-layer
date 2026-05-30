# Arthur Translation Layer
### The Final Hurdle for Linux Audio Production.

> **v4.0 — Virtual Hybrid Console (VHC) Edition**

Arthur is a **Wayland-native VST3/ARA2 translation layer** and **Virtual Hybrid Console** for professional Linux audio production. It bridges Windows VST3 plugins into your Linux DAW with native DSP performance, sub-millisecond latency, hardware-accurate MIDI alignment, and — uniquely — a **real-time monitor / record routing engine** that works just like the channel insert section of a professional hardware mixing console.

---

## What Is Arthur?

For years, Linux audio was held back by clunky bridges, flickering GUIs, and broken ARA2 support. Arthur solves all of these at the architectural level, and goes further — it introduces **virtual DSP isolation** and a **hardware-style console routing matrix** that does not exist anywhere else in the Linux audio ecosystem.

---

## Architecture Overview

```
┌────────────────────────────────────────────────────────────────────────────┐
│                           ARTHUR SYSTEM STACK                              │
├────────────────────────────────────────────────────────────────────────────┤
│  DAW / HOST (Reaper, Ardour, Bitwig, etc.)                                 │
│      │  VST3 Plugin Slot                                                   │
│      ▼                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  ArthurBridge (arthur_bridge.vst3)                                  │   │
│  │  • Decoupled Timing Horizon: Lagrange fractional delay line         │   │
│  │  • Console Mode routing: Playback / Dry+Mon / Record Wet            │   │
│  │  • Pre-rendered ASIO-Guard playback summing (sum_aligned_playback)  │   │
│  │  • Audio → Lock-Free SHM (AudioSharedMemory / AudioIPC.h)          │   │
│  └────────────────────────┬────────────────────────────────────────────┘   │
│                           │  /dev/shm/ArthurAudioIPC<slot>                 │
│  ┌────────────────────────▼────────────────────────────────────────────┐   │
│  │  arthur-daemon (UNIX socket /run/user/.../arthur.sock)              │   │
│  │  • Manages plugin process lifecycle (LOAD / UNLOAD / LIST_SHM)     │   │
│  │  • CONSOLE_MODE command: writes console_mode field to live SHM      │   │
│  │  • VDC slot allocation on isolated CPU cores (cpuset / cgroups)    │   │
│  │  • QEMU VM launch bridge for Multiplexed PCI DSP pass-through      │   │
│  └─────────────────┬─────────────────────────────────────────────────-─┘   │
│                    │                                                        │
│  ┌─────────────────▼───────────────────────┐                               │
│  │  Wine-GE Process (win_guest_agent)       │                               │
│  │  • Reads AudioSharedMemory via SHM       │                               │
│  │  • Runs Windows VST3 DSP natively        │                               │
│  │  • Bridges MIDI in/out events            │                               │
│  │  • Queries HWND → XID for GUI bridging   │                               │
│  └─────────────────────────────────────────┘                               │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  Closed-Loop Latency Sync (pw_module_clls + midi_sync)               │ │
│  │  • Per-device measured RTT via PipeWire loopback                     │ │
│  │  • global_target_rtt: frozen on first device handshake               │ │
│  │  • ALSA sequencer pre-compensation: MIDI events arrive bit-perfect   │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌─────────────────────────────────────────┐                               │
│  │  Arthur Control Center (Qt6 GUI)         │                               │
│  │  • Dashboard, Installer, DSP Rack        │                               │
│  │  • Audio Setup (PipeWire / CLLS)         │                               │
│  │  • VHC Console ← NEW in v4.0            │                               │
│  └─────────────────────────────────────────┘                               │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Key Features

### 🚀 Native DSP Performance
Wine executes your plugin's DSP code **directly on the CPU at bare-metal speed**. Audio data is exchanged via `AudioIPC.h` — a lock-free shared memory ring buffer using custom `ArthurAtomic<T>` intrinsics that are binary-compatible across the GCC/Wine boundary.

### 🔒 Ultra-Stable Timing: Decoupled Timing Horizon
Every channel runs a **Lagrange fractional delay line** that compensates for per-device round-trip latency differences. When the first audio device connects, the system measures its RTT and freezes a `global_target_rtt` — all subsequent devices align to this static horizon. Jitter is clamped at the buffer boundary; latency never drifts.

### 🎚 Virtual Hybrid Console (VHC) — New in v4.0
A **hardware-style channel routing engine** built directly into the VST3 plugin process. Each channel slot can be independently set to one of three modes from the Arthur Control Center GUI:

| Mode | Name | Behaviour |
|------|------|-----------|
| **0** | **Playback** | Processed (wet) signal sent to DAW main output. Default. |
| **1** | **Dry + Monitor Wet** | Raw dry input sent to main output (for recording). Wet signal routed to monitor bus so the performer still hears effects live. Like a hardware insert with direct monitoring. |
| **2** | **Record Wet** | Processed wet signal sent to DAW main output. Bakes effects into the recorded audio. |

Mode is controlled via `AudioSharedMemory::console_mode` (atomic, race-free). The GUI writes it live through the daemon's `CONSOLE_MODE` command — the plugin process picks it up with `memory_order_acquire` on the next audio callback.

### 🎹 ALSA MIDI Clock Alignment
`midi_sync` slaves the ALSA sequencer tick to the PipeWire PCM hardware clock. MIDI events are pre-compensated by the measured `global_target_rtt`, so note-on arrives at the audio engine at the exact sample it was scheduled — **< 10 µs jitter** on a tuned CachyOS system.

### 🖥️ Virtual DSP Cores (VDC)
Plugins are pre-loaded into **isolated CPU cores** (e.g., cores 4–7 on a Ryzen system). The multiplexed memory mapper allows a single PCIe slot's DRAM to appear as multiple independent VDC regions — enabling a full virtual rack without additional hardware.

### 🖼️ Wayland-Native GUI (Zero Flickering)
Plugin GUIs are reparented as **native Wayland subsurfaces** via the XDG foreign protocol. No X11 compositing lag, no black boxes.

### 🔧 ARA2, iLok, and Problem Plugin Support
- **ARA2** (Melodyne, VocAlign): Real-time Unix↔DOS path translation via `AraPathTranslator`
- **iLok / PACE / Native Access**: Handled by the bundled Wine-GE runtime patches
- **Sandboxed Stability**: Each plugin is its own process — a crash cannot affect the DAW

---

## Installation (Flatpak — Recommended)

Arthur ships as a fully self-contained Flatpak:

```bash
# Install
flatpak install --user arthur-translation-layer.flatpak

# Launch GUI
flatpak run org.arthur.TranslationLayer
```

Or download from the [latest release](https://github.com/danny1marshall1587-maker/arthur-translation-layer/releases/latest).

---

## CachyOS One-Click Setup

For the best possible performance, run the included auto-tuner after installing:

1. Open **Arthur Control Center**
2. Navigate to **Audio Setup → System Tuner**
3. Click **Run CachyOS System Tuner**

The tuner script (`arthur_tune_cachyos.sh`) applies:
- `isolcpus` and `nohz_full` kernel parameters for dedicated DSP cores
- `rtirq` + `threadirqs` for hardware IRQ isolation  
- PipeWire real-time tuning (`/etc/pipewire/pipewire.conf.d/`)
- `vm.swappiness=10` and `transparent_hugepages=madvise`
- `limits.conf` MEMLOCK and RTPRIO for your user

> **Requires CachyOS or Arch Linux.** A reboot is needed after tuning to apply kernel parameters.

---

## GUI Walkthrough

### Dashboard
Live monitoring of all system layers:
- **VDC Load** — Real-time DSP core utilisation
- **CLLS Lock** — Measured hardware RTT, phase correction, and jitter
- **ALSA MIDI Alignment** — Live jitter histogram

### Install Plugins
Drag & Drop a Windows `.exe` or `.msi` installer directly into the Arthur window. The installer runs inside the bundled Wine-GE sandbox. Once complete, the plugin appears automatically in your DAW as a bridged VST3.

### Virtual DSP Rack
Pre-load plugins into isolated CPU core slots. Multiple plugins can share a single PCIe memory space via the VDC multiplexer. Configurations are saved as named profiles (`.json`).

### Audio Setup
Configure the PipeWire backend (sample rate, buffer size, interface), run CLLS calibration loops for up to 3 audio interfaces simultaneously, and manage ALSA MIDI clock slaving.

### VHC Console ← New in v4.0
Set per-channel console mode independently for every active Arthur plugin slot. Click **Scan SHM Channels** to discover all running plugin instances. Use the dropdown on each row to switch between **Playback**, **Dry + Monitor Wet**, and **Record Wet** — changes take effect on the next audio callback with zero dropouts.

---

## Quick Start (CLI)

```bash
# Scan Wine prefix for installed plugins and create bridge links
./Arthur-x86_64.AppImage sync

# Check status of running plugin processes
./Arthur-x86_64.AppImage status
```

---

## Technical Deep Dive

### Closed-Loop Latency Sync (CLLS)
CLLS measures the **full round-trip time** of audio through each physical hardware interface using a PipeWire loopback. Up to 8 devices are supported simultaneously via the `CLLSSyncLayout` shared memory segment:

```
CLLSSyncLayout {
    version: 4
    owner_pid[8]       — PID of each measuring process
    measured_rtt[8]    — Per-device RTT in samples
    global_target_rtt  — Frozen max RTT across all devices (+ 128 samples headroom)
}
```

The VST3 bridge reads `global_target_rtt`, computes `target_delay = global_target_rtt - own_rtt`, and applies this as the fractional delay line tap. All devices arrive sample-aligned.

### Shared Memory Layout (`AudioSharedMemory`)
```
offset  field                       purpose
  0     version                     Layout mismatch detection (v5)
 64     state                       Transport FSM (IDLE / HOST_WRITTEN / GUEST_PROCESSED / ERROR)
        sample_count, num_i/o       Block metadata
        sample_rate, playhead_pos   Timing
        input_buffers[32][4096]     32-channel input (host → guest)
        output_buffers[32][4096]    32-channel output (guest → host)
        pre_rendered_playback[32]   ASIO-Guard pre-render pool
        midi_in[256], midi_out[256] Sample-accurate MIDI events
        plugin_latency              VST3 getLatencySamples()
        console_mode                VHC routing mode (0/1/2)
        host_window_xid             DAW X11 window for GUI reparenting
        guest_window_xid            Plugin HWND→XID bridge
```

### Watchdog IPC Handshake
A 5-second atomic watchdog guards the Wine guest connection. If the guest fails to map SHM and signal `STATE_IDLE` within 5 seconds, the daemon terminates the child and returns `ERROR: Shared Memory IPC Connection Timeout` — the DAW never freezes.

### VDC PCI Multiplexer
A single PCI memory aperture is divided into `N` virtual regions using a page-granular offset table. Each VDC slot gets an independent 4 MB region for plugin state, eliminating the need for multiple PCIe cards.

---

## Verification

```bash
# Run the stress test (checks lock-free SHM round-trips and timing)
./build/arthur_stress_test

# Run handshake test suite
python3 scratch/test_handshake.py

# Check CLLS calibration output
./build/pw_module_clls --log-level=3
```

---

## Building from Source

```bash
git clone https://github.com/danny1marshall1587-maker/arthur-translation-layer.git
cd arthur-translation-layer
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

**Dependencies:** Qt6, PipeWire ≥ 1.0, Wine-GE (bundled), ALSA, CMake ≥ 3.20, GCC ≥ 12

---

## Changelog

### v4.0 — Virtual Hybrid Console (VHC)
- **NEW:** VHC Console tab in Arthur Control Center — per-channel console mode switcher
- **NEW:** `CONSOLE_MODE` and `LIST_SHM` daemon IPC commands for live SHM routing control
- **NEW:** Dry + Monitor Wet mode (Mode 1): records dry signal while monitoring wet effects
- **NEW:** Record Wet mode (Mode 2): bakes processed signal directly into DAW recording
- **FIX:** Unified `CLLSSyncLayout` definition in `AudioIPC.h` (SYNC_VERSION 4) — eliminates struct layout divergence between daemon, bridge, and CLLS module
- **FIX:** `global_target_rtt` field added to CLLS sync segment, frozen on first handshake
- **FIX:** Lagrange delay line tap direction corrected (inverted indices)
- **FIX:** ALSA MIDI pre-compensation slaved to `global_target_rtt`
- **FIX:** VDC multiplexed memory space mapping in daemon
- **FIX:** Nav button active-state refresh refactored to single `refreshBtns` lambda (all 5 tabs)

### v3.5.0 — Watchdog IPC & Stable Handshake
- Asynchronous non-blocking port registration
- 5-second watchdog prevents GUI/DAW freeze on bad plugin load
- Cross-boundary SHM namespace standardisation

### v3.3.5 — VDC Preloaded Auto-assignment
- Auto-assign preloaded VST to incoming DAW connections
- Dynamic guest agent fallback spawning
- Persistent slot unloading on exit

---

## License

MIT License — see `LICENSE`.
