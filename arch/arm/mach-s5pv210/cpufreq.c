/* linux/arch/arm/mach-s5pv210/cpufreq.c
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * CPU frequency scaling for S5PC110/S5PV210
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/suspend.h>
#include <linux/reboot.h>
#include <linux/regulator/consumer.h>
#include <linux/cpufreq.h>
#include <linux/platform_device.h>

#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/cpu-freq-v210.h>

static struct clk *cpu_clk;
static struct clk *dmc0_clk;
static struct clk *dmc1_clk;
static struct cpufreq_freqs freqs;
static DEFINE_MUTEX(set_freq_lock);

/* APLL M,P,S values for 1.2G/1.0G/800Mhz */
#define APLL_VAL_1200	((1 << 31) | (150 << 16) | (3 << 8) | 1)
#define APLL_VAL_1000	((1 << 31) | (125 << 16) | (3 << 8) | 1)
#define APLL_VAL_800	((1 << 31) | (100 << 16) | (3 << 8) | 1)

#define SLEEP_FREQ	(800 * 1000) /* Use 800MHz when entering sleep */

/*
 * relation has an additional symantics other than the standard of cpufreq
 *	DISALBE_FURTHER_CPUFREQ: disable further access to target until being re-enabled.
 *	ENABLE_FURTUER_CPUFREQ: re-enable access to target
*/
enum cpufreq_access {
	DISABLE_FURTHER_CPUFREQ = 0x10,
	ENABLE_FURTHER_CPUFREQ = 0x20,
};
static bool no_cpufreq_access;

/*
 * DRAM configurations to calculate refresh counter for changing
 * frequency of memory.
 */
struct dram_conf {
	unsigned long freq;	/* HZ */
	unsigned long refresh;	/* DRAM refresh counter * 1000 */
};

/* DRAM configuration (DMC0 and DMC1) */
static struct dram_conf s5pv210_dram_conf[2];

enum s5pv210_mem_type {
	LPDDR	= 0x1,
	LPDDR2	= 0x2,
	DDR2	= 0x4,
};

enum s5pv210_dmc_port {
	DMC0 = 0,
	DMC1,
};

static struct cpufreq_frequency_table s5pv210_freq_table[] = {
	{OC1, 1200*1000},
	{L0, 1000*1000},
	{L1, 800*1000},
	{L2, 400*1000},
	{L3, 200*1000},
	{0, CPUFREQ_TABLE_END},
};

static struct regulator *arm_regulator;
static struct regulator *internal_regulator;

struct s5pv210_dvs_conf {
	unsigned long	arm_volt; /* uV */
	unsigned long	int_volt; /* uV */
};

#ifdef CONFIG_DVFS_LIMIT
static unsigned int g_dvfs_high_lock_token = 0;
static unsigned int g_dvfs_high_lock_limit = 6;
static unsigned int g_dvfslockval[DVFS_LOCK_TOKEN_NUM];
//static DEFINE_MUTEX(dvfs_high_lock);
#endif

const unsigned long arm_volt_max = 1350000;
const unsigned long int_volt_max = 1250000;

static struct s5pv210_dvs_conf dvs_conf[] = {
	[OC1] = {

		.arm_volt   = 1250000,
		.int_volt   = 1100000,
	},
	[L0] = {
		.arm_volt   = 1225000,
		.int_volt   = 1100000,
	},
	[L1] = {
		.arm_volt   = 1150000,
		.int_volt   = 1100000,
	},
	[L2] = {
		.arm_volt   =  950000,
		.int_volt   = 1100000,
	},
	[L3] = {
		.arm_volt   =  900000,
		.int_volt   = 1100000,
	},
};


static u32 clkdiv_val[7][11] = {
	/*
	 * Clock divider value for following
	 * { APLL, A2M, HCLK_MSYS, PCLK_MSYS,
	 *   HCLK_DSYS, PCLK_DSYS, HCLK_PSYS, PCLK_PSYS,
	 *   ONEDRAM, MFC, G3D }
	 */


	/* OC1 : [1200/200/100][166/83][133/66][200/200] */
	{0, 5, 5, 1, 3, 1, 4, 1, 3, 0, 0},

	/* L0 : [1000/200/100][166/83][133/66][200/200] */
	{0, 4, 4, 1, 3, 1, 4, 1, 3, 0, 0},

	/* L1 : [800/200/100][166/83][133/66][200/200] */
	{0, 3, 3, 1, 3, 1, 4, 1, 3, 0, 0},

	/* L2 : [400/200/100][166/83][133/66][200/200] */
	{1, 3, 1, 1, 3, 1, 4, 1, 3, 0, 0},

	/* L3 : [200/200/100][166/83][133/66][200/200] */
	{3, 3, 0, 1, 3, 1, 4, 1, 3, 0, 0},
};

