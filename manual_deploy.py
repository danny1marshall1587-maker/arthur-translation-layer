import os
import shutil
from pathlib import Path

SOURCE_DIR = Path.home() / ".wine/drive_c/Program Files/Common Files/VST3"
TARGET_DIR = Path.home() / ".vst3"
BRIDGE_SO = Path("/home/dan/Documents/arthur translation layer/build/arthur_bridge.so")

if not BRIDGE_SO.exists():
    print(f"ERROR: Bridge SO not found at {BRIDGE_SO}")
    exit(1)

if TARGET_DIR.exists():
    shutil.rmtree(TARGET_DIR)
TARGET_DIR.mkdir(parents=True)

print(f"Scanning {SOURCE_DIR}...")
for vst in SOURCE_DIR.rglob("*.vst3"):
    if vst.is_dir():
        plugin_name = vst.stem
        print(f"Bridging {plugin_name}...")
        bundle_path = TARGET_DIR / f"{plugin_name}.vst3" / "Contents" / "x86_64-linux"
        bundle_path.mkdir(parents=True, exist_ok=True)
        shutil.copy(BRIDGE_SO, bundle_path / f"{plugin_name}.so")

print("Deployment Complete.")
