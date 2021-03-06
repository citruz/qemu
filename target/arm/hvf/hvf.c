/*
 * QEMU Hypervisor.framework support for Apple Silicon

 * Copyright 2020 Alexander Graf <agraf@csgraf.de>
 * Copyright 2020 Google LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/error-report.h"

#include "sysemu/runstate.h"
#include "sysemu/hvf.h"
#include "sysemu/hvf_int.h"
#include "sysemu/hw_accel.h"

#include <mach/mach_time.h>

#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/intc/gicv3_internal.h"
#include "qemu/main-loop.h"
#include "sysemu/accel.h"
#include "sysemu/cpus.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"

#define HVF_DEBUG 1
#define DPRINTF(...)                                        \
    if (HVF_DEBUG) {                                        \
        fprintf(stderr, "HVF %s:%d ", __func__, __LINE__);  \
        fprintf(stderr, __VA_ARGS__);                       \
        fprintf(stderr, "\n");                              \
    }

#define HVF_SYSREG(crn, crm, op0, op1, op2) \
        ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, crn, crm, op0, op1, op2)
#define PL1_WRITE_MASK 0x4

#define SYSREG(op0, op1, op2, crn, crm) \
    ((op0 << 20) | (op2 << 17) | (op1 << 14) | (crn << 10) | (crm << 1))
#define SYSREG_MASK           SYSREG(0x3, 0x7, 0x7, 0xf, 0xf)
#define SYSREG_CNTPCT_EL0     SYSREG(3, 3, 1, 14, 0)
#define SYSREG_CNTVCT_EL0     SYSREG(3, 3, 2, 14, 0)
#define SYSREG_PMCCNTR_EL0    SYSREG(3, 3, 0, 9, 13)

#define SYSREG_ICC_AP0R0_EL1     SYSREG(3, 0, 4, 12, 8)
#define SYSREG_ICC_AP0R1_EL1     SYSREG(3, 0, 5, 12, 8)
#define SYSREG_ICC_AP0R2_EL1     SYSREG(3, 0, 6, 12, 8)
#define SYSREG_ICC_AP0R3_EL1     SYSREG(3, 0, 7, 12, 8)
#define SYSREG_ICC_AP1R0_EL1     SYSREG(3, 0, 0, 12, 9)
#define SYSREG_ICC_AP1R1_EL1     SYSREG(3, 0, 1, 12, 9)
#define SYSREG_ICC_AP1R2_EL1     SYSREG(3, 0, 2, 12, 9)
#define SYSREG_ICC_AP1R3_EL1     SYSREG(3, 0, 3, 12, 9)
#define SYSREG_ICC_ASGI1R_EL1    SYSREG(3, 0, 6, 12, 11)
#define SYSREG_ICC_BPR0_EL1      SYSREG(3, 0, 3, 12, 8)
#define SYSREG_ICC_BPR1_EL1      SYSREG(3, 0, 3, 12, 12)
#define SYSREG_ICC_CTLR_EL1      SYSREG(3, 0, 4, 12, 12)
#define SYSREG_ICC_DIR_EL1       SYSREG(3, 0, 1, 12, 11)
#define SYSREG_ICC_EOIR0_EL1     SYSREG(3, 0, 1, 12, 8)
#define SYSREG_ICC_EOIR1_EL1     SYSREG(3, 0, 1, 12, 12)
#define SYSREG_ICC_HPPIR0_EL1    SYSREG(3, 0, 2, 12, 8)
#define SYSREG_ICC_HPPIR1_EL1    SYSREG(3, 0, 2, 12, 12)
#define SYSREG_ICC_IAR0_EL1      SYSREG(3, 0, 0, 12, 8)
#define SYSREG_ICC_IAR1_EL1      SYSREG(3, 0, 0, 12, 12)
#define SYSREG_ICC_IGRPEN0_EL1   SYSREG(3, 0, 6, 12, 12)
#define SYSREG_ICC_IGRPEN1_EL1   SYSREG(3, 0, 7, 12, 12)
#define SYSREG_ICC_PMR_EL1       SYSREG(3, 0, 0, 4, 6)
#define SYSREG_ICC_RPR_EL1       SYSREG(3, 0, 3, 12, 11)
#define SYSREG_ICC_SGI0R_EL1     SYSREG(3, 0, 7, 12, 11)
#define SYSREG_ICC_SGI1R_EL1     SYSREG(3, 0, 5, 12, 11)
#define SYSREG_ICC_SRE_EL1       SYSREG(3, 0, 5, 12, 12)

#define SYSREG_APPLE_UNKN_TIMER SYSREG(3, 4, 6, 15, 10)

// PMAP handling
#define SYSREG_MAGIC_PPL         SYSREG(3, 7, 7, 15, 15)

#define PMAP_ENTER_OPTIONS 10
#define PMAP_SIGN_USER_PTR 60
#define PMAP_AUTH_USER_PTR 61

#define WFX_IS_WFE (1 << 0)

struct hvf_reg_match {
    int reg;
    uint64_t offset;
};

static const struct hvf_reg_match hvf_reg_match[] = {
    { HV_REG_X0,   offsetof(CPUARMState, xregs[0]) },
    { HV_REG_X1,   offsetof(CPUARMState, xregs[1]) },
    { HV_REG_X2,   offsetof(CPUARMState, xregs[2]) },
    { HV_REG_X3,   offsetof(CPUARMState, xregs[3]) },
    { HV_REG_X4,   offsetof(CPUARMState, xregs[4]) },
    { HV_REG_X5,   offsetof(CPUARMState, xregs[5]) },
    { HV_REG_X6,   offsetof(CPUARMState, xregs[6]) },
    { HV_REG_X7,   offsetof(CPUARMState, xregs[7]) },
    { HV_REG_X8,   offsetof(CPUARMState, xregs[8]) },
    { HV_REG_X9,   offsetof(CPUARMState, xregs[9]) },
    { HV_REG_X10,  offsetof(CPUARMState, xregs[10]) },
    { HV_REG_X11,  offsetof(CPUARMState, xregs[11]) },
    { HV_REG_X12,  offsetof(CPUARMState, xregs[12]) },
    { HV_REG_X13,  offsetof(CPUARMState, xregs[13]) },
    { HV_REG_X14,  offsetof(CPUARMState, xregs[14]) },
    { HV_REG_X15,  offsetof(CPUARMState, xregs[15]) },
    { HV_REG_X16,  offsetof(CPUARMState, xregs[16]) },
    { HV_REG_X17,  offsetof(CPUARMState, xregs[17]) },
    { HV_REG_X18,  offsetof(CPUARMState, xregs[18]) },
    { HV_REG_X19,  offsetof(CPUARMState, xregs[19]) },
    { HV_REG_X20,  offsetof(CPUARMState, xregs[20]) },
    { HV_REG_X21,  offsetof(CPUARMState, xregs[21]) },
    { HV_REG_X22,  offsetof(CPUARMState, xregs[22]) },
    { HV_REG_X23,  offsetof(CPUARMState, xregs[23]) },
    { HV_REG_X24,  offsetof(CPUARMState, xregs[24]) },
    { HV_REG_X25,  offsetof(CPUARMState, xregs[25]) },
    { HV_REG_X26,  offsetof(CPUARMState, xregs[26]) },
    { HV_REG_X27,  offsetof(CPUARMState, xregs[27]) },
    { HV_REG_X28,  offsetof(CPUARMState, xregs[28]) },
    { HV_REG_X29,  offsetof(CPUARMState, xregs[29]) },
    { HV_REG_X30,  offsetof(CPUARMState, xregs[30]) },
    { HV_REG_PC,   offsetof(CPUARMState, pc) },
};

struct hvf_sreg_match {
    int reg;
    uint32_t key;
};

static const struct hvf_sreg_match hvf_sreg_match[] = {
    { HV_SYS_REG_DBGBVR0_EL1, HVF_SYSREG(0, 0, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR0_EL1, HVF_SYSREG(0, 0, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR0_EL1, HVF_SYSREG(0, 0, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR0_EL1, HVF_SYSREG(0, 0, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR1_EL1, HVF_SYSREG(0, 1, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR1_EL1, HVF_SYSREG(0, 1, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR1_EL1, HVF_SYSREG(0, 1, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR1_EL1, HVF_SYSREG(0, 1, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR2_EL1, HVF_SYSREG(0, 2, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR2_EL1, HVF_SYSREG(0, 2, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR2_EL1, HVF_SYSREG(0, 2, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR2_EL1, HVF_SYSREG(0, 2, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR3_EL1, HVF_SYSREG(0, 3, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR3_EL1, HVF_SYSREG(0, 3, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR3_EL1, HVF_SYSREG(0, 3, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR3_EL1, HVF_SYSREG(0, 3, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR4_EL1, HVF_SYSREG(0, 4, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR4_EL1, HVF_SYSREG(0, 4, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR4_EL1, HVF_SYSREG(0, 4, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR4_EL1, HVF_SYSREG(0, 4, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR5_EL1, HVF_SYSREG(0, 5, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR5_EL1, HVF_SYSREG(0, 5, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR5_EL1, HVF_SYSREG(0, 5, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR5_EL1, HVF_SYSREG(0, 5, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR6_EL1, HVF_SYSREG(0, 6, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR6_EL1, HVF_SYSREG(0, 6, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR6_EL1, HVF_SYSREG(0, 6, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR6_EL1, HVF_SYSREG(0, 6, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR7_EL1, HVF_SYSREG(0, 7, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR7_EL1, HVF_SYSREG(0, 7, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR7_EL1, HVF_SYSREG(0, 7, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR7_EL1, HVF_SYSREG(0, 7, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR8_EL1, HVF_SYSREG(0, 8, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR8_EL1, HVF_SYSREG(0, 8, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR8_EL1, HVF_SYSREG(0, 8, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR8_EL1, HVF_SYSREG(0, 8, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR9_EL1, HVF_SYSREG(0, 9, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR9_EL1, HVF_SYSREG(0, 9, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR9_EL1, HVF_SYSREG(0, 9, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR9_EL1, HVF_SYSREG(0, 9, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR10_EL1, HVF_SYSREG(0, 10, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR10_EL1, HVF_SYSREG(0, 10, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR10_EL1, HVF_SYSREG(0, 10, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR10_EL1, HVF_SYSREG(0, 10, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR11_EL1, HVF_SYSREG(0, 11, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR11_EL1, HVF_SYSREG(0, 11, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR11_EL1, HVF_SYSREG(0, 11, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR11_EL1, HVF_SYSREG(0, 11, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR12_EL1, HVF_SYSREG(0, 12, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR12_EL1, HVF_SYSREG(0, 12, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR12_EL1, HVF_SYSREG(0, 12, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR12_EL1, HVF_SYSREG(0, 12, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR13_EL1, HVF_SYSREG(0, 13, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR13_EL1, HVF_SYSREG(0, 13, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR13_EL1, HVF_SYSREG(0, 13, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR13_EL1, HVF_SYSREG(0, 13, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR14_EL1, HVF_SYSREG(0, 14, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR14_EL1, HVF_SYSREG(0, 14, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR14_EL1, HVF_SYSREG(0, 14, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR14_EL1, HVF_SYSREG(0, 14, 14, 0, 7) },

    { HV_SYS_REG_DBGBVR15_EL1, HVF_SYSREG(0, 15, 14, 0, 4) },
    { HV_SYS_REG_DBGBCR15_EL1, HVF_SYSREG(0, 15, 14, 0, 5) },
    { HV_SYS_REG_DBGWVR15_EL1, HVF_SYSREG(0, 15, 14, 0, 6) },
    { HV_SYS_REG_DBGWCR15_EL1, HVF_SYSREG(0, 15, 14, 0, 7) },

#ifdef SYNC_NO_RAW_REGS
    /*
     * The registers below are manually synced on init because they are
     * marked as NO_RAW. We still list them to make number space sync easier.
     */
    { HV_SYS_REG_MDCCINT_EL1, HVF_SYSREG(0, 2, 2, 0, 0) },
    { HV_SYS_REG_MIDR_EL1, HVF_SYSREG(0, 0, 3, 0, 0) },
    { HV_SYS_REG_MPIDR_EL1, HVF_SYSREG(0, 0, 3, 0, 5) },
    { HV_SYS_REG_ID_AA64PFR0_EL1, HVF_SYSREG(0, 4, 3, 0, 0) },
