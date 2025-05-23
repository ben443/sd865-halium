/*
 * Contains CPU specific errata definitions
 *
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/arm-smccc.h>
#include <linux/psci.h>
#include <linux/types.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/cpufeature.h>

static bool __maybe_unused
is_affected_midr_range(const struct arm64_cpu_capabilities *entry, int scope)
{
	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	return MIDR_IS_CPU_MODEL_RANGE(read_cpuid_id(), entry->midr_model,
				       entry->midr_range_min,
				       entry->midr_range_max);
}

static bool __maybe_unused
is_kryo_midr(const struct arm64_cpu_capabilities *entry, int scope)
{
	u32 model;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	model = read_cpuid_id();
	model &= MIDR_IMPLEMENTOR_MASK | (0xf00 << MIDR_PARTNUM_SHIFT) |
		 MIDR_ARCHITECTURE_MASK;

	return model == entry->midr_model;
}

static bool
has_mismatched_cache_type(const struct arm64_cpu_capabilities *entry,
			  int scope)
{
	u64 mask = CTR_CACHE_MINLINE_MASK;

	/* Skip matching the min line sizes for cache type check */
	if (entry->capability == ARM64_MISMATCHED_CACHE_TYPE)
		mask ^= arm64_ftr_reg_ctrel0.strict_mask;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	return (read_cpuid_cachetype() & mask) !=
	       (arm64_ftr_reg_ctrel0.sys_val & mask);
}

static void
cpu_enable_trap_ctr_access(const struct arm64_cpu_capabilities *__unused)
{
	/* Clear SCTLR_EL1.UCT */
	config_sctlr_el1(SCTLR_EL1_UCT, 0);
}

#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>

DEFINE_PER_CPU_READ_MOSTLY(struct bp_hardening_data, bp_hardening_data);

#ifdef CONFIG_KVM
extern char __smccc_workaround_1_smc_start[];
extern char __smccc_workaround_1_smc_end[];

static void __copy_hyp_vect_bpi(int slot, const char *hyp_vecs_start,
				const char *hyp_vecs_end)
{
	void *dst = lm_alias(__bp_harden_hyp_vecs_start + slot * SZ_2K);
	int i;

	for (i = 0; i < SZ_2K; i += 0x80)
		memcpy(dst + i, hyp_vecs_start, hyp_vecs_end - hyp_vecs_start);

	flush_icache_range((uintptr_t)dst, (uintptr_t)dst + SZ_2K);
}

static void __install_bp_hardening_cb(bp_hardening_cb_t fn,
				      const char *hyp_vecs_start,
				      const char *hyp_vecs_end)
{
	static int last_slot = -1;
	static DEFINE_SPINLOCK(bp_lock);
	int cpu, slot = -1;

	spin_lock(&bp_lock);
	for_each_possible_cpu(cpu) {
		if (per_cpu(bp_hardening_data.fn, cpu) == fn) {
			slot = per_cpu(bp_hardening_data.hyp_vectors_slot, cpu);
			break;
		}
	}

	if (slot == -1) {
		last_slot++;
		BUG_ON(((__bp_harden_hyp_vecs_end - __bp_harden_hyp_vecs_start)
			/ SZ_2K) <= last_slot);
		slot = last_slot;
		__copy_hyp_vect_bpi(slot, hyp_vecs_start, hyp_vecs_end);
	}

	__this_cpu_write(bp_hardening_data.hyp_vectors_slot, slot);
	__this_cpu_write(bp_hardening_data.fn, fn);
	spin_unlock(&bp_lock);
}
#else
#define __qcom_hyp_sanitize_link_stack_start	NULL
#define __qcom_hyp_sanitize_link_stack_end	NULL
#define __smccc_workaround_1_smc_start		NULL
#define __smccc_workaround_1_smc_end		NULL
#define __smccc_workaround_1_hvc_start		NULL
#define __smccc_workaround_1_hvc_end		NULL

static void __install_bp_hardening_cb(bp_hardening_cb_t fn,
				      const char *hyp_vecs_start,
				      const char *hyp_vecs_end)
{
	__this_cpu_write(bp_hardening_data.fn, fn);
}
#endif	/* CONFIG_KVM */

static void  install_bp_hardening_cb(const struct arm64_cpu_capabilities *entry,
				     bp_hardening_cb_t fn,
				     const char *hyp_vecs_start,
				     const char *hyp_vecs_end)
{
	u64 pfr0;

	if (!entry->matches(entry, SCOPE_LOCAL_CPU))
		return;

	pfr0 = read_cpuid(ID_AA64PFR0_EL1);
	if (cpuid_feature_extract_unsigned_field(pfr0, ID_AA64PFR0_CSV2_SHIFT))
		return;

	__install_bp_hardening_cb(fn, hyp_vecs_start, hyp_vecs_end);
}

