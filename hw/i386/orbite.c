/*
 * Orbite - PS4 Safe Mode Environment Emulator based on QEMU
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/i386/x86.h"
#include "target/i386/cpu.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"

#define TYPE_ORBITE_MACHINE MACHINE_TYPE_NAME("orbite")
OBJECT_DECLARE_SIMPLE_TYPE(OrbiteMachineState, ORBITE_MACHINE)

struct OrbiteMachineState {
    X86MachineState parent_obj;
    MemoryRegion ram_memory;
    MemoryRegion reset_rom_high;
    MemoryRegion reset_rom_low;
    MemoryRegion com1_io;
    CharFrontend com1_chr;
};

/* COM1 (0x3F8 ~ 0x3FF) I/O 포트 핸들러 */
static uint64_t orbite_com1_read(void *opaque, hwaddr addr, unsigned size)
{
    /* 0x3FD: Line Status Register (LSR) - Transmitter Empty 비트(0x20 | 0x40) 반환 */
    if (addr == 5) {
        return 0x60;
    }
    return 0x00;
}

static void orbite_com1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    OrbiteMachineState *oms = opaque;
    uint8_t ch = (uint8_t)val;

    /* 0x3F8: Data Register - CPU가 문자를 출력하면 바로 콘솔로 전달 */
    if (addr == 0) {
        if (qemu_chr_fe_backend_connected(&oms->com1_chr)) {
            qemu_chr_fe_write_all(&oms->com1_chr, &ch, 1);
        } else {
            putchar(ch);
            fflush(stdout);
        }
    }
}

static const MemoryRegionOps orbite_com1_ops = {
    .read = orbite_com1_read,
    .write = orbite_com1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void orbite_init(MachineState *machine)
{
    OrbiteMachineState *oms = ORBITE_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *system_io = get_system_io();

    printf("\n===============================================\n");
    printf("  [Orbite] Initializing PS4 Hardware Environment...\n");
    printf("  [Orbite] Target RAM Size: %lu MB\n", (unsigned long)(machine->ram_size / MiB));
    printf("  [Orbite] Target CPU Cores: %d\n", machine->smp.cpus);
    printf("===============================================\n\n");

    /* 1. 시스템 RAM 매핑 (0x0 ~ machine->ram_size) */
    memory_region_init_ram(&oms->ram_memory, NULL, "orbite.ram", machine->ram_size, &error_fatal);
    memory_region_add_subregion(system_memory, 0, &oms->ram_memory);

    /* 2. x86 리셋 벡터 트램펄린 (jmp far 0x0000:0x7c00) */
    static const uint8_t reset_trampoline[16] = {
        0xea, 0x00, 0x7c, 0x00, 0x00, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };

    memory_region_init_ram(&oms->reset_rom_high, NULL, "orbite.reset_high", 16, &error_fatal);
    memcpy(memory_region_get_ram_ptr(&oms->reset_rom_high), reset_trampoline, 16);
    memory_region_add_subregion_overlap(system_memory, 0xfffffff0ULL, &oms->reset_rom_high, 2000);

    memory_region_init_ram(&oms->reset_rom_low, NULL, "orbite.reset_low", 16, &error_fatal);
    memcpy(memory_region_get_ram_ptr(&oms->reset_rom_low), reset_trampoline, 16);
    memory_region_add_subregion_overlap(system_memory, 0xffff0ULL, &oms->reset_rom_low, 2000);

    /* 3. 바이너리 로드 */
    if (machine->kernel_filename) {
        gchar *buf = NULL;
        gsize len = 0;
        GError *err = NULL;

        if (g_file_get_contents(machine->kernel_filename, &buf, &len, &err)) {
            uint8_t *ram_ptr = memory_region_get_ram_ptr(&oms->ram_memory);
            memcpy(ram_ptr + 0x7c00, buf, len);
            printf("  [Orbite] Successfully wrote %" G_GSIZE_FORMAT " bytes into RAM at 0x7C00\n", len);
            g_free(buf);
        } else {
            fprintf(stderr, "[Orbite] Failed to read kernel file: %s\n", err->message);
            g_error_free(err);
            exit(1);
        }
    }

    /* 4. vCPU 코어 생성 */
    for (int i = 0; i < machine->smp.cpus; i++) {
        Object *cpu_obj = object_new(machine->cpu_type);
        object_property_set_int(cpu_obj, "socket-id", 0, &error_fatal);
        object_property_set_int(cpu_obj, "core-id", i, &error_fatal);
        object_property_set_int(cpu_obj, "thread-id", 0, &error_fatal);
        object_property_set_int(cpu_obj, "apic-id", i, &error_fatal);
        qdev_realize(DEVICE(cpu_obj), NULL, &error_fatal);
        object_unref(cpu_obj);
    }
    printf("  [Orbite] %d x86_64 CPU core(s) initialized successfully.\n", machine->smp.cpus);

    /* 5. COM1 (포트 0x3F8 ~ 0x3FF) 직접 I/O 포트에 매핑 */
    if (serial_hd(0)) {
        qemu_chr_fe_init(&oms->com1_chr, serial_hd(0), &error_abort);
    }
    memory_region_init_io(&oms->com1_io, OBJECT(oms), &orbite_com1_ops, oms, "orbite.com1", 8);
    memory_region_add_subregion(system_io, 0x3f8, &oms->com1_io);
    printf("  [Orbite] Direct COM1 Port mapped at I/O 0x3F8.\n");

    printf("  [Orbite] Base hardware setup complete! Machine running.\n\n");
}

static void orbite_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "PS4 Safe Mode Emulation Environment (Orbite)";
    mc->init = orbite_init;
    mc->max_cpus = 8;
    mc->default_cpus = 8;
    mc->default_ram_size = 4 * GiB;
    mc->default_cpu_type = X86_CPU_TYPE_NAME("qemu64");
}

static const TypeInfo orbite_machine_types[] = {
    {
        .name = TYPE_ORBITE_MACHINE,
        .parent = TYPE_X86_MACHINE,
        .instance_size = sizeof(OrbiteMachineState),
        .class_init = orbite_machine_class_init,
    },
};

DEFINE_TYPES(orbite_machine_types)