#endif
    { HV_SYS_REG_ID_AA64PFR1_EL1, HVF_SYSREG(0, 4, 3, 0, 2) },
    { HV_SYS_REG_ID_AA64DFR0_EL1, HVF_SYSREG(0, 5, 3, 0, 0) },
    { HV_SYS_REG_ID_AA64DFR1_EL1, HVF_SYSREG(0, 5, 3, 0, 1) },
    { HV_SYS_REG_ID_AA64ISAR0_EL1, HVF_SYSREG(0, 6, 3, 0, 0) },
    { HV_SYS_REG_ID_AA64ISAR1_EL1, HVF_SYSREG(0, 6, 3, 0, 1) },
#ifdef SYNC_NO_MMFR0
    /* We keep the hardware MMFR0 around. HW limits are there anyway */
    { HV_SYS_REG_ID_AA64MMFR0_EL1, HVF_SYSREG(0, 7, 3, 0, 0) },
#endif
    { HV_SYS_REG_ID_AA64MMFR1_EL1, HVF_SYSREG(0, 7, 3, 0, 1) },
    { HV_SYS_REG_ID_AA64MMFR2_EL1, HVF_SYSREG(0, 7, 3, 0, 2) },

    { HV_SYS_REG_MDSCR_EL1, HVF_SYSREG(0, 2, 2, 0, 2) },
    { HV_SYS_REG_SCTLR_EL1, HVF_SYSREG(1, 0, 3, 0, 0) },
    { HV_SYS_REG_CPACR_EL1, HVF_SYSREG(1, 0, 3, 0, 2) },
    { HV_SYS_REG_TTBR0_EL1, HVF_SYSREG(2, 0, 3, 0, 0) },
    { HV_SYS_REG_TTBR1_EL1, HVF_SYSREG(2, 0, 3, 0, 1) },
    { HV_SYS_REG_TCR_EL1, HVF_SYSREG(2, 0, 3, 0, 2) },

    { HV_SYS_REG_APIAKEYLO_EL1, HVF_SYSREG(2, 1, 3, 0, 0) },
    { HV_SYS_REG_APIAKEYHI_EL1, HVF_SYSREG(2, 1, 3, 0, 1) },
    { HV_SYS_REG_APIBKEYLO_EL1, HVF_SYSREG(2, 1, 3, 0, 2) },
    { HV_SYS_REG_APIBKEYHI_EL1, HVF_SYSREG(2, 1, 3, 0, 3) },
    { HV_SYS_REG_APDAKEYLO_EL1, HVF_SYSREG(2, 2, 3, 0, 0) },
    { HV_SYS_REG_APDAKEYHI_EL1, HVF_SYSREG(2, 2, 3, 0, 1) },
    { HV_SYS_REG_APDBKEYLO_EL1, HVF_SYSREG(2, 2, 3, 0, 2) },
    { HV_SYS_REG_APDBKEYHI_EL1, HVF_SYSREG(2, 2, 3, 0, 3) },
    { HV_SYS_REG_APGAKEYLO_EL1, HVF_SYSREG(2, 3, 3, 0, 0) },
    { HV_SYS_REG_APGAKEYHI_EL1, HVF_SYSREG(2, 3, 3, 0, 1) },

    { HV_SYS_REG_SPSR_EL1, HVF_SYSREG(4, 0, 3, 1, 0) },
    { HV_SYS_REG_ELR_EL1, HVF_SYSREG(4, 0, 3, 0, 1) },
    { HV_SYS_REG_SP_EL0, HVF_SYSREG(4, 1, 3, 0, 0) },
    { HV_SYS_REG_AFSR0_EL1, HVF_SYSREG(5, 1, 3, 0, 0) },
    { HV_SYS_REG_AFSR1_EL1, HVF_SYSREG(5, 1, 3, 0, 1) },
    { HV_SYS_REG_ESR_EL1, HVF_SYSREG(5, 2, 3, 0, 0) },
    { HV_SYS_REG_FAR_EL1, HVF_SYSREG(6, 0, 3, 0, 0) },
    { HV_SYS_REG_PAR_EL1, HVF_SYSREG(7, 4, 3, 0, 0) },
    { HV_SYS_REG_MAIR_EL1, HVF_SYSREG(10, 2, 3, 0, 0) },
    { HV_SYS_REG_AMAIR_EL1, HVF_SYSREG(10, 3, 3, 0, 0) },
    { HV_SYS_REG_VBAR_EL1, HVF_SYSREG(12, 0, 3, 0, 0) },
    { HV_SYS_REG_CONTEXTIDR_EL1, HVF_SYSREG(13, 0, 3, 0, 1) },
    { HV_SYS_REG_TPIDR_EL1, HVF_SYSREG(13, 0, 3, 0, 4) },
    { HV_SYS_REG_CNTKCTL_EL1, HVF_SYSREG(14, 1, 3, 0, 0) },
    { HV_SYS_REG_CSSELR_EL1, HVF_SYSREG(0, 0, 3, 2, 0) },
    { HV_SYS_REG_TPIDR_EL0, HVF_SYSREG(13, 0, 3, 3, 2) },
    { HV_SYS_REG_TPIDRRO_EL0, HVF_SYSREG(13, 0, 3, 3, 3) },
    { HV_SYS_REG_CNTV_CTL_EL0, HVF_SYSREG(14, 3, 3, 3, 1) },
    { HV_SYS_REG_CNTV_CVAL_EL0, HVF_SYSREG(14, 3, 3, 3, 2) },
    { HV_SYS_REG_SP_EL1, HVF_SYSREG(4, 1, 3, 4, 0) },
};

