#!/bin/bash
# run_frap.sh
# 自动生成 FRAP table、编译 NuttX 并启动 QEMU

set -e  # 遇到错误立即退出

# Step 0: 进入内核目录
echo "==> into nuttx..."
cd nuttx

echo "==> make clean..."
make clean

# Step 1: 生成 FRAP table
echo "==> Generating FRAP table..."
cd tools/frap/
python3 frap_table_generator.py frap_demo_config.json frapdemo/frap_table_generated.h
cd ../../  # 回到 nuttx 根目录

# Step 2: 编译 NuttX
echo "==> Building NuttX..."
make -j$(nproc)

# Step 3: 运行 QEMU
echo "==> Launching NuttX in QEMU..."
sudo qemu-system-aarch64 \
    -cpu cortex-a53 \
    -smp 2 \
    -m 1G \
    -machine virt,virtualization=on,gic-version=3 \
    -serial mon:stdio \
    -nographic \
    -kernel nuttx