#include <uapi/linux/psci.h>

static void call_smc_arch_workaround_1(void)
{
	arm_smccc_1_1_smc(ARM_SMCCC_ARCH_WORKAROUND_1, NULL);
}

static void call_hvc_arch_workaround_1(void)
{
	arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_WORKAROUND_1, NULL);
}

static void qcom_link_stack_sanitization(void)
{
	u64 tmp;

	asm volatile("mov	%0, x30		\n"
		     ".rept	16		\n"
		     "bl	. + 4		\n"
		     ".endr			\n"
		     "mov	x30, %0		\n"
		     : "=&r" (tmp));
}

static void
enable_smccc_arch_workaround_1(const struct arm64_cpu_capabilities *entry)
{
	bp_hardening_cb_t cb;
	void *smccc_start, *smccc_end;
	struct arm_smccc_res res;
	u32 midr = read_cpuid_id();

	if (!entry->matches(entry, SCOPE_LOCAL_CPU))
		return;

	if (psci_ops.smccc_version == SMCCC_VERSION_1_0)
		return;

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_1, &res);
		if ((int)res.a0 < 0)
			return;
		cb = call_hvc_arch_workaround_1;
		/* This is a guest, no need to patch KVM vectors */
		smccc_start = NULL;
		smccc_end = NULL;
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_1, &res);
		if ((int)res.a0 < 0)
			return;
		cb = call_smc_arch_workaround_1;
		smccc_start = __smccc_workaround_1_smc_start;
		smccc_end = __smccc_workaround_1_smc_end;
		break;

	default:
		return;
	}

	if (((midr & MIDR_CPU_MODEL_MASK) == MIDR_QCOM_FALKOR) ||
	    ((midr & MIDR_CPU_MODEL_MASK) == MIDR_QCOM_FALKOR_V1))
		cb = qcom_link_stack_sanitization;

	install_bp_hardening_cb(entry, cb, smccc_start, smccc_end);

	return;
}
#endif	/* CONFIG_HARDEN_BRANCH_PREDICTOR */

#ifdef CONFIG_ARM64_SSBD
DEFINE_PER_CPU_READ_MOSTLY(u64, arm64_ssbd_callback_required);

int ssbd_state __read_mostly = ARM64_SSBD_KERNEL;

static const struct ssbd_options {
	const char	*str;
	int		state;
} ssbd_options[] = {
	{ "force-on",	ARM64_SSBD_FORCE_ENABLE, },
	{ "force-off",	ARM64_SSBD_FORCE_DISABLE, },
	{ "kernel",	ARM64_SSBD_KERNEL, },
};

static int __init ssbd_cfg(char *buf)
{
	int i;

	if (!buf || !buf[0])
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ssbd_options); i++) {
		int len = strlen(ssbd_options[i].str);

		if (strncmp(buf, ssbd_options[i].str, len))
			continue;

		ssbd_state = ssbd_options[i].state;
		return 0;
	}

	return -EINVAL;
}
early_param("ssbd", ssbd_cfg);

void arm64_set_ssbd_mitigation(bool state)
{
	if (this_cpu_has_cap(ARM64_SSBS)) {
		if (state)
			asm volatile(SET_PSTATE_SSBS(0));
		else
			asm volatile(SET_PSTATE_SSBS(1));
		return;
	}

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_WORKAROUND_2, state, NULL);
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_WORKAROUND_2, state, NULL);
		break;

	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static bool has_ssbd_mitigation(const struct arm64_cpu_capabilities *entry,
				    int scope)
{
	struct arm_smccc_res res;
	bool required = true;
	s32 val;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	if (this_cpu_has_cap(ARM64_SSBS)) {
		required = false;
		goto out_printmsg;
	}

	if (psci_ops.smccc_version == SMCCC_VERSION_1_0) {
		ssbd_state = ARM64_SSBD_UNKNOWN;
		return false;
	}

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_2, &res);
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_2, &res);
		break;

	default:
		ssbd_state = ARM64_SSBD_UNKNOWN;
		return false;
	}

	val = (s32)res.a0;

	switch (val) {
	case SMCCC_RET_NOT_SUPPORTED:
		ssbd_state = ARM64_SSBD_UNKNOWN;
		return false;

	case SMCCC_RET_NOT_REQUIRED:
		pr_info_once("%s mitigation not required\n", entry->desc);
		ssbd_state = ARM64_SSBD_MITIGATED;
		return false;

	case SMCCC_RET_SUCCESS:
		required = true;
		break;

	case 1:	/* Mitigation not required on this CPU */
		required = false;
		break;

	default:
		WARN_ON(1);
		return false;
	}

	switch (ssbd_state) {
	case ARM64_SSBD_FORCE_DISABLE:
		arm64_set_ssbd_mitigation(false);
		required = false;
		break;

	case ARM64_SSBD_KERNEL:
		if (required) {
			__this_cpu_write(arm64_ssbd_callback_required, 1);
			arm64_set_ssbd_mitigation(true);
		}
		break;

	case ARM64_SSBD_FORCE_ENABLE:
		arm64_set_ssbd_mitigation(true);
		required = true;
		break;

	default:
		WARN_ON(1);
		break;
	}