int hvf_get_registers(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    hv_return_t ret;
    uint64_t val;
    int i;

    for (i = 0; i < ARRAY_SIZE(hvf_reg_match); i++) {
        ret = hv_vcpu_get_reg(cpu->hvf->fd, hvf_reg_match[i].reg, &val);
        *(uint64_t *)((void *)env + hvf_reg_match[i].offset) = val;
        assert_hvf_ok(ret);
    }

    val = 0;
    ret = hv_vcpu_get_reg(cpu->hvf->fd, HV_REG_FPCR, &val);
    assert_hvf_ok(ret);
    vfp_set_fpcr(env, val);

    val = 0;
    ret = hv_vcpu_get_reg(cpu->hvf->fd, HV_REG_FPSR, &val);
    assert_hvf_ok(ret);
    vfp_set_fpsr(env, val);

    ret = hv_vcpu_get_reg(cpu->hvf->fd, HV_REG_CPSR, &val);
    assert_hvf_ok(ret);
    pstate_write(env, val);

    for (i = 0; i < ARRAY_SIZE(hvf_sreg_match); i++) {
        ret = hv_vcpu_get_sys_reg(cpu->hvf->fd, hvf_sreg_match[i].reg, &val);
        assert_hvf_ok(ret);

        arm_cpu->cpreg_values[i] = val;
    }
    write_list_to_cpustate(arm_cpu);

    return 0;
}

