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
#include "hvf_arm.h"

#include <mach/mach_time.h>

#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/intc/gicv3_internal.h"
#include "qemu/main-loop.h"
#include "sysemu/cpus.h"
#include "arm-powerctl.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"
#include "trace/trace-target_arm_hvf.h"

#define HVF_SYSREG(crn, crm, op0, op1, op2) \
        ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, crn, crm, op0, op1, op2)
#define PL1_WRITE_MASK 0x4

#define SYSREG(op0, op1, crn, crm, op2) \
    ((op0 << 20) | (op2 << 17) | (op1 << 14) | (crn << 10) | (crm << 1))
#define SYSREG_MASK           SYSREG(0x3, 0x7, 0xf, 0xf, 0x7)
#define SYSREG_CNTPCT_EL0     SYSREG(3, 3, 14, 0, 1)
#define SYSREG_CNTP_CTL_EL0   SYSREG(3, 3, 14, 2, 1)
#define SYSREG_PMCCNTR_EL0    SYSREG(3, 3, 9, 13, 0)
#define SYSREG_OSLAR_EL1      SYSREG(2, 0, 1, 0, 4)

#define SYSREG_ICC_AP0R0_EL1     SYSREG(3, 0, 12, 8, 4)
#define SYSREG_ICC_AP0R1_EL1     SYSREG(3, 0, 12, 8, 5)
#define SYSREG_ICC_AP0R2_EL1     SYSREG(3, 0, 12, 8, 6)
#define SYSREG_ICC_AP0R3_EL1     SYSREG(3, 0, 12, 8, 7)
#define SYSREG_ICC_AP1R0_EL1     SYSREG(3, 0, 12, 9, 0)
#define SYSREG_ICC_AP1R1_EL1     SYSREG(3, 0, 12, 9, 1)
#define SYSREG_ICC_AP1R2_EL1     SYSREG(3, 0, 12, 9, 2)
#define SYSREG_ICC_AP1R3_EL1     SYSREG(3, 0, 12, 9, 3)
#define SYSREG_ICC_ASGI1R_EL1    SYSREG(3, 0, 12, 11, 6)
#define SYSREG_ICC_BPR0_EL1      SYSREG(3, 0, 12, 8, 3)
#define SYSREG_ICC_BPR1_EL1      SYSREG(3, 0, 12, 12, 3)
#define SYSREG_ICC_CTLR_EL1      SYSREG(3, 0, 12, 12, 4)
#define SYSREG_ICC_DIR_EL1       SYSREG(3, 0, 12, 11, 1)
#define SYSREG_ICC_EOIR0_EL1     SYSREG(3, 0, 12, 8, 1)
#define SYSREG_ICC_EOIR1_EL1     SYSREG(3, 0, 12, 12, 1)
#define SYSREG_ICC_HPPIR0_EL1    SYSREG(3, 0, 12, 8, 2)
#define SYSREG_ICC_HPPIR1_EL1    SYSREG(3, 0, 12, 12, 2)
#define SYSREG_ICC_IAR0_EL1      SYSREG(3, 0, 12, 8, 0)
#define SYSREG_ICC_IAR1_EL1      SYSREG(3, 0, 12, 12, 0)
#define SYSREG_ICC_IGRPEN0_EL1   SYSREG(3, 0, 12, 12, 6)
#define SYSREG_ICC_IGRPEN1_EL1   SYSREG(3, 0, 12, 12, 7)
#define SYSREG_ICC_PMR_EL1       SYSREG(3, 0, 4, 6, 0)
#define SYSREG_ICC_RPR_EL1       SYSREG(3, 0, 12, 11, 3)
#define SYSREG_ICC_SGI0R_EL1     SYSREG(3, 0, 12, 11, 7)
#define SYSREG_ICC_SGI1R_EL1     SYSREG(3, 0, 12, 11, 5)
#define SYSREG_ICC_SRE_EL1       SYSREG(3, 0, 12, 12, 5)

#define WFX_IS_WFE (1 << 0)

#define TMR_CTL_ENABLE  (1 << 0)
#define TMR_CTL_IMASK   (1 << 1)
#define TMR_CTL_ISTATUS (1 << 2)

static void hvf_wfi(CPUState *cpu);

