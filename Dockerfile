# ─────────────────────────────────────────────────────────────────────
#  AuroraOS Reproducible Build Environment
#  ─────────────────────────────────────────────────────────────────────
#  Usage:
#    docker build -t aurora-os .
#    docker run --rm -v $(pwd):/output aurora-os
#  ─────────────────────────────────────────────────────────────────────

FROM ubuntu:24.04

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-x86-64-linux-gnu \
    binutils-x86-64-linux-gnu \
    xorriso \
    grub-pc-bin \
    mtools \
    qemu-system-x86 \
    make \
    git \
    python3 \
    clang-format \
    clang-tidy \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Set up cross-compiler symlinks
RUN ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc && \
    ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld && \
    ln -sf /usr/bin/x86_64-linux-gnu-objcopy /usr/local/bin/x86_64-elf-objcopy

# Set working directory
WORKDIR /aurora-os

# Copy project source
COPY . .

# Default command: build ISO
CMD ["make", "iso"]