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
 * Usage:
 *   qemu-system-arm -M qvp[,boot-core=N][,gdb-port=P][,start-paused=on] -accel tcg,thread=multi
 *     -device loader,file=core0.elf,cpu-num=0 ...   (0 aux .. 4 fcore0)
 *
 * gdb-port=P starts one gdb server per core (core n on port P+n). A stop
 * on one core (breakpoint, step, Ctrl-C) freezes only that core; the
 * rest of the SoC keeps running. Don't combine with -s/-gdb.
 * start-paused=on pauses every core before its first instruction.
 * Console: PL011 at 0xD12000 (GIC INTID 32) on -serial / -nographic.
 * Monitor (also via gdb "monitor ..."):
 *   qvp-core start|hold|reset|pause|resume|status [core|all]
 *
 * Internal use only, not intended for upstream.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/cpu/cluster.h"
#include "hw/char/pl011.h"
#include "hw/intc/arm_gic.h"
#include "hw/misc/unimp.h"
#include "exec/gdbstub.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"
#include "qobject/qdict.h"
#include "system/cpus.h"
#include "system/system.h"
#include "target/arm/arm-powerctl.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"

#define QVP_NUM_CORES       5
#define QVP_NUM_IRQ         256

/* Per-core local view */
#define QVP_LOCAL_ITCM      0x00000000
#define QVP_LOCAL_DTCM      0x00040000
#define QVP_LOCAL_QCC       0x00410000
#define QVP_QCC_SIZE        0x1000

#define QVP_GIC_DIST        0x00400000
#define QVP_GIC_CPU         0x00401000

/*
 * UART: a PL011 stands in for elan_uart until its register spec is
 * modelled. It decodes the first 4K of the 8K window.
 */
#define QVP_UART_BASE       0x00D12000
#define QVP_UART_SIZE       0x2000
#define QVP_PL011_SIZE      0x1000

/* GIC SPI numbers (INTID = 32 + SPI); placeholders until the real map */
#define QVP_SPI_UART        0

/*
 * PMU stub. Placeholder core-control registers until the real
 * eldora_pmu register spec is modelled; everything else is LOG_UNIMP.
 *   CORE_RELEASE  W1: release core n from reset   R: cores out of reset
 *   CORE_HOLD     W1: put core n back into reset  R: cores held in reset
 *   CORE_RESET    W1: pulse reset on running core n
 */
#define QVP_PMU_BASE        0x00D03000
#define QVP_PMU_SIZE        0x1000
#define QVP_PMU_CORE_RELEASE 0x000
#define QVP_PMU_CORE_HOLD    0x004
#define QVP_PMU_CORE_RESET   0x008

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
    { "ext_timer",          0x00D04000, 0x00009000, false },
    { "snor",               0x00D0D000, 0x00001000, false },
    { "smbus",              0x00D11000, 0x00001000, false },
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
    MemoryRegion pmu;

    uint32_t boot_core;
    uint32_t gdb_port;
    bool start_paused;
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
    /* MPIDR.Aff0 = core id; also the id used by arm-powerctl */
    object_property_set_int(OBJECT(&s->cpu[i]), "mp-affinity", i,
                            &error_abort);
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

/* Core control, shared by the PMU stub and the monitor */

typedef enum QVPCoreOp {
    QVP_CORE_START,
    QVP_CORE_HOLD,
    QVP_CORE_RESET,
    QVP_CORE_PAUSE,
    QVP_CORE_RESUME,
} QVPCoreOp;

static bool qvp_core_in_reset(QVPMachineState *s, int i)
{
    return s->cpu[i].power_state == PSCI_OFF;
}

/*
 * A reset pulse on a running core: cpu_reset() alone would put a
 * non-boot core back into its "held" power-on state, so come back on.
 */
static void qvp_core_reset_work(CPUState *cs, run_on_cpu_data data)
{
    cpu_reset(cs);
    cs->halted = 0;
    arm_set_cpu_power_state(ARM_CPU(cs), PSCI_ON);
}

