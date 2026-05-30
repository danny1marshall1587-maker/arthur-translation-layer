#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

export PATH="/usr/sbin:/sbin:/usr/bin:/bin:$PATH"

CORES="4-7"
DRY_RUN=false

echo "=== Arthur: Cross-Distro Auto-Tuner & VDC Manager ==="

# 1. Parse arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -c|--cores) CORES="$2"; shift ;;
        -d|--dry-run) DRY_RUN=true ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -c, --cores <range>   CPU core range to isolate (default: 4-7)"
            echo "  -d, --dry-run         Print proposed modifications without applying them"
            echo "  -h, --help            Show help"
            exit 0
            ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
    shift
done

echo "Target Core Isolation Range: $CORES"
if [ "$DRY_RUN" = true ]; then
    echo "[DRY RUN MODE] No changes will be written to the system."
fi

# 2. Check for root permissions (if not dry run)
if [ "$DRY_RUN" = false ] && [ "$EUID" -ne 0 ]; then
    echo "[ERROR] This script must be run as root to modify system configurations."
    echo "Please run: sudo $0 -c $CORES"
    exit 1
fi

# 3. Detect Linux Distribution
OS_ID=""
OS_NAME=""
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID="$ID"
    OS_NAME="$NAME"
fi
echo "[INFO] Detected OS: $OS_NAME ($OS_ID)"

# Determine appropriate audio/realtime group based on distribution
AUDIO_GROUP="audio"
case "$OS_ID" in
    fedora|rhel|centos)
        AUDIO_GROUP="realtime"
        ;;
    ubuntu|debian|mx|mint|arch|cachyos|manjaro)
        AUDIO_GROUP="audio"
        ;;
    *)
        # Fallback to audio if we don't know
        AUDIO_GROUP="audio"
        ;;
esac

# 4. User Group Membership
TARGET_USER="${SUDO_USER:-$(logname 2>/dev/null || echo $USER)}"
if [ "$DRY_RUN" = true ]; then
    echo "=> [PROPOSED] Add current user ($TARGET_USER) to group: $AUDIO_GROUP"
else
    # Create group if it doesn't exist
    if ! getent group "$AUDIO_GROUP" >/dev/null; then
        echo "Creating group $AUDIO_GROUP..."
        groupadd "$AUDIO_GROUP"
    fi
    # Add user
    if [ ! -z "$TARGET_USER" ] && [ "$TARGET_USER" != "root" ]; then
        echo "Adding user $TARGET_USER to group $AUDIO_GROUP..."
        usermod -aG "$AUDIO_GROUP" "$TARGET_USER"
        echo "[OK] User $TARGET_USER is now a member of $AUDIO_GROUP."
    fi
fi

# 5. Real-Time Limits Configuration (/etc/security/limits.d/)
LIMITS_FILE="/etc/security/limits.d/99-arthur-realtime.conf"
if [ "$DRY_RUN" = true ]; then
    echo "=> [PROPOSED] Write real-time limits config to $LIMITS_FILE:"
    echo "   @$AUDIO_GROUP - rtprio 95"
    echo "   @$AUDIO_GROUP - memlock unlimited"
else
    echo "Configuring real-time priority limits..."
    mkdir -p /etc/security/limits.d
    cat <<EOF > "$LIMITS_FILE"
# Arthur Translation Layer Real-Time Limits
@$AUDIO_GROUP - rtprio 95
@$AUDIO_GROUP - memlock unlimited
EOF
    echo "[OK] Real-time limits configured in $LIMITS_FILE"
fi

# 6. CPU Governor Configuration (Real-Time Performance)
echo ">>> Tuning CPU frequency scaling governor..."
if [ "$DRY_RUN" = true ]; then
    echo "=> [PROPOSED] Set scaling governor to 'performance' for all cores:"
    echo "   echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"
else
    # Apply performance governor directly
    if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
        for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            if [ -f "$gov" ]; then
                echo "performance" > "$gov"
            fi
        done
        echo "[OK] CPU scaling governor locked to 'performance' for all active cores."
    else
        echo "[WARNING] CPU frequency scaling interface not found. Skipping governor lock."
    fi
fi

# 7. Bootloader Configuration (GRUB vs systemd-boot detection)
NEW_PARAMS="isolcpus=$CORES nohz_full=$CORES rcu_nocbs=$CORES"
BOOTLOADER_UPDATED=false

echo ">>> Inspecting bootloader configurations..."

