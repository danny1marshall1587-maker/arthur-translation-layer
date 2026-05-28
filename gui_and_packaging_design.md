# Arthur: Control Center GUI & Packaging Design

This document details the user experience (UX) and packaging design for the **Arthur Control Center**. It is designed to provide a completely terminal-free, Mac-like desktop experience for music producers, hiding all virtualization, Linux kernel tuning, and PipeWire routing behind an intuitive graphical interface.

---

## 1. Distribution & Installation: The Double-Click Flatpak

To eliminate terminal usage and configuration headaches, the entire Arthur system—including the KVM micro-VM manager, Wine-GE runtime, CLLS module, and GUI—is bundled as a single **Flatpak package**.

### Flatpak Sandboxing & Permissions
The Flatpak manifest is pre-configured with the following host access permissions so that the user never has to configure permissions manually:
* `--device=kvm`: Direct access to KVM hardware acceleration for the VM.
* `--filesystem=xdg-run/pipewire-0`: Direct connection to the host PipeWire audio server.
* `--device=dri`: Direct access to the host GPU for hardware-accelerated UI rendering inside the VM.
* `--filesystem=host`: Seamless access to the user's home folder to read VST3 directory paths.

The user simply double-clicks the `.flatpak` installer file (or clicks "Install" in the Software Center), and the entire translation layer is deployed.

---

## 2. Frontend Technology Stack: Tauri + Glassmorphic HTML5/CSS

To achieve a premium, modern, and fluid UI that rivals macOS native applications, we design the frontend using **Tauri** with HTML5/CSS/JavaScript, backed by a high-performance **Rust** system layer.

* **Tauri Backend (Rust):** Communicates with the host system, controls QEMU snapshots via UNIX sockets, and interfaces with the PipeWire API.
* **Tauri Frontend (HTML5/CSS/JS):** A dark-mode, glassmorphic UI featuring smooth animations, reactive status indicators, and drag-and-drop actions.

---

## 3. UI Mockup & Flow Design

### View A: The Unified Dashboard

The landing page of the application gives a high-level overview of the audio engine's health.

```
┌────────────────────────────────────────────────────────────────────────┐
│  ARTHUR CONTROL CENTER                                        [─] [X]  │
├────────────────────────────────────────────────────────────────────────┤
│  [DASHBOARD]     [PLUGINS]     [DSP RACK]     [SETTINGS]               │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  ┌─────────────────────────────┐   ┌────────────────────────────────┐  │
│  │   Virtual DSP Engine        │   │   Closed-Loop Latency Sync     │  │
│  │   [ ACTIVE ]                │   │   [ LOCKED ]                   │  │
│  │                             │   │                                │  │
│  │   Cores Allocated:  4 / 8   │   │   Hardware RTT:   5.048 ms     │  │
│  │   VDC DSP Load:    [■■■■░░] │   │   CLLS Offset:    +0.285 ms    │  │
│  │   Status: Stable (48kHz)    │   │   Phase Jitter:   ±416 ns      │  │
│  └─────────────────────────────┘   └────────────────────────────────┘  │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │   Audio-MIDI Timebase Lock                                       │  │
│  │   [ Locked to Interface PCM Clock ]   Jitter: < 10 microseconds  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

* **Micro-Animations:** The VDC DSP Load bar and Latency Phase Jitter graph pulse dynamically in real-time, reflecting system conditions.
* **No-Terminal Diagnostics:** If an error occurs (e.g., KVM is disabled in BIOS), a clean modal pops up with instructions: *"KVM acceleration is disabled. Please enable 'Virtualization Technology (VT-x/AMD-V)' in your motherboard settings."*

---

### View B: Drag-and-Drop Plugin Installer

Installing Windows plugins is simplified to a single drag-and-drop box.

```
┌────────────────────────────────────────────────────────────────────────┐
│  ARTHUR CONTROL CENTER                                                 │
├────────────────────────────────────────────────────────────────────────┤
│  [DASHBOARD]     [PLUGINS]     [DSP RACK]     [SETTINGS]               │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│    ┌──────────────────────────────────────────────────────────────┐    │
│    │                                                              │    │
│    │                 DRAG & DROP INSTALLERS HERE                  │    │
│    │                                                              │    │
│    │             Supports Windows .exe and .msi files             │    │
│    │                                                              │    │
│    └──────────────────────────────────────────────────────────────┘    │
│                                                                        │
│  Active Installations:                                                 │
│  [x] FabFilter_ProQ3_Installer.exe  ->  [ Installed ]                  │
│  [/] Kontakt_Setup.msi              ->  [ Running Installer GUI... ]   │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

