#!/bin/bash
set -e

echo "Updating apt..."
sudo apt update

echo "Installing build tools and utilities..."
sudo apt install -y build-essential binutils make gcc g++ nasm grub-pc-bin xorriso qemu-system-x86 python3 python3-pip

cat <<'EOF'
Notes:
- This installs native build tools. For the cross-compiler (x86_64-elf-gcc) you may build a cross-toolchain or install appropriate packages.
  Example suggestions:
    sudo apt install gcc-multilib binutils-multiarch
    or build a cross-compiler following osdev wiki.

- To build the project from WSL (Windows D: drive is mounted at /mnt/d):
    cd /mnt/d/自制操作系统
    make
    make run    # runs QEMU inside WSL
EOF

echo "WSL setup complete." 
