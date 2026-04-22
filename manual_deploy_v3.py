import os
import shutil
from pathlib import Path

# Let's target FabFilter DIRECTLY since we know it exists
SOURCE_DIR = Path("/home/dan/.wine/drive_c/Program Files/Common Files/VST3/FabFilter")
TARGET_DIR = Path("/home/dan/.vst3")
BRIDGE_SO = Path("/home/dan/Documents/arthur translation layer/build/arthur_bridge.so")

print(f"Direct Scan of: {SOURCE_DIR}")
if not SOURCE_DIR.exists():
    print("FATAL: Source folder does not exist!")
    exit(1)

for item in os.listdir(SOURCE_DIR):
    if item.endswith(".vst3"):
        plugin_name = item.replace(".vst3", "")
        print(f"Bridging: {plugin_name}")
        bundle_path = TARGET_DIR / f"{plugin_name}.vst3" / "Contents" / "x86_64-linux"
        bundle_path.mkdir(parents=True, exist_ok=True)
        shutil.copy(BRIDGE_SO, bundle_path / f"{plugin_name}.so")

print("DEPOY V3 FINISHED.")
