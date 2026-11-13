/*
 * qvp: internal multi-core (AMP) Cortex-R SoC model
 *
 * Every core sees its own ITCM at 0x0, DTCM at 0x40000 and QCC at
 * 0x410000. The TCMs are also visible to everyone on the AXI bus.
 * Everything else is a single global map shared by all cores.
 *
 * Each core lives in its own CPU cluster so that it is a separate
 * inferior for gdb and has its own TB cache.
 *
 * Internal use only, not intended for upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/cpu/cluster.h"
#include "hw/intc/arm_gic.h"
#include "hw/misc/unimp.h"
#include "target/arm/cpu.h"

#define QVP_NUM_CORES       5
#define QVP_NUM_IRQ         256

/* Per-core local view */
#define QVP_LOCAL_ITCM      0x00000000
#define QVP_LOCAL_DTCM      0x00040000
#define QVP_LOCAL_QCC       0x00410000
#define QVP_QCC_SIZE        0x1000

#define QVP_GIC_DIST        0x00400000
#define QVP_GIC_CPU         0x00401000

typedef struct QVPCoreInfo {
    const char *name;
    hwaddr itcm_axi;
    uint64_t itcm_size;
    hwaddr dtcm_axi;
    uint64_t dtcm_size;
} QVPCoreInfo;

static const QVPCoreInfo qvp_cores[QVP_NUM_CORES] = {
    { "aux",     0x00080000, 0x40000, 0x000C0000, 0x40000 },
    { "mgmt",    0x00100000, 0x40000, 0x00140000, 0x40000 },
    { "pf0",     0x00180000, 0x40000, 0x001C0000, 0x40000 },
    { "pf1",     0x00200000, 0x40000, 0x00240000, 0x40000 },
    { "fcore0",  0x00280000, 0x20000, 0x002C0000, 0x40000 },
};

typedef struct QVPRegion {
    const char *name;
    hwaddr base;
    uint64_t size;
    bool is_ram;
} QVPRegion;

/* Global (core_id 128) map, excluding GIC and per-core QCC */
static const QVPRegion qvp_global[] = {
    { "noc",                0x00600000, 0x00080000, true },
    { "e2e",                0x00A10000, 0x00030000, true },
    { "core_sram0",         0x00C00000, 0x00030000, true },
    { "hcore_shared_sram",  0x00C30000, 0x00088000, true },
    { "secure_buffer_sram", 0x00CB8000, 0x0000B000, true },
    { "ckg",                0x00D00000, 0x00001000, false },
    { "gpio",               0x00D01000, 0x00001000, false },
    { "pmu",                0x00D03000, 0x00001000, false },
    { "ext_timer",          0x00D04000, 0x00009000, false },
    { "snor",               0x00D0D000, 0x00001000, false },
    { "smbus",              0x00D11000, 0x00001000, false },
    { "uart",               0x00D12000, 0x00002000, false },
    { "intd",               0x00D80000, 0x00002000, false },
    { "gaudicdma",          0x00DA8000, 0x00006000, true },
    { "gdma",               0x00EB0000, 0x00010000, false },
};

#define TYPE_QVP_MACHINE MACHINE_TYPE_NAME("qvp")
OBJECT_DECLARE_SIMPLE_TYPE(QVPMachineState, QVP_MACHINE)

struct QVPMachineState {
    MachineState parent;

    CPUClusterState cluster[QVP_NUM_CORES];
    ARMCPU cpu[QVP_NUM_CORES];
    MemoryRegion cpu_mem[QVP_NUM_CORES];
    MemoryRegion sysmem_alias[QVP_NUM_CORES];
    MemoryRegion itcm[QVP_NUM_CORES];
    MemoryRegion dtcm[QVP_NUM_CORES];
    MemoryRegion itcm_local[QVP_NUM_CORES];
    MemoryRegion dtcm_local[QVP_NUM_CORES];
    MemoryRegion qcc[QVP_NUM_CORES];
    MemoryRegion global_ram[ARRAY_SIZE(qvp_global)];
    GICState gic;

    uint32_t boot_core;
};