typedef struct ARMHostCPUFeatures {
    ARMISARegisters isar;
    uint64_t features;
    uint64_t midr;
    uint32_t reset_sctlr;
    const char *dtb_compatible;
} ARMHostCPUFeatures;

static ARMHostCPUFeatures arm_host_cpu_features;

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

static const struct hvf_reg_match hvf_fpreg_match[] = {
    { HV_SIMD_FP_REG_Q0,  offsetof(CPUARMState, vfp.zregs[0]) },
    { HV_SIMD_FP_REG_Q1,  offsetof(CPUARMState, vfp.zregs[1]) },
    { HV_SIMD_FP_REG_Q2,  offsetof(CPUARMState, vfp.zregs[2]) },
    { HV_SIMD_FP_REG_Q3,  offsetof(CPUARMState, vfp.zregs[3]) },
    { HV_SIMD_FP_REG_Q4,  offsetof(CPUARMState, vfp.zregs[4]) },
    { HV_SIMD_FP_REG_Q5,  offsetof(CPUARMState, vfp.zregs[5]) },
    { HV_SIMD_FP_REG_Q6,  offsetof(CPUARMState, vfp.zregs[6]) },
    { HV_SIMD_FP_REG_Q7,  offsetof(CPUARMState, vfp.zregs[7]) },
    { HV_SIMD_FP_REG_Q8,  offsetof(CPUARMState, vfp.zregs[8]) },
    { HV_SIMD_FP_REG_Q9,  offsetof(CPUARMState, vfp.zregs[9]) },
    { HV_SIMD_FP_REG_Q10, offsetof(CPUARMState, vfp.zregs[10]) },
    { HV_SIMD_FP_REG_Q11, offsetof(CPUARMState, vfp.zregs[11]) },
    { HV_SIMD_FP_REG_Q12, offsetof(CPUARMState, vfp.zregs[12]) },
    { HV_SIMD_FP_REG_Q13, offsetof(CPUARMState, vfp.zregs[13]) },
    { HV_SIMD_FP_REG_Q14, offsetof(CPUARMState, vfp.zregs[14]) },
    { HV_SIMD_FP_REG_Q15, offsetof(CPUARMState, vfp.zregs[15]) },
    { HV_SIMD_FP_REG_Q16, offsetof(CPUARMState, vfp.zregs[16]) },
    { HV_SIMD_FP_REG_Q17, offsetof(CPUARMState, vfp.zregs[17]) },
    { HV_SIMD_FP_REG_Q18, offsetof(CPUARMState, vfp.zregs[18]) },
    { HV_SIMD_FP_REG_Q19, offsetof(CPUARMState, vfp.zregs[19]) },
    { HV_SIMD_FP_REG_Q20, offsetof(CPUARMState, vfp.zregs[20]) },
    { HV_SIMD_FP_REG_Q21, offsetof(CPUARMState, vfp.zregs[21]) },
    { HV_SIMD_FP_REG_Q22, offsetof(CPUARMState, vfp.zregs[22]) },
    { HV_SIMD_FP_REG_Q23, offsetof(CPUARMState, vfp.zregs[23]) },
    { HV_SIMD_FP_REG_Q24, offsetof(CPUARMState, vfp.zregs[24]) },
    { HV_SIMD_FP_REG_Q25, offsetof(CPUARMState, vfp.zregs[25]) },
    { HV_SIMD_FP_REG_Q26, offsetof(CPUARMState, vfp.zregs[26]) },
    { HV_SIMD_FP_REG_Q27, offsetof(CPUARMState, vfp.zregs[27]) },
    { HV_SIMD_FP_REG_Q28, offsetof(CPUARMState, vfp.zregs[28]) },
    { HV_SIMD_FP_REG_Q29, offsetof(CPUARMState, vfp.zregs[29]) },
    { HV_SIMD_FP_REG_Q30, offsetof(CPUARMState, vfp.zregs[30]) },
    { HV_SIMD_FP_REG_Q31, offsetof(CPUARMState, vfp.zregs[31]) },
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
    hv_simd_fp_uchar16_t fpval;
    int i;

    for (i = 0; i < ARRAY_SIZE(hvf_reg_match); i++) {
        ret = hv_vcpu_get_reg(cpu->hvf->fd, hvf_reg_match[i].reg, &val);
        *(uint64_t *)((void *)env + hvf_reg_match[i].offset) = val;
        assert_hvf_ok(ret);
    }

    for (i = 0; i < ARRAY_SIZE(hvf_fpreg_match); i++) {
        ret = hv_vcpu_get_simd_fp_reg(cpu->hvf->fd, hvf_fpreg_match[i].reg,
                                      &fpval);
        memcpy((void *)env + hvf_fpreg_match[i].offset, &fpval, sizeof(fpval));
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
    hv_simd_fp_uchar16_t fpval;
    int i;

    for (i = 0; i < ARRAY_SIZE(hvf_reg_match); i++) {
        val = *(uint64_t *)((void *)env + hvf_reg_match[i].offset);
        ret = hv_vcpu_set_reg(cpu->hvf->fd, hvf_reg_match[i].reg, val);
        assert_hvf_ok(ret);
    }

    for (i = 0; i < ARRAY_SIZE(hvf_fpreg_match); i++) {
        memcpy(&fpval, (void *)env + hvf_fpreg_match[i].offset, sizeof(fpval));
        ret = hv_vcpu_set_simd_fp_reg(cpu->hvf->fd, hvf_fpreg_match[i].reg,
                                      fpval);
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

static void hvf_arm_get_host_cpu_features(ARMHostCPUFeatures *ahcf)
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

    ahcf->dtb_compatible = "arm,arm-v8";
    ahcf->features = (1ULL << ARM_FEATURE_V8) |
                     (1ULL << ARM_FEATURE_NEON) |
                     (1ULL << ARM_FEATURE_AARCH64) |
                     (1ULL << ARM_FEATURE_PMU) |
                     (1ULL << ARM_FEATURE_GENERIC_TIMER);

    /* We set up a small vcpu to extract host registers */

    assert_hvf_ok(hv_vcpu_create(&fd, &exit, NULL));
    for (i = 0; i < ARRAY_SIZE(regs); i++) {
        assert_hvf_ok(hv_vcpu_get_sys_reg(fd, regs[i].reg, regs[i].val));
    }
    assert_hvf_ok(hv_vcpu_get_sys_reg(fd, HV_SYS_REG_MIDR_EL1, &ahcf->midr));
    assert_hvf_ok(hv_vcpu_destroy(fd));

    ahcf->isar = host_isar;
    ahcf->reset_sctlr = 0x00c50078;

    /* Make sure we don't advertise AArch32 support for EL0/EL1 */
    g_assert((host_isar.id_aa64pfr0 & 0xff) == 0x11);
}

void hvf_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    if (!arm_host_cpu_features.dtb_compatible) {
        if (!hvf_enabled()) {
            cpu->host_cpu_probe_failed = true;
            return;
        }
        hvf_arm_get_host_cpu_features(&arm_host_cpu_features);
    }

    cpu->dtb_compatible = arm_host_cpu_features.dtb_compatible;
    cpu->isar = arm_host_cpu_features.isar;
    cpu->env.features = arm_host_cpu_features.features;
    cpu->midr = arm_host_cpu_features.midr;
    cpu->reset_sctlr = arm_host_cpu_features.reset_sctlr;
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
    cpus_kick_thread(cpu);
    hv_vcpus_exit(&cpu->hvf->fd, 1);
}

static void hvf_raise_exception(CPUARMState *env, uint32_t excp,
                                uint32_t syndrome)
{
    unsigned int new_el = 1;
    unsigned int old_mode = pstate_read(env);
    unsigned int new_mode = aarch64_pstate_mode(new_el, true);
    target_ulong addr = env->cp15.vbar_el[new_el];

    env->cp15.esr_el[new_el] = syndrome;
    aarch64_save_sp(env, arm_current_el(env));
    env->elr_el[new_el] = env->pc;
    env->banked_spsr[aarch64_banked_spsr_index(new_el)] = old_mode;
    pstate_write(env, PSTATE_DAIF | new_mode);
    aarch64_restore_sp(env, new_el);
    env->pc = addr;
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
        trace_hvf_vgic_read(ri->name, val);
    }

    return val;
}