out_printmsg:
	switch (ssbd_state) {
	case ARM64_SSBD_FORCE_DISABLE:
		pr_info_once("%s disabled from command-line\n", entry->desc);
		break;

	case ARM64_SSBD_FORCE_ENABLE:
		pr_info_once("%s forced from command-line\n", entry->desc);
		break;
	}

	return required;
}
#endif	/* CONFIG_ARM64_SSBD */

#define MIDR_RANGE(model, min, max) \
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM, \
	.matches = is_affected_midr_range, \
	.midr_model = model, \
	.midr_range_min = min, \
	.midr_range_max = max

#define MIDR_ALL_VERSIONS(model) \
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM, \
	.matches = is_affected_midr_range, \
	.midr_model = model, \
	.midr_range_min = 0, \
	.midr_range_max = (MIDR_VARIANT_MASK | MIDR_REVISION_MASK)

const struct arm64_cpu_capabilities arm64_errata[] = {
#if	defined(CONFIG_ARM64_ERRATUM_826319) || \
	defined(CONFIG_ARM64_ERRATUM_827319) || \
	defined(CONFIG_ARM64_ERRATUM_824069)
	{
	/* Cortex-A53 r0p[012] */
		.desc = "ARM errata 826319, 827319, 824069",
		.capability = ARM64_WORKAROUND_CLEAN_CACHE,
		MIDR_RANGE(MIDR_CORTEX_A53, 0x00, 0x02),
		.cpu_enable = cpu_enable_cache_maint_trap,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_819472
	{
	/* Cortex-A53 r0p[01] */
		.desc = "ARM errata 819472",
		.capability = ARM64_WORKAROUND_CLEAN_CACHE,
		MIDR_RANGE(MIDR_CORTEX_A53, 0x00, 0x01),
		.cpu_enable = cpu_enable_cache_maint_trap,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_832075
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 832075",
		.capability = ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE,
		MIDR_RANGE(MIDR_CORTEX_A57,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(1, 2)),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_834220
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 834220",
		.capability = ARM64_WORKAROUND_834220,
		MIDR_RANGE(MIDR_CORTEX_A57,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(1, 2)),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_845719
	{
	/* Cortex-A53 r0p[01234] */
		.desc = "ARM erratum 845719",
		.capability = ARM64_WORKAROUND_845719,
		MIDR_RANGE(MIDR_CORTEX_A53, 0x00, 0x04),
	},
	{
	/* Kryo2xx Silver rAp4 */
		.desc = "Kryo2xx Silver erratum 845719",
		.capability = ARM64_WORKAROUND_845719,
		MIDR_RANGE(MIDR_KRYO2XX_SILVER, 0xA00004, 0xA00004),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_23154
	{
	/* Cavium ThunderX, pass 1.x */
		.desc = "Cavium erratum 23154",
		.capability = ARM64_WORKAROUND_CAVIUM_23154,
		MIDR_RANGE(MIDR_THUNDERX, 0x00, 0x01),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_27456
	{
	/* Cavium ThunderX, T88 pass 1.x - 2.1 */
		.desc = "Cavium erratum 27456",
		.capability = ARM64_WORKAROUND_CAVIUM_27456,
		MIDR_RANGE(MIDR_THUNDERX,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(1, 1)),
	},
	{
	/* Cavium ThunderX, T81 pass 1.0 */
		.desc = "Cavium erratum 27456",
		.capability = ARM64_WORKAROUND_CAVIUM_27456,
		MIDR_RANGE(MIDR_THUNDERX_81XX, 0x00, 0x00),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_30115
	{
	/* Cavium ThunderX, T88 pass 1.x - 2.2 */
		.desc = "Cavium erratum 30115",
		.capability = ARM64_WORKAROUND_CAVIUM_30115,
		MIDR_RANGE(MIDR_THUNDERX, 0x00,
			   (1 << MIDR_VARIANT_SHIFT) | 2),
	},
	{
	/* Cavium ThunderX, T81 pass 1.0 - 1.2 */
		.desc = "Cavium erratum 30115",
		.capability = ARM64_WORKAROUND_CAVIUM_30115,
		MIDR_RANGE(MIDR_THUNDERX_81XX, 0x00, 0x02),
	},
	{
	/* Cavium ThunderX, T83 pass 1.0 */
		.desc = "Cavium erratum 30115",
		.capability = ARM64_WORKAROUND_CAVIUM_30115,
		MIDR_RANGE(MIDR_THUNDERX_83XX, 0x00, 0x00),
	},
#endif
	{
		.desc = "Mismatched cache line size",
		.capability = ARM64_MISMATCHED_CACHE_LINE_SIZE,
		.matches = has_mismatched_cache_type,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.cpu_enable = cpu_enable_trap_ctr_access,
	},
	{
		.desc = "Mismatched cache type",
		.capability = ARM64_MISMATCHED_CACHE_TYPE,
		.matches = has_mismatched_cache_type,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.cpu_enable = cpu_enable_trap_ctr_access,
	},
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1003
	{
		.desc = "Qualcomm Technologies Falkor erratum 1003",
		.capability = ARM64_WORKAROUND_QCOM_FALKOR_E1003,
		MIDR_RANGE(MIDR_QCOM_FALKOR_V1,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(0, 0)),
	},
	{
		.desc = "Qualcomm Technologies Kryo erratum 1003",
		.capability = ARM64_WORKAROUND_QCOM_FALKOR_E1003,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.midr_model = MIDR_QCOM_KRYO,
		.matches = is_kryo_midr,
	},
#endif
#ifdef CONFIG_QCOM_FALKOR_ERRATUM_1009
	{
		.desc = "Qualcomm Technologies Falkor erratum 1009",
		.capability = ARM64_WORKAROUND_REPEAT_TLBI,
		MIDR_RANGE(MIDR_QCOM_FALKOR_V1,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(0, 0)),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_1286807
	{
	/* Cortex-A76 r0p0 to r3p0 */
		.desc = "ARM erratum 1286807",
		.capability = ARM64_WORKAROUND_REPEAT_TLBI,
		MIDR_RANGE(MIDR_CORTEX_A76,
			   MIDR_CPU_VAR_REV(0, 0),
			   MIDR_CPU_VAR_REV(3, 0)),
	},
	{
		.capability = ARM64_WORKAROUND_REPEAT_TLBI,
		MIDR_RANGE(MIDR_KRYO4G,
			   MIDR_CPU_VAR_REV(12, 14),
			   MIDR_CPU_VAR_REV(13, 14)),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_858921
	{
	/* Cortex-A73 all versions */
		.desc = "ARM erratum 858921",
		.capability = ARM64_WORKAROUND_858921,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A73),
	},
	{
	/* KRYO2XX all versions */
		.desc = "ARM erratum 858921",
		.capability = ARM64_WORKAROUND_858921,
		MIDR_ALL_VERSIONS(MIDR_KRYO2XX_GOLD),
	},
#endif
#ifdef CONFIG_HARDEN_BRANCH_PREDICTOR
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A57),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A72),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A73),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CORTEX_A75),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_BRCM_VULCAN),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_CAVIUM_THUNDERX2),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_RANGE(MIDR_KRYO4G,
			   MIDR_CPU_VAR_REV(12, 14),
			   MIDR_CPU_VAR_REV(13, 14)),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		MIDR_ALL_VERSIONS(MIDR_KRYO2XX_GOLD),
		.cpu_enable = enable_smccc_arch_workaround_1,
	},
#endif
#ifdef CONFIG_ARM64_SSBD
	{
		.desc = "Speculative Store Bypass Disable",
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.capability = ARM64_SSBD,
		.matches = has_ssbd_mitigation,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_1188873
	{
		.desc = "ARM erratum 1188873",
		.capability = ARM64_WORKAROUND_1188873,
		/* Cortex-A76 r0p0 to r2p0 */
		MIDR_RANGE(MIDR_CORTEX_A76,
			MIDR_CPU_VAR_REV(0, 0),
			MIDR_CPU_VAR_REV(2, 0)),

	},
	{
		.desc = "ARM erratum 1188873",
		.capability = ARM64_WORKAROUND_1188873,
		/* Kryo-4G r15p14 */
		MIDR_RANGE(MIDR_KRYO4G,
			MIDR_CPU_VAR_REV(15, 14),
			MIDR_CPU_VAR_REV(15, 15)),
	},
#endif
	{
	}
};