# A. Try GRUB (/etc/default/grub)
GRUB_DEFAULT="/etc/default/grub"
if [ -f "$GRUB_DEFAULT" ]; then
    echo "[OK] GRUB configuration file found at $GRUB_DEFAULT"
    CURRENT_LINE=$(grep "GRUB_CMDLINE_LINUX_DEFAULT" "$GRUB_DEFAULT" || true)
    
    if [[ "$CURRENT_LINE" == *"$NEW_PARAMS"* ]]; then
        echo "[OK] Core isolation parameters are already present in GRUB."
        BOOTLOADER_UPDATED=true
    else
        if [ "$DRY_RUN" = true ]; then
            echo "=> [PROPOSED] Add parameters to GRUB:"
            echo "   $NEW_PARAMS"
        else
            echo "Adding isolation parameters to $GRUB_DEFAULT..."
            sed -i "s/GRUB_CMDLINE_LINUX_DEFAULT=\"\([^\"]*\)\"/GRUB_CMDLINE_LINUX_DEFAULT=\"\1 $NEW_PARAMS\"/" "$GRUB_DEFAULT"
            
            # Run grub-update depending on the distro
            echo "Updating bootloader..."
            if command -v update-grub &>/dev/null; then
                update-grub
            elif command -v grub2-mkconfig &>/dev/null; then
                grub2-mkconfig -o /boot/grub2/grub.cfg
            elif command -v grub-mkconfig &>/dev/null; then
                grub-mkconfig -o /boot/grub/grub.cfg
            else
                # Fallback paths for common configs
                if [ -f /boot/grub/grub.cfg ]; then
                    grub-mkconfig -o /boot/grub/grub.cfg
                elif [ -f /boot/grub2/grub.cfg ]; then
                    grub2-mkconfig -o /boot/grub2/grub.cfg
                else
                    echo "[WARNING] GRUB config updated but update command not found. Please run update-grub manually."
                fi
            fi
            echo "[OK] GRUB updated."
            BOOTLOADER_UPDATED=true
        fi
    fi
fi

# B. Try systemd-boot (Check ESP mounts)
# Common mounts for ESP: /boot, /efi, /boot/efi
for ESP in /boot /efi /boot/efi; do
    if [ "$BOOTLOADER_UPDATED" = false ] && [ -d "$ESP/loader/entries" ]; then
        echo "[OK] systemd-boot layout detected at $ESP/loader/"
        
        # Locate the default loader entry
        if [ -f "$ESP/loader/loader.conf" ]; then
            DEFAULT_ENTRY=$(grep "^default" "$ESP/loader/loader.conf" | awk '{print $2}' || true)
            # Remove wildcard or suffix if present
            DEFAULT_ENTRY="${DEFAULT_ENTRY%.conf}.conf"
            
            ENTRY_FILE="$ESP/loader/entries/$DEFAULT_ENTRY"
            if [ -f "$ENTRY_FILE" ]; then
                echo "Found active entry config: $ENTRY_FILE"
                CURRENT_OPTIONS=$(grep "^options" "$ENTRY_FILE" || true)
                
                if [[ "$CURRENT_OPTIONS" == *"$NEW_PARAMS"* ]]; then
                    echo "[OK] Core isolation parameters are already present in systemd-boot."
                    BOOTLOADER_UPDATED=true
                else
                    if [ "$DRY_RUN" = true ]; then
                        echo "=> [PROPOSED] Add parameters to systemd-boot entry option:"
                        echo "   $NEW_PARAMS"
                    else
                        echo "Adding isolation parameters to $ENTRY_FILE..."
                        # Append parameters to the options line
                        sed -i "s/^options \(.*\)/options \1 $NEW_PARAMS/" "$ENTRY_FILE"
                        echo "[OK] systemd-boot options updated."
                        BOOTLOADER_UPDATED=true
                    fi
                fi
            fi
        fi
    fi
done

# C. Try Limine (/boot/limine.conf)
LIMINE_CONF="/boot/limine.conf"
if [ "$BOOTLOADER_UPDATED" = false ] && [ -f "$LIMINE_CONF" ]; then
    echo "[OK] Limine configuration file found at $LIMINE_CONF"
    
    if grep -q "$NEW_PARAMS" "$LIMINE_CONF"; then
        echo "[OK] Core isolation parameters are already present in Limine."
        BOOTLOADER_UPDATED=true
    else
        if [ "$DRY_RUN" = true ]; then
            echo "=> [PROPOSED] Add parameters to Limine entries cmdline:"
            echo "   $NEW_PARAMS"
        else
            echo "Adding isolation parameters to $LIMINE_CONF..."
            # Modify all cmdline lines by appending the parameters
            sed -i "s/\(^[[:space:]]*cmdline:.*\)/\1 $NEW_PARAMS/" "$LIMINE_CONF"
            echo "[OK] Limine configuration updated."
            BOOTLOADER_UPDATED=true
        fi
    fi
fi

if [ "$BOOTLOADER_UPDATED" = false ]; then
    echo "[WARNING] Could not identify or modify an active bootloader configuration."
    echo "If you are using an alternative bootloader (like rEFInd), please add the following parameters manually:"
    echo "  $NEW_PARAMS"
fi

# 8. Check Sound Server Setup
echo ">>> Checking sound server configuration..."
if command -v pw-cli &>/dev/null; then
    echo "[OK] PipeWire is installed."
else
    echo "[WARNING] PipeWire command-line tools not found. If this distro runs legacy JACK/PulseAudio, please consider installing PipeWire for optimal CLLS latency sync support."
fi

echo "=== VDC Tuning Process Completed ==="
if [ "$DRY_RUN" = false ]; then
    echo "Please REBOOT your system to apply core isolation boot parameters and group memberships."
fi
