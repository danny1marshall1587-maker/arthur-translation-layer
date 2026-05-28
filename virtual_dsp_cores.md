# Arthur: Virtual DSP Cores (VDC) Architecture

This document details the design for treating a virtualized Windows plugin host as a dedicated, hardware-like DSP system. By using CPU core isolation, pinning, and pre-loaded plugin profiles, we can simulate the behavior of a dedicated PCIe DSP card (like UAD Apollo or Pro Tools HDX) directly on the host CPU.

---

## 1. The Core Concept: CPU Partitioning

In a standard operating system, the scheduler constantly shifts threads across different CPU cores to balance thermal loads and power consumption. For real-time audio, this introduces context-switch overhead and cache invalidation.

The **Virtual DSP Cores (VDC)** architecture partitions a multi-core CPU into two distinct domains:

```
┌────────────────────────────────────────────────────────────────────────┐
│                          Physical CPU Cores                            │
│                                                                        │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐            │
│  │  Core 0   │  │  Core 1   │  │  Core 2   │  │  Core 3   │            │
│  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘            │
│        └──────────────┴─────┬──┴─────────────┘                         │
│                             ▼                                          │
│                    [Linux Host Domain]                                 │
│             (OS, DAW UI, Native Linux Plugins)                         │
├────────────────────────────────────────────────────────────────────────┤
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐            │
│  │  Core 4   │  │  Core 5   │  │  Core 6   │  │  Core 7   │            │
│  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘  └─────┬─────┘            │
│        └──────────────┴─────┬──┴─────────────┘                         │
│                             ▼                                          │
│                [Isolated Windows VM Domain]                            │
│                (VDC Daemon, Windows VSTs)                              │
└────────────────────────────────────────────────────────────────────────┘
```

1. **Linux Host Domain (Cores 0-3):** Runs the Linux operating system, the DAW user interface, and native Linux audio processes.
2. **Isolated Windows VM Domain (Cores 4-7):** Runs the lightweight Windows NT VM, which hosts the bridged Windows VST plugins. This domain is completely isolated from the Linux kernel scheduler.

---

## 2. Technical Implementation

### A. Host Core Isolation (`isolcpus`)
During system boot, the Linux kernel is configured to ignore the isolated cores using the kernel parameter:
```bash
isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7
```
* `isolcpus`: Prevents the Linux OS scheduler from assigning any user-space processes or kernel threads to Cores 4-7.
* `nohz_full`: Disables the kernel scheduling tick on these cores when a single real-time thread is running, eliminating timer interrupt overhead.
* `rcu_nocbs`: Moves Read-Copy Update (RCU) callback processing off these cores.

### B. QEMU CPU Pinning
When the Windows micro-VM boots, QEMU pins its virtual CPUs (vCPUs) directly to the isolated physical cores:
```bash
qemu-system-x86_64 -enable-kvm -cpu host,migratable=off \
  -smp 4,sockets=1,cores=4,threads=1 \
  -device ivshmem-plain,memdev=ivshmem \
  -numa node,nodeid=0,cpus=4-7
```
This ensures that the Windows NT kernel inside the VM has exclusive access to the isolated hardware execution pipelines.

---

## 3. Pre-Loaded "DSP Profiles" & Constant CPU Load

### The Stability Advantage of Constant Load
In traditional setups, loading or bypassing plugins causes the CPU load to fluctuate. When load drops, the CPU's power management governor (Intel SpeedStep / AMD Cool'n'Quiet) downclocks the core to save power. When a heavy audio load suddenly returns, the core takes time to scale up, causing a late buffer delivery (an xrun).

By running a **VDC Profile**, we lock the isolated cores into a constant-load state:
1. **Startup Loading:** At system boot, the VDC daemon reads a user-configured profile containing their standard mixing channel strips (e.g. 16 instances of SSL Channel Strip, 4 compressors, 2 reverbs).
2. **Background Processing:** The plugins are instantiated immediately inside the Windows VM and process silent audio blocks continuously through the IVSHMEM shared memory ring buffers.
3. **Immediate Availability:** Because the plugins are already running, they are exposed in the PipeWire graph as active virtual "hardware inserts." The DAW can patch audio through them instantly without any plugin instantiation or buffer allocation delays.
4. **Stable Power States:** The pinned CPU cores remain locked in their maximum performance state ($C_0$ state, maximum turbo frequency) with zero frequency transitions.

---

## 4. Hybrid Routing (Static vs. Dynamic Inserts)

To balance predictability and flexibility, the VDC system supports a hybrid routing engine:

```
                       ┌─────────────────────────┐
                       │        Linux DAW        │
                       └────┬────────────────┬───┘
                            │ (Insert A)     │ (Insert B)
                            ▼                ▼
             ┌──────────────────────┐  ┌──────────────────────┐
             │   Static DSP Pool    │  │   Dynamic VST Pool   │
             │ (Cores 4-7, Pinned)  │  │  (Cores 0-3, Shared) │
             │                      │  │                      │
             │  - SSL Channel Strip │  │  - One-off creative  │
             │  - LA-2A Compressor  │  │    effects / delays  │
             │  - Plate Reverb      │  │  - Low priority VSTs │
             └──────────────────────┘  └──────────────────────┘
```

* **Static DSP Pool:** Pre-allocated at boot on isolated cores. Used for critical tracking and mixing tracks (channel EQ, compression, monitoring reverbs) where latency and phase stability are paramount.
* **Dynamic VST Pool:** Loaded on-demand using standard Wine or a secondary dynamic VM. Runs on the host CPU cores (Cores 0-3) alongside the DAW, allowing the user to load creative effects and rare processors as needed without disrupting the stable DSP core pool.

---

## 5. Benefits for the Recording Engineer

1. **Zero-Latency Feel:** Plugins in the Static DSP Pool are already instantiated and processing. Enabling an insert is instantaneous and introduces no audio dropout.
2. **Predictable Headroom:** Just like a hardware console or a UAD Apollo, you know exactly how many plugins your DSP pool can run. The system shows a "VDC DSP Load" indicator (e.g., `DSP Cores: 64% allocated`). Once allocated, the load is fixed and guaranteed.
3. **Impenetrable Stability:** A crash or CPU spike in a dynamic plugin on Core 0 cannot interrupt the execution of the critical tracking plugins running on the isolated Cores 4-7.