* **Seamless Installer Routing:** When a user drops `Kontakt_Setup.msi` into the window, Arthur executes the installer in the background VM. The installer's native graphical interface window (e.g. the Native Access installer wizard) is automatically mapped onto the host Wayland desktop. The user runs through the standard Windows clicks (`Next -> Next -> Finish`).
* **Auto-Bridging:** Once the installer exits, the Arthur daemon automatically scans the virtual Windows VST3 folder, creates the symbolic bridge links in the Linux path (`~/.vst3/`), and sends a notification: *"Kontakt successfully bridged. Open your DAW to use it."*

---

### View C: The Virtual DSP Rack (VDC Manager)

This view allows the user to configure their pre-allocated "DSP console," similar to the Universal Audio Console app.

```
┌────────────────────────────────────────────────────────────────────────┐
│  ARTHUR CONTROL CENTER                                                 │
├────────────────────────────────────────────────────────────────────────┤
│  [DASHBOARD]     [PLUGINS]     [DSP RACK]     [SETTINGS]               │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  VDC Profile: [ Tracking_Session.vdcp  ▼ ]              [ SAVE ]       │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │  CHANNEL 1-8 INSERT RACK                                         │  │
│  │                                                                  │  │
│  │  [ CH 1 ]      [ CH 2 ]      [ CH 3 ]      [ CH 4 ]              │  │
│  │ ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐            │  │
│  │ │ Pro-Q 3 │   │ SSL-Chan│   │ LA-2A   │   │ -empty- │            │  │
│  │ ├─────────┤   ├─────────┤   ├─────────┤   ├─────────┤            │  │
│  │ │ 1176LN  │   │ -empty- │   │ -empty- │   │ -empty- │            │  │
│  │ └─────────┘   └─────────┘   └─────────┘   └─────────┘            │  │
│  └──────────────────────────────────────────────────────────────────┘  │
│  DSP Core Allocation: [■■■■■■■■■■■■■■■■■■■■■■■■■■░░░░░░] 78%           │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

* **Interactive Rack:** Users click on slot drop-downs to choose from their bridged Windows VST3s.
* **Background Pre-loading:** Saving this rack writes it to the active VDC profile. The VDC daemon immediately instantiates these plugins in the background VM, securing their processing loops on the isolated CPU cores.

---

### View D: Audio-MIDI Setup (System Settings)

```
┌────────────────────────────────────────────────────────────────────────┐
│  ARTHUR CONTROL CENTER                                                 │
├────────────────────────────────────────────────────────────────────────┤
│  [DASHBOARD]     [PLUGINS]     [DSP RACK]     [SETTINGS]               │
├────────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  Audio Device:      [ Focusrite Scarlett 18i20 (USB)              ▼ ]  │
│  Sample Rate:       [ 48,000 Hz                                   ▼ ]  │
│  Buffer Size:       [ 128 samples (2.7 ms)                        ▼ ]  │
│                                                                        │
│  CLLS Loopback:     [ Port 8 (Loopback Ch 8)                      ▼ ]  │
│  MIDI Clock Sync:   [x] Slave ALSA Sequencer to Audio PCM Timer        │
│                                                                        │
│  CPU DSP Slider:    [  Isolated Core 4-7 (4 Cores for DSP)        ▼ ]  │
│                     ◄───────────────────●───────────────────►          │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

* **Automatic Setup:** Selecting the audio interface automatically configures PipeWire's default sample rate and buffer quantum behind the scenes.
* **One-Click Kernel Isolation:** The "CPU DSP Slider" allocates cores for isolation. When changed, the application prompts: *"Core isolation requires a reboot to apply kernel boot parameters. Reboot now? [YES] [NO]"*. The Rust backend handles editing the GRUB bootloader configuration files safely.

---

## 4. The Dedicated DAW Distribution: "Studio OS"

For the ultimate out-of-the-box experience, we can package this system as a custom Linux distribution (based on a minimal base like Arch or Debian):

1. **Custom ISO Installer:** A graphical installer (like Calamares) that partitions the drive, sets up a real-time kernel, and isolates CPU cores during the initial installation process.
2. **Pre-configured Audio Server:** PipeWire, PipeWire-JACK, and the CLLS module are configured out-of-the-box.
3. **Pre-loaded DAWs:** Comes pre-installed with Bitwig Studio (Demo/Licence login) and REAPER, with their audio preferences pre-configured to output to the PipeWire-JACK server.
4. **Boot to Desktop:** The system boots directly into a customized, lightweight desktop environment (like a streamlined GNOME or custom Wayland compositor) with the Arthur Control Center open, ready for the user to drop in their Windows installers.
