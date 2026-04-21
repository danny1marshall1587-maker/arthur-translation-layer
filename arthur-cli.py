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
VERSION = "v1.4.8"

# Detect AppImage environment
APPDIR = os.environ.get('APPDIR')
OWNDIR = Path(os.environ.get('APPIMAGE', sys.argv[0])).parent

# Defaults
DEFAULT_WINE_PREFIX = Path.home() / ".wine"
WINE_VST3_DIR = DEFAULT_WINE_PREFIX / "drive_c" / "Program Files" / "Common Files" / "VST3"
LINUX_VST3_DIR = Path.home() / ".vst3" / "Arthur"
WINETRICKS_URL = "https://raw.githubusercontent.com/Winetricks/winetricks/master/src/winetricks"
GITHUB_API_URL = "https://api.github.com/repos/danny1marshall1587-maker/arthur-translation-layer/releases/latest"

# Standard Install Path
INSTALL_PATH = Path.home() / ".local" / "bin" / "arthur-manager"
DESKTOP_ENTRY_PATH = Path.home() / ".local" / "share" / "applications" / "arthur.desktop"

# Path to the bridge library
if APPDIR:
    BRIDGE_SO_PATH = Path(APPDIR) / "usr" / "lib" / "libarthur_bridge.so"
    WINE_CMD = "wine" 
else:
    BRIDGE_SO_PATH = Path.cwd() / "build" / "libarthur_bridge.so"
    WINE_CMD = "wine"

def setup_directories():
    """Ensure the target Linux VST3 directory exists."""
    LINUX_VST3_DIR.mkdir(parents=True, exist_ok=True)
    INSTALL_PATH.parent.mkdir(parents=True, exist_ok=True)

def find_wine_plugins():
    """Scans the Wine VST3 directory for plugin folders/files."""
    if not WINE_VST3_DIR.exists():
        return []

    plugins = []
    for item in WINE_VST3_DIR.iterdir():
        if item.suffix.lower() == '.vst3':
            plugins.append(item)
    return plugins