int hvf_put_registers(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    hv_return_t ret;
    uint64_t val;
    int i;

    for (i = 0; i < ARRAY_SIZE(hvf_reg_match); i++) {
        val = *(uint64_t *)((void *)env + hvf_reg_match[i].offset);
        ret = hv_vcpu_set_reg(cpu->hvf->fd, hvf_reg_match[i].reg, val);

        assert_hvf_ok(ret);
    }

    ret = hv_vcpu_set_reg(cpu->hvf->fd, HV_REG_FPCR, vfp_get_fpcr(env));
    assert_hvf_ok(ret);

    ret = hv_vcpu_set_reg(cpu->hvf->fd, HV_REG_FPSR, vfp_get_fpsr(env));
    assert_hvf_ok(ret);

    ret = hv_vcpu_set_reg(cpu->hvf->fd, HV_REG_CPSR, pstate_read(env));
    assert_hvf_ok(ret);

    write_cpustate_to_list(arm_cpu, false);
    for (i = 0; i < ARRAY_SIZE(hvf_sreg_match); i++) {
        val = arm_cpu->cpreg_values[i];
        ret = hv_vcpu_set_sys_reg(cpu->hvf->fd, hvf_sreg_match[i].reg, val);
        assert_hvf_ok(ret);
    }

    return 0;
}

static void flush_cpu_state(CPUState *cpu)
{
    if (cpu->vcpu_dirty) {
        hvf_put_registers(cpu);
        cpu->vcpu_dirty = false;
    }
}

static void hvf_set_reg(CPUState *cpu, int rt, uint64_t val)
{
    hv_return_t r;

    flush_cpu_state(cpu);

    if (rt < 31) {
        r = hv_vcpu_set_reg(cpu->hvf->fd, HV_REG_X0 + rt, val);
        assert_hvf_ok(r);
    }
}

static uint64_t hvf_get_reg(CPUState *cpu, int rt)
{
    uint64_t val = 0;
    hv_return_t r;

    flush_cpu_state(cpu);

    if (rt < 31) {
        r = hv_vcpu_get_reg(cpu->hvf->fd, HV_REG_X0 + rt, &val);
        assert_hvf_ok(r);
    }

    return val;
}

void hvf_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    ARMISARegisters host_isar;
    const struct isar_regs {
        int reg;
        uint64_t *val;
    } regs[] = {
        { HV_SYS_REG_ID_AA64PFR0_EL1, &host_isar.id_aa64pfr0 },
        { HV_SYS_REG_ID_AA64PFR1_EL1, &host_isar.id_aa64pfr1 },
        { HV_SYS_REG_ID_AA64DFR0_EL1, &host_isar.id_aa64dfr0 },
        { HV_SYS_REG_ID_AA64DFR1_EL1, &host_isar.id_aa64dfr1 },
        { HV_SYS_REG_ID_AA64ISAR0_EL1, &host_isar.id_aa64isar0 },
        { HV_SYS_REG_ID_AA64ISAR1_EL1, &host_isar.id_aa64isar1 },
        { HV_SYS_REG_ID_AA64MMFR0_EL1, &host_isar.id_aa64mmfr0 },
        { HV_SYS_REG_ID_AA64MMFR1_EL1, &host_isar.id_aa64mmfr1 },
        { HV_SYS_REG_ID_AA64MMFR2_EL1, &host_isar.id_aa64mmfr2 },
    };
    hv_vcpu_t fd;
    hv_vcpu_exit_t *exit;
    int i;

    cpu->dtb_compatible = "arm,arm-v8";
    cpu->env.features = (1ULL << ARM_FEATURE_V8) |
                        (1ULL << ARM_FEATURE_NEON) |
                        (1ULL << ARM_FEATURE_AARCH64) |
                        (1ULL << ARM_FEATURE_PMU) |
                        (1ULL << ARM_FEATURE_GENERIC_TIMER);

    /* We set up a small vcpu to extract host registers */

    assert_hvf_ok(hv_vcpu_create(&fd, &exit, NULL));
    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        assert_hvf_ok(hv_vcpu_get_sys_reg(fd, regs[i].reg, regs[i].val));
    }
    assert_hvf_ok(hv_vcpu_get_sys_reg(fd, HV_SYS_REG_MIDR_EL1, &cpu->midr));
    assert_hvf_ok(hv_vcpu_destroy(fd));

    cpu->isar = host_isar;
    cpu->reset_sctlr = 0x00c50078;
}

void hvf_arch_vcpu_destroy(CPUState *cpu)
{
}

