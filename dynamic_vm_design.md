# Arthur: Dynamic VM & Installer Integration Design

This document details the design for making the Arthur Micro-VM backend fully dynamic (on-demand boot via QEMU snapshots), easily distributable via Flatpak, and integrated with an automated Windows installer runner that generates Linux plugin symbolic links.

---

## 1. On-Demand Dynamic VM Lifecycle (Instant Boot)

To prevent the Windows VM from permanently consuming RAM and CPU resources, we implement an **asynchronous snapshot-resume** lifecycle.

```
                  DAW Launches / Plugin Loaded
                               │
                       [Arthur Daemon]
                               │
                     Is VM already running?
                     ├── Yes ──> Route Audio IPC
                     └── No  ──> QEMU Snapshot Restore (~1.0s)
                               │
                    [VM Active & Processing]
                               │
                    DAW Closes / Idle Timeout
                               │
                       [Arthur Daemon]
                               │
                     Suspend VM to RAM / disk
                     (Release RAM and CPU back to Host)
```

### Technical Implementation
*   **QEMU VM Snapshots (`-loadvm`):** We boot the guest Windows VM once, optimize it, load the Arthur Guest Server, and save the VM state using QEMU's `savevm` command.
*   **Instant Resume:** When `arthur-daemon` receives a `LOAD` command from the DAW bridge, it executes:
    ```bash
    qemu-system-x86_64 -m 4G -enable-kvm -snapshot -loadvm "ready_state" ...
    ```
    This restores the running VM state directly from disk into RAM in **under 1.5 seconds**, bypassing the Windows boot sequence.
*   **Idle Suspension:** If no audio processing requests are received for a specified period (e.g., 5 minutes after the DAW closes), the daemon calls QEMU's monitor interface to pause the VM (`pmemsave` or `stop`) and release the host memory.

---

## 2. Flatpak Packaging & Sandbox Configuration

Flatpak is used to deliver the entire system (including QEMU, the minimal Windows image, the Arthur manager, and the bridges) as a single, sandboxed, double-clickable app.

### Flatpak Manifest Requirements (`org.arthur.vst_bridge.json`)
To run a hardware-accelerated VM and share low-latency memory, the Flatpak container is granted specific permissions:

```json
{
  "id": "org.arthur.vst_bridge",
  "runtime": "org.freedesktop.Platform",
  "runtime-version": "23.08",
  "sdk": "org.freedesktop.Sdk",
  "command": "arthur-manager",
  "finish-args": [
    "--device=kvm",            /* Required for hardware virtualization speed */
    "--filesystem=xdg-run/shm",/* Required for Host/Guest IVSHMEM shared memory */
    "--filesystem=home",       /* To write bridged .so files to ~/.vst3/ */
    "--socket=wayland",        /* Native Wayland GUI rendering */
    "--socket=fallback-x11",
    "--share=ipc"              /* IPC access for Unix sockets */
  ]
}
```

---

## 3. Seamless Installer Pipeline ("Install VST inside VM")

To handle plugins with complex `.exe` or `.msi` installers (like Arturia, Native Instruments, or iLok License Manager), Arthur provides a seamless installation wizard.

```
  1. User selects "Install Windows VST" in Arthur Manager (Linux UI)
                               │
  2. Arthur Daemon mounts installer directory & boots VM with GUI enabled
                               │
  3. QEMU displays installer screen in Linux Wayland window (RDP/VNC or GTK)
                               │
  4. User clicks "Next", registers serial, and completes install in VM
                               │
  5. Windows Guest Agent scans VM's VST3 directory:
     e.g., C:\Program Files\Common Files\VST3\FabFilter Pro-Q 3.vst3
                               │
  6. Windows Guest Agent sends plugin metadata back to Arthur Daemon
                               │
  7. Arthur Daemon generates Linux Bridge Loader:
     ~/.vst3/FabFilter Pro-Q 3.vst3/Contents/x86_64-linux/FabFilter Pro-Q 3.so
                               │
  8. VM is suspended; Plugin is ready to load in Linux DAW
```

### Auto-Generation of Bridge Symbolic Links
When a new plugin is detected in the VM:
1. The Linux daemon generates a standard VST3 bundle directory in the host's plugin folder:
   `~/.vst3/<PluginName>.vst3/Contents/x86_64-linux/`
2. It copies a generic `arthur_bridge.so` to this folder and renames it to `<PluginName>.so`.
3. When the Linux DAW loads this `.so` file, the bridge reads its own filename, realizes it represents `<PluginName>`, connects to the VM, and tells the Windows Guest Server to load the corresponding DLL path inside the VM.
