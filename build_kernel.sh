#!/bin/bash

# Set architecture and branch
ARCH=arm64
BRANCH=master

# Set compiler configurations
CLANG_TRIPLE=aarch64-linux-gnu-
CROSS_COMPILE=toolchain/aarch64-linux-android-4.9/bin/aarch64-linux-android-
REAL_CC=toolchain/llvm-arm-toolchain-ship/8.0/bin/clang

# Define the default configuration file
DEFCONFIG=d2q_hybris_defconfig

# Extra commands (if any)
EXTRA_CMDS=''

# Post-configuration commands
POST_DEFCONFIG_CMDS="check_defconfig"

# Device Tree Compiler (DTC) path
DTC_EXT=tools/dtc

# Enable build for ARM64 device tree overlay
CONFIG_BUILD_ARM64_DT_OVERLAY=y

# Files to build
FILES="arch/arm64/boot/Image-dtb"

# Start the build process
echo "Starting build process for $ARCH on branch $BRANCH..."

# Export necessary variables
export ARCH
export CLANG_TRIPLE
export CROSS_COMPILE
export REAL_CC
export CONFIG_BUILD_ARM64_DT_OVERLAY

# Run the defconfig
echo "Running defconfig: $DEFCONFIG"
make $DEFCONFIG

# Run post-configuration commands
if [ -n "$POST_DEFCONFIG_CMDS" ]; then
    echo "Running post-defconfig commands: $POST_DEFCONFIG_CMDS"
    eval $POST_DEFCONFIG_CMDS
fi

# Build the kernel
echo "Building kernel..."
make -j$(nproc)

# Verify if the required files were generated
for file in $FILES; do
    if [ -f $file ]; then
        echo "Generated: $file"
    else
        echo "Error: $file not found!"
        exit 1
    fi
done

echo "Build completed successfully!"