int hvf_arch_init_vcpu(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    uint32_t sregs_match_len = ARRAY_SIZE(hvf_sreg_match);
    uint64_t pfr;
    hv_return_t ret;
    int i;

    env->aarch64 = 1;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(arm_cpu->gt_cntfrq_hz));

    /* Allocate enough space for our sysreg sync */
    arm_cpu->cpreg_indexes = g_renew(uint64_t, arm_cpu->cpreg_indexes,
                                     sregs_match_len);
    arm_cpu->cpreg_values = g_renew(uint64_t, arm_cpu->cpreg_values,
                                    sregs_match_len);
    arm_cpu->cpreg_vmstate_indexes = g_renew(uint64_t,
                                             arm_cpu->cpreg_vmstate_indexes,
                                             sregs_match_len);
    arm_cpu->cpreg_vmstate_values = g_renew(uint64_t,
                                            arm_cpu->cpreg_vmstate_values,
                                            sregs_match_len);

    memset(arm_cpu->cpreg_values, 0, sregs_match_len * sizeof(uint64_t));
    arm_cpu->cpreg_array_len = sregs_match_len;
    arm_cpu->cpreg_vmstate_array_len = sregs_match_len;

    /* Populate cp list for all known sysregs */
    for (i = 0; i < sregs_match_len; i++) {
        const ARMCPRegInfo *ri;

        arm_cpu->cpreg_indexes[i] = cpreg_to_kvm_id(hvf_sreg_match[i].key);

        ri = get_arm_cp_reginfo(arm_cpu->cp_regs, hvf_sreg_match[i].key);
        if (ri) {
            assert(!(ri->type & ARM_CP_NO_RAW));
        }
    }
    write_cpustate_to_list(arm_cpu, false);

    /* Set CP_NO_RAW system registers on init */
    ret = hv_vcpu_set_sys_reg(cpu->hvf->fd, HV_SYS_REG_MIDR_EL1,
                              arm_cpu->midr);
    assert_hvf_ok(ret);

    ret = hv_vcpu_set_sys_reg(cpu->hvf->fd, HV_SYS_REG_MPIDR_EL1,
                              arm_cpu->mp_affinity);
    assert_hvf_ok(ret);

    ret = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_ID_AA64PFR0_EL1, &pfr);
    assert_hvf_ok(ret);
    pfr |= env->gicv3state ? (1 << 24) : 0;
    ret = hv_vcpu_set_sys_reg(cpu->hvf->fd, HV_SYS_REG_ID_AA64PFR0_EL1, pfr);
    assert_hvf_ok(ret);

    /* We're limited to underlying hardware caps, override internal versions */
    ret = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_ID_AA64MMFR0_EL1,
                              &arm_cpu->isar.id_aa64mmfr0);
    assert_hvf_ok(ret);

    return 0;
}

void hvf_kick_vcpu_thread(CPUState *cpu)
{
    printf("%s tid=%p\n", __func__, pthread_self());
    cpus_kick_thread(cpu);
    if(cpu->stop) {
        hv_vcpus_exit(&cpu->hvf->fd, 1);
    }
}

static uint32_t hvf_reg2cp_reg(uint32_t reg)
{
    return ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP,
                              (reg >> 10) & 0xf,
                              (reg >> 1) & 0xf,
                              (reg >> 20) & 0x3,
                              (reg >> 14) & 0x7,
                              (reg >> 17) & 0x7);
}


static void my_cpu_dump_state(CPUState *cpu) {
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;

    for (int i = HV_SIMD_FP_REG_Q0; i < 32; i++) {
        hv_simd_fp_uchar16_t neon_reg;
        uint64_t high, low;
        hv_return_t r = hv_vcpu_get_simd_fp_reg(cpu->hvf->fd, i, &neon_reg);
        assert_hvf_ok(r);

        low = neon_reg[0] | neon_reg[1] << 8 | neon_reg[2] << 16 | neon_reg[3] << 24 | (uint64_t)neon_reg[4] << 32 | (uint64_t)neon_reg[5] << 40 | (uint64_t)neon_reg[6] << 48 | (uint64_t)neon_reg[7] << 56;
        high = neon_reg[8] | neon_reg[9] << 8 | neon_reg[10] << 16 | neon_reg[11] << 24 | (uint64_t)neon_reg[12] << 32 | (uint64_t)neon_reg[13] << 40 | (uint64_t)neon_reg[14] << 48 | (uint64_t)neon_reg[15] << 56;
        printf("q%02d=0x%016llx %016llx\n", i, high, low);
    }

    for (int i = 0; i < 4; i++) { 
        printf("far_el[%d]=%#018llx\n", i, env->cp15.far_el[i]);
    }
}

static uint64_t hvf_sysreg_read_cp(CPUState *cpu, uint32_t reg)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    const ARMCPRegInfo *ri;
    uint64_t val = 0;

    ri = get_arm_cp_reginfo(arm_cpu->cp_regs, hvf_reg2cp_reg(reg));
    if (ri) {
        if (ri->type & ARM_CP_CONST) {
            val = ri->resetvalue;
        } else if (ri->readfn) {
            val = ri->readfn(env, ri);
        } else {
            val = CPREG_FIELD64(env, ri);
        }
        DPRINTF("vgic read from %s [val=%016llx]", ri->name, val);
    } else if (reg == SYSREG_MAGIC_PPL) {
        printf("SYSREG_MAGIC_PPL\n");
        cpu_dump_state(cpu, stderr, 0);
        my_cpu_dump_state(cpu);
        exit(1);
    } else {

        printf("unhandled sysreg read @ %#llx %08x (op0=%d op1=%d op2=%d "
                    "crn=%d crm=%d)\n", env->pc, reg, (reg >> 20) & 0x3,
                    (reg >> 14) & 0x7, (reg >> 17) & 0x7,
                    (reg >> 10) & 0xf, (reg >> 1) & 0xf);
        cpu_dump_state(cpu, stderr, 0);

        exit(1);
    }
    return val;
}


