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
- **Batch Installation:** Select multiple `.exe` or `.msi` files at once. Arthur will queue them up and install them one-by-one in the background.
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
4.  Click **"Batch Install (.exe/.msi)"** to select your Windows installers.
5.  Click **"Scan & Sync Plugins"** once the installations are finished.

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

## Watchdog IPC Handshake & Safe Fault Handling (v3.5.0)

To eliminate GUI freezes and resolve real-time shared-memory (SHM) handshake deadlocks:
- **Asynchronous Non-Blocking Port Registration:** Port registration and routing actions are executed on dedicated background threads. The main audio thread and the Qt Control Center GUI never block on PipeWire or JACK graph synchronization.
- **Volatile Protection & Atomic Fences:** Control indices and transport state variables inside `AudioIPC.h` are qualified as `volatile` with explicit sequentially-consistent memory fences (`std::memory_order_seq_cst`) to prevent compiler register caching and guarantee memory visibility across the Wine process boundary.
- **Handshake Watchdog Timer:** A 5000ms atomic watchdog timer guards the connection. If the Wine guest process fails to map the shared memory and signal `STATE_IDLE` within 5 seconds, the daemon terminates the child process and returns a clean `ERROR: Shared Memory IPC Connection Timeout` instead of spinning infinitely or freezing the DAW/GUI.
- **Cross-Boundary SHM Namespaces:** Standardized naming maps `/dev/shm/ArthurAudioIPC` on the Linux host directly to the global Windows object `"Global\\ArthurAudioIPC"` inside the Wine-GE namespace (bridged via `Z:\dev\shm\ArthurAudioIPC`).

### Running the Handshake Test Suite

To verify the timed watchdog and handshake behaviors locally:
```bash
python3 scratch/test_handshake.py
```
This runs automated scenarios checking both the 5-second graceful timeout on missing/delayed guests and successful connection handshakes under Wine.

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

---

## Phase 3: Unflappable Timing Sync & Virtualized CPU DSP Cores (v4.0.0)

We have successfully finalized the low-latency phase-alignment, virtual hybrid console routing, and system performance optimizations:

### 1. Multi-Device CLLS Aggregation & Drift Compensation
To allow combining multiple audio interfaces with sample-accurate phase locking (software-based wordclock) without clock drift or clicks:
* **Decentralized Coordination (`/dev/shm/arthur_clls_sync`)**: Allocates a secure [CLLSSyncLayout](file:///home/dan/audos/AudioIPC.h) tracking the measured RTT of each device.
* **Frozen Timing Horizon**: The first device or VST3 bridge instance that detects active measurements calculates a stable target latency ($T = \max(L_{\text{active}}) + 128.0\text{ samples}$) and freezes it inside the shared `global_target_rtt` atomic field.
* **Lagrange Fractional Interpolation**: All active plugins ([main.cpp](file:///home/dan/audos/main.cpp)) read this identical shared target, dynamically computing the compensation delay ($D_i(t) = T - L_i(t)$) and applying a 3rd-order Lagrange fractional delay line in real-time. This dynamically absorbs clock drift with sub-sample precision.
* **ALSA MIDI Synchronization**: The MIDI sync daemon ([midi_sync.cpp](file:///home/dan/audos/midi_sync.cpp)) pre-compensates outgoing events slaved to the exact same `global_target_rtt` to eliminate scheduling jitter.

### 2. Inline Virtual Hybrid Console (VHC) Routing
Allows seamless switching of console channels between tracking (zero-latency monitoring) and mixing (DAW playback) without duplicating plugin instances:
* **console_mode**: The shared memory segment tracks channel states (0 = Playback, 1 = Record Dry / Monitor Wet, 2 = Record Wet).
* **Record Dry / Monitor Wet (`MODE_RECORD_DRY_MONITOR_WET`)**: Routes physical input signals dry directly to the DAW's main output buffers for recording, while simultaneously streaming the processed wet monitor audio to the auxiliary `Monitor Output` VST3 bus for the performer.
* **Playback Pool (`MODE_PLAYBACK`)**: Reroutes DAW track playback buffers directly back through the same console insert slot during mixing.

### 3. Single-PCI Multiplexed VDC (Virtual DSP Cores)
* Employs a single 256MB shared memory file (`/arthur_vdc_multiplex`) partitioned into 128 dynamic slots at 2MB strides.
* The daemon ([arthur-daemon.cpp](file:///home/dan/audos/arthur-daemon.cpp)) handles thread-safe allocation and placement-new initialization of slots, passing slot indices and strides directly to spawned guest processes.
* **Version Validation**: The guest agent ([win_guest_agent.cpp](file:///home/dan/audos/win_guest_agent.cpp)) validates the mapped memory layout against `AudioSharedMemory::SHM_VERSION` to prevent layout corruption.

### 4. CachyOS Performance Auto-Tuning
Arthur is fully optimized to run on **CachyOS** (or standard Arch Linux) using our specialized performance tuning script:
```bash
sudo ./arthur_tune_cachyos.sh
```
This automates the entire system configuration:
1. **Core Isolation**: Inserts kernel boot parameters (`isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7`) in systemd-boot or GRUB to dedicate cores 4–7 strictly to real-time audio threads.
2. **Real-time Priorities**: Configures limits for the `audio` and `realtime` groups (`rtprio 98` and `memlock unlimited`).
3. **PipeWire Realtime Limits**: Copies and modifies PipeWire `client.conf` and `jack.conf` templates to set `rt.prio = 95` and allow unconstrained memory locks.
4. **Bridge Auto-Linking**: Installs the native bridge loader (`arthur_bridge.so`) into user `~/.vst3/`.

### 5. Flatpak sandbox configuration
The Flatpak manifest (`org.arthur.TranslationLayer.json`) packages the entire app bundle (GUI, Daemon, VM, Wine dependencies) as a sandboxed desktop application. To enable zero-latency IPC and hardware virtualization acceleration, it is configured with:
* `--device=kvm` (Hardware virtualization speed)
* `--filesystem=xdg-run/shm` (Host/Guest shared memory)
* `--filesystem=host` (Auto-writing bridged `.so` files to `~/.vst3/`)
* `--share=ipc` (UNIX socket communication)

---

## License

MIT — Built on the [MIT-licensed VST3 SDK](https://github.com/steinbergmedia/vst3sdk).