static int hvf_psci_cpu_off(ARMCPU *arm_cpu)
{
    int32_t ret = 0;
    ret = arm_set_cpu_off(arm_cpu->mp_affinity);
    assert(ret == QEMU_ARM_POWERCTL_RET_SUCCESS);

    return 0;
}

static int hvf_handle_psci_call(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    uint64_t param[4] = {
        env->xregs[0],
        env->xregs[1],
        env->xregs[2],
        env->xregs[3]
    };
    uint64_t context_id, mpidr;
    bool target_aarch64 = true;
    CPUState *target_cpu_state;
    ARMCPU *target_cpu;
    target_ulong entry;
    int target_el = 1;
    int32_t ret = 0;

    trace_hvf_psci_call(param[0], param[1], param[2], param[3],
                        arm_cpu->mp_affinity);

    switch (param[0]) {
    case QEMU_PSCI_0_2_FN_PSCI_VERSION:
        ret = QEMU_PSCI_0_2_RET_VERSION_0_2;
        break;
    case QEMU_PSCI_0_2_FN_MIGRATE_INFO_TYPE:
        ret = QEMU_PSCI_0_2_RET_TOS_MIGRATION_NOT_REQUIRED; /* No trusted OS */
        break;
    case QEMU_PSCI_0_2_FN_AFFINITY_INFO:
    case QEMU_PSCI_0_2_FN64_AFFINITY_INFO:
        mpidr = param[1];

        switch (param[2]) {
        case 0:
            target_cpu_state = arm_get_cpu_by_id(mpidr);
            if (!target_cpu_state) {
                ret = QEMU_PSCI_RET_INVALID_PARAMS;
                break;
            }
            target_cpu = ARM_CPU(target_cpu_state);

            ret = target_cpu->power_state;
            break;
        default:
            /* Everything above affinity level 0 is always on. */
            ret = 0;
        }
        break;
    case QEMU_PSCI_0_2_FN_SYSTEM_RESET:
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        /* QEMU reset and shutdown are async requests, but PSCI
         * mandates that we never return from the reset/shutdown
         * call, so power the CPU off now so it doesn't execute
         * anything further.
         */
        return hvf_psci_cpu_off(arm_cpu);
    case QEMU_PSCI_0_2_FN_SYSTEM_OFF:
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        return hvf_psci_cpu_off(arm_cpu);
    case QEMU_PSCI_0_1_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN64_CPU_ON:
        mpidr = param[1];
        entry = param[2];
        context_id = param[3];
        ret = arm_set_cpu_on(mpidr, entry, context_id,
                             target_el, target_aarch64);
        break;
    case QEMU_PSCI_0_1_FN_CPU_OFF:
    case QEMU_PSCI_0_2_FN_CPU_OFF:
        return hvf_psci_cpu_off(arm_cpu);
    case QEMU_PSCI_0_1_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN64_CPU_SUSPEND:
        /* Affinity levels are not supported in QEMU */
        if (param[1] & 0xfffe0000) {
            ret = QEMU_PSCI_RET_INVALID_PARAMS;
            break;
        }
        /* Powerdown is not supported, we always go into WFI */
        env->xregs[0] = 0;
        hvf_wfi(cpu);
        break;
    case QEMU_PSCI_0_1_FN_MIGRATE:
    case QEMU_PSCI_0_2_FN_MIGRATE:
        ret = QEMU_PSCI_RET_NOT_SUPPORTED;
        break;
    default:
        return 1;
    }

    env->xregs[0] = ret;
    return 0;
}