static void qvp_create_core(QVPMachineState *s, int i, MemoryRegion *sysmem)
{
    MachineState *ms = MACHINE(s);
    const QVPCoreInfo *ci = &qvp_cores[i];
    Object *owner = OBJECT(s);
    g_autofree char *cl_name = g_strdup_printf("cluster-%s", ci->name);
    g_autofree char *mem_name = g_strdup_printf("%s-memory", ci->name);
    g_autofree char *alias_name = g_strdup_printf("%s-sysmem", ci->name);
    g_autofree char *itcm_name = g_strdup_printf("%s-itcm", ci->name);
    g_autofree char *dtcm_name = g_strdup_printf("%s-dtcm", ci->name);
    g_autofree char *itcm_l_name = g_strdup_printf("%s-itcm-local", ci->name);
    g_autofree char *dtcm_l_name = g_strdup_printf("%s-dtcm-local", ci->name);
    g_autofree char *qcc_name = g_strdup_printf("%s-qcc", ci->name);

    /* TCMs live on the AXI bus ... */
    memory_region_init_ram(&s->itcm[i], NULL, itcm_name, ci->itcm_size,
                           &error_fatal);
    memory_region_init_ram(&s->dtcm[i], NULL, dtcm_name, ci->dtcm_size,
                           &error_fatal);
    memory_region_add_subregion(sysmem, ci->itcm_axi, &s->itcm[i]);
    memory_region_add_subregion(sysmem, ci->dtcm_axi, &s->dtcm[i]);

    /* ... and the core's own view overlays them (and QCC) on the bus */
    memory_region_init(&s->cpu_mem[i], owner, mem_name, UINT64_MAX);
    memory_region_init_alias(&s->sysmem_alias[i], owner, alias_name,
                             sysmem, 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&s->cpu_mem[i], 0,
                                        &s->sysmem_alias[i], -1);

    memory_region_init_alias(&s->itcm_local[i], owner, itcm_l_name,
                             &s->itcm[i], 0, ci->itcm_size);
    memory_region_init_alias(&s->dtcm_local[i], owner, dtcm_l_name,
                             &s->dtcm[i], 0, ci->dtcm_size);
    memory_region_add_subregion(&s->cpu_mem[i], QVP_LOCAL_ITCM,
                                &s->itcm_local[i]);
    memory_region_add_subregion(&s->cpu_mem[i], QVP_LOCAL_DTCM,
                                &s->dtcm_local[i]);

    memory_region_init_ram(&s->qcc[i], NULL, qcc_name, QVP_QCC_SIZE,
                           &error_fatal);
    memory_region_add_subregion(&s->cpu_mem[i], QVP_LOCAL_QCC, &s->qcc[i]);

    object_initialize_child(owner, cl_name, &s->cluster[i], TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->cluster[i]), "cluster-id", i);

    object_initialize_child(OBJECT(&s->cluster[i]), "cpu", &s->cpu[i],
                            ms->cpu_type);
    object_property_set_link(OBJECT(&s->cpu[i]), "memory",
                             OBJECT(&s->cpu_mem[i]), &error_abort);
    /* Non-boot cores are held until software releases them */
    object_property_set_bool(OBJECT(&s->cpu[i]), "start-powered-off",
                             i != s->boot_core, &error_abort);
    qdev_realize(DEVICE(&s->cpu[i]), NULL, &error_fatal);
    qdev_realize(DEVICE(&s->cluster[i]), NULL, &error_fatal);
}

static void qvp_create_gic(QVPMachineState *s, MemoryRegion *sysmem)
{
    SysBusDevice *sbd;

    object_initialize_child(OBJECT(s), "gic", &s->gic, TYPE_ARM_GIC);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-cpu", QVP_NUM_CORES);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-irq", QVP_NUM_IRQ);
    qdev_prop_set_uint32(DEVICE(&s->gic), "revision", 2);
    sbd = SYS_BUS_DEVICE(&s->gic);
    sysbus_realize(sbd, &error_fatal);

    memory_region_add_subregion(sysmem, QVP_GIC_DIST,
                                sysbus_mmio_get_region(sbd, 0));
    /* Region 1 is the banked "current CPU" interface */
    memory_region_add_subregion(sysmem, QVP_GIC_CPU,
                                sysbus_mmio_get_region(sbd, 1));

    for (int i = 0; i < QVP_NUM_CORES; i++) {
        DeviceState *cpudev = DEVICE(&s->cpu[i]);

        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(sbd, i + QVP_NUM_CORES,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }
}

static void qvp_init(MachineState *ms)
{
    QVPMachineState *s = QVP_MACHINE(ms);
    MemoryRegion *sysmem = get_system_memory();

    if (s->boot_core >= QVP_NUM_CORES) {
        error_report("qvp: boot-core must be 0..%d", QVP_NUM_CORES - 1);
        exit(1);
    }

    for (int i = 0; i < QVP_NUM_CORES; i++) {
        qvp_create_core(s, i, sysmem);
    }

    qvp_create_gic(s, sysmem);

    for (int i = 0; i < ARRAY_SIZE(qvp_global); i++) {
        const QVPRegion *r = &qvp_global[i];

        if (r->is_ram) {
            memory_region_init_ram(&s->global_ram[i], NULL, r->name,
                                   r->size, &error_fatal);
            memory_region_add_subregion(sysmem, r->base, &s->global_ram[i]);
        } else {
            create_unimplemented_device(r->name, r->base, r->size);
        }
    }
}

static void qvp_instance_init(Object *obj)
{
    QVPMachineState *s = QVP_MACHINE(obj);

    s->boot_core = 1; /* mgmt */
    object_property_add_uint32_ptr(obj, "boot-core", &s->boot_core,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "boot-core",
                                    "Core running out of reset (0..4, "
                                    "default 1 = mgmt); others are held");
}

static void qvp_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-r5"),
        ARM_CPU_TYPE_NAME("cortex-r5f"),
        ARM_CPU_TYPE_NAME("cortex-r52"),
        NULL
    };

    mc->desc = "qvp multi-core Cortex-R SoC (internal)";
    mc->init = qvp_init;
    mc->min_cpus = QVP_NUM_CORES;
    mc->max_cpus = QVP_NUM_CORES;
    mc->default_cpus = QVP_NUM_CORES;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-r5f");
    mc->valid_cpu_types = valid_cpu_types;
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
}

static const TypeInfo qvp_machine_types[] = {
    {
        .name = TYPE_QVP_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(QVPMachineState),
        .instance_init = qvp_instance_init,
        .class_init = qvp_class_init,
    },
};

DEFINE_TYPES(qvp_machine_types)
