import os
import shutil
from pathlib import Path

SOURCE_DIR = Path("/home/dan/.wine/drive_c/Program Files/Common Files/VST3")
TARGET_DIR = Path("/home/dan/.vst3")
BRIDGE_SO = Path("/home/dan/Documents/arthur translation layer/build/arthur_bridge.so")

print(f"DEBUG: SOURCE_DIR={SOURCE_DIR}")
print(f"DEBUG: TARGET_DIR={TARGET_DIR}")

if not TARGET_DIR.exists():
    TARGET_DIR.mkdir(parents=True)

# Use os.walk for guaranteed discovery
for root, dirs, files in os.walk(SOURCE_DIR):
    for d in dirs:
        if d.endswith(".vst3"):
            plugin_path = Path(root) / d
            plugin_name = Path(d).stem
            print(f"FOUND: {plugin_name} at {plugin_path}")
            
            bundle_path = TARGET_DIR / f"{plugin_name}.vst3" / "Contents" / "x86_64-linux"
            bundle_path.mkdir(parents=True, exist_ok=True)
            print(f"CREATING: {bundle_path}")
            shutil.copy(BRIDGE_SO, bundle_path / f"{plugin_name}.so")

print("MANUAL DEPLOY V2 COMPLETE.")
