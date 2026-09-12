set -e
X="/c/Users/malih/Downloads/vivido/SDK/2019.1"
export PATH="$X/gnu/aarch32/nt/gcc-arm-none-eabi/bin:$X/gnuwin/bin:$PATH"
W=/f/vivado_projects/pl_irq/pl_irq.sdk
BSP=$W/fsbl_bsp/ps7_cortexa9_0
HW=$W/design_1_wrapper_hw_platform_0
OUT=$W/fsbl/Release
mkdir -p "$OUT"
cd "$W/fsbl"

CFLAGS="-Wall -O2 -g -c -fmessage-length=0 -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard -DFSBL_DEBUG_INFO -I src -I $BSP/include -I $HW"

OBJS=""
for s in src/*.c "$HW/ps7_init.c"; do
    o="$OUT/$(basename ${s%.c}).o"
    arm-none-eabi-gcc $CFLAGS -o "$o" "$s"
    OBJS="$OBJS $o"
done
for s in src/*.S; do
    o="$OUT/$(basename ${s%.S}).o"
    arm-none-eabi-gcc $CFLAGS -o "$o" "$s"
    OBJS="$OBJS $o"
done

arm-none-eabi-gcc -Wl,-build-id=none -specs="src/Xilinx.spec" \
    -mcpu=cortex-a9 -mfpu=vfpv3 -mfloat-abi=hard \
    -Wl,-T -Wl,src/lscript.ld -L "$BSP/lib" \
    -o "$OUT/fsbl.elf" $OBJS \
    -Wl,--start-group,-lxil,-lgcc,-lc,--end-group \
    -Wl,--start-group,-lxilffs,-lxil,-lgcc,-lc,--end-group \
    -Wl,--start-group,-lrsa,-lxil,-lgcc,-lc,--end-group

arm-none-eabi-size "$OUT/fsbl.elf"
arm-none-eabi-readelf -h "$OUT/fsbl.elf" | grep -i entry