/*
 * This function set DRAM refresh counter
 * accoriding to operating frequency of DRAM
 * ch: DMC port number 0 or 1
 * freq: Operating frequency of DRAM(KHz)
 */
static void s5pv210_set_refresh(enum s5pv210_dmc_port ch, unsigned long freq)
{
	unsigned long tmp, tmp1;
	void __iomem *reg = NULL;

	if (ch == DMC0) {
		reg = (S5P_VA_DMC0 + 0x30);
	} else if (ch == DMC1) {
		reg = (S5P_VA_DMC1 + 0x30);
	} else {
		printk(KERN_ERR "Cannot find DMC port\n");
		return;
	}

	/* Find current DRAM frequency */
	tmp = s5pv210_dram_conf[ch].freq;

	do_div(tmp, freq);

	tmp1 = s5pv210_dram_conf[ch].refresh;

	do_div(tmp1, tmp);

	__raw_writel(tmp1, reg);
}

int s5pv210_verify_speed(struct cpufreq_policy *policy)
{
	if (policy->cpu)
		return -EINVAL;

	return cpufreq_frequency_table_verify(policy, s5pv210_freq_table);
}

unsigned int s5pv210_getspeed(unsigned int cpu)
{
	if (cpu)
		return 0;

	return clk_get_rate(cpu_clk) / 1000;
}

#ifdef CONFIG_DVFS_LIMIT
void s5pv210_lock_dvfs_high_level(uint nToken, uint perf_level)
{
	//printk(KERN_DEBUG "%s : lock with token(%d) level(%d) current(%X)\n",
	//		__func__, nToken, perf_level, g_dvfs_high_lock_token);

	if (g_dvfs_high_lock_token & (1 << nToken))
		return;

	if (perf_level > (MAX_PERF_LEVEL - 1))
		return;

	//mutex_lock(&dvfs_high_lock);

	g_dvfs_high_lock_token |= (1 << nToken);
	g_dvfslockval[nToken] = perf_level;

	if (perf_level <  g_dvfs_high_lock_limit)
		g_dvfs_high_lock_limit = perf_level;

	//mutex_unlock(&dvfs_high_lock);

	/* Reevaluate cpufreq policy with the effect of calling the governor with a
	 * CPUFREQ_GOV_LIMITS event, so that the governor sets its preferred
	 * frequency.  The governor MUST call __cpufreq_driver_target, even if it
	 * decides not to change frequencies, as the DVFS limit takes effect in
	 * doing so. */
	cpufreq_update_policy(0);
}
EXPORT_SYMBOL(s5pv210_lock_dvfs_high_level);

void s5pv210_unlock_dvfs_high_level(unsigned int nToken)
{
	unsigned int i;

	//mutex_lock(&dvfs_high_lock);

	g_dvfs_high_lock_token &= ~(1 << nToken);
	g_dvfslockval[nToken] = MAX_PERF_LEVEL;
	g_dvfs_high_lock_limit = MAX_PERF_LEVEL;

	if (g_dvfs_high_lock_token) {
		for (i = 0; i < DVFS_LOCK_TOKEN_NUM; i++) {
			if (g_dvfslockval[i] < g_dvfs_high_lock_limit)
				g_dvfs_high_lock_limit = g_dvfslockval[i];
		}
	}

	//mutex_unlock(&dvfs_high_lock);

	//printk(KERN_DEBUG "%s : unlock with token(%d) current(%X) level(%d)\n",
	//		__func__, nToken, g_dvfs_high_lock_token, g_dvfs_high_lock_limit);

	/* Reevaluate cpufreq policy with the effect of calling the governor with a
	 * CPUFREQ_GOV_LIMITS event, so that the governor sets its preferred
	 * frequency with the new (or no) DVFS limit. */
	cpufreq_update_policy(0);
}
EXPORT_SYMBOL(s5pv210_unlock_dvfs_high_level);
#endif

