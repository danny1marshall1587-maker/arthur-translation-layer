#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

INSTALLER_PATH=""
WINEPREFIX_DIR="$HOME/.wine"
LINUX_VST3_DIR="$HOME/.vst3"
BRIDGE_SRC_DIR="$(pwd)"

if [ -f "/.flatpak-info" ] || [ -d "/app" ]; then
    BRIDGE_SO_PATH="/app/lib/arthur_bridge.so"
else
    BRIDGE_SO_PATH="$BRIDGE_SRC_DIR/build/arthur_bridge.so"
    if [ ! -f "$BRIDGE_SO_PATH" ]; then
        SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
        BRIDGE_SO_PATH="$SCRIPT_DIR/build/arthur_bridge.so"
    fi
fi

echo "=== Arthur: Installer Router & Auto-Bridger ==="

# 1. Parse CLI arguments
if [ -z "$1" ]; then
    echo "Usage: $0 <path_to_installer_exe_or_msi>"
    exit 1
fi
INSTALLER_PATH="$1"

if [ ! -f "$INSTALLER_PATH" ]; then
    echo "[ERROR] Installer file not found: $INSTALLER_PATH"
    exit 1
fi

# Ensure Linux VST3 folder exists
mkdir -p "$LINUX_VST3_DIR"

# 2. Get timestamp before installation to identify newly added plugins
echo ">>> Checking existing plugins..."
BEFORE_FILE_LIST=$(find "$WINEPREFIX_DIR/drive_c/Program Files/Common Files/VST3" -type f -name "*.vst3" 2>/dev/null || true)

# 3. Route installer to Wine/VM
echo ">>> Launching installer GUI: $INSTALLER_PATH"
EXT="${INSTALLER_PATH##*.}"

if [ "$EXT" = "msi" ]; then
    wine msiexec /i "$INSTALLER_PATH"
else
    wine "$INSTALLER_PATH"
fi

echo ">>> Installer finished. Scanning for new plugins..."

# 4. Identify newly added VST3 DLLs
AFTER_FILE_LIST=$(find "$WINEPREFIX_DIR/drive_c/Program Files/Common Files/VST3" -type f -name "*.vst3" 2>/dev/null || true)

NEW_PLUGINS=()
while IFS= read -r file; do
    if [ ! -z "$file" ] && ! echo "$BEFORE_FILE_LIST" | grep -q "$file"; then
        NEW_PLUGINS+=("$file")
    fi
done <<< "$AFTER_FILE_LIST"

if [ ${#NEW_PLUGINS[@]} -eq 0 ]; then
    echo "[WARNING] No new VST3 files were detected in the standard path."
    echo "If your plugin installed to a custom folder, please bridge it manually."
    exit 0
fi

# 5. Compile and sync a Linux bridge for each new plugin
echo ">>> Found ${#NEW_PLUGINS[@]} new plugin(s) to bridge:"
for plugin in "${NEW_PLUGINS[@]}"; do
    # Get plugin filename (e.g. Synth.vst3)
    PLUGIN_FILENAME=$(basename "$plugin")
    PLUGIN_BASENAME="${PLUGIN_FILENAME%.*}"
    
    echo "  - Bridging: $PLUGIN_FILENAME"
    
    # Create copy of arthur_bridge.so compiled with this name
    # In VST3, the host loader maps dlls by their filename.
    # So we copy our arthur_bridge.so to ~/.vst3/PluginName.vst3/Contents/x86_64-linux/PluginName.so
    BRIDGE_BUNDLE_DIR="$LINUX_VST3_DIR/$PLUGIN_BASENAME.vst3/Contents/x86_64-linux"
    mkdir -p "$BRIDGE_BUNDLE_DIR"
    
    # Copy host bridge
    if [ -f "$BRIDGE_SO_PATH" ]; then
        cp "$BRIDGE_SO_PATH" "$BRIDGE_BUNDLE_DIR/$PLUGIN_BASENAME.so"
        echo "    [OK] Copied Linux bridge to $BRIDGE_BUNDLE_DIR/$PLUGIN_BASENAME.so"
    else
        echo "    [ERROR] arthur_bridge.so not found at $BRIDGE_SO_PATH! Please run make first."
        exit 1
    fi
    
    # Write metadata association so arthur-daemon knows which guest DLL this bridge links to
    METADATA_FILE="$LINUX_VST3_DIR/$PLUGIN_BASENAME.vst3/arthur.metadata"
    echo "guest_dll_path=$plugin" > "$METADATA_FILE"
    echo "    [OK] Wrote bridge metadata association to $METADATA_FILE"
done

echo "=== Auto-Bridging Process Completed ==="
