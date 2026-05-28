#!/usr/bin/env bash

# Exit immediately if a command exits with a non-zero status
set -e

# Default settings
VM_IMG="win10.qcow2"
VM_MEM="4G"
SHM_FILE="/dev/shm/ivshmem"
SHM_SIZE="4M"
ISOLATED_CORES=(4 5 6 7) # The cores isolated via isolcpus

echo "=== Arthur: KVM Micro-VM Launcher ==="

# 1. Parse CLI arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -i|--image) VM_IMG="$2"; shift ;;
        -m|--memory) VM_MEM="$2"; shift ;;
        -s|--shm) SHM_FILE="$2"; shift ;;
        -c|--cores) IFS=',' read -r -a ISOLATED_CORES <<< "$2"; shift ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -i, --image <file>   Path to the Windows VM qcow2 image (default: win10.qcow2)"
            echo "  -m, --memory <size>  VM memory allocation (default: 4G)"
            echo "  -s, --shm <path>     Host path to the IVSHMEM shared memory device (default: /dev/shm/ivshmem)"
            echo "  -c, --cores <c1,c2>  List of physical cores to pin the vCPUs to (default: 4,5,6,7)"
            exit 0
            ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
    shift
done

# 2. Check for KVM availability
if [ ! -e /dev/kvm ]; then
    echo "[WARNING] /dev/kvm does not exist! VM will run without hardware acceleration (very slow)."
    KVM_ARG=""
else
    echo "[OK] KVM hardware acceleration detected."
    KVM_ARG="-enable-kvm"
fi

# 3. Initialize the IVSHMEM backing file
echo ">>> Initializing IVSHMEM shared memory segment..."
# QEMU requires the backing file to exist and have the exact size specified
if [ -f "$SHM_FILE" ]; then
    # Resize it just to be sure
    truncate -s "$SHM_SIZE" "$SHM_FILE"
else
    truncate -s "$SHM_SIZE" "$SHM_FILE"
fi
chmod 666 "$SHM_FILE"
echo "[OK] Shared memory file initialized at $SHM_FILE ($SHM_SIZE)"

# 4. Construct QEMU command line
# - Enable host CPU features
# - Allocate smp based on number of isolated cores
# - Bind IVSHMEM-plain device mapped to host /dev/shm/ivshmem
# - Enable VirtIO GPU for hardware accelerated UI routing
NUM_VCPUS=${#ISOLATED_CORES[@]}
echo ">>> Starting QEMU VM with $NUM_VCPUS vCPUs..."

# Check if image file exists, if not warn and create a dummy one for testing dry-run
if [ ! -f "$VM_IMG" ]; then
    echo "[WARNING] VM image $VM_IMG not found."
    echo "Creating a dummy image 'win10.qcow2' for configuration validation..."
    qemu-img create -f qcow2 "$VM_IMG" 10G
fi

qemu-system-x86_64 \
    $KVM_ARG \
    -cpu host,migratable=off \
    -smp "$NUM_VCPUS",sockets=1,cores="$NUM_VCPUS",threads=1 \
    -m "$VM_MEM" \
    -drive file="$VM_IMG",format=qcow2,if=virtio \
    -vga virtio \
    -display gtk,gl=on \
    -device ivshmem-plain,memdev=ivshmem_backend \
    -object memory-backend-file,id=ivshmem_backend,share=on,mem-path="$SHM_FILE",size="$SHM_SIZE" \
    -net nic,model=virtio -net user \
    -nographic \
    &

QEMU_PID=$!
echo "[OK] QEMU started with PID: $QEMU_PID"

# 5. Perform CPU Pinning on isolated cores
echo ">>> Waiting for VM vCPU threads to initialize..."
sleep 2

if [ -d "/proc/$QEMU_PID/task" ]; then
    CPU_IDX=0
    for TID in $(ls "/proc/$QEMU_PID/task"); do
        COMM_FILE="/proc/$QEMU_PID/task/$TID/comm"
        if [ -f "$COMM_FILE" ]; then
            COMM=$(cat "$COMM_FILE")
            # QEMU names vCPU threads like "CPU 0/KVM"
            if [[ "$COMM" == *"CPU"* ]]; then
                TARGET_CORE=${ISOLATED_CORES[$CPU_IDX]}
                if [ ! -z "$TARGET_CORE" ]; then
                    echo "=> Pinning thread $TID ($COMM) to Physical Core $TARGET_CORE"
                    # Apply taskset to pin the thread
                    taskset -pc "$TARGET_CORE" "$TID" > /dev/null
                    CPU_IDX=$((CPU_IDX + 1))
                fi
            fi
        fi
    done
    echo "[OK] CPU Pinning complete. Isolated cores ${ISOLATED_CORES[*]} are now dedicated to vCPUs."
else
    echo "[ERROR] Failed to read QEMU task directory. Pinning aborted."
fi

echo "=== VM Launcher Background Run Active ==="
echo "To terminate the VM, run: kill $QEMU_PID"