static int s5pv210_target(struct cpufreq_policy *policy,
			  unsigned int target_freq,
			  unsigned int relation)
{
	unsigned long reg;
	unsigned int index;
	unsigned int pll_changing = 0;
	unsigned int bus_speed_changing = 0;
	unsigned int arm_volt, int_volt;
	int ret = 0;

	mutex_lock(&set_freq_lock);

	if (relation & ENABLE_FURTHER_CPUFREQ)
		no_cpufreq_access = false;
	if (no_cpufreq_access) {
#ifdef CONFIG_PM_VERBOSE
		pr_err("%s:%d denied access to %s as it is disabled"
				"temporarily\n", __FILE__, __LINE__, __func__);
#endif
		ret = -EINVAL;
		goto out;
	}
	if (relation & DISABLE_FURTHER_CPUFREQ)
		no_cpufreq_access = true;
	relation &= ~(ENABLE_FURTHER_CPUFREQ | DISABLE_FURTHER_CPUFREQ);

	freqs.old = s5pv210_getspeed(0);

	if (cpufreq_frequency_table_target(policy, s5pv210_freq_table,
					   target_freq, relation, &index)) {
		ret = -EINVAL;
		goto out;
	}

#ifdef CONFIG_DVFS_LIMIT
	if (g_dvfs_high_lock_token) {
		if (index > g_dvfs_high_lock_limit)
			index = g_dvfs_high_lock_limit;
	}
#endif

	freqs.new = s5pv210_freq_table[index].frequency;
	freqs.cpu = 0;

	if (freqs.new == freqs.old)
		goto out;

	arm_volt = dvs_conf[index].arm_volt;
	int_volt = dvs_conf[index].int_volt;

	if (freqs.new > freqs.old) {
		/* Voltage up code: increase ARM first */
		if (!IS_ERR_OR_NULL(arm_regulator) &&
				!IS_ERR_OR_NULL(internal_regulator)) {
			ret = regulator_set_voltage(arm_regulator,
						    arm_volt, arm_volt_max);
			if (ret)
				goto out;
			ret = regulator_set_voltage(internal_regulator,
						    int_volt, int_volt_max);
			if (ret)
				goto out;
		}
	}

	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);

	/* Don't use cpufreq_frequency_table_target() any more as it */
	/* may not be accurate. Compare against freqs.old instead */

	/* Check if there need to change PLL */
	if ((index <= L0) || (freqs.old >= s5pv210_freq_table[L0].frequency))
		pll_changing = 1;

	/*
	 * APLL should be changed in this level
	 * APLL -> MPLL(for stable transition) -> APLL
	 * Some clock source's clock API are not prepared.
	 * Do not use clock API in below code.
	 */
	if (pll_changing) {
		/*
		 * 1. Temporary Change divider for MFC and G3D
		 * SCLKA2M(200/1=200)->(200/4=50)Mhz
		 */
		reg = __raw_readl(S5P_CLK_DIV2);
		reg &= ~(S5P_CLKDIV2_G3D_MASK | S5P_CLKDIV2_MFC_MASK);
		reg |= (3 << S5P_CLKDIV2_G3D_SHIFT) |
			(3 << S5P_CLKDIV2_MFC_SHIFT);
		__raw_writel(reg, S5P_CLK_DIV2);

		/* For MFC, G3D dividing */
		do {
			reg = __raw_readl(S5P_CLKDIV_STAT0);
		} while (reg & ((1 << 16) | (1 << 17)));

		/*
		 * 2. Change SCLKA2M(200Mhz)to SCLKMPLL in MFC_MUX, G3D MUX
		 * (200/4=50)->(667/4=166)Mhz
		 */
		reg = __raw_readl(S5P_CLK_SRC2);
		reg &= ~(S5P_CLKSRC2_G3D_MASK | S5P_CLKSRC2_MFC_MASK);
		reg |= (1 << S5P_CLKSRC2_G3D_SHIFT) |
			(1 << S5P_CLKSRC2_MFC_SHIFT);
		__raw_writel(reg, S5P_CLK_SRC2);

		do {
			reg = __raw_readl(S5P_CLKMUX_STAT1);
		} while (reg & ((1 << 7) | (1 << 3)));

		/*
		 * 3. DMC1 refresh counter
		 */
		s5pv210_set_refresh(DMC1, 133000);

		/* 4. SCLKAPLL -> SCLKMPLL */
		reg = __raw_readl(S5P_CLK_SRC0);
		reg &= ~(S5P_CLKSRC0_MUX200_MASK);
		reg |= (0x1 << S5P_CLKSRC0_MUX200_SHIFT);
		__raw_writel(reg, S5P_CLK_SRC0);

		do {
			reg = __raw_readl(S5P_CLKMUX_STAT0);
		} while (reg & (0x1 << 18));

	}

	/* Change divider */
	reg = __raw_readl(S5P_CLK_DIV0);

	reg &= ~(S5P_CLKDIV0_APLL_MASK | S5P_CLKDIV0_A2M_MASK |
		S5P_CLKDIV0_HCLK200_MASK | S5P_CLKDIV0_PCLK100_MASK |
		S5P_CLKDIV0_HCLK166_MASK | S5P_CLKDIV0_PCLK83_MASK |
		S5P_CLKDIV0_HCLK133_MASK | S5P_CLKDIV0_PCLK66_MASK);

	reg |= ((clkdiv_val[index][0] << S5P_CLKDIV0_APLL_SHIFT) |
		(clkdiv_val[index][1] << S5P_CLKDIV0_A2M_SHIFT) |
		(clkdiv_val[index][2] << S5P_CLKDIV0_HCLK200_SHIFT) |
		(clkdiv_val[index][3] << S5P_CLKDIV0_PCLK100_SHIFT) |
		(clkdiv_val[index][4] << S5P_CLKDIV0_HCLK166_SHIFT) |
		(clkdiv_val[index][5] << S5P_CLKDIV0_PCLK83_SHIFT) |
		(clkdiv_val[index][6] << S5P_CLKDIV0_HCLK133_SHIFT) |
		(clkdiv_val[index][7] << S5P_CLKDIV0_PCLK66_SHIFT));

	__raw_writel(reg, S5P_CLK_DIV0);

	do {
		reg = __raw_readl(S5P_CLKDIV_STAT0);
	} while (reg & 0xff);

	/* ARM MCS value changed */
	reg = __raw_readl(S5P_ARM_MCS_CON);
	reg &= ~0x3;
	if (index >= L3)
		reg |= 0x3;
	else
		reg |= 0x1;

	__raw_writel(reg, S5P_ARM_MCS_CON);

	if (pll_changing) {
		/* 5. Set Lock time = 30us*24Mhz = 0x2cf */
		__raw_writel(0x2cf, S5P_APLL_LOCK);

		/*
		 * 6. Turn on APLL
		 * 6-1. Set PMS values
		 * 6-2. Wait untile the PLL is locked
		 */
		switch (index) {
		case OC1:
			__raw_writel(APLL_VAL_1200, S5P_APLL_CON);
			break;
		case L0:
			__raw_writel(APLL_VAL_1000, S5P_APLL_CON);
			break;
		default:
			__raw_writel(APLL_VAL_800, S5P_APLL_CON);
			break;
		}

		do {
			reg = __raw_readl(S5P_APLL_CON);
		} while (!(reg & (0x1 << 29)));

		/*
		 * 7. Change souce clock from SCLKMPLL(667Mhz)
		 * to SCLKA2M(200Mhz) in MFC_MUX and G3D MUX
		 * (667/4=166)->(200/4=50)Mhz
		 */
		reg = __raw_readl(S5P_CLK_SRC2);
		reg &= ~(S5P_CLKSRC2_G3D_MASK | S5P_CLKSRC2_MFC_MASK);
		reg |= (0 << S5P_CLKSRC2_G3D_SHIFT) |
			(0 << S5P_CLKSRC2_MFC_SHIFT);
		__raw_writel(reg, S5P_CLK_SRC2);

		do {
			reg = __raw_readl(S5P_CLKMUX_STAT1);
		} while (reg & ((1 << 7) | (1 << 3)));

		/*
		 * 8. Change divider for MFC and G3D
		 * (200/4=50)->(200/1=200)Mhz
		 */
		reg = __raw_readl(S5P_CLK_DIV2);
		reg &= ~(S5P_CLKDIV2_G3D_MASK | S5P_CLKDIV2_MFC_MASK);
		reg |= (clkdiv_val[index][10] << S5P_CLKDIV2_G3D_SHIFT) |
			(clkdiv_val[index][9] << S5P_CLKDIV2_MFC_SHIFT);
		__raw_writel(reg, S5P_CLK_DIV2);

		/* For MFC, G3D dividing */
		do {
			reg = __raw_readl(S5P_CLKDIV_STAT0);
		} while (reg & ((1 << 16) | (1 << 17)));

		/* 9. Change MPLL to APLL in MSYS_MUX */
		reg = __raw_readl(S5P_CLK_SRC0);
		reg &= ~(S5P_CLKSRC0_MUX200_MASK);
		reg |= (0x0 << S5P_CLKSRC0_MUX200_SHIFT);
		__raw_writel(reg, S5P_CLK_SRC0);

		do {
			reg = __raw_readl(S5P_CLKMUX_STAT0);
		} while (reg & (0x1 << 18));

		/*
		 * 10. DMC1 refresh counter
		 */
		s5pv210_set_refresh(DMC1, 200000);
	}

	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	if (freqs.new < freqs.old) {
		/* Voltage down: decrease INT first */
		if (!IS_ERR_OR_NULL(arm_regulator) &&
				!IS_ERR_OR_NULL(internal_regulator)) {
			regulator_set_voltage(internal_regulator,
					int_volt, int_volt_max);
			regulator_set_voltage(arm_regulator,
					arm_volt, arm_volt_max);
		}
	}

	pr_debug("Perf changed[L%d]\n", index);