static const char *qvp_core_op(QVPMachineState *s, int i, QVPCoreOp op)
{
    CPUState *cs = CPU(&s->cpu[i]);
    int ret = QEMU_ARM_POWERCTL_RET_SUCCESS;

    switch (op) {
    case QVP_CORE_START:
        ret = arm_set_cpu_on_and_reset(i);
        break;
    case QVP_CORE_HOLD:
        ret = arm_set_cpu_off(i);
        break;
    case QVP_CORE_RESET:
        if (qvp_core_in_reset(s, i)) {
            return "held in reset";
        }
        async_run_on_cpu(cs, qvp_core_reset_work, RUN_ON_CPU_NULL);
        break;
    case QVP_CORE_PAUSE:
        if (qatomic_read(&cs->frozen)) {
            return "already paused";
        }
        cpu_freeze(cs);
        break;
    case QVP_CORE_RESUME:
        if (!qatomic_read(&cs->frozen)) {
            return "not paused";
        }
        cpu_thaw(cs);
        break;
    }

    switch (ret) {
    case QEMU_ARM_POWERCTL_RET_SUCCESS:
        return NULL;
    case QEMU_ARM_POWERCTL_ALREADY_ON:
        return "already out of reset";
    case QEMU_ARM_POWERCTL_ON_PENDING:
        return "release already pending";
    case QEMU_ARM_POWERCTL_IS_OFF:
        return "held in reset";
    default:
        return "failed";
    }
}

static uint64_t qvp_pmu_read(void *opaque, hwaddr addr, unsigned size)
{
    QVPMachineState *s = opaque;
    uint32_t out_of_reset = 0;

    for (int i = 0; i < QVP_NUM_CORES; i++) {
        if (!qvp_core_in_reset(s, i)) {
            out_of_reset |= BIT(i);
        }
    }

    switch (addr) {
    case QVP_PMU_CORE_RELEASE:
        return out_of_reset;
    case QVP_PMU_CORE_HOLD:
        return ~out_of_reset & MAKE_64BIT_MASK(0, QVP_NUM_CORES);
    case QVP_PMU_CORE_RESET:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "qvp-pmu: unimplemented read @0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void qvp_pmu_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    QVPMachineState *s = opaque;
    QVPCoreOp op;

    switch (addr) {
    case QVP_PMU_CORE_RELEASE:
        op = QVP_CORE_START;
        break;
    case QVP_PMU_CORE_HOLD:
        op = QVP_CORE_HOLD;
        break;
    case QVP_PMU_CORE_RESET:
        op = QVP_CORE_RESET;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "qvp-pmu: unimplemented write @0x%"
                      HWADDR_PRIx " = 0x%" PRIx64 "\n", addr, val);
        return;
    }

    for (int i = 0; i < QVP_NUM_CORES; i++) {
        if (val & BIT(i)) {
            const char *err = qvp_core_op(s, i, op);

            if (err) {
                qemu_log_mask(LOG_GUEST_ERROR, "qvp-pmu: core %d (%s): %s\n",
                              i, qvp_cores[i].name, err);
            }
        }
    }
}

