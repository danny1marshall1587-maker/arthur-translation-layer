#!/usr/bin/env python3

import os
import sys
import shutil
import argparse
from pathlib import Path

# Defaults
DEFAULT_WINE_PREFIX = Path.home() / ".wine"
WINE_VST3_DIR = DEFAULT_WINE_PREFIX / "drive_c" / "Program Files" / "Common Files" / "VST3"
LINUX_VST3_DIR = Path.home() / ".vst3" / "Arthur"

# The compiled bridge library that will be symlinked/copied
# Assuming the user runs this from the project root after compiling
BRIDGE_SO_PATH = Path.cwd() / "build" / "libarthur_bridge.so"

def setup_directories():
    """Ensure the target Linux VST3 directory exists."""
    LINUX_VST3_DIR.mkdir(parents=True, exist_ok=True)

def find_wine_plugins():
    """Scans the Wine VST3 directory for plugin folders/files."""
    if not WINE_VST3_DIR.exists():
        print(f"[!] Warning: Wine VST3 directory not found at {WINE_VST3_DIR}")
        return []

    plugins = []
    # VST3 plugins can be .vst3 directories or just .vst3 files
    for item in WINE_VST3_DIR.iterdir():
        if item.suffix.lower() == '.vst3':
            plugins.append(item)
    return plugins

def sync():
    """Scans Wine directory and creates bridge links in the Linux VST3 folder."""
    print(">>> Scanning for Windows VST3 plugins...")
    
    if not BRIDGE_SO_PATH.exists():
        print(f"[ERROR] Bridge library not found at {BRIDGE_SO_PATH}")
        print("Please compile the Arthur Translation Layer first by running 'make' in the build directory.")
        return

    setup_directories()
    plugins = find_wine_plugins()

    if not plugins:
        print("No plugins found to sync.")
        return

    synced_count = 0
    for plugin in plugins:
        target_link = LINUX_VST3_DIR / plugin.name
        
        # If a link or file already exists, skip it unless forced or clean is run
        if target_link.exists():
            print(f"[-] Skipping (already exists): {plugin.name}")
            continue

        try:
            # Create a symlink to the compiled bridge, named as the plugin
            os.symlink(BRIDGE_SO_PATH, target_link)
            print(f"[+] Synced: {plugin.name}")
            synced_count += 1
        except Exception as e:
            print(f"[ERROR] Failed to sync {plugin.name}: {e}")

    print(f"\n>>> Sync Complete! {synced_count} new plugins bridged.")

def clean():
    """Removes all bridged plugins from the Linux VST3 directory."""
    print(">>> Cleaning Arthur bridged plugins...")
    if not LINUX_VST3_DIR.exists():
        print("Nothing to clean.")
        return

    removed_count = 0
    for item in LINUX_VST3_DIR.iterdir():
        if item.is_symlink() or item.is_file():
            try:
                item.unlink()
                removed_count += 1
            except Exception as e:
                print(f"[ERROR] Failed to remove {item.name}: {e}")

    print(f">>> Clean Complete! Removed {removed_count} bridged plugins.")

def status():
    """Shows currently synced plugins."""
    print(">>> Arthur Translation Layer Status\n")
    print(f"Wine VST3 Path:  {WINE_VST3_DIR}")
    print(f"Linux VST3 Path: {LINUX_VST3_DIR}\n")
    
    if not LINUX_VST3_DIR.exists() or not any(LINUX_VST3_DIR.iterdir()):
        print("No plugins are currently synced.")
        return

    print("Currently Synced Plugins:")
    for item in LINUX_VST3_DIR.iterdir():
        print(f"  - {item.name}")

def main():
    parser = argparse.ArgumentParser(description="Arthur Translation Layer CLI Scanner")
    parser.add_argument("command", choices=["sync", "clean", "resync", "status"], 
                        help="Command to run (sync, clean, resync, status)")
    
    args = parser.parse_args()

    if args.command == "sync":
        sync()
    elif args.command == "clean":
        clean()
    elif args.command == "resync":
        clean()
        sync()
    elif args.command == "status":
        status()

if __name__ == "__main__":
    main()