out:
	mutex_unlock(&set_freq_lock);
	return ret;
}

#ifdef CONFIG_PM
static int s5pv210_cpufreq_suspend(struct cpufreq_policy *policy)
{
	return 0;
}

static int s5pv210_cpufreq_resume(struct cpufreq_policy *policy)
{
	return 0;
}
#endif

static int check_mem_type(void __iomem *dmc_reg)
{
	unsigned long val;

	val = __raw_readl(dmc_reg + 0x4);
	val = (val & (0xf << 8));

	return val >> 8;
}

static int __init s5pv210_cpu_init(struct cpufreq_policy *policy)
{
	unsigned long mem_type;
	int ret;

	cpu_clk = clk_get(NULL, "armclk");
	if (IS_ERR(cpu_clk))
		return PTR_ERR(cpu_clk);

	dmc0_clk = clk_get(NULL, "sclk_dmc0");
	if (IS_ERR(dmc0_clk)) {
		clk_put(cpu_clk);
		return PTR_ERR(dmc0_clk);
	}

	dmc1_clk = clk_get(NULL, "hclk_msys");
	if (IS_ERR(dmc1_clk)) {
		clk_put(dmc0_clk);
		clk_put(cpu_clk);
		return PTR_ERR(dmc1_clk);
	}

	if (policy->cpu != 0)
		return -EINVAL;

	/*
	 * check_mem_type : This driver only support LPDDR & LPDDR2.
	 * other memory type is not supported.
	 */
	mem_type = check_mem_type(S5P_VA_DMC0);

	if ((mem_type != LPDDR) && (mem_type != LPDDR2)) {
		printk(KERN_ERR "CPUFreq doesn't support this memory type\n");
		return -EINVAL;
	}

	/* Find current refresh counter and frequency each DMC */
	s5pv210_dram_conf[0].refresh = (__raw_readl(S5P_VA_DMC0 + 0x30) * 1000);
	s5pv210_dram_conf[0].freq = clk_get_rate(dmc0_clk);

	s5pv210_dram_conf[1].refresh = (__raw_readl(S5P_VA_DMC1 + 0x30) * 1000);
	s5pv210_dram_conf[1].freq = clk_get_rate(dmc1_clk);

	policy->cur = policy->min = policy->max = s5pv210_getspeed(0);

	cpufreq_frequency_table_get_attr(s5pv210_freq_table, policy->cpu);

	policy->cpuinfo.transition_latency = 40000;

#ifdef CONFIG_DVFS_LIMIT
	int i;
	for (i = 0; i < DVFS_LOCK_TOKEN_NUM; i++)
		g_dvfslockval[i] = MAX_PERF_LEVEL;
#endif

	/* Set max freq to 1.2GHz on startup */
	ret = cpufreq_frequency_table_cpuinfo(policy, s5pv210_freq_table);
	policy->min = 200000;
	policy->max = 1200000;

	return ret;
}