static uint64_t hvf_sysreg_read(CPUState *cpu, uint32_t reg)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    uint64_t val = 0;

    switch (reg) {
    case SYSREG_CNTPCT_EL0:
        val = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
              gt_cntfrq_period_ns(arm_cpu);
        //printf("read SYSREG_CNTPCT_EL0 val = %#llx\n", val);
        break;
    case SYSREG_PMCCNTR_EL0:
        val = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        DPRINTF("read SYSREG_PMCCNTR_EL0 val = %#llx\n", val);
        break;
    case SYSREG_APPLE_UNKN_TIMER:
    case SYSREG_ICC_AP0R0_EL1:
    case SYSREG_ICC_AP0R1_EL1:
    case SYSREG_ICC_AP0R2_EL1:
    case SYSREG_ICC_AP0R3_EL1:
    case SYSREG_ICC_AP1R0_EL1:
    case SYSREG_ICC_AP1R1_EL1:
    case SYSREG_ICC_AP1R2_EL1:
    case SYSREG_ICC_AP1R3_EL1:
    case SYSREG_ICC_ASGI1R_EL1:
    case SYSREG_ICC_BPR0_EL1:
    case SYSREG_ICC_BPR1_EL1:
    case SYSREG_ICC_DIR_EL1:
    case SYSREG_ICC_EOIR0_EL1:
    case SYSREG_ICC_EOIR1_EL1:
    case SYSREG_ICC_HPPIR0_EL1:
    case SYSREG_ICC_HPPIR1_EL1:
    case SYSREG_ICC_IAR0_EL1:
    case SYSREG_ICC_IAR1_EL1:
    case SYSREG_ICC_IGRPEN0_EL1:
    case SYSREG_ICC_IGRPEN1_EL1:
    case SYSREG_ICC_PMR_EL1:
    case SYSREG_ICC_SGI0R_EL1:
    case SYSREG_ICC_SGI1R_EL1:
    case SYSREG_ICC_SRE_EL1:
        val = hvf_sysreg_read_cp(cpu, reg);
        break;
    case SYSREG_ICC_CTLR_EL1:
        val = hvf_sysreg_read_cp(cpu, reg);

        /* AP0R registers above 0 don't trap, expose less PRIs to fit */
        val &= ~ICC_CTLR_EL1_PRIBITS_MASK;
        val |= 4 << ICC_CTLR_EL1_PRIBITS_SHIFT;
        break;
    default:
        val = hvf_sysreg_read_cp(cpu, reg);
        if (!val) {
            DPRINTF("unhandled sysreg read %08x (op0=%d op1=%d op2=%d "
                    "crn=%d crm=%d)", reg, (reg >> 20) & 0x3,
                    (reg >> 14) & 0x7, (reg >> 17) & 0x7,
                    (reg >> 10) & 0xf, (reg >> 1) & 0xf);
        }
        break;
    }

    return val;
}

static void hvf_sysreg_write_cp(CPUState *cpu, uint32_t reg, uint64_t val)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    const ARMCPRegInfo *ri;

    ri = get_arm_cp_reginfo(arm_cpu->cp_regs, hvf_reg2cp_reg(reg));

    if (ri) {
        if (ri->writefn) {
            ri->writefn(env, ri, val);
        } else {
            CPREG_FIELD64(env, ri) = val;
        }
        DPRINTF("vgic write to %s [val=%016llx]", ri->name, val);
    } else {

        printf("unhandled sysreg write @ %#llx %08x (op0=%d op1=%d op2=%d "
                    "crn=%d crm=%d)\n", env->pc, reg, (reg >> 20) & 0x3,
                    (reg >> 14) & 0x7, (reg >> 17) & 0x7,
                    (reg >> 10) & 0xf, (reg >> 1) & 0xf);
        cpu_dump_state(cpu, stderr, 0);
        exit(1);
    }
}

static void hvf_sysreg_write(CPUState *cpu, uint32_t reg, uint64_t val)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);

    switch (reg) {
    case SYSREG_CNTPCT_EL0:
        break;
    case SYSREG_APPLE_UNKN_TIMER:
    case SYSREG_ICC_AP0R0_EL1:
    case SYSREG_ICC_AP0R1_EL1:
    case SYSREG_ICC_AP0R2_EL1:
    case SYSREG_ICC_AP0R3_EL1:
    case SYSREG_ICC_AP1R0_EL1:
    case SYSREG_ICC_AP1R1_EL1:
    case SYSREG_ICC_AP1R2_EL1:
    case SYSREG_ICC_AP1R3_EL1:
    case SYSREG_ICC_ASGI1R_EL1:
    case SYSREG_ICC_BPR0_EL1:
    case SYSREG_ICC_BPR1_EL1:
    case SYSREG_ICC_CTLR_EL1:
    case SYSREG_ICC_DIR_EL1:
    case SYSREG_ICC_HPPIR0_EL1:
    case SYSREG_ICC_HPPIR1_EL1:
    case SYSREG_ICC_IAR0_EL1:
    case SYSREG_ICC_IAR1_EL1:
    case SYSREG_ICC_IGRPEN0_EL1:
    case SYSREG_ICC_IGRPEN1_EL1:
    case SYSREG_ICC_PMR_EL1:
    case SYSREG_ICC_SGI0R_EL1:
    case SYSREG_ICC_SGI1R_EL1:
    case SYSREG_ICC_SRE_EL1:
        hvf_sysreg_write_cp(cpu, reg, val);
        break;
    case SYSREG_ICC_EOIR0_EL1:
    case SYSREG_ICC_EOIR1_EL1:
        printf("Wrote EOIR");
        hvf_sysreg_write_cp(cpu, reg, val);
        qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], 0);
        hv_vcpu_set_vtimer_mask(cpu->hvf->fd, false);
    default:
        hvf_sysreg_write_cp(cpu, reg, val);
        //DPRINTF("unhandled sysreg write %08x", reg);
        break;
    }
}

static int hvf_inject_interrupts(CPUState *cpu)
{
    if (cpu->interrupt_request & CPU_INTERRUPT_FIQ && qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) > 0x12d34f688) {
        printf("injecting FIQ\n");
        hv_vcpu_set_pending_interrupt(cpu->hvf->fd, HV_INTERRUPT_TYPE_FIQ, true);
    }

    if (cpu->interrupt_request & CPU_INTERRUPT_HARD) {
        printf("injecting IRQ");
        hv_vcpu_set_pending_interrupt(cpu->hvf->fd, HV_INTERRUPT_TYPE_IRQ, true);
    }

    return 0;
}

static void hvf_wait_for_ipi(CPUState *cpu, struct timespec *ts)
{
    /*
     * Use pselect to sleep so that other threads can IPI us while we're
     * sleeping.
     */
    printf("wait for ipi\n");
    qatomic_mb_set(&cpu->thread_kicked, false);
    qemu_mutex_unlock_iothread();
    pselect(0, 0, 0, 0, ts, &cpu->hvf->unblock_ipi_mask);
    qemu_mutex_lock_iothread();
}

static void hvf_invoke_set_guest_debug(CPUState *cpu, run_on_cpu_data data);