static uint64_t hvf_sysreg_read(CPUState *cpu, uint32_t reg)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    uint64_t val = 0;

    switch (reg) {
    case SYSREG_CNTPCT_EL0:
        val = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) /
              gt_cntfrq_period_ns(arm_cpu);
        break;
    case SYSREG_PMCCNTR_EL0:
        val = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;
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
        cpu_synchronize_state(cpu);
        trace_hvf_unhandled_sysreg_read(reg,
                                        arm_cpu->env.pc,
                                        (reg >> 20) & 0x3,
                                        (reg >> 14) & 0x7,
                                        (reg >> 10) & 0xf,
                                        (reg >> 1) & 0xf,
                                        (reg >> 17) & 0x7);
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
        trace_hvf_vgic_write(ri->name, val);
    }
}

static void hvf_sysreg_write(CPUState *cpu, uint32_t reg, uint64_t val)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);

    switch (reg) {
    case SYSREG_CNTPCT_EL0:
    case SYSREG_CNTP_CTL_EL0:
    case SYSREG_OSLAR_EL1:
        break;
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
        hvf_sysreg_write_cp(cpu, reg, val);
        qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], 0);
        hv_vcpu_set_vtimer_mask(cpu->hvf->fd, false);
        break;
    default:
        cpu_synchronize_state(cpu);
        trace_hvf_unhandled_sysreg_write(reg,
                                         arm_cpu->env.pc,
                                         (reg >> 20) & 0x3,
                                         (reg >> 14) & 0x7,
                                         (reg >> 10) & 0xf,
                                         (reg >> 1) & 0xf,
                                         (reg >> 17) & 0x7);
        break;
    }
}