static int s5pv210_cpufreq_notifier_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	int ret;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		ret = cpufreq_driver_target(cpufreq_cpu_get(0), SLEEP_FREQ,
				DISABLE_FURTHER_CPUFREQ);
		if (ret < 0)
			return NOTIFY_BAD;
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		cpufreq_driver_target(cpufreq_cpu_get(0), SLEEP_FREQ,
				ENABLE_FURTHER_CPUFREQ);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static int s5pv210_cpufreq_reboot_notifier_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	int ret = 0;

	ret = cpufreq_driver_target(cpufreq_cpu_get(0), SLEEP_FREQ,
			DISABLE_FURTHER_CPUFREQ);
	if (ret < 0)
		return NOTIFY_BAD;

	return NOTIFY_DONE;
}

static struct freq_attr *s5pv210_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver s5pv210_driver = {
	.flags		= CPUFREQ_STICKY,
	.verify		= s5pv210_verify_speed,
	.target		= s5pv210_target,
	.get		= s5pv210_getspeed,
	.init		= s5pv210_cpu_init,
	.name		= "s5pv210",
#ifdef CONFIG_PM
	.suspend	= s5pv210_cpufreq_suspend,
	.resume		= s5pv210_cpufreq_resume,
#endif
	.attr		= s5pv210_cpufreq_attr,
};

