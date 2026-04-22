# Arthur VST3 Bridge: Project Snapshot & Overriding Truths

## 1. The Core Issue: Host Rejection (The "Scanning Wall")
The primary blocker was REAPER (and other Linux hosts) silently rejecting the bridge binary. Through forensic analysis, we identified three "Truths" required for a Linux VST3 bridge to be considered valid:

### TRUTH A: Symbol Visibility (The Identity Crisis)
- **Problem:** On Linux, VST3 Identity Symbols (`IID`) are not exported by the SDK headers by default. REAPER sees an "empty" binary even if `GetPluginFactory` exists.
- **Fix:** We created `vstiids.cpp` to manually define and force-export `FUnknown::iid`, `IPluginFactory::iid`, `IComponent::iid`, `IAudioProcessor::iid`, and `IEditController::iid`.
- **Result:** The binary now has a "Fingerprint" that matches known working plugins like Serum.

### TRUTH B: Lifecycle Entry Points
- **Problem:** A minimalist VST3 only needs `GetPluginFactory`, but Linux hosts often require `ModuleEntry` and `ModuleExit` to safely initialize the shared library during a scan.
- **Fix:** Re-implemented these as explicit exports in `main.cpp`.

### TRUTH C: Environment Parity (The ABI Conflict)
- **Problem:** CachyOS uses a very new GCC/libstdc++. If the bridge links dynamically, it might conflict with REAPER's internal libraries.
- **Fix:** Forced `-static-libstdc++` and `-static-libgcc` in `CMakeLists.txt` to make the bridge self-contained.

---

## 2. The Current "Working Shell" Status
- **File:** `main.cpp` (Minimalist Proxy)
- **File:** `vstiids.cpp` (Manual Symbol Parity)
- **Status:** **SCAN SUCCESS**. REAPER recognizes the plugin and lists it in the FX browser.

---

## 3. The Path to "Fully Functional" (The Remaining Gaps)

### GAP 1: IPC Initialization (The "Loading" Failure)
- **Why it failed before:** We tried to launch Wine *inside* the bridge's initialization. REAPER's scanner felt the "weight" of the process launch and timed out, marking the plugin as "Crashed" during scan.
- **New Strategy:** Launch the Wine Guest **asynchronously** only when `createInstance` is called (not during scan). Use a "Lazy Load" pattern.

### GAP 2: ID Conflicts (The "Serum vs. Diva" Problem)
- **Why it failed before:** Every bridge had the same hardcoded ID.
- **New Strategy:** Re-implement the **Dynamic Name Hashing** but inside the *Functional* code, ensuring it doesn't break the *Scan* code.

### GAP 3: The Audio/UI Path
- **Gap:** The bridge doesn't yet pass audio buffers through the IPC FIFOs.
- **Gap:** The Wayland windowing bridge is not yet connected to the VST3 `attached` call.

---

## 4. Immediate Action Plan

1. **Keep the "Rescue" Symbols:** Never touch `vstiids.cpp` again. It is the reason the plugin is visible.
2. **Lazily Launch Wine:** Update `main.cpp` so it launches the guest **only** when a track actually tries to use the plugin.
3. **Connect the Pipes:** Pipe the VST3 `process()` call into the `AudioSharedMemory` transport we built in `AudioIPC.h`.

---

## 5. Overriding Truth
> **"A bridge must be a ghost during the scan, but a commander during the load."**
> - To pass the scan, the binary must look like a standard VST3.
> - To work as a plugin, it must manage a Wine subprocess without stalling the DAW's audio thread.
