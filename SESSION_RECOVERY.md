# ARTHUR PROJECT: EMERGENCY RECOVERY & SYSTEM STATE
**Date:** 2026-04-22
**Status:** EMERGENCY SYSTEM REINSTALL REQUIRED
**Version:** v2.2.0-STABLE-RECOVERY

## 1. CRITICAL RECOVERY SUMMARY
The project reached a state where the VST3 translation layer (Arthur Bridge) caused a fundamental failure in the host application's (REAPER) VST3 engine. Despite binary purges and app re-installs, the VST3 category remains missing from the system. The user is proceeding with a full OS/System wipe.

## 2. PROJECT SNAPSHOT (GOLDEN STATE)
Before the system failure, the following architectural milestones were achieved:
- **Arthur Daemon**: Background service implemented in `arthur-daemon.cpp`. Handles plugin lifecycles and Wine guest spawning.
- **Audio IPC**: High-performance, lock-free Shared Memory (`shm_buffer.h`) implemented using atomic sync flags for Linux-to-Wine audio transport.
- **Wine Guest**: `guest.cpp` implemented with `LoadLibrary` support for Windows .vst3 binaries.
- **Universal Manager**: `arthur-cli.py` handles deployment and automated Wine-GE setup.

## 3. HOW TO RESTORE THIS PROJECT ON A FRESH OS
1. **Clone the Repository**:
   ```bash
   git clone <repo_url> "arthur translation layer"
   ```
2. **Setup Build Environment**:
   - Install `g++`, `cmake`, and `python3`.
   - Re-initialize the virtual environment:
     ```bash
     python3 -m venv build_env && ./build_env/bin/pip install cmake
     ```
3. **Re-build Arthur Stack**:
   ```bash
   ./build_env/bin/cmake -B build -DCMAKE_BUILD_TYPE=Release
   ./build_env/bin/cmake --build build -j$(nproc)
   ```
4. **Deploy via Manager**:
   - Ensure Wine is installed.
   - Run `python3 arthur-cli.py` and use the "FULL REPAIR" button to link your Windows plugins.

## 4. ROOT CAUSE ANALYSIS (FOR NEXT SESSION)
The "Missing VST3" issue was likely caused by REAPER's **plugin-cache persistence** in the home directory (`~/.config/REAPER`). A full system wipe will resolve this by default, but future development must ensure that the `arthur_bridge.so` binary is always strictly compliant with the VST3 ABI before deployment to avoid blacklisting.

## 5. RECOVERY ARTIFACTS
All current source code (Daemon, Bridge, Guest, SHM) is preserved in the `/home/dan/Documents/arthur translation layer` directory. Ensure this folder is backed up before the wipe.

---
*End of Recovery Log. Ready for post-wipe restoration.*