int hvf_vcpu_exec(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    hv_vcpu_exit_t *hvf_exit = cpu->hvf->exit;
    hv_return_t r;

    while (1) {
        bool advance_pc = false;

        qemu_wait_io_event_common(cpu);
        flush_cpu_state(cpu);

        if (hvf_inject_interrupts(cpu)) {
            printf("EXCP_INTERRUPT\n");
            return EXCP_INTERRUPT;
        }

        if (cpu->halted) {
            printf("HALTED\n");
            return EXCP_HLT;
        }

        qemu_mutex_unlock_iothread();
        printf(">> running vcpu\n");
        hvf_invoke_set_guest_debug(cpu, RUN_ON_CPU_NULL);

        {
            uint64_t mdscr = -1, spsr = -1, elr = -1;

            r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_SPSR_EL1, &spsr);
            assert_hvf_ok(r);

            r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_MDSCR_EL1, &mdscr);
            assert_hvf_ok(r);

            r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_ELR_EL1, &elr);
            assert_hvf_ok(r);

            printf(" SPSR_EL1=%#llx MDSCR_EL1=%#llx ELR_EL1=%#llx\n", spsr, mdscr, elr);
            cpu_dump_state(cpu, stderr, 0);

        }

        assert_hvf_ok(hv_vcpu_run(cpu->hvf->fd));

        /* handle VMEXIT */
        uint64_t exit_reason = hvf_exit->reason;
        uint64_t syndrome = hvf_exit->exception.syndrome;
        uint32_t ec = syn_get_ec(syndrome);

        qemu_mutex_lock_iothread();
    
        cpu_synchronize_state(cpu);
        DPRINTF("VM Exit: exit_reason=%#llx ec=%#x pc=0x%llx", exit_reason, ec, env->pc);
        {
            uint64_t mdscr = -1, spsr = -1, elr = -1;

            r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_SPSR_EL1, &spsr);
            assert_hvf_ok(r);

            r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_MDSCR_EL1, &mdscr);
            assert_hvf_ok(r);

            r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_ELR_EL1, &elr);
            assert_hvf_ok(r);

            printf(" SPSR_EL1=%#llx MDSCR_EL1=%#llx ELR_EL1=%#llx\n", spsr, mdscr, elr);
            cpu_dump_state(cpu, stderr, 0);

        }

        switch (exit_reason) {
        case HV_EXIT_REASON_EXCEPTION:
            /* This is the main one, handle below. */
            break;
        case HV_EXIT_REASON_VTIMER_ACTIVATED:
            DPRINTF("HV_EXIT_REASON_VTIMER_ACTIVATED pc=0x%llx clk=%#llx\n", env->pc, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], 1);
            continue;
        case HV_EXIT_REASON_CANCELED:
            /* we got kicked, no exit to process */
            //hvf_invoke_set_guest_debug(cpu, RUN_ON_CPU_NULL);
            return 0;
        default:
            printf("unhandled exit reason %llu\n", exit_reason);
            assert(0);
        }

        // check for timer
        {
            uint64_t ctl;
            bool timer_active;

            r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_CNTV_CTL_EL0,
                                    &ctl);
            assert_hvf_ok(r);

            timer_active = !!(ctl & (1 << 2));

            if (timer_active != (cpu->interrupt_request & CPU_INTERRUPT_FIQ)) {
                qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], timer_active);
            }
        }

        switch (ec) {
        case EC_DATAABORT: {
            bool isv = syndrome & ARM_EL_ISV;
            bool iswrite = (syndrome >> 6) & 1;
            bool s1ptw = (syndrome >> 7) & 1;
            uint32_t sas = (syndrome >> 22) & 3;
            uint32_t len = 1 << sas;
            uint32_t srt = (syndrome >> 16) & 0x1f;
            uint64_t val = 0;
            int cur_el = arm_current_el(env);

            DPRINTF("data abort: [pc=0x%llx va=0x%016llx pa=0x%016llx isv=%x "
                    "iswrite=%x s1ptw=%x len=%d srt=%d el=%d]",
                    env->pc, hvf_exit->exception.virtual_address,
                    hvf_exit->exception.physical_address, isv, iswrite,
                    s1ptw, len, srt, cur_el);

            if (!isv) {
                extern void aarch64_cpu_dump_state(CPUState *cs, FILE *f, int flags);

                aarch64_cpu_dump_state(cpu, stdout, 0);
                exit(0);
            }

            if (iswrite) {
                val = hvf_get_reg(cpu, srt);
                address_space_write(&address_space_memory,
                                    hvf_exit->exception.physical_address,
                                    MEMTXATTRS_UNSPECIFIED, &val, len);

                /*
                 * We do not have a callback to see if the timer is out of
                 * pending state. That means every MMIO write could
                 * potentially be an EOI ends the vtimer. Until we get an
                 * actual callback, let's just see if the timer is still
                 * pending on every possible toggle point.
                 */
                qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], 0);
                hv_vcpu_set_vtimer_mask(cpu->hvf->fd, false);
            } else {
                address_space_read(&address_space_memory,
                                   hvf_exit->exception.physical_address,
                                   MEMTXATTRS_UNSPECIFIED, &val, len);
                hvf_set_reg(cpu, srt, val);
            }

            advance_pc = true;
            break;
        }
        case EC_SYSTEMREGISTERTRAP: {
            bool isread = (syndrome >> 0) & 1;
            uint32_t rt = (syndrome >> 5) & 0x1f;
            uint32_t reg = syndrome & SYSREG_MASK;
            uint64_t val = 0;

            DPRINTF("sysreg %s operation reg=%08x val=%#llx (op0=%d op1=%d op2=%d "
                    "crn=%d crm=%d)", (isread) ? "read" : "write",
                    reg, isread ? hvf_sysreg_read(cpu, reg) : val, (reg >> 20) & 0x3,
                    (reg >> 14) & 0x7, (reg >> 17) & 0x7,
                    (reg >> 10) & 0xf, (reg >> 1) & 0xf);

            if (isread) {
                hvf_set_reg(cpu, rt, hvf_sysreg_read(cpu, reg));
            } else {
                val = hvf_get_reg(cpu, rt);
                hvf_sysreg_write(cpu, reg, val);
            }

            advance_pc = true;
            break;
        }
        case EC_WFX_TRAP:
            printf("wfx trap\n");
            advance_pc = true;
            if (!(syndrome & WFX_IS_WFE) && !(cpu->interrupt_request &
                (CPU_INTERRUPT_HARD | CPU_INTERRUPT_FIQ))) {

                uint64_t ctl;
                r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_CNTV_CTL_EL0,
                                        &ctl);
                assert_hvf_ok(r);

                if (!(ctl & 1) || (ctl & 2)) {
                    /* Timer disabled or masked, just wait for an IPI. */
                    hvf_wait_for_ipi(cpu, NULL);
                    break;
                }

                uint64_t cval;
                r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_CNTV_CVAL_EL0,
                                        &cval);
                assert_hvf_ok(r);

                int64_t ticks_to_sleep = cval - mach_absolute_time();
                if (ticks_to_sleep < 0) {
                    break;
                }

                uint64_t seconds = ticks_to_sleep / arm_cpu->gt_cntfrq_hz;
                uint64_t nanos =
                    (ticks_to_sleep - arm_cpu->gt_cntfrq_hz * seconds) *
                    1000000000 / arm_cpu->gt_cntfrq_hz;

                /*
                 * Don't sleep for less than 2ms. This is believed to improve
                 * latency of message passing workloads.
                 */
                if (!seconds && nanos < 2000000) {
                    break;
                }

                struct timespec ts = { seconds, nanos };
                hvf_wait_for_ipi(cpu, &ts);
            }
            break;
        case EC_AA64_HVC:
            cpu_synchronize_state(cpu);
            if (arm_is_psci_call(arm_cpu, EXCP_HVC)) {
                arm_handle_psci_call(arm_cpu);
            } else {
                printf("unknown HVC! %016llx", env->xregs[0]);
                env->xregs[0] = -1;
                exit(1);
            }
            break;
        case EC_AA64_SMC:
            cpu_synchronize_state(cpu);
            if (arm_is_psci_call(arm_cpu, EXCP_SMC)) {
                arm_handle_psci_call(arm_cpu);
            } else {
                printf("unknown SMC! %016llx", env->xregs[0]);
                env->xregs[0] = -1;
                exit(1);
            }
            env->pc += 4;
            break;
        case EC_SOFTWARESTEP:
            cpu_synchronize_state(cpu);
            DPRINTF("exit: EC_SOFTWARESTEP [ec=0x%x pc=0x%llx pstate.ss=%d]", ec, env->pc, env->pstate & PSTATE_SS);

            //env->pc += 4; // not sure
            {
                uint64_t pc;

                flush_cpu_state(cpu);

                r = hv_vcpu_get_reg(cpu->hvf->fd, HV_REG_PC, &pc);
                assert_hvf_ok(r);
                pc += 4;
                r = hv_vcpu_set_reg(cpu->hvf->fd, HV_REG_PC, pc);
                assert_hvf_ok(r);
            }
            return EXCP_DEBUG;
        case EC_AA64_BKPT:
            cpu_synchronize_state(cpu);
            DPRINTF("exit: EC_AA64_BKPT [ec=0x%x pc=0x%llx]", ec, env->pc);

            return EXCP_DEBUG;

        default:
            cpu_synchronize_state(cpu);
            DPRINTF("exit: %llx [ec=0x%x pc=0x%llx]", syndrome, ec, env->pc);
            error_report("%llx: unhandled exit %llx", env->pc, exit_reason);
            exit(1);
        }

        if (advance_pc) {
            uint64_t pc;

            flush_cpu_state(cpu);

            r = hv_vcpu_get_reg(cpu->hvf->fd, HV_REG_PC, &pc);
            assert_hvf_ok(r);
            pc += 4;
            r = hv_vcpu_set_reg(cpu->hvf->fd, HV_REG_PC, pc);
            assert_hvf_ok(r);
        }
    }
}

