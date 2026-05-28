# Arthur: Micro-VM (KVM) Backend Design

To solve the "Wine DRM Dumpster Fire," Arthur can be extended with an optional **Micro-VM backend** alongside its Wine backend. By running a real Windows NT kernel inside a lightweight, sandboxed QEMU/KVM virtual machine, we can run PACE/iLok, elicenser, and other kernel-level copy protections natively while streaming audio buffers to the Linux host DAW in real-time.

---

## 1. High-Level Architecture

Instead of launching the Windows VST3 inside a Wine process, Arthur spawns or communicates with a lightweight Windows guest VM.

```
  ┌────────────────────────────────────────────────────────┐
  │                   Linux Host (DAW)                     │
  │                                                        │
  │  ┌──────────────┐         ┌─────────────────────────┐  │
  │  │ Native DAW   │ <─────> │ arthur_bridge.so        │  │
  │  │ (e.g. Reaper)│  (VST3) │ (Linux Host Client)     │  │
  │  └──────────────┘         └────────────┬────────────┘  │
  └────────────────────────────────────────┼───────────────┘
                                           │
                        IVSHMEM (Inter-VM Shared Memory)
                        /dev/shm mapped directly to VM RAM
                                           │
  ┌────────────────────────────────────────┼───────────────┐
  │            QEMU/KVM Windows Guest      │               │
  │                                        │               │
  │  ┌──────────────┐         ┌────────────▼────────────┐  │
  │  │ PACE/iLok    │ <─────> │ arthur-guest-vm.exe     │  │
  │  │ Kernel VST3  │ (VST3)  │ (Windows Guest Server)  │  │
  │  └──────────────┘         └─────────────────────────┘  │
  └────────────────────────────────────────────────────────┘
```

---

## 2. Low-Latency Audio IPC: IVSHMEM (Inter-VM Shared Memory)

To achieve sub-millisecond round-trip audio latency, we cannot use virtual network sockets (TCP/UDP). Instead, we use QEMU's **IVSHMEM v2** device to share a region of the host's physical RAM directly with the guest VM.

### Host-Side Setup (Linux)
Create a shared memory file:
```bash
sudo ipcs -m # View shared memory
# Map a 4MB memory region for the VM
sudo dd if=/dev/zero of=/dev/shm/ivshmem-arthur bs=1M count=4
sudo chmod 666 /dev/shm/ivshmem-arthur
```

Launch QEMU with the shared memory device:
```bash
qemu-system-x86_64 \
  -enable-kvm \
  -m 4G \
  -smp 4 \
  -drive file=win10-studio.qcow2,if=virtio \
  -device ivshmem-plain,memdev=ivshmem-arthur-dev \
  -object memory-backend-file,id=ivshmem-arthur-dev,share=on,mem-path=/dev/shm/ivshmem-arthur,size=4M
```

### Guest-Side Setup (Windows VM)
1. Install the IVSHMEM driver from the **VirtIO-win** driver suite.
2. The shared memory region appears in Windows as a PCI device memory range.
3. `arthur-guest-vm.exe` maps this PCI memory space into its virtual address space.

---

## 3. Shared Memory Layout (`AudioIPC.h` Compatibility)

We can reuse Arthur's existing `AudioSharedMemory` structure in `AudioIPC.h`:

```cpp
struct AudioSharedMemory {
    alignas(64) std::atomic<TransportState> state;
    uint32_t sample_count;
    uint32_t num_inputs;
    uint32_t num_outputs;
    double sample_rate;
    int64_t playhead_pos;

    float input_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES];
    float output_buffers[SHM_MAX_CHANNELS][SHM_MAX_SAMPLES];

    uint32_t midi_in_count;
    ShmMidiEvent midi_in[256];
    uint32_t midi_out_count;
    ShmMidiEvent midi_out[256];
};
```

---

## 4. Modified Processing Loop

### Linux Host Bridge (`main.cpp`)
1. Write input audio channels to `/dev/shm/ivshmem-arthur`.
2. Write MIDI events to the MIDI input queue in shared memory.
3. Transition `state` to `STATE_HOST_WRITTEN`.
4. Wait (spin-lock with timeout or eventfd) for `state` to become `STATE_GUEST_PROCESSED`.
5. Read output audio channels and MIDI events.

### Windows VM Guest Server (`guest-vm.cpp`)
1. Poll the memory-mapped PCI region.
2. When `state` is `STATE_HOST_WRITTEN`:
   * Pull MIDI events.
   * Call `process` on the real Windows VST3 plugin loaded in memory.
   * Copy the output buffers back to the PCI memory region.
   * Transition `state` to `STATE_GUEST_PROCESSED`.
3. Yield/Sleep(0) to avoid pegging the VM's CPU core when idle.

---

## 5. Implementation Steps for Arthur

To build a prototype of this VM bridge:

1.  **Add VM-aware Guest Code (`guest_vm.cpp`)**:
    We write a Windows guest server that accesses the IVSHMEM PCI memory region instead of calling `shm_open` (which is a POSIX call and does not exist in standard Windows NT outside of Win32 file mapping).
2.  **Add IVSHMEM Support to Host (`main.cpp`)**:
    Add a compiler flag to allow mapping `/dev/shm/ivshmem-arthur` directly instead of opening a POSIX shm file.
3.  **Command-Line VM Management**:
    Update `arthur-cli.py` to add a "Launch VM" button that starts a headless QEMU instance with the required parameters.
