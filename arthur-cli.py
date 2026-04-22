import os
import sys
import shutil
import argparse
import subprocess
import threading
import urllib.request
import json
import tkinter as tk
from tkinter import messagebox, scrolledtext, filedialog, ttk
from pathlib import Path

# Application Version
VERSION = "v2.2.0-FINAL-FIX"

# Defaults
LINUX_VST3_DIR = Path.home() / ".vst3"
REAPER_CONFIG_DIR = Path.home() / ".config" / "REAPER"

# Path to the bridge library
BRIDGE_SO_PATH = Path(__file__).parent / "build" / "arthur_bridge.so"

def clear_reaper_cache():
    inifiles = ["reaper-vstplugins64.ini", "reaper-vstplugins64-failed.ini"]
    for ini in inifiles:
        p = REAPER_CONFIG_DIR / ini
        if p.exists():
            p.unlink()
    return True

def deploy_bridge():
    if not BRIDGE_SO_PATH.exists():
        return False, f"Bridge binary missing at {BRIDGE_SO_PATH}. Run build first!"
    
    # 1. Clean the target VST3 dir
    if LINUX_VST3_DIR.exists():
        shutil.rmtree(LINUX_VST3_DIR)
    LINUX_VST3_DIR.mkdir(parents=True)
    
    # 2. Search for plugins recursively (FabFilter is in a subfolder!)
    search_paths = [
        Path.home() / ".wine/drive_c/Program Files/Common Files/VST3",
        Path.home() / ".wine/drive_c/Program Files (x86)/Common Files/VST3"
    ]
    
    count = 0
    for base_p in search_paths:
        if base_p.exists():
            # Recursive search for .vst3
            for vst in base_p.rglob("*.vst3"):
                # If it's a directory (standard VST3 bundle)
                if vst.is_dir():
                    plugin_name = vst.stem
                    target_bundle = LINUX_VST3_DIR / f"{plugin_name}.vst3" / "Contents" / "x86_64-linux"
                    target_bundle.mkdir(parents=True, exist_ok=True)
                    shutil.copy(BRIDGE_SO_PATH, target_bundle / f"{plugin_name}.so")
                    count += 1
    
    return True, f"Successfully deployed {count} plugins to {LINUX_VST3_DIR}"

def run_gui():
    window = tk.Tk()
    window.title(f"Arthur FINAL REPAIR ({VERSION})")
    window.geometry("850x650")

    tk.Label(window, text="Arthur Universal Repair Manager", font=("Arial", 16, "bold")).pack(pady=10)

    log_area = scrolledtext.ScrolledText(window, width=90, height=25, bg="black", fg="#00FF00", font=("Monospace", 10))
    log_area.pack(pady=10, padx=10)

    def log(msg):
        log_area.insert(tk.END, msg + "\n")
        log_area.see(tk.END)
        window.update_idletasks()

    def on_full_repair():
        log_area.delete(1.0, tk.END)
        log(">>> INITIATING RECOVERY...")
        clear_reaper_cache()
        log(">>> [OK] Cache Cleared.")
        success, msg = deploy_bridge()
        if success:
            log(f">>> [OK] {msg}")
        else:
            log(f">>> [ERROR] {msg}")
        log("\n>>> RESTART REAPER NOW.")
        messagebox.showinfo("Arthur", "Recovery Complete. Restart REAPER.")

    btn_frame = tk.Frame(window)
    btn_frame.pack(pady=10)
    tk.Button(btn_frame, text="FULL REPAIR & SYNC", command=on_full_repair, width=30, height=2, bg="#4CAF50", fg="white", font=("Arial", 12, "bold")).pack()

    log(">>> System Ready. Click 'FULL REPAIR' to find and bridge FabFilter + other plugins.")
    window.mainloop()

if __name__ == "__main__":
    run_gui()