def install_desktop_entry():
    """Creates a desktop entry for the Start Menu integration."""
    try:
        DESKTOP_ENTRY_PATH.parent.mkdir(parents=True, exist_ok=True)
        # Use the current running file (AppImage or script) as the target
        current_app = os.environ.get('APPIMAGE', os.path.abspath(sys.argv[0]))
        
        # Add the no-fuse flag if we are running as an AppImage
        args = ""
        if os.environ.get('APPIMAGE'):
            args = "--appimage-extract-and-run"

        content = f"""[Desktop Entry]
Name=Arthur Manager
Exec="{current_app}" {args}
Icon=audio-x-generic
Type=Application
Categories=AudioVideo;Audio;
Comment=Arthur VST3 Translation Layer Manager
Terminal=false
"""
        with open(DESKTOP_ENTRY_PATH, "w") as f:
            f.write(content)
        DESKTOP_ENTRY_PATH.chmod(0o755)
        return True
    except Exception:
        return False

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
    window.title(f"Arthur Translation Layer Manager ({VERSION})")
    window.geometry("850x650")

    # Integrate into Start Menu on launch if not already there
    install_desktop_entry()

    label = tk.Label(window, text="Arthur Translation Layer", font=("Arial", 16, "bold"))
    label.pack(pady=10)

    btn_frame = tk.Frame(window)
    btn_frame.pack(pady=10)

    log_area = scrolledtext.ScrolledText(window, width=100, height=22)
    log_area.pack(pady=10, padx=10)

    def log(msg):
        log_area.insert(tk.END, msg + "\n")
        log_area.see(tk.END)
        window.update_idletasks()

    # Log Area
    log_area = scrolledtext.ScrolledText(window, width=90, height=20, bg="black", fg="#00FF00", font=("Monospace", 10))
    log_area.pack(pady=10, padx=10)

    # Progress Bar
    progress_frame = tk.Frame(window)
    progress_frame.pack(fill="x", padx=10, pady=5)
    progress_var = tk.DoubleVar()
    progress_bar = ttk.Progressbar(progress_frame, variable=progress_var, maximum=100)
    progress_bar.pack(fill="x")

    def log(msg):
        log_area.insert(tk.END, msg + "\n")
        log_area.see(tk.END)
        window.update_idletasks()

    def update_progress(val):
        progress_var.set(val)
        window.update_idletasks()

    def update_worker(download_url):
        log(f">>> DOWNLOADING UPDATE: {download_url}")
        update_progress(10)
        try:
            temp_path = INSTALL_PATH.with_suffix(".tmp")
            urllib.request.urlretrieve(download_url, temp_path)
            update_progress(90)
            temp_path.chmod(0o755)
            
            if os.environ.get('APPIMAGE'):
                log("[SUCCESS] Update downloaded successfully.")
                log(f"[ACTION REQUIRED] Please replace your current AppImage with the new one found at: {temp_path}")
                messagebox.showinfo("Update Ready", f"The new version has been downloaded to:\n\n{temp_path}\n\nPlease replace your current AppImage file with this one and restart.")
            else:
                log("[SUCCESS] Update downloaded. Ready to replace.")
                shutil.move(temp_path, INSTALL_PATH)
                log("Update applied to ~/.local/bin/arthur-manager.")
            update_progress(100)
        except Exception as e:
            log(f"[ERROR] Failed to download update: {e}")
            update_progress(0)

    def on_check_updates():
        log(">>> Checking for updates...")
        try:
            with urllib.request.urlopen(GITHUB_API_URL) as response:
                data = json.loads(response.read().decode())
                latest_tag = data.get("tag_name")
                if latest_tag and latest_tag != VERSION:
                    assets = data.get("assets", [])
                    appimage_asset = next((a for a in assets if "AppImage" in a["name"]), None)
                    if appimage_asset:
                        if messagebox.askyesno("Update Available", f"A new version ({latest_tag}) is available!\n\nWould you like to download and install it now?"):
                            threading.Thread(target=update_worker, args=(appimage_asset["browser_download_url"],), daemon=True).start()
                    else:
                        log(f"New version {latest_tag} found, but no AppImage asset detected.")
                else:
                    log("You are running the latest version.")
        except Exception as e:
            log(f"[ERROR] Could not check for updates: {e}")

    def on_install_to_system(silent=False):
        """Copies the running AppImage to the standard install path."""
        try:
            appimage_path = os.environ.get('APPIMAGE')
            if not appimage_path:
                if not silent:
                    messagebox.showwarning("Installation", "You are not running from an AppImage. Installation is only supported for AppImage versions.")
                return

            if Path(appimage_path) == INSTALL_PATH:
                if not silent:
                    messagebox.showinfo("Installation", "Arthur is already installed and running from the system location.")
                return

            if silent or messagebox.askyesno("Install to System", f"Welcome to Arthur!\n\nWould you like to install Arthur to your system?\n\nThis will copy it to: {INSTALL_PATH}\nand add it to your Start Menu."):
                log(">>> Installing Arthur to system...")
                update_progress(20)
                setup_directories()
                update_progress(50)
                shutil.copy2(appimage_path, INSTALL_PATH)
                INSTALL_PATH.chmod(0o755)
                update_progress(80)
                install_desktop_entry() 
                update_progress(100)
                log(f"[SUCCESS] Arthur installed to {INSTALL_PATH}")
                log("[INFO] You can now delete the AppImage from your Downloads folder.")
                if not silent:
                    messagebox.showinfo("Installation Success", "Arthur has been installed to your system!\n\nYou can now launch it from your application menu.")
        except Exception as e:
            log(f"[ERROR] Installation failed: {e}")
            update_progress(0)

    def install_worker(file_paths):
        log(f">>> Batch Installation Started: {len(file_paths)} installers queued.")
        total = len(file_paths)
        for i, file_path in enumerate(file_paths):
            log(f"\n[{i+1}/{total}] Installing: {os.path.basename(file_path)}")
            update_progress((i / total) * 100)
            try:
                process = subprocess.Popen([WINE_CMD, file_path])
                process.wait()
                log(f"[SUCCESS] Finished installer: {os.path.basename(file_path)}")
            except Exception as e:
                log(f"[ERROR] Failed during installation of {file_path}: {e}")
        
        update_progress(100)
        log("\n>>> ALL INSTALLATIONS COMPLETE.")
        log("[TIP] Click 'Scan & Sync Plugins' now to bridge your new tools!")

    def on_install():
        file_paths = filedialog.askopenfilenames(
            title="Select Windows Installers (Select Multiple!)",
            filetypes=[("Installers", "*.exe *.msi"), ("All files", "*.*")]
        )
        if file_paths:
            threading.Thread(target=install_worker, args=(file_paths,), daemon=True).start()

    def prep_worker():
        log(">>> PREPARING WINE FOR PRO AUDIO...")
        update_progress(5)
        winetricks_path = Path.home() / ".cache" / "arthur" / "winetricks"
        winetricks_path.parent.mkdir(parents=True, exist_ok=True)

        if not winetricks_path.exists():
            log("[1/3] Downloading winetricks...")
            try:
                urllib.request.urlretrieve(WINETRICKS_URL, winetricks_path)
                winetricks_path.chmod(0o755)
            except Exception as e:
                log(f"[ERROR] Failed to download winetricks: {e}")
                return
        
        update_progress(20)
        log("[2/3] Installing core libraries (vcrun2015, d3dcompiler_47, corefonts)...")
        try:
            subprocess.run([str(winetricks_path), "-q", "vcrun2015", "d3dcompiler_47", "corefonts"], check=True)
            log("[SUCCESS] Core libraries installed.")
        except Exception as e:
            log(f"[ERROR] Failed to install core libraries: {e}")

        update_progress(60)
        log("[3/3] Installing DXVK (Vulkan Graphics)...")
        try:
            subprocess.run([str(winetricks_path), "-q", "dxvk"], check=True)
            log("[SUCCESS] DXVK enabled.")
        except Exception as e:
            log(f"[ERROR] Failed to enable DXVK: {e}")

        update_progress(100)
        log("\n>>> WINE ENVIRONMENT IS NOW PRO-READY.")

    def on_prepare():
        if messagebox.askyesno("Prepare Wine", "This will install Visual C++, DirectX components, and Vulkan Graphics into your Wine prefix. Recommended for high-end plugins.\n\nContinue?"):
            log_area.delete(1.0, tk.END)
            threading.Thread(target=prep_worker, daemon=True).start()

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

    tk.Button(btn_frame, text="Prepare Wine for Pro Audio", command=on_prepare, width=22, bg="#FF9800", fg="white").grid(row=0, column=0, padx=5)
    tk.Button(btn_frame, text="Batch Install (.exe/.msi)", command=on_install, width=22, bg="#2196F3", fg="white").grid(row=0, column=1, padx=5)
    tk.Button(btn_frame, text="Scan & Sync Plugins", command=on_sync, width=20, bg="#4CAF50", fg="white").grid(row=0, column=2, padx=5)
    tk.Button(btn_frame, text="Check Updates", command=on_check_updates, width=15, bg="#9C27B0", fg="white").grid(row=0, column=3, padx=5)
    
    status_frame = tk.Frame(window)
    status_frame.pack(pady=5)
    tk.Button(status_frame, text="Install to System", command=on_install_to_system, width=15, bg="#607D8B", fg="white").grid(row=0, column=0, padx=5)
    tk.Button(status_frame, text="Show Status", command=on_status, width=12).grid(row=0, column=1, padx=5)
    tk.Button(status_frame, text="Clean All", command=on_clean, width=12, bg="#F44336", fg="white").grid(row=0, column=2, padx=5)

    on_status()
    
    # Auto-Install Logic for New Users (Schedules on Main Thread to avoid deadlocks)
    def check_first_run():
        appimage_path = os.environ.get('APPIMAGE')
        if appimage_path and Path(appimage_path) != INSTALL_PATH:
            log("\n[!] Arthur is not installed to your system yet.")
            on_install_to_system(silent=False)

    window.after(500, check_first_run)
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