static void hvf_invoke_set_guest_debug(CPUState *cpu, run_on_cpu_data data)
{
    printf("%s cpu->singlestep_enabled=%d hvf->enable_debug=%d tid=%p\n", __func__, cpu->singlestep_enabled, cpu->hvf->enable_debug,  pthread_self());
    uint64_t mdscr;
    hv_return_t r;

    cpu->hvf->enable_debug = cpu->singlestep_enabled || cpu->hvf->enable_debug;

    r = hv_vcpu_set_trap_debug_exceptions(cpu->hvf->fd, cpu->hvf->enable_debug);
    assert_hvf_ok(r);


    // breakpoints work fine with this
    //return;

    // set mdscr_el1.ss
    r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_MDSCR_EL1, &mdscr);
    assert_hvf_ok(r);

    if (cpu->singlestep_enabled) {
        mdscr |= (1 << 0);// | (1 << 13);
    } else {
        mdscr &= ~(1ULL << 0);
    }

    r = hv_vcpu_set_sys_reg(cpu->hvf->fd, HV_SYS_REG_MDSCR_EL1, mdscr);
    assert_hvf_ok(r);
    printf("%s mdscr=%#llx\n", __func__, mdscr);

    
    uint64_t spsr;
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;

    // set spsr_el1.ss
    r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_SPSR_EL1, &spsr);
    assert_hvf_ok(r);
    if (cpu->singlestep_enabled) {
        spsr |= (1 << 21);
    } else {
        spsr &= ~(1ULL << 21);

    }

    r = hv_vcpu_set_sys_reg(cpu->hvf->fd, HV_SYS_REG_SPSR_EL1, spsr);
    assert_hvf_ok(r);
    printf("%s spsr=%#llx\n", __func__, spsr);

    // set elr_el1 = pc
    r = hv_vcpu_set_sys_reg(cpu->hvf->fd, HV_SYS_REG_ELR_EL1, env->pc);
    assert_hvf_ok(r);
    printf("%s elr_el1=%#llx\n", __func__, env->pc);
}

void hvf_arch_update_guest_debug(CPUState *cpu)
{
    printf("%s\n", __func__);
    run_on_cpu(cpu, hvf_invoke_set_guest_debug, RUN_ON_CPU_NULL);

}