static int hvf_inject_interrupts(CPUState *cpu)
{
    if (cpu->interrupt_request & CPU_INTERRUPT_FIQ) {
        trace_hvf_inject_fiq();
        hv_vcpu_set_pending_interrupt(cpu->hvf->fd, HV_INTERRUPT_TYPE_FIQ,
                                      true);
    }

    if (cpu->interrupt_request & CPU_INTERRUPT_HARD) {
        trace_hvf_inject_irq();
        hv_vcpu_set_pending_interrupt(cpu->hvf->fd, HV_INTERRUPT_TYPE_IRQ,
                                      true);
    }

    return 0;
}

static void hvf_wait_for_ipi(CPUState *cpu, struct timespec *ts)
{
    /*
     * Use pselect to sleep so that other threads can IPI us while we're
     * sleeping.
     */
    qatomic_mb_set(&cpu->thread_kicked, false);
    qemu_mutex_unlock_iothread();
    pselect(0, 0, 0, 0, ts, &cpu->hvf->unblock_ipi_mask);
    qemu_mutex_lock_iothread();
}

static void hvf_wfi(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    hv_return_t r;
    uint64_t ctl;

    printf("%s\n", __func__);

    if (cpu->interrupt_request & (CPU_INTERRUPT_HARD | CPU_INTERRUPT_FIQ)) {
        /* Interrupt pending, no need to wait */
        return;
    }

    r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_CNTV_CTL_EL0,
                            &ctl);
    assert_hvf_ok(r);

    if (!(ctl & 1) || (ctl & 2)) {
        /* Timer disabled or masked, just wait for an IPI. */
        hvf_wait_for_ipi(cpu, NULL);
        return;
    }

    uint64_t cval;
    r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_CNTV_CVAL_EL0,
                            &cval);
    assert_hvf_ok(r);

    int64_t ticks_to_sleep = cval - mach_absolute_time();
    if (ticks_to_sleep < 0) {
        return;
    }

    uint64_t seconds = ticks_to_sleep / arm_cpu->gt_cntfrq_hz;
    uint64_t nanos =
        (ticks_to_sleep - arm_cpu->gt_cntfrq_hz * seconds) *
        1000000000 / arm_cpu->gt_cntfrq_hz;

    /*
     * Don't sleep for less than the time a context switch would take,
     * so that we can satisfy fast timer requests on the same CPU.
     * Measurements on M1 show the sweet spot to be ~2ms.
     */
    if (!seconds && nanos < 2000000) {
        return;
    }

    struct timespec ts = { seconds, nanos };
    hvf_wait_for_ipi(cpu, &ts);
}

static void hvf_sync_vtimer(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    hv_return_t r;
    uint64_t ctl;
    bool irq_state;

    if (!cpu->hvf->vtimer_masked) {
        /* We will get notified on vtimer changes by hvf, nothing to do */
        return;
    }

    r = hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_CNTV_CTL_EL0, &ctl);
    assert_hvf_ok(r);

    irq_state = (ctl & (TMR_CTL_ENABLE | TMR_CTL_IMASK | TMR_CTL_ISTATUS)) ==
                (TMR_CTL_ENABLE | TMR_CTL_ISTATUS);
    qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], irq_state);

    if (!irq_state) {
        /* Timer no longer asserting, we can unmask it */
        hv_vcpu_set_vtimer_mask(cpu->hvf->fd, false);
        cpu->hvf->vtimer_masked = false;
    }
}