static const MemoryRegionOps qvp_pmu_ops = {
    .read = qvp_pmu_read,
    .write = qvp_pmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int qvp_parse_core(const char *arg)
{
    for (int i = 0; i < QVP_NUM_CORES; i++) {
        if (!strcmp(arg, qvp_cores[i].name)) {
            return i;
        }
    }
    if (arg[0] >= '0' && arg[0] < '0' + QVP_NUM_CORES && !arg[1]) {
        return arg[0] - '0';
    }
    return -1;
}

static void qvp_core_status(MonitorHMP *hmp, QVPMachineState *s, int i)
{
    ARMCPU *cpu = &s->cpu[i];
    const char *state;

    switch (cpu->power_state) {
    case PSCI_ON:
        state = "running";
        break;
    case PSCI_OFF:
        state = "held";
        break;
    default:
        state = "releasing";
        break;
    }
    monitor_hmp_printf(hmp, "  core %d %-7s %-9s %-6s pc=0x%08x\n", i,
                       qvp_cores[i].name, state,
                       qatomic_read(&CPU(cpu)->frozen) ? "paused" : "",
                       cpu->env.regs[15]);
}

static void hmp_qvp_core(MonitorHMP *hmp, const QDict *qdict)
{
    static const char * const ops[] = {
        [QVP_CORE_START] = "start",
        [QVP_CORE_HOLD] = "hold",
        [QVP_CORE_RESET] = "reset",
        [QVP_CORE_PAUSE] = "pause",
        [QVP_CORE_RESUME] = "resume",
    };
    QVPMachineState *s = QVP_MACHINE(qdev_get_machine());
    const char *action = qdict_get_str(qdict, "action");
    const char *core = qdict_get_try_str(qdict, "core");
    int first = 0, last = QVP_NUM_CORES - 1;
    int op = -1;

    if (core && strcmp(core, "all")) {
        first = last = qvp_parse_core(core);
        if (first < 0) {
            monitor_hmp_printf(hmp, "unknown core '%s'\n", core);
            return;
        }
    } else if (!core && strcmp(action, "status")) {
        monitor_hmp_printf(hmp, "qvp-core %s needs a core (or 'all')\n",
                           action);
        return;
    }

    if (!strcmp(action, "status")) {
        for (int i = first; i <= last; i++) {
            qvp_core_status(hmp, s, i);
        }
        return;
    }

    for (int i = 0; i < ARRAY_SIZE(ops); i++) {
        if (!strcmp(action, ops[i])) {
            op = i;
        }
    }
    if (op < 0) {
        monitor_hmp_printf(hmp, "unknown action '%s'\n", action);
        return;
    }

    for (int i = first; i <= last; i++) {
        const char *err = qvp_core_op(s, i, op);

        if (err) {
            monitor_hmp_printf(hmp, "core %d (%s): %s\n", i,
                               qvp_cores[i].name, err);
        }
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
        /* vCPUs have not run yet, so this needs no cpu_freeze() handshake */
        CPU(&s->cpu[i])->frozen = s->start_paused;
    }

    qvp_create_gic(s, sysmem);

    pl011_create(QVP_UART_BASE,
                 qdev_get_gpio_in(DEVICE(&s->gic), QVP_SPI_UART),
                 serial_hd(0));
    create_unimplemented_device("uart-hi", QVP_UART_BASE + QVP_PL011_SIZE,
                                QVP_UART_SIZE - QVP_PL011_SIZE);

    memory_region_init_io(&s->pmu, OBJECT(s), &qvp_pmu_ops, s, "pmu",
                          QVP_PMU_SIZE);
    memory_region_add_subregion(sysmem, QVP_PMU_BASE, &s->pmu);

    monitor_register_hmp("qvp-core", false, hmp_qvp_core);

    /* One gdb server per core: core i listens on gdb-port + i */
    for (int i = 0; s->gdb_port && i < QVP_NUM_CORES; i++) {
        g_autofree char *dev = g_strdup_printf("tcp::%u", s->gdb_port + i);

        if (!gdbserver_start_core(CPU(&s->cpu[i]), dev, &error_fatal)) {
            exit(1);
        }
        info_report("qvp: core %d (%s) gdb server on port %u", i,
                    qvp_cores[i].name, s->gdb_port + i);
    }

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

static bool qvp_get_start_paused(Object *obj, Error **errp)
{
    return QVP_MACHINE(obj)->start_paused;
}

static void qvp_set_start_paused(Object *obj, bool value, Error **errp)
{
    QVP_MACHINE(obj)->start_paused = value;
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

    s->gdb_port = 0;
    object_property_add_uint32_ptr(obj, "gdb-port", &s->gdb_port,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "gdb-port",
                                    "Start a gdb server per core, core n "
                                    "on TCP port gdb-port + n (0 = off). "
                                    "Do not combine with -s/-gdb");

    object_property_add_bool(obj, "start-paused", qvp_get_start_paused,
                             qvp_set_start_paused);
    object_property_set_description(obj, "start-paused",
                                    "Every core starts paused at its reset "
                                    "vector; resume it from its gdb or with "
                                    "qvp-core resume");
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
