#!/bin/bash
set -euo pipefail

# Install prerequisites (requires sudo)
sudo apt update
sudo apt install -y build-essential binutils make gcc g++ nasm grub-pc-bin xorriso qemu-system-x86 python3

PROJECT_DIR="/mnt/d/自制操作系统"
if [ ! -d "$PROJECT_DIR" ]; then
  echo "Project directory not found: $PROJECT_DIR"
  echo "Mount D: in WSL or adjust PROJECT_DIR in this script."
  exit 1
fi

cd "$PROJECT_DIR"

# Build userspace, embed and build kernel
./scripts/build_all.sh

# Run QEMU with serial attached
if [ -f os.iso ]; then
  echo "Starting QEMU (serial -> this terminal). Press Ctrl-A X to quit qemu-nox or use Ctrl-C." 
  qemu-system-x86_64 -m 512 -cdrom os.iso -serial stdio -display none
else
  echo "os.iso not found after build"
  exit 1
fi