static struct notifier_block s5pv210_cpufreq_notifier = {
	.notifier_call = s5pv210_cpufreq_notifier_event,
};

static struct notifier_block s5pv210_cpufreq_reboot_notifier = {
	.notifier_call	= s5pv210_cpufreq_reboot_notifier_event,
};

static int __init s5pv210_cpufreq_probe(struct platform_device *pdev)
{
	struct s5pv210_cpufreq_data *pdata = dev_get_platdata(&pdev->dev);
	int i, j;

	if (pdata && pdata->size) {
		for (i = 0; i < pdata->size; i++) {
			j = 0;
			while (s5pv210_freq_table[j].frequency != CPUFREQ_TABLE_END) {
				if (s5pv210_freq_table[j].frequency == pdata->volt[i].freq) {
					dvs_conf[j].arm_volt = pdata->volt[i].varm;
					dvs_conf[j].int_volt = pdata->volt[i].vint;
					break;
				}
				j++;
			}
		}
	}

	arm_regulator = regulator_get(NULL, "vddarm");
	if (IS_ERR(arm_regulator)) {
		pr_err("failed to get regulater resource vddarm\n");
		goto error;
	}
	internal_regulator = regulator_get(NULL, "vddint");
	if (IS_ERR(internal_regulator)) {
		pr_err("failed to get regulater resource vddint\n");
		goto error;
	}
	goto finish;
error:
	pr_warn("Cannot get vddarm or vddint. CPUFREQ Will not"
		       " change the voltage.\n");
finish:
	register_pm_notifier(&s5pv210_cpufreq_notifier);
	register_reboot_notifier(&s5pv210_cpufreq_reboot_notifier);

	return cpufreq_register_driver(&s5pv210_driver);
}

static struct platform_driver s5pv210_cpufreq_drv = {
	.probe		= s5pv210_cpufreq_probe,
	.driver		= {
		.owner	= THIS_MODULE,
		.name	= "s5pv210-cpufreq",
	},
};

static int __init s5pv210_cpufreq_init(void)
{
	int ret;

	ret = platform_driver_register(&s5pv210_cpufreq_drv);
	if (!ret)
		pr_info("%s: S5PV210 cpu-freq driver\n", __func__);

	return ret;
}

late_initcall(s5pv210_cpufreq_init);

ssize_t show_UV_mV_table(struct cpufreq_policy *policy, char *buf)
{
	int i, len = 0;
	for (i = 0; i <= MAX_PERF_LEVEL; i++) {
		len += sprintf(buf + len, "%dmhz: %d mV\n", s5pv210_freq_table[i].frequency / 1000, dvs_conf[i].arm_volt / 1000);
	}
	return len;
}

ssize_t store_UV_mV_table(struct cpufreq_policy *policy,
							const char *buf, size_t count)
{
	int ret = -EINVAL;
	int i = 0;
	int j = 0;
	int u[MAX_PERF_LEVEL + 1];
	while (j < MAX_PERF_LEVEL + 1) {
		int consumed;
		int val;
		ret = sscanf(buf, "%d%n", &val, &consumed);
		if (ret > 0) {
			buf += consumed;
			u[j++] = val;
		}
		else {
			break;
		}
	}

	for (i = 0; i < j; i++) {
		if (u[i] > arm_volt_max / 1000) {
			u[i] = arm_volt_max / 1000;
		}
		dvs_conf[i].arm_volt = u[i] * 1000;
	}

	return count;
}
