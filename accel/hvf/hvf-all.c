/*
 * QEMU Hypervisor.framework support
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/error-report.h"
#include "sysemu/hvf.h"
#include "sysemu/hvf_int.h"

void assert_hvf_ok(hv_return_t ret)
{
    if (ret == HV_SUCCESS) {
        return;
    }

    switch (ret) {
    case HV_ERROR:
        error_report("Error: HV_ERROR");
        break;
    case HV_BUSY:
        error_report("Error: HV_BUSY");
        break;
    case HV_BAD_ARGUMENT:
        error_report("Error: HV_BAD_ARGUMENT");
        break;
    case HV_NO_RESOURCES:
        error_report("Error: HV_NO_RESOURCES");
        break;
    case HV_NO_DEVICE:
        error_report("Error: HV_NO_DEVICE");
        break;
    case HV_UNSUPPORTED:
        error_report("Error: HV_UNSUPPORTED");
        break;
    default:
        error_report("Unknown Error");
    }

    abort();
}

static const uint32_t brk_insn = 0xd4200000;
uint32_t saved_insn;

int hvf_insert_breakpoint(CPUState *cpu, target_ulong addr,
                          target_ulong len, int type)
{
    //struct hvf_sw_breakpoint *bp;
    //int err;

    printf("%s type=%d addr=%#llx len=%llu\n", __func__, type, addr, len);

    cpu->hvf->enable_debug = true;

    if (cpu_memory_rw_debug(cpu, addr, (uint8_t *)&saved_insn, 4, 0) ||
        cpu_memory_rw_debug(cpu, addr, (uint8_t *)&brk_insn, 4, 1)) {
        return -EINVAL;
    }
/*
    if (type == GDB_BREAKPOINT_SW) {
        bp = kvm_find_sw_breakpoint(cpu, addr);
        if (bp) {
            bp->use_count++;
            return 0;
        }
        bp = g_malloc(sizeof(struct kvm_sw_breakpoint));
        bp->pc = addr;
        bp->use_count = 1;
        err = kvm_arch_insert_sw_breakpoint(cpu, bp);
        if (err) {
            g_free(bp);
            return err;
        }
        QTAILQ_INSERT_HEAD(&cpu->kvm_state->kvm_sw_breakpoints, bp, entry);
    } else {
        err = kvm_arch_insert_hw_breakpoint(addr, len, type);
        if (err) {
            return err;
        }
    }
    CPU_FOREACH(cpu) {
        err = kvm_update_guest_debug(cpu, 0);
        if (err) {
            return err;
        }
    }*/
    return 0;
}


int hvf_remove_breakpoint(CPUState *cpu, target_ulong addr,
                          target_ulong len, int type)
{

    printf("%s type=%d addr=%#llx len=%llu\n", __func__, type, addr, len);

    if (cpu_memory_rw_debug(cpu, addr, (uint8_t *)&saved_insn, 4, 1)) {
        return -EINVAL;
    }
    return 0;
}

int hvf_update_guest_debug(CPUState *cpu)
{
    hvf_arch_update_guest_debug(cpu);

    //return data.err;
    return 0;
}
