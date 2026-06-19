#!/bin/bash
set -euo pipefail

# Paste into Ubuntu WSL shell and run. Adjust PROJECT_DIR if your repo is elsewhere.
PROJECT_DIR="/mnt/d/自制操作系统"

if [ ! -d "$PROJECT_DIR" ]; then
  echo -e "Project directory '$PROJECT_DIR' not found.\nPlease ensure your D: drive is mounted in WSL (/mnt/d) and the repo path is correct.\nEdit PROJECT_DIR at top of this script if needed."
  exit 1
fi

echo "Updating apt and installing build dependencies (requires sudo)..."
sudo apt update
sudo apt install -y build-essential binutils make gcc g++ nasm grub-pc-bin xorriso qemu-system-x86

# Create simple wrappers for x86_64-elf-* if cross tools are not installed
for tool in gcc ld objcopy; do
  name="x86_64-elf-$tool"
  if ! command -v "$name" >/dev/null 2>&1; then
    echo "Creating wrapper for $name -> $tool"
    sudo tee /usr/local/bin/$name > /dev/null <<'EOFWRAP'
#!/bin/sh
exec $tool "$@"
EOFWRAP
    sudo chmod +x /usr/local/bin/$name
  fi
done

cd "$PROJECT_DIR"
make -j"$(nproc 2>/dev/null || echo 1)"
if [ $? -ne 0 ]; then
  echo "Build failed."
  exit 2
fi

echo "Build succeeded. Creating ISO and running QEMU (may open window)."
make run
