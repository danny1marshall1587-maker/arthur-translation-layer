#!/usr/bin/env bash

# ==============================================================================
# Arthur: CachyOS Audio Tuner & Real-Time Performance Optimiser
# ==============================================================================
# Automates core isolation (cores 4-7), real-time limits, PipeWire low-latency
# configurations, and VST3 host-bridge linking.
# ==============================================================================

set -euo pipefail

# Output Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Helper Functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}
log_success() {
    echo -e "${GREEN}[OK]${NC} $1"
}
log_warn() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}
log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 1. Check Root Privileges
if [ "$EUID" -ne 0 ]; then
    log_error "This script must be run as root (sudo) to apply kernel, bootloader, and system-wide limit changes."
    echo "Usage: sudo $0 [options]"
    exit 1
fi

CORES="4-7"
TARGET_USER="${SUDO_USER:-$(logname 2>/dev/null || echo $USER)}"
USER_HOME=$(eval echo "~$TARGET_USER")

log_info "=== Starting CachyOS Audio Tuner ==="
log_info "Target User: $TARGET_USER"
log_info "User Home Directory: $USER_HOME"
log_info "Isolated CPU Core Range: $CORES"

# 2. Add User to Audio and Realtime Groups
log_info "Configuring group memberships..."
for group in audio realtime; do
    if ! getent group "$group" >/dev/null; then
        log_info "Creating group '$group'..."
        groupadd "$group"
    fi
    log_info "Adding $TARGET_USER to group '$group'..."
    usermod -aG "$group" "$TARGET_USER"
done
log_success "User groups configured."

# 3. Real-Time Security Limits Configuration
LIMITS_FILE="/etc/security/limits.d/99-arthur-realtime.conf"
log_info "Writing real-time security limits configuration to $LIMITS_FILE..."
mkdir -p /etc/security/limits.d
cat <<EOF > "$LIMITS_FILE"
# Arthur VDC Real-time and Memlock Privileges
@audio          -       rtprio          98
@audio          -       memlock         unlimited
@realtime       -       rtprio          98
@realtime       -       memlock         unlimited
EOF
log_success "Real-time limits written."

# 4. CPU Governor Lock (Performance Mode)
log_info "Setting CPU frequency governor to performance..."
if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
    for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        if [ -f "$governor" ]; then
            echo "performance" > "$governor"
        fi
    done
    log_success "CPU scaling governor locked to 'performance' for all cores."
else
    log_warn "CPU frequency scaling interface not found. Scaling governor tune skipped."
fi

# 5. Bootloader Tuning (systemd-boot vs GRUB)
BOOTLOADER_UPDATED=false
PARAMS="isolcpus=$CORES nohz_full=$CORES rcu_nocbs=$CORES"

log_info "Inspecting active bootloader configurations to apply isolated cores ($CORES)..."

# systemd-boot Check (Standard on CachyOS)
for ESP in /boot /efi /boot/efi; do
    if [ -d "$ESP/loader/entries" ]; then
        log_info "systemd-boot layout detected at $ESP/loader/"
        if [ -f "$ESP/loader/loader.conf" ]; then
            DEFAULT_ENTRY=$(grep "^default" "$ESP/loader/loader.conf" | awk '{print $2}' || true)
            DEFAULT_ENTRY="${DEFAULT_ENTRY%.conf}.conf"
            ENTRY_FILE="$ESP/loader/entries/$DEFAULT_ENTRY"
            if [ -f "$ENTRY_FILE" ]; then
                log_info "Modifying default systemd-boot entry: $ENTRY_FILE"
                if grep -q "isolcpus" "$ENTRY_FILE"; then
                    log_warn "Core isolation options already present in systemd-boot entry options."
                else
                    sed -i "s/^options \(.*\)/options \1 $PARAMS/" "$ENTRY_FILE"
                    log_success "Appended core isolation parameters to systemd-boot entry options."
                fi
                BOOTLOADER_UPDATED=true
            fi
        fi
    fi
done

# GRUB Check
GRUB_CONFIG="/etc/default/grub"
if [ "$BOOTLOADER_UPDATED" = false ] && [ -f "$GRUB_CONFIG" ]; then
    log_info "GRUB layout detected at $GRUB_CONFIG"
    if grep -q "isolcpus" "$GRUB_CONFIG"; then
        log_warn "Core isolation options already present in GRUB configuration."
    else
        sed -i "s/GRUB_CMDLINE_LINUX_DEFAULT=\"\([^\"]*\)\"/GRUB_CMDLINE_LINUX_DEFAULT=\"\1 $PARAMS\"/" "$GRUB_CONFIG"
        log_info "Updating GRUB configuration..."
        if command -v update-grub &>/dev/null; then
            update-grub
        elif command -v grub-mkconfig &>/dev/null; then
            grub-mkconfig -o /boot/grub/grub.cfg
        elif command -v grub2-mkconfig &>/dev/null; then
            grub2-mkconfig -o /boot/grub2/grub.cfg
        fi
        log_success "Core isolation parameters added to GRUB."
        BOOTLOADER_UPDATED=true
    fi
fi

if [ "$BOOTLOADER_UPDATED" = false ]; then
    log_warn "Could not automatically modify bootloader cmdline options. Please add the following manually:"
    echo "  $PARAMS"
fi

# 6. PipeWire Real-time Client Configurations
PW_CONF_DIR="$USER_HOME/.config/pipewire"
log_info "Configuring user-specific PipeWire client realtime priority..."
sudo -u "$TARGET_USER" mkdir -p "$PW_CONF_DIR"

# Copy default templates if they don't exist
for conf in client.conf jack.conf; do
    SYS_CONF="/usr/share/pipewire/$conf"
    USER_CONF="$PW_CONF_DIR/$conf"
    if [ -f "$SYS_CONF" ] && [ ! -f "$USER_CONF" ]; then
        sudo -u "$TARGET_USER" cp "$SYS_CONF" "$USER_CONF"
    fi
    
    # Configure real-time priority properties
    if [ -f "$USER_CONF" ]; then
        log_info "Tuning real-time priorities in $USER_CONF..."
        # Uncomment/modify rt.prio settings
        sudo -u "$TARGET_USER" sed -i 's/#\?\(rt.prio\s*=\s*\)[0-9]*/\195/' "$USER_CONF"
        sudo -u "$TARGET_USER" sed -i 's/#\?\(rt.time.soft\s*=\s*\)-*[0-9]*/\1-1/' "$USER_CONF"
        sudo -u "$TARGET_USER" sed -i 's/#\?\(rt.time.hard\s*=\s*\)-*[0-9]*/\1-1/' "$USER_CONF"
    fi
done
log_success "PipeWire client configuration updated."

# 7. Bridge Loader Generation helper
log_info "Setting up local VST3 user folder for native bridge links..."
sudo -u "$TARGET_USER" mkdir -p "$USER_HOME/.vst3"

# Link compiled host bridge if available
HOST_BRIDGE_SO="$(pwd)/build/arthur_bridge.so"
if [ -f "$HOST_BRIDGE_SO" ]; then
    log_info "Host VST3 Bridge exists. Copying to user ~/.vst3/..."
    sudo -u "$TARGET_USER" cp "$HOST_BRIDGE_SO" "$USER_HOME/.vst3/arthur_bridge.so"
    log_success "arthur_bridge.so copied to $USER_HOME/.vst3/arthur_bridge.so"
else
    log_warn "Compiled host bridge not found at $HOST_BRIDGE_SO. Please run: cmake --build build"
fi

log_success "CachyOS Audio Tuner completed successfully."
log_warn "Please REBOOT your system to apply core isolation boot parameters and group permissions."
