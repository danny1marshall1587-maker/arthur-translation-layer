#!/usr/bin/env python3

import os
import sys
import shutil
import argparse
import subprocess
import tkinter as tk
from tkinter import messagebox, scrolledtext, filedialog
from pathlib import Path

# Detect AppImage environment
APPDIR = os.environ.get('APPDIR')

# Defaults
DEFAULT_WINE_PREFIX = Path.home() / ".wine"
WINE_VST3_DIR = DEFAULT_WINE_PREFIX / "drive_c" / "Program Files" / "Common Files" / "VST3"
LINUX_VST3_DIR = Path.home() / ".vst3" / "Arthur"

# Path to the bridge library
if APPDIR:
    BRIDGE_SO_PATH = Path(APPDIR) / "usr" / "lib" / "libarthur_bridge.so"
    WINE_CMD = "wine" # Should be in PATH via AppRun
else:
    BRIDGE_SO_PATH = Path.cwd() / "build" / "libarthur_bridge.so"
    WINE_CMD = "wine"

def setup_directories():
    """Ensure the target Linux VST3 directory exists."""
    LINUX_VST3_DIR.mkdir(parents=True, exist_ok=True)

def find_wine_plugins():
    """Scans the Wine VST3 directory for plugin folders/files."""
    if not WINE_VST3_DIR.exists():
        return []

    plugins = []
    for item in WINE_VST3_DIR.iterdir():
        if item.suffix.lower() == '.vst3':
            plugins.append(item)
    return plugins

def sync(log_callback=print):
    """Scans Wine directory and creates bridge links."""
    log_callback(">>> Scanning for Windows VST3 plugins...")
    
    if not BRIDGE_SO_PATH.exists():
        log_callback(f"[ERROR] Bridge library not found at {BRIDGE_SO_PATH}")
        return

    setup_directories()
    plugins = find_wine_plugins()

    if not plugins:
        log_callback("No plugins found to sync. Try installing some first!")
        return

    synced_count = 0
    for plugin in plugins:
        target_link = LINUX_VST3_DIR / plugin.name
        if target_link.exists():
            log_callback(f"[-] Skipping (already exists): {plugin.name}")
            continue

        try:
            os.symlink(BRIDGE_SO_PATH, target_link)
            log_callback(f"[+] Synced: {plugin.name}")
            synced_count += 1
        except Exception as e:
            log_callback(f"[ERROR] Failed to sync {plugin.name}: {e}")

    log_callback(f"\n>>> Sync Complete! {synced_count} new plugins bridged.")

def clean(log_callback=print):
    """Removes all bridged plugins."""
    log_callback(">>> Cleaning Arthur bridged plugins...")
    if not LINUX_VST3_DIR.exists():
        log_callback("Nothing to clean.")
        return

    removed_count = 0
    for item in LINUX_VST3_DIR.iterdir():
        if item.is_symlink() or item.is_file():
            try:
                item.unlink()
                removed_count += 1
            except Exception as e:
                log_callback(f"[ERROR] Failed to remove {item.name}: {e}")

    log_callback(f">>> Clean Complete! Removed {removed_count} bridged plugins.")

def run_gui():
    """Simple Tkinter GUI for the Arthur Translation Layer."""
    window = tk.Tk()
    window.title("Arthur Translation Layer Manager")
    window.geometry("700x500")

    label = tk.Label(window, text="Arthur Translation Layer", font=("Arial", 16, "bold"))
    label.pack(pady=10)

    btn_frame = tk.Frame(window)
    btn_frame.pack(pady=10)

    log_area = scrolledtext.ScrolledText(window, width=80, height=15)
    log_area.pack(pady=10, padx=10)

    def log(msg):
        log_area.insert(tk.END, msg + "\n")
        log_area.see(tk.END)
        window.update_idletasks()

    def on_sync():
        log_area.delete(1.0, tk.END)
        sync(log)
    
    def on_clean():
        log_area.delete(1.0, tk.END)
        clean(log)

    def on_install():
        file_path = filedialog.askopenfilename(
            title="Select Windows Installer",
            filetypes=[("Installers", "*.exe *.msi"), ("All files", "*.*")]
        )
        if file_path:
            log(f">>> Running Installer: {file_path}")
            try:
                # Run the installer using the bundled wine
                subprocess.Popen([WINE_CMD, file_path])
                log("[INFO] Installer started. Please follow the instructions in the installer window.")
                log("[TIP] After installation, click 'Scan & Sync Plugins' to bridge the new plugin.")
            except Exception as e:
                log(f"[ERROR] Failed to start installer: {e}")

    def on_status():
        log_area.delete(1.0, tk.END)
        log(">>> Arthur Translation Layer Status\n")
        log(f"Wine VST3 Path:  {WINE_VST3_DIR}")
        log(f"Linux VST3 Path: {LINUX_VST3_DIR}\n")
        if not LINUX_VST3_DIR.exists() or not any(LINUX_VST3_DIR.iterdir()):
            log("No plugins are currently synced.")
        else:
            log("Currently Synced Plugins:")
            for item in LINUX_VST3_DIR.iterdir():
                log(f"  - {item.name}")

    tk.Button(btn_frame, text="Install New Plugin (.exe)", command=on_install, width=22, bg="#2196F3", fg="white").grid(row=0, column=0, padx=5)
    tk.Button(btn_frame, text="Scan & Sync Plugins", command=on_sync, width=22, bg="#4CAF50", fg="white").grid(row=0, column=1, padx=5)
    tk.Button(btn_frame, text="Show Status", command=on_status, width=15).grid(row=0, column=2, padx=5)
    tk.Button(btn_frame, text="Clean All", command=on_clean, width=15, bg="#F44336", fg="white").grid(row=0, column=3, padx=5)

    on_status()
    window.mainloop()

def main():
    parser = argparse.ArgumentParser(description="Arthur Translation Layer CLI Scanner")
    parser.add_argument("command", nargs="?", choices=["sync", "clean", "resync", "status", "gui"], 
                        default="gui", help="Command to run")
    
    args = parser.parse_args()

    if args.command == "gui":
        run_gui()
    elif args.command == "sync":
        sync()
    elif args.command == "clean":
        clean()
    elif args.command == "resync":
        clean()
        sync()
    elif args.command == "status":
        status_terminal()

def status_terminal():
    print(">>> Arthur Translation Layer Status\n")
    print(f"Wine VST3 Path:  {WINE_VST3_DIR}")
    print(f"Linux VST3 Path: {LINUX_VST3_DIR}\n")
    if not LINUX_VST3_DIR.exists() or not any(LINUX_VST3_DIR.iterdir()):
        print("No plugins are currently synced.")
    else:
        print("Currently Synced Plugins:")
        for item in LINUX_VST3_DIR.iterdir():
            print(f"  - {item.name}")

if __name__ == "__main__":
    main()

if __name__ == "__main__":
    main()