static void hvf_invoke_set_guest_debug(CPUState *cpu, run_on_cpu_data data);

static uint64_t exit_timestamp = 0;

int hvf_vcpu_exec(CPUState *cpu)
{
    ARMCPU *arm_cpu = ARM_CPU(cpu);
    CPUARMState *env = &arm_cpu->env;
    hv_vcpu_exit_t *hvf_exit = cpu->hvf->exit;
    hv_return_t r;
    bool advance_pc = false;
    int res = 0;

    flush_cpu_state(cpu);

    hvf_sync_vtimer(cpu);

    if (hvf_inject_interrupts(cpu)) {
        return EXCP_INTERRUPT;
    }

    if (cpu->halted) {
        /* On unhalt, we usually have CPU state changes. Prepare for them. */
        cpu_synchronize_state(cpu);
        return EXCP_HLT;
    }

    hvf_invoke_set_guest_debug(cpu, RUN_ON_CPU_NULL);

    if (exit_timestamp != 0) {
        uint64_t time_passed = mach_absolute_time() - exit_timestamp;
        uint64_t vtimer_offset;

        // adjust vtimer offset for the ticks we spent on host
        assert_hvf_ok(hv_vcpu_get_vtimer_offset(cpu->hvf->fd, &vtimer_offset));
        vtimer_offset += time_passed;
        assert_hvf_ok(hv_vcpu_set_vtimer_offset(cpu->hvf->fd, vtimer_offset));
    }


    qemu_mutex_unlock_iothread();
    assert_hvf_ok(hv_vcpu_run(cpu->hvf->fd));

    exit_timestamp = mach_absolute_time();
    /* handle VMEXIT */
    uint64_t exit_reason = hvf_exit->reason;
    uint64_t syndrome = hvf_exit->exception.syndrome;
    uint32_t ec = syn_get_ec(syndrome);

    qemu_mutex_lock_iothread();
    switch (exit_reason) {
    case HV_EXIT_REASON_EXCEPTION:
        /* This is the main one, handle below. */
        break;
    case HV_EXIT_REASON_VTIMER_ACTIVATED:
        qemu_set_irq(arm_cpu->gt_timer_outputs[GTIMER_VIRT], 1);
        cpu->hvf->vtimer_masked = true;
        return 0;
    case HV_EXIT_REASON_CANCELED:
        /* we got kicked, no exit to process */
        return 0;
    default:
        assert(0);
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

        trace_hvf_data_abort(env->pc, hvf_exit->exception.virtual_address,
                             hvf_exit->exception.physical_address, isv,
                             iswrite, s1ptw, len, srt);

        assert(isv);

        if (iswrite) {
            val = hvf_get_reg(cpu, srt);
            address_space_write(&address_space_memory,
                                hvf_exit->exception.physical_address,
                                MEMTXATTRS_UNSPECIFIED, &val, len);
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

        if (isread) {
            val = hvf_sysreg_read(cpu, reg);
            trace_hvf_sysreg_read(reg,
                                  (reg >> 20) & 0x3,
                                  (reg >> 14) & 0x7,
                                  (reg >> 10) & 0xf,
                                  (reg >> 1) & 0xf,
                                  (reg >> 17) & 0x7,
                                  val);
            hvf_set_reg(cpu, rt, val);
        } else {
            val = hvf_get_reg(cpu, rt);
            trace_hvf_sysreg_write(reg,
                                   (reg >> 20) & 0x3,
                                   (reg >> 14) & 0x7,
                                   (reg >> 10) & 0xf,
                                   (reg >> 1) & 0xf,
                                   (reg >> 17) & 0x7,
                                   val);
            hvf_sysreg_write(cpu, reg, val);
        }

        advance_pc = true;
        break;
    }
    case EC_WFX_TRAP:
        advance_pc = true;
        if (!(syndrome & WFX_IS_WFE)) {
            hvf_wfi(cpu);
        }
        break;
    case EC_AA64_HVC:
        cpu_synchronize_state(cpu);
        if (hvf_handle_psci_call(cpu)) {
            if ((env->xregs[0] & 0xC1000000) == 0xC1000000) {
                // CPU service call
                uint32_t function_num = env->xregs[0] & 0xFFFF;

                switch (function_num) {
                    case 0:
                        // this is called right after vbar_el1 is set
                        // not sure why, no return value is expected
                        break;
                    case 1:
                        // get rop and jop pid
                        env->xregs[2] = 0;
                        env->xregs[3] = 0;
                        break;
                    case 3:
                        // this is called as part of machine_switch_context, maybe to inform hyp of new rop key
                        // x1 contains a rop key
                        // no return value seems to be expected
                        break;
                    case 5:
                        // called with
                        //  x1 = 0 or 1
                        //  x2 = <jop pid>
                        break;
                    default:
                    printf("%s: unhandled CPU service call #%u pc=0x%llx\n", __func__, function_num, env->pc);
                }
            } else {
                trace_hvf_unknown_hvf(env->xregs[0]);
                hvf_raise_exception(env, EXCP_UDEF, syn_uncategorized());
            }
        }
        break;
    case EC_AA64_SMC:
        cpu_synchronize_state(cpu);
        if (!hvf_handle_psci_call(cpu)) {
            advance_pc = true;
        } else if (env->xregs[0] == QEMU_SMCCC_TC_WINDOWS10_BOOT) {
            /* This special SMC is called by Windows 10 on boot. Return error */
            env->xregs[0] = -1;
            advance_pc = true;
        } else {
            trace_hvf_unknown_smc(env->xregs[0]);
            hvf_raise_exception(env, EXCP_UDEF, syn_uncategorized());
        }
        break;
    case EC_SOFTWARESTEP:
        cpu_synchronize_state(cpu);
        printf("exit: EC_SOFTWARESTEP [pc=0x%llx]\n", env->pc);
        res = EXCP_DEBUG;
        break;
    case EC_AA64_BKPT:
        cpu_synchronize_state(cpu);
        printf("exit: EC_AA64_BKPT [pc=0x%llx]", env->pc);

        res = EXCP_DEBUG;
        break;
    default:
        cpu_synchronize_state(cpu);
        trace_hvf_exit(syndrome, ec, env->pc);
        error_report("0x%llx: unhandled exit 0x%llx", env->pc, exit_reason);
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

    return res;
}

static void hvf_invoke_set_guest_debug(CPUState *cpu, run_on_cpu_data data)
{
    //printf("%s cpu->singlestep_enabled=%d hvf->enable_debug=%d tid=%p\n", __func__, cpu->singlestep_enabled, cpu->hvf->enable_debug,  pthread_self());
    uint64_t mdscr;

    cpu->hvf->enable_debug = cpu->singlestep_enabled || cpu->hvf->enable_debug;

    assert_hvf_ok(hv_vcpu_set_trap_debug_exceptions(cpu->hvf->fd, cpu->hvf->enable_debug));

    // set mdscr_el1.ss
    assert_hvf_ok(hv_vcpu_get_sys_reg(cpu->hvf->fd, HV_SYS_REG_MDSCR_EL1, &mdscr));

    if (cpu->singlestep_enabled) {
        mdscr |= (1 << 0);
    } else {
        mdscr &= ~(1ULL << 0);
    }

    assert_hvf_ok(hv_vcpu_set_sys_reg(cpu->hvf->fd, HV_SYS_REG_MDSCR_EL1, mdscr));

    // set cpsr.ss
    uint64_t cpsr;
    assert_hvf_ok(hv_vcpu_get_reg(cpu->hvf->fd, HV_REG_CPSR, &cpsr));
    if (cpu->singlestep_enabled) {
        cpsr |= (PSTATE_SS);
    } else {
        cpsr &= ~(PSTATE_SS);
    }
    assert_hvf_ok(hv_vcpu_set_reg(cpu->hvf->fd, HV_REG_CPSR, cpsr));
}

void hvf_arch_update_guest_debug(CPUState *cpu)
{
    run_on_cpu(cpu, hvf_invoke_set_guest_debug, RUN_ON_CPU_NULL);

}
