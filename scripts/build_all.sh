#!/bin/bash
set -euo pipefail

# Build userspace, embed into kernel, and build kernel ISO.
# Run this inside WSL/Ubuntu or MSYS2 (with python3 available).
ROOT="/mnt/d/自制操作系统"
cd "$ROOT"

# Build userspace
echo "Building userspace..."
cd userspace
make -f Makefile.user all
cd ..

# Embed hello and shell into kernel
echo "Embedding userspace/hello and userspace/shell into kernel..."
python3 scripts/embed_binary.py userspace/hello kernel/embedded_files.c
python3 scripts/embed_binary.py userspace/shell kernel/embedded_files.c

# Build kernel
echo "Building kernel..."
make -j1

# Done
echo "Build complete. Generated os.iso"
