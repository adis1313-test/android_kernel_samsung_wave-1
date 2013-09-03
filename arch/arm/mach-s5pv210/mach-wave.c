/* linux/arch/arm/mach-s5pv210/mach-wave.c
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * Copyright (c) 2012 Dominik Marszk <dmarszk@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/serial_core.h>
#include <linux/gpio.h>
#include <linux/gpio_event.h>
#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/i2c-gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/mfd/max8998.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/usb/ch9.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi_gpio.h>
#include <linux/clk.h>
#include <linux/usb/ch9.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/skbuff.h>
#include <linux/console.h>
#include <linux/gpio_keys.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/system.h>

#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/gpio.h>
#include <mach/gpio-wave.h>
#include <mach/adc.h>
#include <mach/param.h>
#include <mach/system.h>
#include <mach/sec_switch.h>

#include <linux/usb/gadget.h>
#include <linux/fsa9480.h>
#include <linux/pn544.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/wlan_plat.h>
#include <linux/mfd/wm8994/wm8994_pdata.h>

#ifdef CONFIG_ANDROID_PMEM
#include <linux/android_pmem.h>
#endif

#include <plat/media.h>
#include <mach/media.h>

#ifdef CONFIG_S5PV210_POWER_DOMAIN
#include <mach/power-domain.h>
#endif
#include <mach/cpu-freq-v210.h>

#include <media/ce147_platform.h>
#include <media/s5ka3dfx_platform.h>

#include <plat/regs-serial.h>
#include <plat/s5pv210.h>
#include <plat/devs.h>
#include <plat/cpu.h>
#include <plat/fb.h>
#include <plat/mfc.h>
#include <plat/iic.h>
#include <plat/pm.h>
#include <plat/s5p-time.h>

#include <plat/keypad.h>
#include <plat/sdhci.h>
#include <plat/fimc.h>
#include <plat/jpeg.h>
#include <plat/clock.h>
#include <plat/regs-otg.h>

#include <linux/gp2a.h>

#include <linux/i2c/ak8973.h>
#include <../../../drivers/video/samsung/s3cfb.h>
#include <linux/sec_jack.h>
#include <linux/input/mxt224.h>
#include <linux/mfd/max8998.h>
#include <linux/switch.h>

#ifdef CONFIG_KERNEL_DEBUG_SEC
#include <linux/kernel_sec_common.h>
#endif

#include "wave.h"

#undef pr_debug
#define pr_debug pr_info

struct class *sec_class;
EXPORT_SYMBOL(sec_class);

struct device *switch_dev;
EXPORT_SYMBOL(switch_dev);

void (*sec_set_param_value)(int idx, void *value);
EXPORT_SYMBOL(sec_set_param_value);

void (*sec_get_param_value)(int idx, void *value);
EXPORT_SYMBOL(sec_get_param_value);

#define KERNEL_REBOOT_MASK      0xFFFFFFFF
#define REBOOT_MODE_FAST_BOOT		7

#define PREALLOC_WLAN_SEC_NUM		4
#define PREALLOC_WLAN_BUF_NUM		160
#define PREALLOC_WLAN_SECTION_HEADER	24

#define WLAN_SECTION_SIZE_0	(PREALLOC_WLAN_BUF_NUM * 128)
#define WLAN_SECTION_SIZE_1	(PREALLOC_WLAN_BUF_NUM * 128)
#define WLAN_SECTION_SIZE_2	(PREALLOC_WLAN_BUF_NUM * 512)
#define WLAN_SECTION_SIZE_3	(PREALLOC_WLAN_BUF_NUM * 1024)

#define WLAN_SKB_BUF_NUM	16

static struct sk_buff *wlan_static_skb[WLAN_SKB_BUF_NUM];

struct wifi_mem_prealloc {
	void *mem_ptr;
	unsigned long size;
};

static int wave_notifier_call(struct notifier_block *this,
					unsigned long code, void *_cmd)
{
	int mode = REBOOT_MODE_NONE;

	if ((code == SYS_RESTART) && _cmd) {
		if (!strcmp((char *)_cmd, "recovery"))
			mode = 2; // It's not REBOOT_MODE_RECOVERY, blame Samsung
		else
			mode = REBOOT_MODE_NONE;
	}
	__raw_writel(mode, S5P_INFORM6);

	return NOTIFY_DONE;
}

static struct notifier_block wave_reboot_notifier = {
	.notifier_call = wave_notifier_call,
};

static void uart_switch_init(void)
{
	int ret;
	struct device *uartswitch_dev;

	uartswitch_dev = device_create(sec_class, NULL, 0, NULL, "uart_switch");
	if (IS_ERR(uartswitch_dev)) {
		pr_err("Failed to create device(uart_switch)!\n");
		return;
	}

	ret = gpio_request(GPIO_UART_SEL, "UART_SEL");
	if (ret < 0) {
		pr_err("Failed to request GPIO_UART_SEL!\n");
		return;
	}
	s3c_gpio_setpull(GPIO_UART_SEL, S3C_GPIO_PULL_NONE);
	s3c_gpio_cfgpin(GPIO_UART_SEL, S3C_GPIO_OUTPUT);
	gpio_direction_output(GPIO_UART_SEL, 1);

	gpio_export(GPIO_UART_SEL, 1);

	gpio_export_link(uartswitch_dev, "UART_SEL", GPIO_UART_SEL);
}

static void wave_switch_init(void)
{
	sec_class = class_create(THIS_MODULE, "sec");

	if (IS_ERR(sec_class))
		pr_err("Failed to create class(sec)!\n");

	switch_dev = device_create(sec_class, NULL, 0, NULL, "switch");

	if (IS_ERR(switch_dev))
		pr_err("Failed to create device(switch)!\n");
};

/* Following are default values for UCON, ULCON and UFCON UART registers */
#define S5PV210_UCON_DEFAULT	(S3C2410_UCON_TXILEVEL |	\
				 S3C2410_UCON_RXILEVEL |	\
				 S3C2410_UCON_TXIRQMODE |	\
				 S3C2410_UCON_RXIRQMODE |	\
				 S3C2410_UCON_RXFIFO_TOI |	\
				 S3C2443_UCON_RXERR_IRQEN)

#define S5PV210_ULCON_DEFAULT	S3C2410_LCON_CS8

#define S5PV210_UFCON_DEFAULT	(S3C2410_UFCON_FIFOMODE |	\
				 S5PV210_UFCON_TXTRIG4 |	\
				 S5PV210_UFCON_RXTRIG4)

static struct s3c2410_uartcfg wave_uartcfgs[] __initdata = {
	{
		.hwport		= 0,
		.flags		= 0,
		.ucon		= S5PV210_UCON_DEFAULT,
		.ulcon		= S5PV210_ULCON_DEFAULT,
		.ufcon		= S5PV210_UFCON_DEFAULT,
		.wake_peer	= wave_bt_uart_wake_peer,
	},
	{
		.hwport		= 1,
		.flags		= 0,
		.ucon		= S5PV210_UCON_DEFAULT,
		.ulcon		= S5PV210_ULCON_DEFAULT,
		.ufcon		= S5PV210_UFCON_DEFAULT,
	},
#if !defined(CONFIG_FIQ_DEBUGGER) && 0
	{
		.hwport		= 2,
		.flags		= 0,
		.ucon		= S5PV210_UCON_DEFAULT,
		.ulcon		= S5PV210_ULCON_DEFAULT,
		.ufcon		= S5PV210_UFCON_DEFAULT,
	},
#endif
	{
		.hwport		= 3,
		.flags		= 0,
		.ucon		= S5PV210_UCON_DEFAULT,
		.ulcon		= S5PV210_ULCON_DEFAULT,
		.ufcon		= S5PV210_UFCON_DEFAULT,
	},
};

#define S5PV210_LCD_WIDTH 480
#define S5PV210_LCD_HEIGHT 800

#ifdef CONFIG_FB_S3C_LG4573
//LG4573(LCD type 0 & 3) S8530
static struct s3cfb_lcd lg4573 = {
	.width 		= S5PV210_LCD_WIDTH,
	.height 	= S5PV210_LCD_HEIGHT,
	.p_width 	= 52,	//  width of lcd in mm
	.p_height 	= 86,	//  height of lcd in mm
	.bpp 		= 24,   //
	.freq 		= 60,	//
	.timing = {

		.h_fp 	= 17,
		.h_bp 	= 17,   // HBP
		.h_sw 	= 3,
		.v_fp 	= 29,
		.v_fpe 	= 29,
		.v_bp 	= 2,    // VBP
		.v_bpe 	= 2,
		.v_sw 	= 1,
	},
	.polarity = {
		.rise_vclk 	= 1, // video data fetch at DOTCLK rising edge
		.inv_hsync 	= 1,	// High active
		.inv_vsync 	= 1,	// High active
		.inv_vden 	= 1,	// data is vaild when DE-pin is high
	},
};
#endif

#ifdef CONFIG_FB_S3C_TL2796
static struct s3cfb_lcd s6e63m0 = {
	.width = S5PV210_LCD_WIDTH,
	.height = S5PV210_LCD_HEIGHT,
	.p_width = 52,
	.p_height = 86,
	.bpp = 24,
	.freq = 60,

	.timing = {
		.h_fp = 16,
		.h_bp = 16,
		.h_sw = 2,
		.v_fp = 28,
		.v_fpe = 1,
		.v_bp = 1,
		.v_bpe = 1,
		.v_sw = 2,
	},
	.polarity = {
		.rise_vclk = 1,
		.inv_hsync = 1,
		.inv_vsync = 1,
		.inv_vden = 1,
	},
};
#endif

#define  S5PV210_VIDEO_SAMSUNG_MEMSIZE_FIMC0 (11264 * SZ_1K)
#define  S5PV210_VIDEO_SAMSUNG_MEMSIZE_FIMC2 (11264 * SZ_1K)
#define  S5PV210_VIDEO_SAMSUNG_MEMSIZE_MFC0 (11264 * SZ_1K)
#define  S5PV210_VIDEO_SAMSUNG_MEMSIZE_MFC1 (11264 * SZ_1K)
#define  S5PV210_VIDEO_SAMSUNG_MEMSIZE_FIMD (S5PV210_LCD_WIDTH * \
					     S5PV210_LCD_HEIGHT * 4 * \
					     (CONFIG_FB_S3C_NR_BUFFERS + \
						 (CONFIG_FB_S3C_NUM_OVLY_WIN * \
						  CONFIG_FB_S3C_NUM_BUF_OVLY_WIN)))
#define  S5PV210_VIDEO_SAMSUNG_MEMSIZE_JPEG (4096 * SZ_1K)
#define  S5PV210_ANDROID_PMEM_MEMSIZE_PMEM (2048 * SZ_1K)
#define  S5PV210_ANDROID_PMEM_MEMSIZE_PMEM_GPU1 (3000 * SZ_1K)
#define  S5PV210_ANDROID_PMEM_MEMSIZE_PMEM_ADSP (1500 * SZ_1K)
#define  S5PV210_VIDEO_SAMSUNG_MEMSIZE_TEXTSTREAM (3000 * SZ_1K)


static struct s5p_media_device wave_media_devs[] = {
	[0] = {
		.id = S5P_MDEV_MFC,
		.name = "mfc",
		.bank = 0,
		.memsize = S5PV210_VIDEO_SAMSUNG_MEMSIZE_MFC0,
		.paddr = 0,
	},
	[1] = {
		.id = S5P_MDEV_MFC,
		.name = "mfc",
		.bank = 1,
		.memsize = S5PV210_VIDEO_SAMSUNG_MEMSIZE_MFC1,
		.paddr = 0,
	},
	[2] = {
		.id = S5P_MDEV_FIMC0,
		.name = "fimc0",
		.bank = 1,
		.memsize = S5PV210_VIDEO_SAMSUNG_MEMSIZE_FIMC0,
		.paddr = 0,
	},
	[4] = {
		.id = S5P_MDEV_FIMC2,
		.name = "fimc2",
		.bank = 1,
		.memsize = S5PV210_VIDEO_SAMSUNG_MEMSIZE_FIMC2,
		.paddr = 0,
	},
	[5] = {
		.id = S5P_MDEV_JPEG,
		.name = "jpeg",
		.bank = 0,
		.memsize = S5PV210_VIDEO_SAMSUNG_MEMSIZE_JPEG,
		.paddr = 0,
	},
	[6] = {
		.id = S5P_MDEV_FIMD,
		.name = "fimd",
		.bank = 1,
		.memsize = S5PV210_VIDEO_SAMSUNG_MEMSIZE_FIMD,
		.paddr = 0,
	},
#ifdef CONFIG_ANDROID_PMEM
	[7] = {
		.id = S5P_MDEV_PMEM,
		.name = "pmem",
		.bank = 0,
		.memsize = S5PV210_ANDROID_PMEM_MEMSIZE_PMEM,
		.paddr = 0,
	},
	[8] = {
		.id = S5P_MDEV_PMEM_GPU1,
		.name = "pmem_gpu1",
		.bank = 0,
		.memsize = S5PV210_ANDROID_PMEM_MEMSIZE_PMEM_GPU1,
		.paddr = 0,
	},
	[9] = {
		.id = S5P_MDEV_PMEM_ADSP,
		.name = "pmem_adsp",
		.bank = 0,
		.memsize = S5PV210_ANDROID_PMEM_MEMSIZE_PMEM_ADSP,
		.paddr = 0,
		},
	[10] = {
		.id = S5P_MDEV_TEXSTREAM,
		.name = "s3c_bc",
		.bank = 1,
		.memsize = S5PV210_VIDEO_SAMSUNG_MEMSIZE_TEXTSTREAM,
		.paddr = 0,
	},	
#endif
};

#ifdef CONFIG_CPU_FREQ
static struct s5pv210_cpufreq_voltage smdkc110_cpufreq_volt[] = {
	{
		.freq	= 1320000,
		.varm	= 1200000,
		.vint	= 1100000,
	}, {
		.freq	= 1200000,
		.varm	= 1175000,
		.vint	= 1100000,
	}, {
		.freq	= 1000000,
		.varm	= 1150000,
		.vint	= 1100000,
	}, {
		.freq	=  800000,
		.varm	= 1125000,
		.vint	= 1100000,
	}, {
		.freq	=  400000,
		.varm	=  925000,
		.vint	= 1100000,
	}, {
		.freq	=  200000,
		.varm	=  875000,
		.vint	= 1100000,
	},
};

static struct s5pv210_cpufreq_data smdkc110_cpufreq_plat = {
	.volt	= smdkc110_cpufreq_volt,
	.size	= ARRAY_SIZE(smdkc110_cpufreq_volt),
};
#endif

static struct regulator_consumer_supply ldo3_consumer[] = {
	REGULATOR_SUPPLY("pd_io", "s3c-usbgadget")
};

static struct regulator_consumer_supply ldo5_consumer[] = {
	REGULATOR_SUPPLY("vmmc", NULL),
};

static struct regulator_consumer_supply ldo7_consumer[] = {
	{	.supply	= "vlcd", },
};

static struct regulator_consumer_supply ldo8_consumer[] = {
	REGULATOR_SUPPLY("pd_core", "s3c-usbgadget"),
        REGULATOR_SUPPLY("tvout", NULL),
};

static struct regulator_consumer_supply ldo11_consumer[] = {
	{	.supply	= "cam_af", },
};

static struct regulator_consumer_supply ldo12_consumer[] = {
	{	.supply	= "cam_sensor", },
};

static struct regulator_consumer_supply ldo13_consumer[] = {
	{	.supply	= "vga_avdd", },
};

static struct regulator_consumer_supply ldo14_consumer[] = {
	{	.supply	= "cam_isp", },
};

static struct regulator_consumer_supply ldo15_consumer[] = {
	{	.supply	= "cam_isp_host", },
};

static struct regulator_consumer_supply ldo16_consumer[] = {
	{	.supply	= "vga_dvdd", },
};

static struct regulator_consumer_supply ldo17_consumer[] = {
	{	.supply	= "vcc_lcd", },
};

static struct regulator_consumer_supply buck1_consumer[] = {
	{	.supply	= "vddarm", },
};

static struct regulator_consumer_supply buck2_consumer[] = {
	{	.supply	= "vddint", },
};

static struct regulator_consumer_supply buck4_consumer[] = {
	{	.supply	= "cam_isp_core", },
};

static struct regulator_init_data wave_ldo2_data = {
	.constraints	= {
		.name		= "VALIVE_1.2V",
		.min_uV		= 1100000,
		.max_uV		= 1100000,
		.apply_uV	= 1,
		.always_on	= 1,
		.state_mem	= {
			.enabled = 1,
		},
	},
};

static struct regulator_init_data wave_ldo3_data = {
	.constraints	= {
		.name		= "VUSB_1.1V",
		.min_uV		= 1000000,
		.max_uV		= 1000000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo3_consumer),
	.consumer_supplies	= ldo3_consumer,
};

static struct regulator_init_data wave_ldo4_data = {
	.constraints	= {
		.name		= "VADC_3.3V",
		.min_uV		= 3200000,
		.max_uV		= 3200000,
		.apply_uV	= 1,
		.always_on	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
};

#ifndef CONFIG_SAMSUNG_FASCINATE
static struct regulator_init_data wave_ldo5_data = {
	.constraints	= {
		.name		= "VTF_2.8V",
		.min_uV		= 2700000,
		.max_uV		= 2700000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo5_consumer),
	.consumer_supplies	= ldo5_consumer,
};
#endif

static struct regulator_init_data wave_ldo7_data = {
	.constraints	= {
		.name		= "VLCD_1.8V",
		.min_uV		= 1700000,
		.max_uV		= 1700000,
		.apply_uV	= 1,
		.always_on	= 0,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo7_consumer),
	.consumer_supplies	= ldo7_consumer,
};

static struct regulator_init_data wave_ldo8_data = {
	.constraints	= {
		.name		= "VUSB_3.3V",
		.min_uV		= 3200000,
		.max_uV		= 3200000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo8_consumer),
	.consumer_supplies	= ldo8_consumer,
};

static struct regulator_init_data wave_ldo9_data = {
	.constraints	= {
		.name		= "VCC_2.8V_PDA",
		.min_uV		= 2800000,
		.max_uV		= 2800000,
		.apply_uV	= 1,
		.always_on	= 1,
	},
};

static struct regulator_init_data wave_ldo11_data = {
	.constraints	= {
		.name		= "CAM_AF_2.8V",
		.min_uV		= 2800000,
		.max_uV		= 2800000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo11_consumer),
	.consumer_supplies	= ldo11_consumer,
};

static struct regulator_init_data wave_ldo12_data = {
	.constraints	= {
		.name		= "CAM_SENSOR_1.2V",
		.min_uV		= 1200000,
		.max_uV		= 1200000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo12_consumer),
	.consumer_supplies	= ldo12_consumer,
};

static struct regulator_init_data wave_ldo13_data = {
	.constraints	= {
		.name		= "CAM_SENSOR_A2.8V",
		.min_uV		= 2800000,
		.max_uV		= 2800000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo13_consumer),
	.consumer_supplies	= ldo13_consumer,
};

static struct regulator_init_data wave_ldo14_data = {
	.constraints	= {
		.name		= "CAM_ISP_1.8V",
		.min_uV		= 1800000,
		.max_uV		= 1800000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo14_consumer),
	.consumer_supplies	= ldo14_consumer,
};

static struct regulator_init_data wave_ldo15_data = {
	.constraints	= {
		.name		= "CAM_ISP_HOST_2.8V",
		.min_uV		= 2800000,
		.max_uV		= 2800000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo15_consumer),
	.consumer_supplies	= ldo15_consumer,
};

static struct regulator_init_data wave_ldo16_data = {
	.constraints	= {
		.name		= "VGA_DVDD_1.8V",
		.min_uV		= 1700000,
		.max_uV		= 1700000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo16_consumer),
	.consumer_supplies	= ldo16_consumer,
};

static struct regulator_init_data wave_ldo17_data = {
	.constraints	= {
		.name		= "VCC_3.0V_LCD",
		.min_uV		= 2800000,
		.max_uV		= 3000000,
		/* service manuals of S8500 & S8530 mark it as 3.2V but there are known configs of 3.0 and 2.8 */
		.apply_uV	= 1,
		.always_on	= 0,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(ldo17_consumer),
	.consumer_supplies	= ldo17_consumer,
};

static struct regulator_init_data wave_buck1_data = {
	.constraints	= {
		.name		= "VDD_ARM",
		.min_uV		= 750000,
		.max_uV		= 1450000,
		.apply_uV	= 1,
		.valid_ops_mask	= REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.uV	= 1250000,
			.mode	= REGULATOR_MODE_NORMAL,
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(buck1_consumer),
	.consumer_supplies	= buck1_consumer,
};

static struct regulator_init_data wave_buck2_data = {
	.constraints	= {
		.name		= "VDD_INT",
		.min_uV		= 750000,
		.max_uV		= 1450000,
		.apply_uV	= 1,
		.valid_ops_mask	= REGULATOR_CHANGE_VOLTAGE |
				  REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.uV	= 1100000,
			.mode	= REGULATOR_MODE_NORMAL,
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(buck2_consumer),
	.consumer_supplies	= buck2_consumer,
};

static struct regulator_init_data wave_buck3_data = {
	.constraints	= {
		.name		= "VCC_1.8V",
		.min_uV		= 1700000,
		.max_uV		= 1700000,
		.apply_uV	= 1,
		.always_on	= 1,
	},
};

static struct regulator_init_data wave_buck4_data = {
	.constraints	= {
		.name		= "CAM_ISP_CORE_1.2V",
		.min_uV		= 1200000,
		.max_uV		= 1200000,
		.apply_uV	= 1,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
		.state_mem	= {
			.disabled = 1,
		},
	},
	.num_consumer_supplies	= ARRAY_SIZE(buck4_consumer),
	.consumer_supplies	= buck4_consumer,
};

static struct max8998_regulator_data wave_regulators[] = {
	{ MAX8998_LDO2,  &wave_ldo2_data },
	{ MAX8998_LDO3,  &wave_ldo3_data },
	{ MAX8998_LDO4,  &wave_ldo4_data },
#ifndef CONFIG_SAMSUNG_FASCINATE
	{ MAX8998_LDO5,  &wave_ldo5_data },
#endif
	{ MAX8998_LDO7,  &wave_ldo7_data },
	{ MAX8998_LDO8,  &wave_ldo8_data },
	{ MAX8998_LDO9,  &wave_ldo9_data },
	{ MAX8998_LDO11, &wave_ldo11_data },
	{ MAX8998_LDO12, &wave_ldo12_data },
	{ MAX8998_LDO13, &wave_ldo13_data },
	{ MAX8998_LDO14, &wave_ldo14_data },
	{ MAX8998_LDO15, &wave_ldo15_data },
	{ MAX8998_LDO16, &wave_ldo16_data },
	{ MAX8998_LDO17, &wave_ldo17_data },
	{ MAX8998_BUCK1, &wave_buck1_data },
	{ MAX8998_BUCK2, &wave_buck2_data },
	{ MAX8998_BUCK3, &wave_buck3_data },
	{ MAX8998_BUCK4, &wave_buck4_data },
};

static struct max8998_adc_table_data temper_table[] =  {
//TODO: Compare with bada.
	{  264,  650 },
	{  275,  640 },
	{  286,  630 },
	{  293,  620 },
	{  299,  610 },
	{  306,  600 },
	{  324,  590 },
	{  341,  580 },
	{  354,  570 },
	{  368,  560 },
	{  381,  550 },
	{  396,  540 },
	{  411,  530 },
	{  427,  520 },
	{  442,  510 },
	{  457,  500 },
	{  472,  490 },
	{  487,  480 },
	{  503,  470 },
	{  518,  460 },
	{  533,  450 },
	{  554,  440 },
	{  574,  430 },
	{  595,  420 },
	{  615,  410 },
	{  636,  400 },
	{  656,  390 },
	{  677,  380 },
	{  697,  370 },
	{  718,  360 },
	{  738,  350 },
	{  761,  340 },
	{  784,  330 },
	{  806,  320 },
	{  829,  310 },
	{  852,  300 },
	{  875,  290 },
	{  898,  280 },
	{  920,  270 },
	{  943,  260 },
	{  966,  250 },
	{  990,  240 },
	{ 1013,  230 },
	{ 1037,  220 },
	{ 1060,  210 },
	{ 1084,  200 },
	{ 1108,  190 },
	{ 1131,  180 },
	{ 1155,  170 },
	{ 1178,  160 },
	{ 1202,  150 },
	{ 1226,  140 },
	{ 1251,  130 },
	{ 1275,  120 },
	{ 1299,  110 },
	{ 1324,  100 },
	{ 1348,   90 },
	{ 1372,   80 },
	{ 1396,   70 },
	{ 1421,   60 },
	{ 1445,   50 },
	{ 1468,   40 },
	{ 1491,   30 },
	{ 1513,   20 },
	{ 1536,   10 },
	{ 1559,    0 },
	{ 1577,  -10 },
	{ 1596,  -20 },
	{ 1614,  -30 },
	{ 1619,  -40 },
	{ 1632,  -50 },
	{ 1658,  -60 },
	{ 1667,  -70 }, 
};
struct max8998_charger_callbacks *charger_callbacks;
static enum cable_type_t set_cable_status;

static void max8998_charger_register_callbacks(
		struct max8998_charger_callbacks *ptr)
{
	charger_callbacks = ptr;
	/* if there was a cable status change before the charger was
	ready, send this now */
	if ((set_cable_status != 0) && charger_callbacks && charger_callbacks->set_cable)
		charger_callbacks->set_cable(charger_callbacks, set_cable_status);
}

static struct max8998_charger_data wave_charger = {
	.register_callbacks	= &max8998_charger_register_callbacks,
	.adc_table		= temper_table,
	.adc_array_size		= ARRAY_SIZE(temper_table),
};

struct max8998_power_callbacks *power_callbacks;

static struct max8998_platform_data max8998_pdata = {
	.num_regulators = ARRAY_SIZE(wave_regulators),
	.regulators     = wave_regulators,
	.charger        = &wave_charger,
	/* Preloads must be in increasing order of voltage value */
	.buck1_voltage4	= 950000,
	.buck1_voltage3	= 1050000,
	.buck1_voltage2	= 1200000,
	.buck1_voltage1	= 1275000,
	.buck2_voltage2	= 1000000,
	.buck2_voltage1	= 1100000,
	.buck1_set1	= GPIO_BUCK_1_EN_A,
	.buck1_set2	= GPIO_BUCK_1_EN_B,
	.buck2_set3	= GPIO_BUCK_2_EN,
	.buck1_default_idx = 1,
	.buck2_default_idx = 0,
	.power_callbacks = &power_callbacks,
};

struct platform_device sec_device_dpram = {
	.name	= "dpram-device",
	.id	= -1,
};

static void panel_cfg_gpio(struct platform_device *pdev)
{
	int i;
	
	/* configure DISPLAY_HSYNC, VSYNC, DE, PCLK and RGB pins */
	for (i = 0; i < 8; i++)
		s3c_gpio_cfgpin(S5PV210_GPF0(i), S3C_GPIO_SFN(2));

	for (i = 0; i < 8; i++)
		s3c_gpio_cfgpin(S5PV210_GPF1(i), S3C_GPIO_SFN(2));

	for (i = 0; i < 8; i++)
		s3c_gpio_cfgpin(S5PV210_GPF2(i), S3C_GPIO_SFN(2));

	for (i = 0; i < 4; i++)
		s3c_gpio_cfgpin(S5PV210_GPF3(i), S3C_GPIO_SFN(2));


	/* mDNIe SEL: why we shall write 0x2 ? */
#ifdef CONFIG_FB_S3C_MDNIE
	writel(0x1, S5P_MDNIE_SEL);
#else
	writel(0x2, S5P_MDNIE_SEL);
#endif

	if(machine_is_wave2()) {
		/* S8530 LCD Backlight is set by LG4573 driver*/

		/* S8530 LCD_ID pins */	
		
		s3c_gpio_cfgpin(GPIO_MLCD_RST, S3C_GPIO_OUTPUT);
		s3c_gpio_set_drvstrength(GPIO_MLCD_RST, S3C_GPIO_DRVSTR_2X);
		/* do not set RST value yet, panel_reset will deal with it */
	} else {
		s3c_gpio_setpull(GPIO_OLED_ID, S3C_GPIO_PULL_NONE);
		s3c_gpio_setpull(GPIO_DIC_ID, S3C_GPIO_PULL_NONE);
	}
	s3c_gpio_setpull(GPIO_OLED_DET, S3C_GPIO_PULL_NONE);
}

void lcd_cfg_gpio_early_suspend(void)
{
	int i;

	for (i = 0; i < 8; i++) {
		s3c_gpio_cfgpin(S5PV210_GPF0(i), S3C_GPIO_OUTPUT);
		gpio_set_value(S5PV210_GPF0(i), 0);
	}

	for (i = 0; i < 8; i++) {
		s3c_gpio_cfgpin(S5PV210_GPF1(i), S3C_GPIO_OUTPUT);
		gpio_set_value(S5PV210_GPF1(i), 0);
	}

	for (i = 0; i < 8; i++) {
		s3c_gpio_cfgpin(S5PV210_GPF2(i), S3C_GPIO_OUTPUT);
		gpio_set_value(S5PV210_GPF2(i), 0);
	}

	for (i = 0; i < 4; i++) {
		s3c_gpio_cfgpin(S5PV210_GPF3(i), S3C_GPIO_OUTPUT);
		gpio_set_value(S5PV210_GPF3(i), 0);
	}

	gpio_set_value(GPIO_MLCD_RST, 0);

	gpio_set_value(GPIO_DISPLAY_CS, 0);
	gpio_set_value(GPIO_DISPLAY_CLK, 0);
	gpio_set_value(GPIO_DISPLAY_SI, 0);
	if(machine_is_wave()){
		s3c_gpio_setpull(GPIO_OLED_DET, S3C_GPIO_PULL_DOWN);
		s3c_gpio_setpull(GPIO_OLED_ID, S3C_GPIO_PULL_DOWN);
		s3c_gpio_setpull(GPIO_DIC_ID, S3C_GPIO_PULL_DOWN);
	}
}
EXPORT_SYMBOL(lcd_cfg_gpio_early_suspend);

void lcd_cfg_gpio_late_resume(void)
{
	/* there's panel_cfg_gpio called by s3cfb driver after this so no need to cfg anything */
}
EXPORT_SYMBOL(lcd_cfg_gpio_late_resume);

static int panel_reset_lcd(struct platform_device *pdev)
{
	int err;
	printk(KERN_ERR "panel_reset_lcd\n");
	err = gpio_request(GPIO_MLCD_RST, "MLCD_RST");
	if (err) {
		printk(KERN_ERR "failed to request GPIO_MLCD_RST for "
				"lcd reset control\n");
		return err;
	}

	gpio_direction_output(GPIO_MLCD_RST, 1);
	msleep(25);

	gpio_set_value(GPIO_MLCD_RST, 0);
	msleep(10);

	gpio_set_value(GPIO_MLCD_RST, 1);
	msleep(150);

	gpio_free(GPIO_MLCD_RST);

	return 0;
}

#define LCD_BUS_NUM     3

#ifdef CONFIG_FB_S3C_LG4573

//SLCD for S8530
static struct s3c_platform_fb lg4573_data __initdata = {
	.hw_ver		= 0x62,
	.clk_name	= "sclk_fimd",
	.nr_wins	= 5,
	.default_win	= CONFIG_FB_S3C_DEFAULT_WINDOW,
	.swap		= FB_SWAP_HWORD | FB_SWAP_WORD,

	.lcd = &lg4573,
	.cfg_gpio	= panel_cfg_gpio,
	.reset_lcd	= panel_reset_lcd,
};

//for S8530 - LG4573
static struct spi_board_info lg4573_spi_board_info[] __initdata = {
	{
		.modalias	= "lg4573",
		.platform_data	= &wave_lg4573_panel_data,
		.max_speed_hz	= 1200000,
		.bus_num	= LCD_BUS_NUM,
		.chip_select	= 0,
		.mode		= SPI_MODE_3,
		.controller_data = (void *)GPIO_DISPLAY_CS,
	},
};
#endif

#ifdef CONFIG_FB_S3C_TL2796

static struct s3c_platform_fb tl2796_data __initdata = {
	.hw_ver		= 0x62,
	.clk_name	= "sclk_fimd",
	.nr_wins	= 5,
	.default_win	= CONFIG_FB_S3C_DEFAULT_WINDOW,
	.swap		= FB_SWAP_HWORD | FB_SWAP_WORD,

	.lcd = &s6e63m0,
	.cfg_gpio	= panel_cfg_gpio,
	.reset_lcd	= panel_reset_lcd,
};

static struct spi_board_info tl2796_spi_board_info[] __initdata = {
	{
		.modalias	= "tl2796",
		.platform_data	= &wave_tl2796_panel_data,
		.max_speed_hz	= 1200000,
		.bus_num	= LCD_BUS_NUM,
		.chip_select	= 0,
		.mode		= SPI_MODE_3,
		.controller_data = (void *)GPIO_DISPLAY_CS,
	},
};
#endif
//common spi bus description for all LCD ICs
static struct spi_gpio_platform_data wave_display_spi_gpio_data = {
	.sck	= GPIO_DISPLAY_CLK,
	.mosi	= GPIO_DISPLAY_SI,
	.miso	= SPI_GPIO_NO_MISO,
	.num_chipselect = 2,
};

static struct platform_device s3c_display_spi_gpio = {
	.name	= "spi_gpio",
	.id	= LCD_BUS_NUM,
	.dev	= {
		.parent		= &s3c_device_fb.dev,
		.platform_data	= &wave_display_spi_gpio_data,
	},
};

static struct i2c_gpio_platform_data i2c4_platdata = {
	.sda_pin		= GPIO_AP_SDA_18V,
	.scl_pin		= GPIO_AP_SCL_18V,
	.udelay			= 2, /* 250KHz */
	.sda_is_open_drain	= 0,
	.scl_is_open_drain	= 0,
	.scl_is_output_only	= 0,
};

static struct platform_device wave_s3c_device_i2c4 = {
	.name			= "i2c-gpio",
	.id			= 4,
	.dev.platform_data	= &i2c4_platdata,
};

static struct i2c_gpio_platform_data i2c5_platdata = {
	.sda_pin		= GPIO_AP_SDA_28V,
	.scl_pin		= GPIO_AP_SCL_28V,
	.udelay			= 2, /* 250KHz */
	.sda_is_open_drain	= 0,
	.scl_is_open_drain	= 0,
	.scl_is_output_only	= 0,
};

static struct platform_device wave_s3c_device_i2c5 = {
	.name			= "i2c-gpio",
	.id			= 5,
	.dev.platform_data	= &i2c5_platdata,
};

static struct i2c_gpio_platform_data i2c6_platdata = {
	.sda_pin                = GPIO_AP_PMIC_SDA,
	.scl_pin                = GPIO_AP_PMIC_SCL,
	.udelay                 = 2,    /* 250KHz */
	.sda_is_open_drain      = 0,
	.scl_is_open_drain      = 0,
	.scl_is_output_only     = 0,
};

static struct platform_device wave_s3c_device_i2c6 = {
	.name			= "i2c-gpio",
	.id			= 6,
	.dev.platform_data      = &i2c6_platdata,
};

static  struct  i2c_gpio_platform_data  i2c7_platdata = {
	.sda_pin                = GPIO_USB_SDA_28V,
	.scl_pin                = GPIO_USB_SCL_28V,
	.udelay                 = 2,    /* 250KHz */
	.sda_is_open_drain      = 0,
	.scl_is_open_drain      = 0,
	.scl_is_output_only     = 0,
};

static struct platform_device wave_s3c_device_i2c7 = {
	.name			= "i2c-gpio",
	.id			= 7,
	.dev.platform_data	= &i2c7_platdata,
};
// For FM radio
#if defined (CONFIG_SAMSUNG_GALAXYS) || defined (CONFIG_SAMSUNG_GALAXYSB) || defined (CONFIG_MACH_WAVE)
static  struct  i2c_gpio_platform_data  i2c8_platdata = {
        .sda_pin                = GPIO_FM_SDA_28V,
        .scl_pin                = GPIO_FM_SCL_28V,
        .udelay                 = 2,    /* 250KHz */
        .sda_is_open_drain      = 0,
        .scl_is_open_drain      = 0,
        .scl_is_output_only     = 0,
};

static struct platform_device s3c_device_i2c8 = {
        .name                           = "i2c-gpio",
        .id                                     = 8,
        .dev.platform_data      = &i2c8_platdata,
};
#endif

static struct i2c_gpio_platform_data i2c11_platdata = {
	.sda_pin                = GPIO_ALS_SDA_28V,
	.scl_pin                = GPIO_ALS_SCL_28V,
	.udelay                 = 2,    /* 250KHz */
	.sda_is_open_drain      = 0,
	.scl_is_open_drain      = 0,
	.scl_is_output_only     = 0,
};

static struct platform_device s3c_device_i2c11 = {
	.name			= "i2c-gpio",
	.id			= 11,
	.dev.platform_data	= &i2c11_platdata,
};

static struct i2c_gpio_platform_data i2c12_platdata = {
	.sda_pin                = GPIO_MSENSE_SDA_28V,
	.scl_pin                = GPIO_MSENSE_SCL_28V,
	.udelay                 = 0,    /* 250KHz */
	.sda_is_open_drain      = 0,
	.scl_is_open_drain      = 0,
	.scl_is_output_only     = 0,
};

static struct platform_device s3c_device_i2c12 = {
	.name			= "i2c-gpio",
	.id			= 12,
	.dev.platform_data	= &i2c12_platdata,
};

#if defined (CONFIG_SAMSUNG_CAPTIVATE)
static struct i2c_gpio_platform_data	i2c13_platdata = {
    .sda_pin		= GPIO_A1026_SDA,
    .scl_pin		= GPIO_A1026_SCL,
    .udelay			= 1,	/* 250KHz */
    .sda_is_open_drain	= 0,
    .scl_is_open_drain	= 0,
    .scl_is_output_only	= 0,
  };
static struct platform_device s3c_device_i2c13 = {
  	.name				= "i2c-gpio",
  	.id					= 13,
  	.dev.platform_data	= &i2c13_platdata,
  };
#endif

static struct gpio_keys_button wave_gpio_keys_table[] = {
	{
		.code 		= KEY_BACK,
		.gpio		= GPIO_nPOWER,
		.desc		= "gpio-keys: KEY_BACK",
		.type		= EV_KEY,
		.active_low	= 1,
		.wakeup		= 1,
		.debounce_interval = 1,
	},
};

static struct gpio_keys_platform_data wave_gpio_keys_data = {
	.buttons	= wave_gpio_keys_table,
	.nbuttons	= ARRAY_SIZE(wave_gpio_keys_table),
};

static struct platform_device wave_device_gpiokeys = {
	.name = "gpio-keys",
	.dev = {
		.platform_data = &wave_gpio_keys_data,
	},
};
#ifdef CONFIG_S5P_ADC
static struct s3c_adc_mach_info s3c_adc_platform __initdata = {
	/* s5pc110 support 12-bit resolution */
	.delay		= 10000,
	.presc		= 65,
	.resolution	= 12,
};
#endif

unsigned int HWREV;
EXPORT_SYMBOL(HWREV);

/* in revisions before 0.9, there is a common mic bias gpio */

static DEFINE_SPINLOCK(mic_bias_lock);
static bool wm8994_mic_bias;
static bool jack_mic_bias;
static void set_shared_mic_bias(void)
{
	gpio_set_value(GPIO_MICBIAS_EN, wm8994_mic_bias || jack_mic_bias);
    gpio_set_value(GPIO_EARPATH_SEL, wm8994_mic_bias || jack_mic_bias);
	gpio_set_value(GPIO_PCM_SEL, wm8994_mic_bias || jack_mic_bias);
}

static void wm8994_set_mic_bias(bool on)
{
    unsigned long flags;
    pr_debug("%s: HWREV=%d, on=%d\n", __func__, HWREV, on ? 1 : 0);
    spin_lock_irqsave(&mic_bias_lock, flags);
    wm8994_mic_bias = on;
    set_shared_mic_bias();
    spin_unlock_irqrestore(&mic_bias_lock, flags);
}

static void sec_jack_set_micbias_state(bool on)
{
    unsigned long flags;
    pr_debug("%s: HWREV=%d, on=%d\n", __func__, HWREV, on ? 1 : 0);
    spin_lock_irqsave(&mic_bias_lock, flags);
    jack_mic_bias = on;
    set_shared_mic_bias();
    spin_unlock_irqrestore(&mic_bias_lock, flags);
}

static struct wm8994_platform_data wm8994_pdata = {
	.ldo = GPIO_CODEC_LDO_EN,
	.ear_sel = -1,
	.set_mic_bias = wm8994_set_mic_bias,
};

#if defined(CONFIG_VIDEO_S5KA3DFX) || defined(CONFIG_VIDEO_CE147)

static struct regulator *cam_isp_core_regulator;/*buck4*/
static struct regulator *cam_af_regulator;/*11*/
static struct regulator *cam_sensor_core_regulator;/*12*/
static struct regulator *cam_vga_vddio_regulator;/*13*/
static struct regulator *cam_isp_regulator;/*14*/
static struct regulator *cam_isp_host_regulator;/*15*/
static struct regulator *cam_vga_dvdd_regulator;/*16*/

static int camera_regulator_init(void)
{
/*BUCK 4*/
	if (IS_ERR_OR_NULL(cam_isp_core_regulator)) {
		cam_isp_core_regulator = regulator_get(NULL, "cam_isp_core");
		if (IS_ERR_OR_NULL(cam_isp_core_regulator)) {
			pr_err("failed to get cam_isp_core regulator");
			return -EINVAL;
		}
	}
/*ldo 11*/
	if (IS_ERR_OR_NULL(cam_af_regulator)) {
		cam_af_regulator = regulator_get(NULL, "cam_af");
		if (IS_ERR_OR_NULL(cam_af_regulator)) {
			pr_err("failed to get cam_af regulator");
			return -EINVAL;
		}
	}
/*ldo 12*/
	if (IS_ERR_OR_NULL(cam_sensor_core_regulator)) {
		cam_sensor_core_regulator = regulator_get(NULL, "cam_sensor");
		if (IS_ERR_OR_NULL(cam_sensor_core_regulator)) {
			pr_err("failed to get cam_sensor regulator");
			return -EINVAL;
		}
	}
/*ldo 13*/
	if (IS_ERR_OR_NULL(cam_vga_vddio_regulator)) {
		cam_vga_vddio_regulator = regulator_get(NULL, "vga_avdd");
		if (IS_ERR_OR_NULL(cam_vga_vddio_regulator)) {
			pr_err("failed to get vga_vddio regulator");
			return -EINVAL;
		}
	}

/*ldo 14*/
	if (IS_ERR_OR_NULL(cam_isp_regulator)) {
		cam_isp_regulator = regulator_get(NULL, "cam_isp");
		if (IS_ERR_OR_NULL(cam_isp_regulator)) {
			pr_err("failed to get vga_avdd regulator");
			return -EINVAL;
		}
	}

/*ldo 15*/
	if (IS_ERR_OR_NULL(cam_isp_host_regulator)) {
		cam_isp_host_regulator = regulator_get(NULL, "cam_isp_host");
		if (IS_ERR_OR_NULL(cam_isp_host_regulator)) {
			pr_err("failed to get cam_isp_host regulator");
			return -EINVAL;
		}
	}


/*ldo 16*/
	if (IS_ERR_OR_NULL(cam_vga_dvdd_regulator)) {
		cam_vga_dvdd_regulator = regulator_get(NULL, "vga_dvdd");
		if (IS_ERR_OR_NULL(cam_vga_dvdd_regulator)) {
			pr_err("failed to get vga_dvdd regulator");
			return -EINVAL;
		}
	}
	pr_debug("cam_isp_core_regulator = %p\n", cam_isp_core_regulator);
	pr_debug("cam_isp_host_regulator = %p\n", cam_isp_host_regulator);
	pr_debug("cam_af_regulator = %p\n", cam_af_regulator);
	pr_debug("cam_sensor_core_regulator = %p\n", cam_sensor_core_regulator);
	pr_debug("cam_vga_vddio_regulator = %p\n", cam_vga_vddio_regulator);
	pr_debug("cam_isp_regulator = %p\n", cam_isp_regulator);
	pr_debug("cam_vga_dvdd_regulator = %p\n", cam_vga_dvdd_regulator);
	return 0;
}

static int camera_ldo_en(bool en)
{
	int err = 0;
	int result;

	if (IS_ERR_OR_NULL(cam_isp_core_regulator) ||
		IS_ERR_OR_NULL(cam_isp_host_regulator) ||
		IS_ERR_OR_NULL(cam_af_regulator) || //) {// ||
		IS_ERR_OR_NULL(cam_sensor_core_regulator) ||
		IS_ERR_OR_NULL(cam_vga_vddio_regulator) ||
		IS_ERR_OR_NULL(cam_isp_regulator) ||
		IS_ERR_OR_NULL(cam_vga_dvdd_regulator)) {
		pr_err("Camera regulators not initialized\n");
		return -EINVAL;
	}

	if (!en)
		goto off;

	err = regulator_enable(cam_isp_core_regulator);
	if (err) {
		pr_err("Failed to enable regulator cam_isp_core\n");
		goto off;
	}
	mdelay(1);

	err = regulator_enable(cam_af_regulator);
	if (err) {
		pr_err("Failed to enable regulator cam_af\n");
		goto off;
	}
	udelay(50);

	err = regulator_enable(cam_sensor_core_regulator);
	if (err) {
		pr_err("Failed to enable regulator cam_sensor\n");
		goto off;
	}
	udelay(50);

	err = regulator_enable(cam_vga_dvdd_regulator);
	if (err) {
		pr_err("Failed to enable regulator cam_vga_dvdd\n");
		goto off;
	}
	udelay(50);

	err = regulator_enable(cam_vga_vddio_regulator);
	if (err) {
		pr_err("Failed to enable regulator cam_vga_vddio\n");
		goto off;
	}
	udelay(50);

	err = regulator_enable(cam_isp_regulator);
	if (err) {
		pr_err("Failed to enable regulator cam_isp_regulator\n");
		goto off;
	}
	udelay(50);

	err = regulator_enable(cam_isp_host_regulator);
	if (err) {
		pr_err("Failed to enable regulator cam_isp_host\n");
		goto off;
	}
	udelay(50);



	return 0;

off:
	result = err;

	gpio_set_value(GPIO_CAM_ANALOG_EN, 0);

	/* ldo 11 */
	err = regulator_disable(cam_af_regulator);
	if (err) {
		pr_err("Failed to disable regulator cam_af\n");
		result = err;
	}
	/* ldo 12 */
	err = regulator_disable(cam_sensor_core_regulator);
	if (err) {
		pr_err("Failed to disable regulator cam_sensor\n");
		result = err;
	}
	/* ldo 13 */
	err = regulator_disable(cam_vga_vddio_regulator);
	if (err) {
		pr_err("Failed to disable regulator cam_vga_vddio\n");
		result = err;
	}
	/* ldo 16 */
	err = regulator_disable(cam_vga_dvdd_regulator);
	if (err) {
		pr_err("Failed to disable regulator cam_vga_dvdd\n");
		result = err;
	}
	/* ldo 15 */
	err = regulator_disable(cam_isp_host_regulator);
	if (err) {
		pr_err("Failed to disable regulator cam_isp_host\n");
		result = err;
	}

	err = regulator_disable(cam_isp_regulator);
	if (err) {
		pr_err("Failed to disable regulator cam_isp_regulator\n");
		result = err;
	}
	/* BUCK 4 */
	err = regulator_disable(cam_isp_core_regulator);
	if (err) {
		pr_err("Failed to disable regulator cam_isp_core\n");
		result = err;
	}
	return result;
}
#endif

#ifdef CONFIG_VIDEO_CE147
/*
 * Guide for Camera Configuration for Jupiter board
 * ITU CAM CH A: CE147
*/

static bool ce147_powered_on;


static void ce147_init(void)
{
	/* GPIO_CAM_ANALOG_EN - GPJ1(0) */
	if (gpio_request(GPIO_CAM_ANALOG_EN, "GPIO_CAM_ANALOG_EN") < 0)
		pr_err("failed gpio_request(GPIO_CAM_ANALOG_EN) for camera control\n");
	/* CAM_MEGA_nRST - GPJ1(5) */
	if (gpio_request(GPIO_CAM_MEGA_nRST, "GPIO_CAM_MEGA_nRST") < 0)
		pr_err("failed gpio_request(GPIO_CAM_MEGA_nRST) for camera control\n");
	/* CAM_MEGA_EN - GPJ0(6) */
	if (gpio_request(GPIO_CAM_MEGA_EN, "GPIO_CAM_MEGA_EN") < 0)
		pr_err("failed gpio_request(GPIO_CAM_MEGA_EN) for camera control\n");
}


static int ce147_power_on(void)
{	
	int err;

	if (camera_regulator_init()) {
			pr_err("Failed to initialize camera regulators\n");
			return -EINVAL;
	}
	
	ce147_init();

	/* CAM_VGA_nSTBY - GPB(0)  */
	err = gpio_request(GPIO_CAM_VGA_nSTBY, "GPB0");

	if (err) {
		printk(KERN_ERR "failed to request GPB0 for camera control\n");

		return err;
	}

	/* CAM_VGA_nRST - GPB(2) */
	err = gpio_request(GPIO_CAM_VGA_nRST, "GPB2");

	if (err) {
		printk(KERN_ERR "failed to request GPB2 for camera control\n");

		return err;
	}
	err = camera_ldo_en(true);
	if(err)
	{	printk(KERN_ERR "failed to camera_ldo_en\n");

		return err;
	}
	
	/* Turn GPIO_CAM_ANALOG_EN */
	gpio_direction_output(GPIO_CAM_ANALOG_EN, 0);
	gpio_set_value(GPIO_CAM_ANALOG_EN, 1);
	mdelay(2);

	// CAM_VGA_nSTBY  HIGH		
	gpio_direction_output(GPIO_CAM_VGA_nSTBY, 0);

	gpio_set_value(GPIO_CAM_VGA_nSTBY, 1);

	mdelay(1);

	// Mclk enable
	s3c_gpio_cfgpin(GPIO_CAM_MCLK, S3C_GPIO_SFN(0x02));

	mdelay(1);

	// CAM_VGA_nRST  HIGH		
	gpio_direction_output(GPIO_CAM_VGA_nRST, 0);

	gpio_set_value(GPIO_CAM_VGA_nRST, 1);	

	mdelay(1);

	// CAM_VGA_nSTBY  LOW	
	gpio_direction_output(GPIO_CAM_VGA_nSTBY, 1);

	gpio_set_value(GPIO_CAM_VGA_nSTBY, 0);

	mdelay(1);

	// CAM_MEGA_EN HIGH
	gpio_direction_output(GPIO_CAM_MEGA_EN, 0);

	gpio_set_value(GPIO_CAM_MEGA_EN, 1);

	mdelay(1);

	// CAM_MEGA_nRST HIGH
	gpio_direction_output(GPIO_CAM_MEGA_nRST, 0);

	gpio_set_value(GPIO_CAM_MEGA_nRST, 1);

	gpio_free(GPIO_CAM_MEGA_EN);
	gpio_free(GPIO_CAM_MEGA_nRST);
	gpio_free(GPIO_CAM_VGA_nSTBY);
	gpio_free(GPIO_CAM_VGA_nRST);
	gpio_free(GPIO_CAM_ANALOG_EN);

	mdelay(5);

	return 0;
}


static int ce147_power_off(void)
{
	int err;

	/* GPIO_CAM_ANALOG_EN - GPJ1(0) */
	err = gpio_request(GPIO_CAM_ANALOG_EN, "GPIO_CAM_ANALOG_EN");
	
	if(err) {
		printk(KERN_ERR "failed to request GPIO_CAM_ANALOG_EN for camera control\n");
	
		return err;
	}

	/* CAM_MEGA_EN - GPJ0(6) */
	err = gpio_request(GPIO_CAM_MEGA_EN, "GPIO_CAM_MEGA_EN");

	if(err) {
		printk(KERN_ERR "failed to request GPIO_CAM_MEGA_EN for camera control\n");
	
		return err;
	}

	/* CAM_MEGA_nRST - GPJ1(5) */
	err = gpio_request(GPIO_CAM_MEGA_nRST, "GPIO_CAM_MEGA_nRST");
	
	if(err) {
		printk(KERN_ERR "failed to request GPIO_CAM_MEGA_nRST for camera control\n");
	
		return err;
	}

	/* CAM_VGA_nRST - GPB(2) */
	err = gpio_request(GPIO_CAM_VGA_nRST, "GPIO_CAM_VGA_nRST");

	if (err) {
		printk(KERN_ERR "failed to request GPIO_CAM_VGA_nRST for camera control\n");

		return err;
	}
	/* CAM_VGA_nSTBY - GPB(0)  */
	err = gpio_request(GPIO_CAM_VGA_nSTBY, "GPIO_CAM_VGA_nSTBY");

	if (err) {
		printk(KERN_ERR "failed to request GPIO_CAM_VGA_nSTBY for camera control\n");

		return err;
	}

	// CAM_VGA_nSTBY  LOW	
	gpio_direction_output(GPIO_CAM_VGA_nSTBY, 1);

	gpio_set_value(GPIO_CAM_VGA_nSTBY, 0);

	mdelay(1);

	// CAM_VGA_nRST  LOW		
	gpio_direction_output(GPIO_CAM_VGA_nRST, 1);
	
	gpio_set_value(GPIO_CAM_VGA_nRST, 0);

	mdelay(1);

	// CAM_MEGA_nRST - GPJ1(5) LOW
	gpio_direction_output(GPIO_CAM_MEGA_nRST, 1);
	
	gpio_set_value(GPIO_CAM_MEGA_nRST, 0);
	
	mdelay(1);

	// Mclk disable
	s3c_gpio_cfgpin(GPIO_CAM_MCLK, 0);
	
	mdelay(1);

	// CAM_MEGA_EN - GPJ0(6) LOW
	gpio_direction_output(GPIO_CAM_MEGA_EN, 1);
	
	gpio_set_value(GPIO_CAM_MEGA_EN, 0);

	mdelay(1);

	camera_ldo_en(false);

	mdelay(1);
	
	gpio_free(GPIO_CAM_MEGA_EN);
	gpio_free(GPIO_CAM_MEGA_nRST);
	gpio_free(GPIO_CAM_VGA_nRST);
	gpio_free(GPIO_CAM_VGA_nSTBY);
	gpio_free(GPIO_CAM_ANALOG_EN);

	return 0;
}


static int ce147_power_en(int onoff)
{
/*	int bd_level; // unused variable */
	int err = 0;


	if (onoff != ce147_powered_on) {
		if (onoff)
			err = ce147_power_on();
		else {
			err = ce147_power_off();
			s3c_i2c0_force_stop();
		}
		if (!err)
			ce147_powered_on = onoff;
	}

	return 0;
}

/*
 * Guide for Camera Configuration for Jupiter board
 * ITU CAM CH A: CE147
*/

/* External camera module setting */
static struct ce147_platform_data ce147_plat = {
	.default_width = 640,
	.default_height = 480,
	.pixelformat = V4L2_PIX_FMT_UYVY,
	.freq = 24000000,
	.is_mipi = 0,
	.power_en = ce147_power_en,
};

static struct i2c_board_info  ce147_i2c_info = {
	I2C_BOARD_INFO("CE147", 0x78>>1),
	.platform_data = &ce147_plat,
};

static struct s3c_platform_camera ce147 = {
	.id		= CAMERA_PAR_A,
	.type		= CAM_TYPE_ITU,
	.fmt		= ITU_601_YCBCR422_8BIT,
	.order422	= CAM_ORDER422_8BIT_CBYCRY,
	.i2c_busnum	= 0,
	.info		= &ce147_i2c_info,
	.pixelformat	= V4L2_PIX_FMT_UYVY,
	.srclk_name	= "xusbxti",
	.clk_name	= "sclk_cam",//"sclk_cam0",
	.clk_rate	= 24000000,
	.line_length	= 1920,
	.width		= 640,
	.height		= 480,
	.window		= {
		.left	= 0,
		.top	= 0,
		.width	= 640,
		.height	= 480,
	},

	// Polarity 
	.inv_pclk	= 0,
	.inv_vsync	= 1,
	.inv_href	= 0,
	.inv_hsync	= 0,

	.initialized	= 0,
	.cam_power	= ce147_power_en,
};
#endif

#ifdef CONFIG_VIDEO_S5KA3DFX
/* External camera module setting */
static DEFINE_MUTEX(s5ka3dfx_lock);
static bool s5ka3dfx_powered_on;

static int s5ka3dfx_request_gpio(void)
{
	int err;

	/* CAM_VGA_nSTBY - GPB(0) */
	err = gpio_request(GPIO_CAM_VGA_nSTBY, "GPIO_CAM_VGA_nSTBY");
	if (err) {
		pr_err("Failed to request GPIO_CAM_VGA_nSTBY for camera control\n");
		return -EINVAL;
	}

	/* CAM_VGA_nRST - GPB(2) */
	err = gpio_request(GPIO_CAM_VGA_nRST, "GPIO_CAM_VGA_nRST");
	if (err) {
		pr_err("Failed to request GPIO_CAM_VGA_nRST for camera control\n");
		gpio_free(GPIO_CAM_VGA_nSTBY);
		return -EINVAL;
	}

	err = gpio_request(GPIO_CAM_ANALOG_EN, "GPIO_CAM_ANALOG_EN");

	if(err) {
		pr_err("Failed to request GPIO_CAM_ANALOG_EN for camera control\n");
		gpio_free(GPIO_CAM_VGA_nSTBY);
		gpio_free(GPIO_CAM_VGA_nRST);
		return -EINVAL;
	}
	/*
	err = gpio_request(GPIO_CAM_MEGA_EN, "GPIO_CAM_MEGA_EN");

	if(err) {
		pr_err("Failed to request GPIO_CAM_MEGA_EN for camera control\n");
		gpio_free(GPIO_CAM_ANALOG_EN);
		gpio_free(GPIO_CAM_VGA_nRST);
		gpio_free(GPIO_CAM_VGA_nSTBY);
		return -EINVAL;
	}*/

	return 0;
}


static int s5ka3dfx_power_on(void)
{
	int err = 0;

	if (camera_regulator_init()) {
		pr_err("Failed to get all regulator\n");
		return -EINVAL;
	}

	s5ka3dfx_request_gpio();

	gpio_direction_output(GPIO_CAM_ANALOG_EN, 0);
	gpio_set_value(GPIO_CAM_ANALOG_EN, 1);

	err = camera_ldo_en(true);
	if(err){
		pr_err("Failed camera_ldo_en\n");
		return -EINVAL;
	}
	mdelay(1);

	/* CAM_VGA_nSTBY HIGH */
	gpio_direction_output(GPIO_CAM_VGA_nSTBY, 0);
	gpio_set_value(GPIO_CAM_VGA_nSTBY, 1);

	mdelay(1);

//bada enable clocks here
	/* Mclk enable */
	s3c_gpio_cfgpin(GPIO_CAM_MCLK, S3C_GPIO_SFN(0x02));
	udelay(500);


	/* CAM_VGA_nRST HIGH */
	gpio_direction_output(GPIO_CAM_VGA_nRST, 0);
	gpio_set_value(GPIO_CAM_VGA_nRST, 1);
	mdelay(10);

	return 0;
}

static int s5ka3dfx_power_off(void)
{
	int err;


	err = camera_ldo_en(false);
	if (err) {
		pr_err("Failed camera_ldo_en\n");
		return -EINVAL;
	}

	/* CAM_VGA_nRST LOW */
	gpio_direction_output(GPIO_CAM_VGA_nRST, 1);
	gpio_set_value(GPIO_CAM_VGA_nRST, 0);
	udelay(430);

	/* Mclk disable */
	s3c_gpio_cfgpin(GPIO_CAM_MCLK, 0);

	udelay(1);

	gpio_set_value(GPIO_CAM_ANALOG_EN, 0);


	gpio_free(GPIO_CAM_ANALOG_EN);
	gpio_free(GPIO_CAM_VGA_nRST);
	gpio_free(GPIO_CAM_VGA_nSTBY);

	return err;
}

static int s5ka3dfx_power_en(int onoff)
{
	int err = 0;
	mutex_lock(&s5ka3dfx_lock);
	/* we can be asked to turn off even if we never were turned
	 * on if something odd happens and we are closed
	 * by camera framework before we even completely opened.
	 */
	if (onoff != s5ka3dfx_powered_on) {
		if (onoff)
			err = s5ka3dfx_power_on();
		else {
			err = s5ka3dfx_power_off();
			s3c_i2c0_force_stop();
		}
		if (!err)
			s5ka3dfx_powered_on = onoff;
	}
	mutex_unlock(&s5ka3dfx_lock);

	return err;
}

static struct s5ka3dfx_platform_data s5ka3dfx_plat = {
	.default_width = 640,
	.default_height = 480,
	.pixelformat = V4L2_PIX_FMT_UYVY,
	.freq = 24000000,
	.is_mipi = 0,

	.cam_power = s5ka3dfx_power_en,
};

static struct i2c_board_info s5ka3dfx_i2c_info = {
	I2C_BOARD_INFO("S5KA3DFX", 0xc4>>1),
	.platform_data = &s5ka3dfx_plat,
};

static struct s3c_platform_camera s5ka3dfx = {
	.id		= CAMERA_PAR_A,
	.type		= CAM_TYPE_ITU,
	.fmt		= ITU_601_YCBCR422_8BIT,
	.order422	= CAM_ORDER422_8BIT_CBYCRY,
	.i2c_busnum	= 0,
	.info		= &s5ka3dfx_i2c_info,
	.pixelformat	= V4L2_PIX_FMT_UYVY,
	.srclk_name	= "xusbxti",
	.clk_name	= "sclk_cam",
	.clk_rate	= 24000000,
	.line_length	= 480,
	.width		= 640,
	.height		= 480,
	.window		= {
		.left	= 0,
		.top	= 0,
		.width	= 640,
		.height	= 480,
	},

	/* Polarity */
	.inv_pclk	= 0,
	.inv_vsync	= 1,
	.inv_href	= 0,
	.inv_hsync	= 0,

	.initialized	= 0,
	.cam_power	= s5ka3dfx_power_en,
};
#endif

/* Interface setting */
static struct s3c_platform_fimc fimc_plat_lsi = {
	.srclk_name	= "mout_mpll",
	.clk_name	= "sclk_fimc",
	.lclk_name	= "fimc",
	.clk_rate	= 166750000,
	.default_cam	= CAMERA_PAR_A,
	.camera		= {
		&ce147,
		&s5ka3dfx,
	},
	.hw_ver		= 0x43,
};

#ifdef CONFIG_VIDEO_JPEG_V2
static struct s3c_platform_jpeg jpeg_plat __initdata = {
	.max_main_width	= 800,
	.max_main_height	= 480,
	.max_thumb_width	= 320,
	.max_thumb_height	= 240,
};
#endif

static struct i2c_board_info i2c_devs4[] __initdata = {
	{
		I2C_BOARD_INFO("wm8994-samsung", (0x34>>1)),
		.platform_data = &wm8994_pdata,
	},
};

static void mxt224_power_on(void)
{
	gpio_direction_output(GPIO_TOUCH_EN, 1);

	mdelay(40);
}

static void mxt224_power_off(void)
{
	gpio_direction_output(GPIO_TOUCH_EN, 0);
}

#define MXT224_MAX_MT_FINGERS 5

static u8 t7_config[] = {GEN_POWERCONFIG_T7,
64, 255, 50};
static u8 t8_config[] = {GEN_ACQUISITIONCONFIG_T8,
7, 0, 5, 0, 0, 0, 9, 35};

static u8 t9_s8530_config[] = {TOUCH_MULTITOUCHSCREEN_T9, // for Wave2
139,
0, 0, //xorigin, yorigin
19,11, //xsize, ysize
0, 33, 30, 2, 7, 0, 3, 1,
46, MXT224_MAX_MT_FINGERS,
5, 40,
10, //amphyst
0, 0, //xrange, yrange
0, 0, 0, 0, 0, 0,
143, 40, //xedgectrl, dist
143, 80, //yedgectrl, dist
18//jumplimit
};

static u8 t9_s8500_config[] = {TOUCH_MULTITOUCHSCREEN_T9, // for Wave1
139,
0, 0, //xorigin, yorigin
16,10, //xsize, ysize
0, 33, 30, 2, 7, 0, 3, 1,
46, MXT224_MAX_MT_FINGERS,
5, 40,
10, //amphyst
0, 0, //xrange, yrange
0, 0, 0, 0, 0, 0,
143, 80, //xedgectrl, dist
143, 120, //yedgectrl, dist
18//jumplimit
};

static u8 t18_config[] = {SPT_COMCONFIG_T18,
0, 1};
static u8 t20_config[] = {PROCI_GRIPFACESUPPRESSION_T20,
7, 0, 0, 0, 0, 0, 0, 80, 40, 4, 35, 10};
static u8 t22_config[] = {PROCG_NOISESUPPRESSION_T22,
5, 0, 0, 0, 0, 0, 0, 3, 30, 0, 0, 29, 34, 39,
49, 58, 3};
static u8 t28_config[] = {SPT_CTECONFIG_T28,
1, 0, 3, 16, 63, 60};
static u8 end_config[] = {RESERVED_T255};
static const u8 *mxt224_config[] = {
	t7_config,
	t8_config,
	NULL, /* place for T9 config */
//	t9_config,
	t18_config,
	t20_config,
	t22_config,
	t28_config,
	end_config,
};

static struct mxt224_platform_data mxt224_data = {
	.max_finger_touches = MXT224_MAX_MT_FINGERS,
	.gpio_read_done = GPIO_TOUCH_INT,
	.config = mxt224_config,
	.min_x = 0,
	.max_x = 1023,
	.min_y = 0,
	.max_y = 1023,
	.min_z = 0,
	.max_z = 255,
	.min_w = 0,
	.max_w = 30,
	.power_on = mxt224_power_on,
	.power_off = mxt224_power_off,
};

/* I2C2 */
static struct i2c_board_info i2c_devs2[] __initdata = {
	{
		I2C_BOARD_INFO(MXT224_DEV_NAME, 0x4a),
		.platform_data = &mxt224_data,
		.irq = IRQ_EINT_GROUP(18, 5),
	},
};

static void mxt224_init(void)
{
	if(machine_is_wave2())
		mxt224_config[2] = t9_s8530_config;
	else
		mxt224_config[2] = t9_s8500_config;
	return;
}

static struct i2c_board_info i2c_devs5[] __initdata = {
#if defined(CONFIG_SAMSUNG_FASCINATE)
	{
		I2C_BOARD_INFO("kr3dh", 0x19),
	},
#else
	{
		I2C_BOARD_INFO("bma023", 0x38),
	},
#endif
};

static struct i2c_board_info i2c_devs8[] __initdata = {
	{
		I2C_BOARD_INFO("Si4709", (0x20 >> 1)),
	},
};

static void fsa9480_usb_cb(bool attached)
{
	struct usb_gadget *gadget = platform_get_drvdata(&s3c_device_usbgadget);

	if (gadget) {
		if (attached)
			usb_gadget_vbus_connect(gadget);
		else
			usb_gadget_vbus_disconnect(gadget);
	}

	set_cable_status = attached ? CABLE_TYPE_USB : CABLE_TYPE_NONE;
	if (charger_callbacks && charger_callbacks->set_cable)
		charger_callbacks->set_cable(charger_callbacks, set_cable_status);
}

static void fsa9480_charger_cb(bool attached)
{
	set_cable_status = attached ? CABLE_TYPE_AC : CABLE_TYPE_NONE;
	if (charger_callbacks && charger_callbacks->set_cable)
		charger_callbacks->set_cable(charger_callbacks, set_cable_status);
}

static struct switch_dev switch_dock = {
	.name = "dock",
};

static void fsa9480_deskdock_cb(bool attached)
{
	struct usb_gadget *gadget = platform_get_drvdata(&s3c_device_usbgadget);

	if (attached)
		switch_set_state(&switch_dock, 1);
	else
		switch_set_state(&switch_dock, 0);

	if (gadget) {
		if (attached)
			usb_gadget_vbus_connect(gadget);
		else
			usb_gadget_vbus_disconnect(gadget);
	}

	set_cable_status = attached ? CABLE_TYPE_USB : CABLE_TYPE_NONE;
	if (charger_callbacks && charger_callbacks->set_cable)
		charger_callbacks->set_cable(charger_callbacks, set_cable_status);
}

static void fsa9480_cardock_cb(bool attached)
{
	if (attached)
		switch_set_state(&switch_dock, 2);
	else
		switch_set_state(&switch_dock, 0);
}

static void fsa9480_reset_cb(void)
{
	int ret;

	/* for CarDock, DeskDock */
	ret = switch_dev_register(&switch_dock);
	if (ret < 0)
		pr_err("Failed to register dock switch. %d\n", ret);
}

static struct fsa9480_platform_data fsa9480_pdata = {
	.usb_cb = fsa9480_usb_cb,
	.charger_cb = fsa9480_charger_cb,
	.deskdock_cb = fsa9480_deskdock_cb,
	.cardock_cb = fsa9480_cardock_cb,
	.reset_cb = fsa9480_reset_cb,
};

static struct i2c_board_info i2c_devs7[] __initdata = {
	{
		I2C_BOARD_INFO("fsa9480", 0x4A >> 1),
		.platform_data = &fsa9480_pdata,
		.irq = IRQ_EINT(23),
	},
};

static struct i2c_board_info i2c_devs6[] __initdata = {
#ifdef CONFIG_REGULATOR_MAX8998
	{
		/* The address is 0xCC used since SRAD = 0 */
		I2C_BOARD_INFO("max8998", (0xCC >> 1)),
		.platform_data	= &max8998_pdata,
		.irq		= IRQ_EINT7,
	},
/* MAX8998 MFC Driver will register it, so no need to even try it
	{
		I2C_BOARD_INFO("rtc_max8998", (0x0C >> 1)),
	},
*/
#endif
};

static int gp2a_power(bool on)
{
	/* this controls the power supply rail to the gp2a IC */
	gpio_direction_output(GPIO_PS_ON, on);
	return 0;
}


static struct gp2a_platform_data gp2a_pdata = {
	.power = gp2a_power,
	.p_out = GPIO_PS_VOUT,
};

static void gp2a_gpio_init(void)
{
	int ret = gpio_request(GPIO_PS_ON, "gp2a_power_supply_on");
	if (ret)
		printk(KERN_ERR "Failed to request gpio gp2a power supply.\n");

#ifdef CONFIG_SAMSUNG_FASCINATE
        s3c_gpio_cfgpin(GPIO_PS_VOUT, S3C_GPIO_SFN(GPIO_PS_VOUT_AF));
        s3c_gpio_setpull(GPIO_PS_VOUT, S3C_GPIO_PULL_NONE);
        irq_set_irq_type(IRQ_EINT1, IRQ_TYPE_EDGE_BOTH);
        gp2a_pdata.p_irq = gpio_to_irq(GPIO_PS_VOUT);
        gp2a_pdata.p_out = GPIO_PS_VOUT;
#endif
}

static struct i2c_board_info i2c_dev_gp2a[] __initdata = {
	{
		I2C_BOARD_INFO("gp2a", (0x88 >> 1)),
		.platform_data = &gp2a_pdata,
	},
};

static struct akm8973_platform_data akm8973_pdata = {
	.reset_line = GPIO_MSENSE_nRST,
	.reset_asserted = GPIO_LEVEL_LOW,
	.gpio_data_ready_int = GPIO_MSENSE_IRQ,
};

static struct i2c_board_info i2c_devs12[] __initdata = {
	{
		I2C_BOARD_INFO("ak8973", 0x1c),
		.platform_data = &akm8973_pdata,
	},
};

static struct resource ram_console_resource[] = {
	{
		.flags = IORESOURCE_MEM,
	}
};

static struct platform_device ram_console_device = {
	.name = "ram_console",
	.id = -1,
	.num_resources = ARRAY_SIZE(ram_console_resource),
	.resource = ram_console_resource,
};

#ifdef CONFIG_ANDROID_PMEM
static struct android_pmem_platform_data pmem_pdata = {
	.name = "pmem",
	.no_allocator = 1,
	.cached = 1,
	.start = 0,
	.size = 0,
};

static struct android_pmem_platform_data pmem_gpu1_pdata = {
	.name = "pmem_gpu1",
	.no_allocator = 1,
	.cached = 1,
	.buffered = 1,
	.start = 0,
	.size = 0,
};

static struct android_pmem_platform_data pmem_adsp_pdata = {
	.name = "pmem_adsp",
	.no_allocator = 1,
	.cached = 1,
	.buffered = 1,
	.start = 0,
	.size = 0,
};

static struct platform_device pmem_device = {
	.name = "android_pmem",
	.id = 0,
	.dev = { .platform_data = &pmem_pdata },
};

static struct platform_device pmem_gpu1_device = {
	.name = "android_pmem",
	.id = 1,
	.dev = { .platform_data = &pmem_gpu1_pdata },
};

static struct platform_device pmem_adsp_device = {
	.name = "android_pmem",
	.id = 2,
	.dev = { .platform_data = &pmem_adsp_pdata },
};

static void __init android_pmem_set_platdata(void)
{
	pmem_pdata.start = (u32)s5p_get_media_memory_bank(S5P_MDEV_PMEM, 0);
	pmem_pdata.size = (u32)s5p_get_media_memsize_bank(S5P_MDEV_PMEM, 0);

	pmem_gpu1_pdata.start =
		(u32)s5p_get_media_memory_bank(S5P_MDEV_PMEM_GPU1, 0);
	pmem_gpu1_pdata.size =
		(u32)s5p_get_media_memsize_bank(S5P_MDEV_PMEM_GPU1, 0);

	pmem_adsp_pdata.start =
		(u32)s5p_get_media_memory_bank(S5P_MDEV_PMEM_ADSP, 0);
	pmem_adsp_pdata.size =
		(u32)s5p_get_media_memsize_bank(S5P_MDEV_PMEM_ADSP, 0);
}
#endif

struct platform_device wave_charger_device = {
	.name	= "wave_charger",
	.id	= -1,
};

static struct platform_device sec_device_rfkill = {
	.name	= "bt_rfkill",
	.id	= -1,
};

static struct platform_device sec_device_btsleep = {
	.name	= "bt_sleep",
	.id	= -1,
};

static struct sec_jack_zone sec_jack_zones[] = {
	{
		/* adc == 0, unstable zone, default to 3pole if it stays
		 * in this range for a half second (20ms delays, 25 samples)
		 */
		.adc_high = 0,
		.delay_ms = 20,
		.check_count = 25,
		.jack_type = SEC_HEADSET_3POLE,
	},
#if defined(CONFIG_SAMSUNG_CAPTIVATE)
	{
		/* 0 < adc <= 700, unstable zone, default to 3pole if it stays
		 * in this range for a second (10ms delays, 80 samples)
		 */
		.adc_high = 700,
		.delay_ms = 10,
		.check_count = 80,
		.jack_type = SEC_HEADSET_3POLE,
	},
	{
		/* 700 < adc <= 2500, default to 4pole if it
		 * stays in this range for 800ms second (10ms delays, 80 samples)
		 */
		.adc_high = 2500,
		.delay_ms = 10,
		.check_count = 80,
		.jack_type = SEC_HEADSET_4POLE,
	},
#elif defined(CONFIG_SAMSUNG_VIBRANT)
	{
		/* 0 < adc <= 500, unstable zone, default to 3pole if it stays
		 * in this range for 800ms (10ms delays, 80 samples)
		 */
		.adc_high = 500,
		.delay_ms = 10,
		.check_count = 80,
		.jack_type = SEC_HEADSET_3POLE,
	},
	{
		/* 500 < adc <= 3300, default to 4pole if it
		 * stays in this range for 800ms (10ms delays, 80 samples)
		 */
		.adc_high = 3300,
		.delay_ms = 10,
		.check_count = 80,
		.jack_type = SEC_HEADSET_4POLE,
	},
#else
	{
		/* 0 < adc <= 900, unstable zone, default to 3pole if it stays
		 * in this range for 800ms (10ms delays, 80 samples)
		 */
		.adc_high = 900,
		.delay_ms = 10,
		.check_count = 80,
		.jack_type = SEC_HEADSET_3POLE,
	},
	{
		/* 900 < adc <= 2000, unstable zone, default to 4pole if it
		 * stays in this range for 800ms (10ms delays, 80 samples)
		 */
		.adc_high = 2000,
		.delay_ms = 10,
		.check_count = 80,
		.jack_type = SEC_HEADSET_4POLE,
	},
	{
		/* 2000 < adc <= 3400, 4 pole zone, default to 4pole if it
		 * stays in this range for 100ms (10ms delays, 10 samples)
		 */
		.adc_high = 3400,
		.delay_ms = 10,
		.check_count = 10,
		.jack_type = SEC_HEADSET_4POLE,
	},
#endif
	{
		/* adc > max for device above, unstable zone, default to 3pole if it stays
		 * in this range for two seconds (10ms delays, 200 samples)
		 */
		.adc_high = 0x7fffffff,
		.delay_ms = 10,
		.check_count = 200,
		.jack_type = SEC_HEADSET_3POLE,
	},
};

/* To support 3-buttons earjack */
static struct sec_jack_buttons_zone sec_jack_buttons_zones[] = {
#if defined(CONFIG_SAMSUNG_CAPTIVATE) || defined(CONFIG_SAMSUNG_VIBRANT)
	{
		/* 0 <= adc <=110, stable zone */
		.code		= KEY_MEDIA,
		.adc_low	= 0,
		.adc_high	= 110,
	},
	{
		/* 130 <= adc <= 365, stable zone */
		.code		= KEY_PREVIOUSSONG,
		.adc_low	= 130,
		.adc_high	= 365,
	},
	{
		/* 385 <= adc <= 870, stable zone */
		.code		= KEY_NEXTSONG,
		.adc_low	= 385,
		.adc_high	= 870,
	},
#else
	{
		/* 0 <= adc <=1000, stable zone */
		.code		= KEY_MEDIA,
		.adc_low	= 0,
		.adc_high	= 1000,
	},
#endif
};

static int sec_jack_get_adc_value(void)
{
    pr_info("%s: sec_jack adc value = %i \n", __func__, s3c_adc_get_adc_data(3));
	return s3c_adc_get_adc_data(3);
}

struct sec_jack_platform_data sec_jack_pdata = {
	.set_micbias_state = sec_jack_set_micbias_state,
	.get_adc_value = sec_jack_get_adc_value,
	.zones = sec_jack_zones,
	.num_zones = ARRAY_SIZE(sec_jack_zones),
	.buttons_zones = sec_jack_buttons_zones,
	.num_buttons_zones = ARRAY_SIZE(sec_jack_buttons_zones),
	.det_gpio = GPIO_DET_35,
#if defined(CONFIG_SAMSUNG_CAPTIVATE)
	.send_end_gpio = GPIO_EAR_SEND_END35,
#else
	.send_end_gpio = GPIO_EAR_SEND_END,
#endif
};

static struct platform_device sec_device_jack = {
	.name			= "sec_jack",
	.id			= 1, /* will be used also for gpio_event id */
	.dev.platform_data	= &sec_jack_pdata,
};


#define S3C_GPIO_SETPIN_ZERO         0
#define S3C_GPIO_SETPIN_ONE          1
#define S3C_GPIO_SETPIN_NONE	     2

// just4info
// S3C_GPIO_SFN(0xF) = S3C_GPIO_EINT = S3C_GPIO_SPECIAL(0xF)

struct gpio_init_data {
	uint num;
	uint cfg;
	uint val;
	uint pud;
	uint drv;
};

static struct gpio_init_data wave_init_gpios[] = {

    // GPB -----------------------------
	{
		.num	= GPIO_CAM_VGA_nSTBY,
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPB(1), // GPIO_MSENSE_nRST
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPB(2), // GPIO_CAM_VGA_nRST
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= GPIO_BT_nRST, // GPIO_BT_nRST
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPB(4), // GPIO_BOOT_MODE
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPB(5), // GPIO_WLAN_BT_EN	
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= GPIO_GPB6, //NC
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPB(7), //NC
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPC0 ----------------------------
	{
		.num	= S5PV210_GPC0(0), //NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC0(1), //NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC0(2), //NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC0(3), // NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC0(4), // NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPC1 ----------------------------
	{
		.num	= S5PV210_GPC1(0), // BLOW_PCM_CLK
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC1(1),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC1(2),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC1(3),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPC1(4),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPD0 ----------------------------
	{
		.num	= GPIO_LCD_BL_PWM,
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPD0(1), // NC
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= GPIO_VIBTONE_PWM,
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPD0(3),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPD1 ----------------------------
	{
		.num	= S5PV210_GPD1(0), // GPIO_CAM_SDA_29V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPD1(1), // GPIO_CAM_SCL_29V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPD1(2), // GPIO_FM_SDA_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPD1(3), // GPIO_FM_SCL_28V	
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPD1(4), // GPIO_TSP_SDA_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPD1(5), // GPIO_TSP_SCL_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, 
	
	// GPE0 ----------------------------
	{
		.num	= S5PV210_GPE0(0), // GPIO_CAM_PCLK
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE0(1), // GPIO_CAM_VSYNC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE0(2), // GPIO_CAM_HSYNC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE0(3), // GPIO_CAM_D0
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE0(4), // GPIO_CAM_D1
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE0(5), // GPIO_CAM_D2
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE0(6), // GPIO_CAM_D3
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE0(7), // GPIO_CAM_D4
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPE1 ----------------------------
	{
		.num	= S5PV210_GPE1(0), // GPIO_CAM_D5
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE1(1), // GPIO_CAM_D6
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE1(2), // GPIO_CAM_D7
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE1(3), // GPIO_CAM_MCLK
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPE1(4),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPF3 ----------------------------
	{
		.num	= S5PV210_GPF3(4), // GPIO_CODEC_LDO_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPF3(5), //NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPG0 ----------------------------
	{
		.num	= S5PV210_GPG0(0), // GPIO_NAND_CLK
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG0(1), // GPIO_NAND_CMD
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG0(2), // GPIO_ALS_SCL_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG0(3), // GPIO_NAND_D0
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG0(4), // GPIO_NAND_D1
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG0(5), // GPIO_NAND_D2
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG0(6), // GPIO_NAND_D3
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPG1 ----------------------------
	{
		.num	= S5PV210_GPG1(0), // GPIO_WLAN_SDIO_CLK
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG1(1), // GPIO_WLAN_SDIO_CMD
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG1(2), // GPIO_WLAN_nRST
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG1(3), // GPIO_WLAN_SDIO_D0
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG1(4), // GPIO_WLAN_SDIO_D1 
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG1(5), // GPIO_WLAN_SDIO_D2 
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG1(6), // GPIO_WLAN_SDIO_D3
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPG2 ----------------------------
	{
		.num	= S5PV210_GPG2(0), // GPIO_T_FLASH_CLK
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG2(1), // GPIO_T_FLASH_CMD
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG2(2), // GPIO_ALS_SDA_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG2(3), // GPIO_T_FLASH_D0
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG2(4), // GPIO_T_FLASH_D1
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG2(5), // GPIO_T_FLASH_D2
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG2(6), // GPIO_T_FLASH_D3
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPG3 ----------------------------
	{
		.num	= S5PV210_GPG3(0), // NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG3(1), // NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG3(2), // NC
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG3(3), // GPIO_TA_CURRENT_SEL_AP
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG3(4), // GPIO_BT_WAKE
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= GPIO_WLAN_WAKE, // GPIO_WLAN_WAKE
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPG3(6),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPH0 ----------------------------
	{
		.num	= S5PV210_GPH0(0), // GPIO_AP_PS_HOLD
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH0(1), // GPIO_ACC_INT
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH0(2), // GPIO_PS_VOUT
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH0(3), // GPIO_BUCK_1_EN_A
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH0(4), // GPIO_BUCK_1_EN_B
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH0(5), // GPIO_BUCK_2_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, { /* GPIO_DET_35 - 3.5" ear jack */
		.num	= S5PV210_GPH0(6), // GPIO_DET_35
		.cfg	= S3C_GPIO_SFN(GPIO_DET_35_AF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH0(7), // GPIO_AP_PMIC_IRQ
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPH1 ----------------------------
	{
		.num	= S5PV210_GPH1(0),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH1(1),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH1(2),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH1(3),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, { /* NFC_IRQ */
		.num	= S5PV210_GPH1(4),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, { /* NFC_EN */
		.num	= S5PV210_GPH1(5),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, { /* NFC_FIRM */
		.num	= S5PV210_GPH1(6),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH1(7), // GPIO_PHONE_ACTIVE
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPH2 ----------------------------
	{
		.num	= S5PV210_GPH2(0),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH2(1),
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, { /* GPIO_EAR_SEND_END_35 */
		.num	= S5PV210_GPH2(2), // GPIO_EAR_SEND_END35, GPIO_KBC2
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH2(3),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH2(4), // GPIO_WLAN_HOST_WAKE
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH2(5), // GPIO_BT_HOST_WAKE
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH2(6), // GPIO_nPOWER
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH2(7), // GPIO_JACK_nINT
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPH3 ----------------------------
	{
		.num	= S5PV210_GPH3(0),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH3(1),
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH3(2),
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH3(3),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPH3(4), // GPIO_T_FLASH_DETECT
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
#if defined (CONFIG_SAMSUNG_GALAXYS) || defined (CONFIG_SAMSUNG_GALAXYSB) /* HOME Key */
		.num	= S5PV210_GPH3(5), // GPIO_MSENSE_IRQ
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
#else
		.num	= S5PV210_GPH3(5), // GPIO_MSENSE_IRQ
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
#endif
	}, { /* GPIO_EAR_SEND_END */
#if defined(CONFIG_SAMSUNG_CAPTIVATE) || defined (CONFIG_SAMSUNG_VIBRANT)
		.num	= S5PV210_GPH3(6), // GPIO_EAR_SEND_END
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
#else
		.num	= S5PV210_GPH3(6), // GPIO_EAR_SEND_END
		.cfg	= S3C_GPIO_SFN(GPIO_EAR_SEND_END_AF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
#endif
	}, {
#if defined(CONFIG_SAMSUNG_FASCINATE)
		.num	= S5PV210_GPH3(7), // GPIO_CP_RST	
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
#else
		.num	= S5PV210_GPH3(7), // GPIO_CP_RST	
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
#endif
	},

	// GPI ----------------------------
	{
		.num	= S5PV210_GPI(0), // GPIO_CODEC_I2S_CLK
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPI(1),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPI(2), // GPIO_CODEC_I2S_WS
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPI(3), // GPIO_CODEC_I3S_DI
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPI(4), // GPIO_CODEC_I3S_DO
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPI(5),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPI(6),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPJ0 ----------------------------
	{
		.num	= S5PV210_GPJ0(0), // GPIO_MSENSE_SCL_28V	
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ0(1),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ0(2), // GPIO_HWREV_MODE0
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ0(3), // GPIO_HWREV_MODE1
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ0(4), // GPIO_HWREV_MODE2
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ0(5), // GPIO_TOUCH_INT
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ0(6), // GPIO_CAM_MEGA_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
#if defined(CONFIG_SAMSUNG_GALAXYSB)
		.num	= S5PV210_GPJ0(7), // GPIO_CAM_MEGA_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
#else
		.num	= S5PV210_GPJ0(7), // GPIO_HWREV_MODE3
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
#endif
	},

	// GPJ1 ----------------------------
	{
		.num	= S5PV210_GPJ1(0),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ1(1), // GPIO_VIBTONE_EN1
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ1(2),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ1(3), // GPIO_TOUCH_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ1(4), // GPIO_PS_ON
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ1(5), // GPIO_CAM_MEGA_nRST
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPJ2 ----------------------------
#if defined(CONFIG_SAMSUNG_GALAXYSB)
	{
		.num	= S5PV210_GPJ2(0), // GPIO_CAM_FLASH_EN
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ2(1), // GPIO_CAM_FLASH_SET
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},
#else
	{
		.num	= S5PV210_GPJ2(0),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ2(1),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},
#endif
	{
		.num	= S5PV210_GPJ2(2), // GPIO_OLED_DET
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
#if defined(CONFIG_SAMSUNG_GALAXYSB)
		.num	= S5PV210_GPJ2(3), // GPIO_CODEC_XTAL_EN
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
#else
		.num	= S5PV210_GPJ2(3), // GPIO_CODEC_XTAL_EN
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
#endif
	}, {
		.num	= S5PV210_GPJ2(4), // GPIO_FM_INT
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_UP,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
#if defined(CONFIG_SAMSUNG_VIBRANT)
		.num	= S5PV210_GPJ2(5), // GPIO_MICBIAS_EN2
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
#else
		.num	= S5PV210_GPJ2(5), // GPIO_FM_RST	
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
#endif
	}, {
		.num	= S5PV210_GPJ2(6), // GPIO_EARPATH_SEL
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ2(7), // GPIO_MASSMEMORY_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPJ3 ----------------------------
	{
		.num	= S5PV210_GPJ3(0), // _3_TOUCH_SDA_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ3(1), // _3_TOUCH_SCL_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ3(2), // _3_GPIO_TOUCH_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, { /* GPIO_EAR_ADC_SEL */
		.num	= S5PV210_GPJ3(3), // _3_GPIO_TOUCH_CE
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ3(4), // GPIO_USB_SDA_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ3(5), // GPIO_USB_SCL_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ3(6), // GPIO_AP_SDA_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ3(7), // GPIO_AP_SCL_28V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// GPJ4 ----------------------------
	{
		.num	= S5PV210_GPJ4(0), // GPIO_AP_PMIC_SDA
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ4(1), // _3_GPIO_TOUCH_INT
		.cfg	= S3C_GPIO_SFN(0xF),
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ4(2), // GPIO_MICBIAS_EN
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ4(3), // GPIO_AP_PMIC_SCL
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_GPJ4(4), // GPIO_TV_EN, GPIO_EAR_MICBIAS_EN
#if defined(CONFIG_SAMSUNG_CAPTIVATE)
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
#else
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
#endif
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// MP01 ----------------------------
	{
		.num	= S5PV210_MP01(0),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP01(2), // GPIO_SUB_DISPLAY_CS
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP01(5), // GPIO_DIC_ID
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// MP02 ----------------------------
	{
		.num	= S5PV210_MP02(0),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP02(1),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP02(3),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// MP03 ----------------------------
	{
		.num	= S5PV210_MP03(3), // GPIO_PDA_ACTIVE
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},
	/* leave value set by bootloader */
/*	{
		.num	= S5PV210_MP03(5), // GPIO_CP_nRST
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, 
*/
	{
		.num	= S5PV210_MP03(6),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP03(7), // GPIO_PCM_SEL
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// MP04 ----------------------------
	{
		.num	= S5PV210_MP04(0), // GPIO_USB_SEL
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP04(2),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, 
	/* leave value set by bootloader */
	/*{
		.num	= S5PV210_MP04(4), // GPIO_A1026_SCL
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, */
	{
		.num	= S5PV210_MP04(5), // GPIO_A1026_SDA
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},
	/* leave value set by bootloader */
/*	{
		.num	= S5PV210_MP04(6),
		.cfg	= S3C_GPIO_OUTPUT,
		.val	= S3C_GPIO_SETPIN_ZERO,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},*/
	{
		.num	= S5PV210_MP04(7),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},

	// MP05 ----------------------------
	{
		.num	= S5PV210_MP05(0), // FUEL_SCL_18V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP05(1), // FUEL_SDA_18V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP05(2), // GPIO_AP_SCL_18V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP05(3), // GPIO_AP_SDA_18V
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_NONE,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP05(4),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	}, {
		.num	= S5PV210_MP05(6),
		.cfg	= S3C_GPIO_INPUT,
		.val	= S3C_GPIO_SETPIN_NONE,
		.pud	= S3C_GPIO_PULL_DOWN,
		.drv	= S3C_GPIO_DRVSTR_1X,
	},
};

void s3c_config_gpio_table(void)
{
	u32 i, gpio;

	for (i = 0; i < ARRAY_SIZE(wave_init_gpios); i++) {
		gpio = wave_init_gpios[i].num;
		s3c_gpio_cfgpin(gpio, wave_init_gpios[i].cfg);
		s3c_gpio_setpull(gpio, wave_init_gpios[i].pud);

		if (wave_init_gpios[i].val != S3C_GPIO_SETPIN_NONE)
			gpio_set_value(gpio, wave_init_gpios[i].val);

		s3c_gpio_set_drvstrength(gpio, wave_init_gpios[i].drv);
	}
}


#define S5PV210_PS_HOLD_CONTROL_REG (S3C_VA_SYS+0xE81C)
static void wave_power_off(void)
{
	while (1) {
		/* Check reboot charging */
		if (set_cable_status) {
			/* watchdog reset */
			pr_info("%s: charger connected, rebooting\n", __func__);
			writel(3, S5P_INFORM6);
			arch_reset('r', NULL);
			pr_crit("%s: waiting for reset!\n", __func__);
			while (1);
		}

		/* wait for power button release */
		if (gpio_get_value(GPIO_nPOWER)) {
			if(power_callbacks && power_callbacks->power_off)
				power_callbacks->power_off(power_callbacks);
				
			pr_info("%s: set PS_HOLD low\n", __func__);

			/* PS_HOLD high  PS_HOLD_CONTROL, R/W, 0xE010_E81C */
			writel(readl(S5PV210_PS_HOLD_CONTROL_REG) & 0xFFFFFEFF,
			       S5PV210_PS_HOLD_CONTROL_REG);

			pr_crit("%s: should not reach here!\n", __func__);
		}

		/* if power button is not released, wait and check TA again */
		pr_info("%s: PowerButton is not released.\n", __func__);
		mdelay(1000);
	}
}

static unsigned int wave_sleep_gpio_table[][3] = {

	// GPA0 ---------------------------------------------------
	{ S5PV210_GPA0(0), S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE}, 	//GPIO_BT_UART_RXD
	{ S5PV210_GPA0(1), S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE}, 	//GPIO_BT_UART_TXD
	{ S5PV210_GPA0(2), S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE}, 	//GPIO_BT_UART_CTS
	{ S5PV210_GPA0(3), S3C_GPIO_SLP_OUT1,   S3C_GPIO_PULL_NONE}, 	//GPIO_BT_UART_RTS

  	{ S5PV210_GPA0(4), S3C_GPIO_SLP_PREV,  S3C_GPIO_PULL_NONE},
  	{ S5PV210_GPA0(5), S3C_GPIO_SLP_PREV,  S3C_GPIO_PULL_NONE},
  	{ S5PV210_GPA0(6), S3C_GPIO_SLP_PREV,  S3C_GPIO_PULL_NONE},
  	{ S5PV210_GPA0(7), S3C_GPIO_SLP_PREV,  S3C_GPIO_PULL_NONE},


	// GPA1 ---------------------------------------------------
	{ S5PV210_GPA1(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPA1(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},

    { S5PV210_GPA1(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},
	{ S5PV210_GPA1(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},

	// GPB ----------------------------------------------------
#if defined(CONFIG_SAMSUNG_CAPTIVATE) || defined (CONFIG_SAMSUNG_VIBRANT)
	{ S5PV210_GPB(0),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN}, 	//GPIO_CAM_VGA_nSTBY
	{ S5PV210_GPB(1),  S3C_GPIO_SLP_OUT1,   S3C_GPIO_PULL_NONE},	//GPIO_MSENSE_nRST
	{ S5PV210_GPB(2),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_CAM_VGA_nRST
#else
	{ S5PV210_GPB(0),  S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPB(1),  S3C_GPIO_SLP_OUT1,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPB(2),  S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
#endif

#if defined(CONFIG_SAMSUNG_GALAXYSB)
	{ S5PV210_GPB(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},	// S5PV210_GPJ0(5)
#else
	{ S5PV210_GPB(3),  S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE},
#endif
	{ S5PV210_GPB(4),  S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPB(5),  S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE},
#if defined(CONFIG_SAMSUNG_GALAXYSB)
	{ S5PV210_GPB(6),  S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE},	// S5PV210_GPB(3)
#else
	{ S5PV210_GPB(6),  S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
#endif
	{ S5PV210_GPB(7),  S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},

	// GPC0 ---------------------------------------------------
	{ S5PV210_GPC0(0), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},
	{ S5PV210_GPC0(1), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPC0(2), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},
	{ S5PV210_GPC0(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},
	{ S5PV210_GPC0(4), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},

	// GPC1 ---------------------------------------------------
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPC1(0), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_GPC10
#else
  	{ S5PV210_GPC1(0), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPC10
#endif
	{ S5PV210_GPC1(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPC1(2), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_GPC12
#else
  	{ S5PV210_GPC1(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPC12
#endif
	{ S5PV210_GPC1(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPC1(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// GPD0 ---------------------------------------------------
	{ S5PV210_GPD0(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPD0(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPD0(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPD0(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// GPD1 ---------------------------------------------------
	{ S5PV210_GPD1(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPD1(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
	{ S5PV210_GPD1(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPD12
	{ S5PV210_GPD1(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPD13
#else
  	{ S5PV210_GPD1(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},
  	{ S5PV210_GPD1(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},
#endif
	{ S5PV210_GPD1(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPD1(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// GPE0 ---------------------------------------------------
	{ S5PV210_GPE0(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE0(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE0(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE0(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE0(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE0(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE0(6), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE0(7), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// GPE1 ---------------------------------------------------
	{ S5PV210_GPE1(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE1(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE1(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPE1(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPE1(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// GPF0 ---------------------------------------------------
	{ S5PV210_GPF0(0), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF0(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF0(2), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF0(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF0(4), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF0(5), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF0(6), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF0(7), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},

	// GPF1 ---------------------------------------------------
	{ S5PV210_GPF1(0), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF1(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF1(2), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF1(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF1(4), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF1(5), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF1(6), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF1(7), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},

	// GPF2 ---------------------------------------------------
	{ S5PV210_GPF2(0), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF2(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF2(2), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF2(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF2(4), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF2(5), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF2(6), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF2(7), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},

	// GPF3 ---------------------------------------------------
	{ S5PV210_GPF3(0), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF3(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF3(2), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF3(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF3(4), S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPF3(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// GPG0 ---------------------------------------------------
#if defined (CONFIG_SAMSUNG_GALAXYS) || defined (CONFIG_SAMSUNG_GALAXYSB)
	{ S5PV210_GPG0(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_UP},
	{ S5PV210_GPG0(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_UP},
#else
	{ S5PV210_GPG0(0), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPG0(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
#endif
#if defined (CONFIG_SAMSUNG_CAPTIVATE) || defined(CONFIG_SAMSUNG_FASCINATE)
  	{ S5PV210_GPG0(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	// GPIO_ALS_SCL_28V
#else
  	{ S5PV210_GPG0(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	// GPIO_ALS_SCL_28V
#endif
#if defined (CONFIG_SAMSUNG_GALAXYS) || defined (CONFIG_SAMSUNG_GALAXYSB)
	{ S5PV210_GPG0(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_UP},
	{ S5PV210_GPG0(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_UP},
	{ S5PV210_GPG0(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_UP},
	{ S5PV210_GPG0(6), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_UP},
#else
	{ S5PV210_GPG0(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPG0(4), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPG0(5), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPG0(6), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
#endif

	// GPG1 ---------------------------------------------------
	{ S5PV210_GPG1(0), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_WLAN_SDIO_CLK
	{ S5PV210_GPG1(1), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_WLAN_SDIO_CMD
	{ S5PV210_GPG1(2), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_WLAN_nRST
	{ S5PV210_GPG1(3), S3C_GPIO_SLP_INPUT,   S3C_GPIO_PULL_NONE},	//GPIO_WLAN_SDIO_D0
	{ S5PV210_GPG1(4), S3C_GPIO_SLP_INPUT,   S3C_GPIO_PULL_NONE},	//GPIO_WLAN_SDIO_D1
	{ S5PV210_GPG1(5), S3C_GPIO_SLP_INPUT,   S3C_GPIO_PULL_NONE},	//GPIO_WLAN_SDIO_D2
	{ S5PV210_GPG1(6), S3C_GPIO_SLP_INPUT,   S3C_GPIO_PULL_NONE},	//GPIO_WLAN_SDIO_D3

	// GPG2 ---------------------------------------------------
	{ S5PV210_GPG2(0), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_T_FLASH_CLK
	{ S5PV210_GPG2(1), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_T_FLASH_CMD
#if defined (CONFIG_SAMSUNG_CAPTIVATE) || defined(CONFIG_SAMSUNG_FASCINATE)
  	{ S5PV210_GPG2(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_ALS_SDA_28V
#else
  	{ S5PV210_GPG2(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_ALS_SDA_28V
#endif
	{ S5PV210_GPG2(3), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_T_FLASH_D0
	{ S5PV210_GPG2(4), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_T_FLASH_D1
	{ S5PV210_GPG2(5), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_T_FLASH_D2
	{ S5PV210_GPG2(6), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_T_FLASH_D3

	// GPG3 ---------------------------------------------------
#if defined (CONFIG_SAMSUNG_VIBRANT)
  	{ S5PV210_GPG3(0), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_UP},
  	{ S5PV210_GPG3(1), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_UP},
#else
  	{ S5PV210_GPG3(0), S3C_GPIO_SLP_OUT1,   S3C_GPIO_PULL_NONE},
  	{ S5PV210_GPG3(1), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},
#endif
#if defined(CONFIG_SAMSUNG_GALAXYSB)
	{ S5PV210_GPG3(2), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
#else
	{ S5PV210_GPG3(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},
#endif
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPG3(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},    //GPIO_TA_CURRENT_SEL_AP
#else
  	{ S5PV210_GPG3(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_UP},      //GPIO_TA_CURRENT_SEL_AP
#endif
	{ S5PV210_GPG3(4), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},    //GPIO_BT_WAKE
	{ GPIO_WLAN_WAKE, S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	
//GPIO_WLAN_WAKE
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPG3(6), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_GPG36
#elif defined (CONFIG_SAMSUNG_VIBRANT)
  	{ S5PV210_GPG3(6), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_GPG36
#else
  	{ S5PV210_GPG3(6), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPG36
#endif

	/* Alive part ending and off part start*/
	// GPI ----------------------------------------------------
	{ S5PV210_GPI(0),  S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_CODEC_I2S_CLK
#if defined (CONFIG_SAMSUNG_CAPTIVATE) || defined (CONFIG_SAMSUNG_VIBRANT)
  	{ S5PV210_GPI(1),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_GPI1 
#else
  	{ S5PV210_GPI(1),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPI1
#endif
	{ S5PV210_GPI(2),  S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_CODEC_I2S_WS
	{ S5PV210_GPI(3),  S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_CODEC_I3S_DI
	{ S5PV210_GPI(4),  S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_CODEC_I3S_DO
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPI(5),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_GPI5
  	{ S5PV210_GPI(6),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_GPI6
#else
  	{ S5PV210_GPI(5),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPI5
  	{ S5PV210_GPI(6),  S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPI6
#endif

	// GPJ0 ---------------------------------------------------
	{ S5PV210_GPJ0(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPJ0(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPJ0(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ0(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ0(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ0(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPJ0(6), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
#if defined (CONFIG_SAMSUNG_GALAXYSB) // ffosilva : OK
	{ S5PV210_GPJ0(7), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},	// S5PV210_GPJ0(6)
#else
	{ S5PV210_GPJ0(7), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
#endif

	// GPJ1 ---------------------------------------------------
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
	{ S5PV210_GPJ1(0), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	
#elif defined(CONFIG_SAMSUNG_FASCINATE)
  	{ S5PV210_GPJ1(0), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_DOWN},
#else
  	{ S5PV210_GPJ1(0), S3C_GPIO_SLP_PREV,  S3C_GPIO_PULL_NONE},
#endif
	{ S5PV210_GPJ1(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ1(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPJ12
	{ S5PV210_GPJ1(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ1(4), S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ1(5), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},

	// GPJ2 ---------------------------------------------------
#if defined(CONFIG_SAMSUNG_GALAXYSB)
	{ S5PV210_GPJ2(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE}, 	// S5PV210_GPJ0(3)
	{ S5PV210_GPJ2(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE}, 	// S5PV210_GPJ0(4)
#elif defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPJ2(0), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_GPJ20
  	{ S5PV210_GPJ2(1), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_GPJ21
#else
  	{ S5PV210_GPJ2(0), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPJ20
  	{ S5PV210_GPJ2(1), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPJ21
#endif
	{ S5PV210_GPJ2(2), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_OLED_DET
#if defined (CONFIG_SAMSUNG_GALAXYSB)
	{ S5PV210_GPJ2(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE}, 	// S5PV210_GPJ0(7)
	{ S5PV210_GPJ2(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPJ2(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
#elif defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPJ2(3), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_GPJ23
  	{ S5PV210_GPJ2(4), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_UP},	//NC
  	{ S5PV210_GPJ2(5), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},
#elif defined (CONFIG_SAMSUNG_VIBRANT)
  	{ S5PV210_GPJ2(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPJ23
  	{ S5PV210_GPJ2(4), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//NC
  	{ S5PV210_GPJ2(5), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_SUB_MICBIAS_EN
#else
  	{ S5PV210_GPJ2(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPJ23
  	{ S5PV210_GPJ2(4), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_UP},
  	{ S5PV210_GPJ2(5), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_SUB_MICBIAS_EN
#endif
	{ S5PV210_GPJ2(6), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_EARPATH_SEL
#if defined (CONFIG_SAMSUNG_GALAXYS) || defined (CONFIG_SAMSUNG_GALAXYSB)|| defined (CONFIG_MACH_WAVE)
	{ S5PV210_GPJ2(7), S3C_GPIO_SLP_OUT1,   S3C_GPIO_PULL_NONE},	//GPIO_MASSMEMORY_EN
#else
	{ S5PV210_GPJ2(7), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_MASSMEMORY_EN
#endif

	// GPJ3 ---------------------------------------------------
	{ S5PV210_GPJ3(0), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//_3_TOUCH_SDA_28V
	{ S5PV210_GPJ3(1), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//_3_TOUCH_SCL_28V
	{ S5PV210_GPJ3(2), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//_3_GPIO_TOUCH_EN
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPJ3(3), S3C_GPIO_SLP_OUT0,   S3C_GPIO_PULL_NONE},	//GPIO_GPJ33
#else
  	{ S5PV210_GPJ3(3), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPJ33
#endif
	{ S5PV210_GPJ3(4), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_USB_SDA_28V
	{ S5PV210_GPJ3(5), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_USB_SCL_28V
	{ S5PV210_GPJ3(6), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_AP_SDA_28V
	{ S5PV210_GPJ3(7), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_AP_SCL_28V

	// GPJ4 ---------------------------------------------------
	{ S5PV210_GPJ4(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ4(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_GPJ4(2), S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE},
	{ S5PV210_GPJ4(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_GPJ4(4), S3C_GPIO_SLP_PREV,   S3C_GPIO_PULL_NONE},	//GPIO_EARMICBIAS_EN
#else
  	{ S5PV210_GPJ4(4), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_GPJ44
#endif

	/* memory part */
	// MP01 ---------------------------------------------------
	{ S5PV210_MP01(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP01(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP01(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP01(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP01(4), S3C_GPIO_SLP_OUT1,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP01(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP01(6), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP01(7), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// MP02 ---------------------------------------------------
	{ S5PV210_MP02(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP02(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP02(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP02(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// MP03 ---------------------------------------------------
	{ S5PV210_MP03(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP03(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
#if defined (CONFIG_SAMSUNG_VIBRANT)
  	{ S5PV210_MP03(2), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//NC
#else
  	{ S5PV210_MP03(2), S3C_GPIO_SLP_OUT1,   S3C_GPIO_PULL_NONE},	//GPIO_MP032
#endif
	{ S5PV210_MP03(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP03(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP03(5), S3C_GPIO_SLP_OUT1,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP03(6), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP03(7), S3C_GPIO_SLP_OUT1,	S3C_GPIO_PULL_NONE},	//PCM_SEL

	// MP04 ---------------------------------------------------
	{ S5PV210_MP04(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP04(1), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP04(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP04(3), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
  	{ S5PV210_MP04(4), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_MP044
  	{ S5PV210_MP04(5), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_NONE},	//GPIO_MP045
#else 
  	{ S5PV210_MP04(4), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_MP044
  	{ S5PV210_MP04(5), S3C_GPIO_SLP_INPUT,  S3C_GPIO_PULL_DOWN},	//GPIO_MP045
#endif
	{ S5PV210_MP04(6), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},
	{ S5PV210_MP04(7), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// MP05 ---------------------------------------------------
	{ S5PV210_MP05(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},	//FUEL_SCL_18V
	{ S5PV210_MP05(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},	//FUEL_SDA_18V
	{ S5PV210_MP05(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},	//GPIO_AP_SCL_18V
	{ S5PV210_MP05(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_NONE},	//GPIO_AP_SDA_18V
	{ S5PV210_MP05(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},	//GPIO_MP054
	{ S5PV210_MP05(5), S3C_GPIO_SLP_OUT0,	S3C_GPIO_PULL_NONE},	//GPIO_MLCD_RST
	{ S5PV210_MP05(6), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},	//GPIO_MP056
	{ S5PV210_MP05(7), S3C_GPIO_SLP_PREV,	S3C_GPIO_PULL_NONE},	//GPIO_UART_SEL

	// MP06 ---------------------------------------------------
	{ S5PV210_MP06(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP06(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP06(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP06(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP06(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP06(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP06(6), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP06(7), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	// MP07 ---------------------------------------------------
	{ S5PV210_MP07(0), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP07(1), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP07(2), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP07(3), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP07(4), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP07(5), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP07(6), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},
	{ S5PV210_MP07(7), S3C_GPIO_SLP_INPUT,	S3C_GPIO_PULL_DOWN},

	/* Memory part ending and off part ending */
};

void s3c_config_sleep_gpio_table(int array_size, unsigned int (*gpio_table)[3])
{
	u32 i, gpio;

	for (i = 0; i < array_size; i++) {
		gpio = gpio_table[i][0];
		s3c_gpio_slp_cfgpin(gpio, gpio_table[i][1]);
		s3c_gpio_slp_setpull_updown(gpio, gpio_table[i][2]);
	}
}

void s3c_config_sleep_gpio(void)
{
	return;
#if defined(CONFIG_SAMSUNG_CAPTIVATE)
	// Reported to cause battery drain and other things on captivate, so we'll
	// disable this for now.
	return;
#endif

	/* setting the alive mode registers */
#if defined(CONFIG_SAMSUNG_FASCINATE) || defined(CONFIG_SAMSUNG_CAPTIVATE)
	s3c_gpio_cfgpin(S5PV210_GPH0(0), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH0(0), S3C_GPIO_PULL_DOWN);
#endif

	s3c_gpio_cfgpin(S5PV210_GPH0(1), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH0(1), S3C_GPIO_PULL_NONE);

	s3c_gpio_cfgpin(S5PV210_GPH0(3), S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(S5PV210_GPH0(3), S3C_GPIO_PULL_NONE);
	gpio_set_value(S5PV210_GPH0(3), 0);

	s3c_gpio_cfgpin(S5PV210_GPH0(4), S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(S5PV210_GPH0(4), S3C_GPIO_PULL_NONE);
	gpio_set_value(S5PV210_GPH0(4), 0);

	s3c_gpio_cfgpin(S5PV210_GPH0(5), S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(S5PV210_GPH0(5), S3C_GPIO_PULL_NONE);
	gpio_set_value(S5PV210_GPH0(5), 0);

#if defined(CONFIG_SAMSUNG_FASCINATE) || defined(CONFIG_SAMSUNG_CAPTIVATE)
	s3c_gpio_cfgpin(S5PV210_GPH0(7), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH0(7), S3C_GPIO_PULL_UP);
#endif

	s3c_gpio_cfgpin(S5PV210_GPH1(0), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH1(0), S3C_GPIO_PULL_DOWN);

	s3c_gpio_cfgpin(S5PV210_GPH1(1), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH1(1), S3C_GPIO_PULL_DOWN);
	s3c_gpio_cfgpin(S5PV210_GPH1(2), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH1(2), S3C_GPIO_PULL_DOWN);

	s3c_gpio_cfgpin(S5PV210_GPH1(4), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH1(4), S3C_GPIO_PULL_DOWN);

	s3c_gpio_cfgpin(S5PV210_GPH1(5), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH1(5), S3C_GPIO_PULL_DOWN);

	s3c_gpio_cfgpin(S5PV210_GPH1(6), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH1(6), S3C_GPIO_PULL_DOWN);

	s3c_gpio_cfgpin(S5PV210_GPH1(7), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH1(7), S3C_GPIO_PULL_NONE);

	s3c_gpio_cfgpin(S5PV210_GPH2(0), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH2(0), S3C_GPIO_PULL_DOWN);

#if defined(CONFIG_SAMSUNG_CAPTIVATE)
	s3c_gpio_cfgpin(S5PV210_GPH2(1), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH2(1), S3C_GPIO_PULL_DOWN);
#else
	s3c_gpio_cfgpin(S5PV210_GPH2(1), S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(S5PV210_GPH2(1), S3C_GPIO_PULL_NONE);
#endif
	gpio_set_value(S5PV210_GPH2(1), 0);

#if !defined(CONFIG_SAMSUNG_FASCINATE)
	s3c_gpio_cfgpin(S5PV210_GPH2(2), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH2(2), S3C_GPIO_PULL_DOWN);

	s3c_gpio_cfgpin(S5PV210_GPH2(3), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH2(3), S3C_GPIO_PULL_DOWN);
#endif

#if defined(CONFIG_SAMSUNG_FASCINATE)
	s3c_gpio_cfgpin(S5PV210_GPH2(6), S3C_GPIO_EINT);
	s3c_gpio_setpull(S5PV210_GPH2(6), S3C_GPIO_PULL_UP);
#endif

	s3c_gpio_cfgpin(S5PV210_GPH3(0), S3C_GPIO_INPUT);
#if defined(CONFIG_SAMSUNG_FASCINATE)
	s3c_gpio_setpull(S5PV210_GPH3(3), S3C_GPIO_PULL_UP);
#else
	s3c_gpio_setpull(S5PV210_GPH3(0), S3C_GPIO_PULL_DOWN);
#endif

#if defined(CONFIG_SAMSUNG_FASCINATE)
	s3c_gpio_cfgpin(S5PV210_GPH3(3), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH3(3), S3C_GPIO_PULL_UP);
#elif !defined(CONFIG_SAMSUNG_CAPTIVATE)
	s3c_gpio_cfgpin(S5PV210_GPH3(3), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH3(3), S3C_GPIO_PULL_DOWN);
#endif

#if defined(CONFIG_SAMSUNG_FASCINATE)
	s3c_gpio_cfgpin(S5PV210_GPH3(4), S3C_GPIO_EINT);
	s3c_gpio_setpull(S5PV210_GPH3(4), S3C_GPIO_PULL_UP);
#endif

#if defined(CONFIG_SAMSUNG_FASCINATE) || defined(CONFIG_SAMSUNG_CAPTIVATE) || defined(CONFIG_SAMSUNG_VIBRANT)
	s3c_gpio_cfgpin(S5PV210_GPH3(5), S3C_GPIO_INPUT);
	s3c_gpio_setpull(S5PV210_GPH3(5), S3C_GPIO_PULL_DOWN);
#endif

	s3c_gpio_cfgpin(S5PV210_GPH3(7), S3C_GPIO_OUTPUT);
#if defined(CONFIG_SAMSUNG_CAPTIVATE)
	s3c_gpio_setpull(S5PV210_GPH3(7), S3C_GPIO_PULL_UP);
#else
	s3c_gpio_setpull(S5PV210_GPH3(7), S3C_GPIO_PULL_NONE);
#endif
	gpio_set_value(S5PV210_GPH2(3), 1);

#if !defined(CONFIG_SAMSUNG_FASCINATE)
	if (gpio_get_value(GPIO_PS_ON)) {
		s3c_gpio_slp_setpull_updown(GPIO_ALS_SDA_28V, S3C_GPIO_PULL_NONE);
		s3c_gpio_slp_setpull_updown(GPIO_ALS_SCL_28V, S3C_GPIO_PULL_NONE);
	} else {
		s3c_gpio_setpull(GPIO_PS_VOUT, S3C_GPIO_PULL_DOWN);
	}
#endif
}
EXPORT_SYMBOL(s3c_config_sleep_gpio);

static unsigned int wlan_sdio_on_table[][4] = {
	{GPIO_WLAN_SDIO_CLK, GPIO_WLAN_SDIO_CLK_AF, GPIO_LEVEL_NONE,
		S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_CMD, GPIO_WLAN_SDIO_CMD_AF, GPIO_LEVEL_NONE,
		S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D0, GPIO_WLAN_SDIO_D0_AF, GPIO_LEVEL_NONE,
		S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D1, GPIO_WLAN_SDIO_D1_AF, GPIO_LEVEL_NONE,
		S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D2, GPIO_WLAN_SDIO_D2_AF, GPIO_LEVEL_NONE,
		S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D3, GPIO_WLAN_SDIO_D3_AF, GPIO_LEVEL_NONE,
		S3C_GPIO_PULL_NONE},
};

static unsigned int wlan_sdio_off_table[][4] = {
	{GPIO_WLAN_SDIO_CLK, 1, GPIO_LEVEL_LOW, S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_CMD, 0, GPIO_LEVEL_NONE, S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D0, 0, GPIO_LEVEL_NONE, S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D1, 0, GPIO_LEVEL_NONE, S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D2, 0, GPIO_LEVEL_NONE, S3C_GPIO_PULL_NONE},
	{GPIO_WLAN_SDIO_D3, 0, GPIO_LEVEL_NONE, S3C_GPIO_PULL_NONE},
};

static int wlan_power_en(int onoff)
{
	if (onoff) {
		s3c_gpio_cfgpin(GPIO_WLAN_HOST_WAKE,
				S3C_GPIO_SFN(GPIO_WLAN_HOST_WAKE_AF));
		s3c_gpio_setpull(GPIO_WLAN_HOST_WAKE, S3C_GPIO_PULL_DOWN);

		s3c_gpio_cfgpin(GPIO_WLAN_WAKE,
				S3C_GPIO_SFN(GPIO_WLAN_WAKE_AF));
		s3c_gpio_setpull(GPIO_WLAN_WAKE, S3C_GPIO_PULL_NONE);
		gpio_set_value(GPIO_WLAN_WAKE, GPIO_LEVEL_LOW);

		s3c_gpio_cfgpin(GPIO_WLAN_nRST,
				S3C_GPIO_SFN(GPIO_WLAN_nRST_AF));
		s3c_gpio_setpull(GPIO_WLAN_nRST, S3C_GPIO_PULL_NONE);
		gpio_set_value(GPIO_WLAN_nRST, GPIO_LEVEL_HIGH);
		s3c_gpio_slp_cfgpin(GPIO_WLAN_nRST, S3C_GPIO_SLP_OUT1);
		s3c_gpio_slp_setpull_updown(GPIO_WLAN_nRST, S3C_GPIO_PULL_NONE);

		s3c_gpio_cfgpin(GPIO_WLAN_BT_EN, S3C_GPIO_OUTPUT);
		s3c_gpio_setpull(GPIO_WLAN_BT_EN, S3C_GPIO_PULL_NONE);
		gpio_set_value(GPIO_WLAN_BT_EN, GPIO_LEVEL_HIGH);
		s3c_gpio_slp_cfgpin(GPIO_WLAN_BT_EN, S3C_GPIO_SLP_OUT1);
		s3c_gpio_slp_setpull_updown(GPIO_WLAN_BT_EN,
					S3C_GPIO_PULL_NONE);

		msleep(200);
	} else {
		gpio_set_value(GPIO_WLAN_nRST, GPIO_LEVEL_LOW);
		s3c_gpio_slp_cfgpin(GPIO_WLAN_nRST, S3C_GPIO_SLP_OUT0);
		s3c_gpio_slp_setpull_updown(GPIO_WLAN_nRST, S3C_GPIO_PULL_NONE);

		if (gpio_get_value(GPIO_BT_nRST) == 0) {
			gpio_set_value(GPIO_WLAN_BT_EN, GPIO_LEVEL_LOW);
			s3c_gpio_slp_cfgpin(GPIO_WLAN_BT_EN, S3C_GPIO_SLP_OUT0);
			s3c_gpio_slp_setpull_updown(GPIO_WLAN_BT_EN,
						S3C_GPIO_PULL_NONE);
		}
	}
	return 0;
}

static int wlan_reset_en(int onoff)
{
	gpio_set_value(GPIO_WLAN_nRST,
			onoff ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
	return 0;
}

static int wlan_carddetect_en(int onoff)
{
	u32 i;
	u32 sdio;

	if (onoff) {
		for (i = 0; i < ARRAY_SIZE(wlan_sdio_on_table); i++) {
			sdio = wlan_sdio_on_table[i][0];
			s3c_gpio_cfgpin(sdio,
					S3C_GPIO_SFN(wlan_sdio_on_table[i][1]));
			s3c_gpio_setpull(sdio, wlan_sdio_on_table[i][3]);
			if (wlan_sdio_on_table[i][2] != GPIO_LEVEL_NONE)
				gpio_set_value(sdio, wlan_sdio_on_table[i][2]);
		}
	} else {
		for (i = 0; i < ARRAY_SIZE(wlan_sdio_off_table); i++) {
			sdio = wlan_sdio_off_table[i][0];
			s3c_gpio_cfgpin(sdio,
				S3C_GPIO_SFN(wlan_sdio_off_table[i][1]));
			s3c_gpio_setpull(sdio, wlan_sdio_off_table[i][3]);
			if (wlan_sdio_off_table[i][2] != GPIO_LEVEL_NONE)
				gpio_set_value(sdio, wlan_sdio_off_table[i][2]);
		}
	}
	udelay(5);

	sdhci_s3c_force_presence_change(&s3c_device_hsmmc1);
	msleep(500); /* wait for carddetect */
	return 0;
}

static struct resource wifi_resources[] = {
	[0] = {
		.name	= "bcm4329_wlan_irq",
		.start	= IRQ_EINT(20),
		.end	= IRQ_EINT(20),
		.flags	= IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHLEVEL,
	},
};

static struct wifi_mem_prealloc wifi_mem_array[PREALLOC_WLAN_SEC_NUM] = {
	{NULL, (WLAN_SECTION_SIZE_0 + PREALLOC_WLAN_SECTION_HEADER)},
	{NULL, (WLAN_SECTION_SIZE_1 + PREALLOC_WLAN_SECTION_HEADER)},
	{NULL, (WLAN_SECTION_SIZE_2 + PREALLOC_WLAN_SECTION_HEADER)},
	{NULL, (WLAN_SECTION_SIZE_3 + PREALLOC_WLAN_SECTION_HEADER)}
};

static void *wave_mem_prealloc(int section, unsigned long size)
{
	if (section == PREALLOC_WLAN_SEC_NUM)
		return wlan_static_skb;

	if ((section < 0) || (section > PREALLOC_WLAN_SEC_NUM))
		return NULL;

	if (wifi_mem_array[section].size < size)
		return NULL;

	return wifi_mem_array[section].mem_ptr;
}

int __init wave_init_wifi_mem(void)
{
	int i;
	int j;

	for (i = 0 ; i < WLAN_SKB_BUF_NUM ; i++) {
		wlan_static_skb[i] = dev_alloc_skb(
				((i < (WLAN_SKB_BUF_NUM / 2)) ? 4096 : 8192));

		if (!wlan_static_skb[i])
			goto err_skb_alloc;
	}

	for (i = 0 ; i < PREALLOC_WLAN_SEC_NUM ; i++) {
		wifi_mem_array[i].mem_ptr =
				kmalloc(wifi_mem_array[i].size, GFP_KERNEL);

		if (!wifi_mem_array[i].mem_ptr)
			goto err_mem_alloc;
	}
	return 0;

 err_mem_alloc:
	pr_err("Failed to mem_alloc for WLAN\n");
	for (j = 0 ; j < i ; j++)
		kfree(wifi_mem_array[j].mem_ptr);

	i = WLAN_SKB_BUF_NUM;

 err_skb_alloc:
	pr_err("Failed to skb_alloc for WLAN\n");
	for (j = 0 ; j < i ; j++)
		dev_kfree_skb(wlan_static_skb[j]);

	return -ENOMEM;
}

/* Customized Locale table : OPTIONAL feature */
#define WLC_CNTRY_BUF_SZ	4
typedef struct cntry_locales_custom {
	char iso_abbrev[WLC_CNTRY_BUF_SZ];
	char custom_locale[WLC_CNTRY_BUF_SZ];
	int  custom_locale_rev;
} cntry_locales_custom_t;

static cntry_locales_custom_t wave_wifi_translate_custom_table[] = {
/* Table should be filled out based on custom platform regulatory requirement */
	{"",   "XY", 4},  /* universal */
	{"US", "US", 69}, /* input ISO "US" to : US regrev 69 */
	{"CA", "US", 69}, /* input ISO "CA" to : US regrev 69 */
	{"EU", "EU", 5},  /* European union countries */
	{"AT", "EU", 5},
	{"BE", "EU", 5},
	{"BG", "EU", 5},
	{"CY", "EU", 5},
	{"CZ", "EU", 5},
	{"DK", "EU", 5},
	{"EE", "EU", 5},
	{"FI", "EU", 5},
	{"FR", "EU", 5},
	{"DE", "EU", 5},
	{"GR", "EU", 5},
	{"HU", "EU", 5},
	{"IE", "EU", 5},
	{"IT", "EU", 5},
	{"LV", "EU", 5},
	{"LI", "EU", 5},
	{"LT", "EU", 5},
	{"LU", "EU", 5},
	{"MT", "EU", 5},
	{"NL", "EU", 5},
	{"PL", "EU", 5},
	{"PT", "EU", 5},
	{"RO", "EU", 5},
	{"SK", "EU", 5},
	{"SI", "EU", 5},
	{"ES", "EU", 5},
	{"SE", "EU", 5},
	{"GB", "EU", 5},  /* input ISO "GB" to : EU regrev 05 */
	{"IL", "IL", 0},
	{"CH", "CH", 0},
	{"TR", "TR", 0},
	{"NO", "NO", 0},
	{"KR", "XY", 3},
	{"AU", "XY", 3},
	{"CN", "XY", 3},  /* input ISO "CN" to : XY regrev 03 */
	{"TW", "XY", 3},
	{"AR", "XY", 3},
	{"MX", "XY", 3}
};

static void *wave_wifi_get_country_code(char *ccode)
{
	int size = ARRAY_SIZE(wave_wifi_translate_custom_table);
	int i;

	if (!ccode)
		return NULL;

	for (i = 0; i < size; i++)
		if (strcmp(ccode, wave_wifi_translate_custom_table[i].iso_abbrev) == 0)
			return &wave_wifi_translate_custom_table[i];
	return &wave_wifi_translate_custom_table[0];
}

static struct wifi_platform_data wifi_pdata = {
	.set_power		= wlan_power_en,
	.set_reset		= wlan_reset_en,
	.set_carddetect		= wlan_carddetect_en,
	.mem_prealloc		= wave_mem_prealloc,
	.get_country_code	= wave_wifi_get_country_code,
};

static struct platform_device sec_device_wifi = {
	.name			= "bcm4329_wlan",
	.id			= 1,
	.num_resources		= ARRAY_SIZE(wifi_resources),
	.resource		= wifi_resources,
	.dev			= {
		.platform_data = &wifi_pdata,
	},
};

static struct platform_device watchdog_device = {
	.name = "watchdog",
	.id = -1,
};

static struct platform_device *wave_devices[] __initdata = {
	&watchdog_device,
#ifdef CONFIG_FIQ_DEBUGGER
	&s5pv210_device_fiqdbg_uart2,
#endif
	&s5p_device_onenand,
#ifdef CONFIG_RTC_DRV_S3C
	&s5p_device_rtc,
#endif
	//&wave_input_device,

	&wave_device_gpiokeys,
	&samsung_device_keypad,

	&s5pv210_device_iis0,
	&s3c_device_wdt,

#ifdef CONFIG_VIDEO_MFC50
	&s3c_device_mfc,
#endif
#ifdef	CONFIG_S5P_ADC
	&s3c_device_adc,
#endif

#ifdef CONFIG_FB_S3C
	&s3c_device_fb,
#endif
#ifdef CONFIG_VIDEO_FIMC
	&s3c_device_fimc0,
	&s3c_device_fimc1,
	&s3c_device_fimc2,
#endif

#ifdef CONFIG_VIDEO_JPEG_V2
	&s3c_device_jpeg,
#endif

	&s3c_device_g3d,
	&s3c_device_lcd,

#if defined(CONFIG_FB_S3C_TL2796) || defined(CONFIG_FB_S3C_LG4573)
	&s3c_display_spi_gpio,
#endif
	&sec_device_jack,

	&s3c_device_i2c0,
#if defined(CONFIG_S3C_DEV_I2C1)
	&s3c_device_i2c1,
#endif

#if defined(CONFIG_S3C_DEV_I2C2)
	&s3c_device_i2c2,
#endif
	&wave_s3c_device_i2c4,
	&wave_s3c_device_i2c5,  /* accel sensor */
	&wave_s3c_device_i2c6,
	&wave_s3c_device_i2c7,
#if defined(CONFIG_SAMSUNG_GALAXYS) || defined (CONFIG_SAMSUNG_GALAXYSB) || defined (CONFIG_MACH_WAVE)
	&s3c_device_i2c8,  /* si470x: fm radio */
#endif
	&s3c_device_i2c11, /* optical sensor */
	&s3c_device_i2c12, /* magnetic sensor */
#if defined (CONFIG_SAMSUNG_CAPTIVATE)
	&s3c_device_i2c13,
#endif
#ifdef CONFIG_USB_GADGET
	&s3c_device_usbgadget,
#endif
#ifdef CONFIG_USB_ANDROID
	&s3c_device_android_usb,
#ifdef CONFIG_USB_ANDROID_MASS_STORAGE
	&s3c_device_usb_mass_storage,
#endif
#ifdef CONFIG_USB_ANDROID_RNDIS
	&s3c_device_rndis,
#endif
#endif

#ifdef CONFIG_PHONE_ARIES_CDMA
	&sec_device_dpram,
#endif

#ifdef CONFIG_S3C_DEV_HSMMC
	&s3c_device_hsmmc0,
#endif
#ifdef CONFIG_S3C_DEV_HSMMC1
	&s3c_device_hsmmc1,
#endif
#ifdef CONFIG_S3C_DEV_HSMMC2
	&s3c_device_hsmmc2,
#endif
#ifdef CONFIG_S3C_DEV_HSMMC3
	&s3c_device_hsmmc3,
#endif
#ifdef CONFIG_VIDEO_TV20
	&s5p_device_tvout,
#endif
	&wave_charger_device,


#ifdef CONFIG_S5PV210_POWER_DOMAIN
	&s5pv210_pd_audio,
	&s5pv210_pd_cam,
	&s5pv210_pd_tv,
	&s5pv210_pd_lcd,
	&s5pv210_pd_g3d,
	&s5pv210_pd_mfc,
#endif

#ifdef CONFIG_ANDROID_PMEM
	&pmem_device,
	&pmem_gpu1_device,
	&pmem_adsp_device,
#endif

#ifdef CONFIG_HAVE_PWM
	&s3c_device_timer[0],
	&s3c_device_timer[1],
	&s3c_device_timer[2],
	&s3c_device_timer[3],
#endif

#ifdef CONFIG_CPU_FREQ
	&s5pv210_device_cpufreq,
#endif

	&sec_device_rfkill,
	&sec_device_btsleep,
	&ram_console_device,
	&sec_device_wifi,
	&samsung_asoc_dma,
};

static void __init wave_map_io(void)
{
	s5p_init_io(NULL, 0, S5P_VA_CHIPID);
	s3c24xx_init_clocks(24000000);
	s5pv210_gpiolib_init();
	s3c24xx_init_uarts(wave_uartcfgs, ARRAY_SIZE(wave_uartcfgs));
	#ifndef CONFIG_S5P_HIGH_RES_TIMERS
		s5p_set_timer_source(S5P_PWM3, S5P_PWM4);
	#endif
	s5p_reserve_bootmem(wave_media_devs,
		ARRAY_SIZE(wave_media_devs), S5P_RANGE_MFC);
#ifdef CONFIG_MTD_ONENAND
	s5p_device_onenand.name = "s5pc110-onenand";
#endif
}

unsigned int pm_debug_scratchpad;

static unsigned int ram_console_start;
static unsigned int ram_console_size;

static void __init wave_fixup(struct machine_desc *desc,
		struct tag *tags, char **cmdline,
		struct meminfo *mi)
{
	mi->bank[0].start = S5PV210_PA_SDRAM;
	mi->bank[0].size = 80 * SZ_1M;

	mi->bank[1].start = 0x40000000;
	/* 1M-4K for ram_console buffer */
	mi->bank[1].size = 255 * SZ_1M;
	mi->nr_banks = 2;

	ram_console_start = mi->bank[1].start + mi->bank[1].size + SZ_1K; /* 1K to preserve Bada BootSharedInfo */
	ram_console_size = SZ_1M - SZ_4K - SZ_1K; /* 4K for PM debug scratchpad  */

	pm_debug_scratchpad = ram_console_start + ram_console_size;
}

/* this function are used to detect s5pc110 chip version temporally */
int s5pc110_version ;

void _hw_version_check(void)
{
	void __iomem *phy_address ;
	int temp;

	phy_address = ioremap(0x40, 1);

	temp = __raw_readl(phy_address);

	if (temp == 0xE59F010C)
		s5pc110_version = 0;
	else
		s5pc110_version = 1;

	printk(KERN_INFO "S5PC110 Hardware version : EVT%d\n",
				s5pc110_version);

	iounmap(phy_address);
}

/*
 * Temporally used
 * return value 0 -> EVT 0
 * value 1 -> evt 1
 */

int hw_version_check(void)
{
	return s5pc110_version ;
}
EXPORT_SYMBOL(hw_version_check);

static void wave_init_gpio(void)
{
	s3c_config_gpio_table();
	s3c_config_sleep_gpio_table(ARRAY_SIZE(wave_sleep_gpio_table),
			wave_sleep_gpio_table);
}

static void __init fsa9480_gpio_init(void)
{
	/*
	s3c_gpio_cfgpin(GPIO_USB_SEL, S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(GPIO_USB_SEL, S3C_GPIO_PULL_NONE);
	*/
	s3c_gpio_cfgpin(GPIO_UART_SEL, S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(GPIO_UART_SEL, S3C_GPIO_PULL_NONE);

	s3c_gpio_cfgpin(GPIO_JACK_nINT, S3C_GPIO_SFN(0xf));
	s3c_gpio_setpull(GPIO_JACK_nINT, S3C_GPIO_PULL_NONE);
}

static void __init setup_ram_console_mem(void)
{
	ram_console_resource[0].start = ram_console_start;
	ram_console_resource[0].end = ram_console_start + ram_console_size - 1;
}

static void __init sound_init(void)
{
	u32 reg;

	reg = __raw_readl(S5P_OTHERS);
	reg &= ~(0x3 << 8);
	reg |= 3 << 8;
	__raw_writel(reg, S5P_OTHERS);

	reg = __raw_readl(S5P_CLK_OUT);
	reg &= ~(0x1f << 12);
	reg |= 19 << 12;
	__raw_writel(reg, S5P_CLK_OUT);

	reg = __raw_readl(S5P_CLK_OUT);
	reg &= ~0x1;
	reg |= 0x1;
	__raw_writel(reg, S5P_CLK_OUT);

#if defined(CONFIG_SAMSUNG_VIBRANT)
    if((HWREV == 0x0A) || (HWREV == 0x0C) || (HWREV == 0x0D) || (HWREV == 0x0E) ) //0x0A:00, 0x0C:00, 0x0D:01, 0x0E:05
        gpio_request(GPIO_MICBIAS_EN, "micbias_enable");
    else {
        gpio_request(GPIO_MICBIAS_EN2, "micbias_enable2");
        gpio_request(GPIO_MICBIAS_EN, "micbias_enable");
    }
#else
	gpio_request(GPIO_EARPATH_SEL, "earpath_sel");
	gpio_request(GPIO_PCM_SEL, "pcm_sel");
	gpio_request(GPIO_MICBIAS_EN, "micbias_enable");
#endif
}

static bool console_flushed;

static void flush_console(void)
{
	if (console_flushed)
		return;

	console_flushed = true;

	printk("\n");
	pr_emerg("Restarting %s\n", linux_banner);
	if (!is_console_locked())
		return;

	mdelay(50);

	local_irq_disable();
	if (console_trylock())
		pr_emerg("flush_console: console was locked! busting!\n");
	else
		pr_emerg("flush_console: console was locked!\n");
	console_unlock();
}

static void wave_pm_restart(char mode, const char *cmd)
{
	flush_console();

	/* On a normal reboot, INFORM6 will contain a small integer
	 * reason code from the notifier hook.  On a panic, it will
	 * contain the 0xee we set at boot.  Write 0xbb to differentiate
	 * a watchdog-timeout-and-reboot (0xee) from a controlled reboot
	 * (0xbb)
	 */
	if (__raw_readl(S5P_INFORM6) == 0xee)
		__raw_writel(0xbb, S5P_INFORM6);

	arm_machine_restart(mode, cmd);
}

// Ugly hack to inject parameters (e.g. device serial, bootmode) into /proc/cmdline
static void __init wave_inject_cmdline(void) {
	char *new_command_line;
	int bootmode = __raw_readl(S5P_INFORM6);
	int size;

	size = strlen(boot_command_line);
	new_command_line = kmalloc(size + 40 + 11, GFP_KERNEL);
	strcpy(new_command_line, saved_command_line);
	size += sprintf(new_command_line + size, " androidboot.serialno=%08X%08X",
				system_serial_high, system_serial_low);

	// Only write bootmode when less than 10 to prevent confusion with watchdog
	// reboot (0xee = 238)
	if (bootmode < 10) {
		size += sprintf(new_command_line + size, " bootmode=%d", bootmode);
	}

	saved_command_line = new_command_line;
}

static uint32_t wave_keymap[] __initdata = {
	/* KEY(row, col, keycode) */
		KEY(0, 0, KEY_MENU),		/* Middle key */
		KEY(0, 2, KEY_SETUP),		/* Call key */
		KEY(1, 0, KEY_CONFIG),		/* Cam Half shot */
		KEY(1, 1, KEY_VOLUMEDOWN),
		KEY(2, 0, KEY_CAMERA),		/* Cam Full shot */
		KEY(2, 1, KEY_VOLUMEUP),
		KEY(2, 2, KEY_SLEEP),	/* Screen on/off */
		//END key is handled separately
};

static struct matrix_keymap_data wave_keymap_data __initdata = {
	.keymap		= wave_keymap,
	.keymap_size	= ARRAY_SIZE(wave_keymap),
};

static struct samsung_keypad_platdata wave_keypad_data __initdata = {
	.keymap_data	= &wave_keymap_data,
	.rows		= 3,
	.cols		= 3,
	.wakeup		= 1,

};

static void __init onenand_init(void)
{
	struct clk *clk = clk_get(NULL, "onenand");
	BUG_ON(!clk);
	clk_enable(clk);
}

extern void setup_tzpc();

static void __init wave_machine_init(void)
{
	arm_pm_restart = wave_pm_restart;

	setup_tzpc();
	
	setup_ram_console_mem();
	wave_inject_cmdline();
	platform_add_devices(wave_devices, ARRAY_SIZE(wave_devices));
	console_suspend_enabled = 0;

	/* Find out S5PC110 chip version */
	_hw_version_check();

	pm_power_off = wave_power_off ;

	s3c_gpio_cfgpin(GPIO_HWREV_MODE0, S3C_GPIO_INPUT);
	s3c_gpio_setpull(GPIO_HWREV_MODE0, S3C_GPIO_PULL_NONE);
	s3c_gpio_cfgpin(GPIO_HWREV_MODE1, S3C_GPIO_INPUT);
	s3c_gpio_setpull(GPIO_HWREV_MODE1, S3C_GPIO_PULL_NONE);
	s3c_gpio_cfgpin(GPIO_HWREV_MODE2, S3C_GPIO_INPUT);
	s3c_gpio_setpull(GPIO_HWREV_MODE2, S3C_GPIO_PULL_NONE);
	HWREV = gpio_get_value(GPIO_HWREV_MODE0);
	HWREV = HWREV | (gpio_get_value(GPIO_HWREV_MODE1) << 1);
	HWREV = HWREV | (gpio_get_value(GPIO_HWREV_MODE2) << 2);
	s3c_gpio_cfgpin(GPIO_HWREV_MODE3, S3C_GPIO_INPUT);
	s3c_gpio_setpull(GPIO_HWREV_MODE3, S3C_GPIO_PULL_NONE);
	HWREV = HWREV | (gpio_get_value(GPIO_HWREV_MODE3) << 3);

	printk(KERN_INFO "HWREV is 0x%x\n", HWREV);

	/*initialise the gpio's*/
	wave_init_gpio();

#ifdef CONFIG_ANDROID_PMEM
	android_pmem_set_platdata();
#endif


	samsung_keypad_set_platdata(&wave_keypad_data);

	/* headset/earjack detection */
#if defined(CONFIG_SAMSUNG_CAPTIVATE)
    gpio_request(GPIO_EAR_MICBIAS_EN, "ear_micbias_enable");
#endif

	gpio_request(GPIO_TOUCH_EN, "touch en");

	/* i2c */
	s3c_i2c0_set_platdata(NULL);
	s3c_i2c1_set_platdata(NULL);
	s3c_i2c2_set_platdata(NULL);

	/* H/W I2C lines */

	/* mxt224 */
	mxt224_init();
	i2c_register_board_info(2, i2c_devs2, ARRAY_SIZE(i2c_devs2));

	/* wm8994 codec */
	sound_init();
	i2c_register_board_info(4, i2c_devs4, ARRAY_SIZE(i2c_devs4));

	/* accel sensor */
	i2c_register_board_info(5, i2c_devs5, ARRAY_SIZE(i2c_devs5));

	/* max8998 */
	i2c_register_board_info(6, i2c_devs6, ARRAY_SIZE(i2c_devs6));

	/* FSA9480 */
	fsa9480_gpio_init();
	i2c_register_board_info(7, i2c_devs7, ARRAY_SIZE(i2c_devs7));

	/* fm radio */
	i2c_register_board_info(8, i2c_devs8, ARRAY_SIZE(i2c_devs8));


	/* optical sensor */
	gp2a_gpio_init();
	i2c_register_board_info(0, i2c_dev_gp2a, ARRAY_SIZE(i2c_dev_gp2a));
	
	/* AK8973 magnetic sensor */
	i2c_register_board_info(12, i2c_devs12, ARRAY_SIZE(i2c_devs12));

	/* panel */
#ifdef CONFIG_FB_S3C_TL2796
	if(machine_is_wave()) {
		spi_register_board_info(tl2796_spi_board_info, ARRAY_SIZE(tl2796_spi_board_info));
		s3cfb_set_platdata(&tl2796_data);
	}
#endif

#ifdef CONFIG_FB_S3C_LG4573
	if(machine_is_wave2()) {
		spi_register_board_info(lg4573_spi_board_info, ARRAY_SIZE(lg4573_spi_board_info));
		s3cfb_set_platdata(&lg4573_data);
	}
#endif

#if defined(CONFIG_S5P_ADC)
	s3c_adc_set_platdata(&s3c_adc_platform);
#endif

#if defined(CONFIG_PM)
	s3c_pm_init();
#endif

#ifdef CONFIG_VIDEO_FIMC
	/* fimc */
	s3c_fimc0_set_platdata(&fimc_plat_lsi);
	s3c_fimc1_set_platdata(&fimc_plat_lsi);
	s3c_fimc2_set_platdata(&fimc_plat_lsi);
#endif

#ifdef CONFIG_VIDEO_JPEG_V2
	s3c_jpeg_set_platdata(&jpeg_plat);
#endif

#ifdef CONFIG_VIDEO_MFC50
	/* mfc */
	s3c_mfc_set_platdata(NULL);
#endif

#ifdef CONFIG_S3C_DEV_HSMMC
	s5pv210_default_sdhci0();
#endif
#ifdef CONFIG_S3C_DEV_HSMMC1
	s5pv210_default_sdhci1();
#endif
#ifdef CONFIG_S3C_DEV_HSMMC2
	s5pv210_default_sdhci2();
#endif
#ifdef CONFIG_S3C_DEV_HSMMC3
	s5pv210_default_sdhci3();
#endif
#ifdef CONFIG_S5PV210_SETUP_SDHCI
	s3c_sdhci_set_platdata();
#endif

#ifdef CONFIG_CPU_FREQ
	s5pv210_cpufreq_set_platdata(&smdkc110_cpufreq_plat);
#endif

	regulator_has_full_constraints();

	register_reboot_notifier(&wave_reboot_notifier);

	wave_switch_init();

	//gps_gpio_init();

	uart_switch_init();

	wave_init_wifi_mem();

	//onenand_init();
	
	/* write something into the INFORM6 register that we can use to
	 * differentiate an unclear reboot from a clean reboot (which
	 * writes a small integer code to INFORM6).
	 */
	__raw_writel(0xee, S5P_INFORM6);
}

#ifdef CONFIG_USB_SUPPORT
/* Initializes OTG Phy. */
void otg_phy_init(void)
{
	/* USB PHY0 Enable */
	writel(readl(S5P_USB_PHY_CONTROL) | (0x1<<0),
			S5P_USB_PHY_CONTROL);
	writel((readl(S3C_USBOTG_PHYPWR) & ~(0x3<<3) & ~(0x1<<0)) | (0x1<<5),
			S3C_USBOTG_PHYPWR);
	writel((readl(S3C_USBOTG_PHYCLK) & ~(0x5<<2)) | (0x3<<0),
			S3C_USBOTG_PHYCLK);
	writel((readl(S3C_USBOTG_RSTCON) & ~(0x3<<1)) | (0x1<<0),
			S3C_USBOTG_RSTCON);
	msleep(1);
	writel(readl(S3C_USBOTG_RSTCON) & ~(0x7<<0),
			S3C_USBOTG_RSTCON);
	msleep(1);

	/* rising/falling time */
	writel(readl(S3C_USBOTG_PHYTUNE) | (0x1<<20),
			S3C_USBOTG_PHYTUNE);

	/* set DC level as 6 (6%) */
	writel((readl(S3C_USBOTG_PHYTUNE) & ~(0xf)) | (0x1<<2) | (0x1<<1),
			S3C_USBOTG_PHYTUNE);
}
EXPORT_SYMBOL(otg_phy_init);

/* USB Control request data struct must be located here for DMA transfer */
struct usb_ctrlrequest usb_ctrl __attribute__((aligned(64)));

/* OTG PHY Power Off */
void otg_phy_off(void)
{
	writel(readl(S3C_USBOTG_PHYPWR) | (0x3<<3),
			S3C_USBOTG_PHYPWR);
	writel(readl(S5P_USB_PHY_CONTROL) & ~(1<<0),
			S5P_USB_PHY_CONTROL);
}
EXPORT_SYMBOL(otg_phy_off);

void usb_host_phy_init(void)
{
	struct clk *otg_clk;

	otg_clk = clk_get(NULL, "otg");
	clk_enable(otg_clk);

	if (readl(S5P_USB_PHY_CONTROL) & (0x1<<1))
		return;

	__raw_writel(__raw_readl(S5P_USB_PHY_CONTROL) | (0x1<<1),
			S5P_USB_PHY_CONTROL);
	__raw_writel((__raw_readl(S3C_USBOTG_PHYPWR)
			& ~(0x1<<7) & ~(0x1<<6)) | (0x1<<8) | (0x1<<5),
			S3C_USBOTG_PHYPWR);
	__raw_writel((__raw_readl(S3C_USBOTG_PHYCLK) & ~(0x1<<7)) | (0x3<<0),
			S3C_USBOTG_PHYCLK);
	__raw_writel((__raw_readl(S3C_USBOTG_RSTCON)) | (0x1<<4) | (0x1<<3),
			S3C_USBOTG_RSTCON);
	__raw_writel(__raw_readl(S3C_USBOTG_RSTCON) & ~(0x1<<4) & ~(0x1<<3),
			S3C_USBOTG_RSTCON);
}
EXPORT_SYMBOL(usb_host_phy_init);

void usb_host_phy_off(void)
{
	__raw_writel(__raw_readl(S3C_USBOTG_PHYPWR) | (0x1<<7)|(0x1<<6),
			S3C_USBOTG_PHYPWR);
	__raw_writel(__raw_readl(S5P_USB_PHY_CONTROL) & ~(1<<1),
			S5P_USB_PHY_CONTROL);
}
EXPORT_SYMBOL(usb_host_phy_off);
#endif

void s3c_setup_uart_cfg_gpio(unsigned char port)
{
	switch (port) {
	case 0:
		s3c_gpio_cfgpin(GPIO_BT_RXD, S3C_GPIO_SFN(GPIO_BT_RXD_AF));
		s3c_gpio_setpull(GPIO_BT_RXD, S3C_GPIO_PULL_NONE);
		s3c_gpio_cfgpin(GPIO_BT_TXD, S3C_GPIO_SFN(GPIO_BT_TXD_AF));
		s3c_gpio_setpull(GPIO_BT_TXD, S3C_GPIO_PULL_NONE);
		s3c_gpio_cfgpin(GPIO_BT_CTS, S3C_GPIO_SFN(GPIO_BT_CTS_AF));
		s3c_gpio_setpull(GPIO_BT_CTS, S3C_GPIO_PULL_NONE);
		s3c_gpio_cfgpin(GPIO_BT_RTS, S3C_GPIO_SFN(GPIO_BT_RTS_AF));
		s3c_gpio_setpull(GPIO_BT_RTS, S3C_GPIO_PULL_NONE);
		s3c_gpio_slp_cfgpin(GPIO_BT_RXD, S3C_GPIO_SLP_PREV);
		s3c_gpio_slp_setpull_updown(GPIO_BT_RXD, S3C_GPIO_PULL_NONE);
		s3c_gpio_slp_cfgpin(GPIO_BT_TXD, S3C_GPIO_SLP_PREV);
		s3c_gpio_slp_setpull_updown(GPIO_BT_TXD, S3C_GPIO_PULL_NONE);
		s3c_gpio_slp_cfgpin(GPIO_BT_CTS, S3C_GPIO_SLP_PREV);
		s3c_gpio_slp_setpull_updown(GPIO_BT_CTS, S3C_GPIO_PULL_NONE);
		s3c_gpio_slp_cfgpin(GPIO_BT_RTS, S3C_GPIO_SLP_PREV);
		s3c_gpio_slp_setpull_updown(GPIO_BT_RTS, S3C_GPIO_PULL_NONE);
		break;
	case 1:
		s3c_gpio_cfgpin(GPIO_UART1_RXD, S3C_GPIO_SFN(GPIO_UART1_RXD_AF));
		s3c_gpio_setpull(GPIO_UART1_RXD, S3C_GPIO_PULL_UP);
		s3c_gpio_cfgpin(GPIO_UART1_TXD, S3C_GPIO_SFN(GPIO_UART1_TXD_AF));
		s3c_gpio_setpull(GPIO_UART1_TXD, S3C_GPIO_PULL_NONE);
		s3c_gpio_cfgpin(GPIO_UART1_CTS, S3C_GPIO_SFN(GPIO_UART1_CTS_AF));
		s3c_gpio_setpull(GPIO_UART1_CTS, S3C_GPIO_PULL_NONE);
		s3c_gpio_cfgpin(GPIO_UART1_RTS, S3C_GPIO_SFN(GPIO_UART1_RTS_AF));
		s3c_gpio_setpull(GPIO_UART1_RTS, S3C_GPIO_PULL_NONE);
		break;
	case 2:
		s3c_gpio_cfgpin(GPIO_AP_RXD, S3C_GPIO_SFN(GPIO_AP_RXD_AF));
		s3c_gpio_setpull(GPIO_AP_RXD, S3C_GPIO_PULL_NONE);
		s3c_gpio_cfgpin(GPIO_AP_TXD, S3C_GPIO_SFN(GPIO_AP_TXD_AF));
		s3c_gpio_setpull(GPIO_AP_TXD, S3C_GPIO_PULL_NONE);
		break;
	case 3:
		s3c_gpio_cfgpin(GPIO_FLM_RXD, S3C_GPIO_SFN(GPIO_FLM_RXD_AF));
		s3c_gpio_setpull(GPIO_FLM_RXD, S3C_GPIO_PULL_NONE);
		s3c_gpio_cfgpin(GPIO_FLM_TXD, S3C_GPIO_SFN(GPIO_FLM_TXD_AF));
		s3c_gpio_setpull(GPIO_FLM_TXD, S3C_GPIO_PULL_NONE);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(s3c_setup_uart_cfg_gpio);

MACHINE_START(WAVE, "wave")
	.boot_params	= S5P_PA_SDRAM + 0x100,
	.fixup		= wave_fixup,
	.init_irq	= s5pv210_init_irq,
	.map_io		= wave_map_io,
	.init_machine	= wave_machine_init,
#if	defined(CONFIG_S5P_HIGH_RES_TIMERS)
	.timer		= &s5p_systimer,
#else
	.timer		= &s3c24xx_timer,
#endif
MACHINE_END

MACHINE_START(WAVE2, "wave2")
	.boot_params	= S5P_PA_SDRAM + 0x100,
	.fixup		= wave_fixup,
	.init_irq	= s5pv210_init_irq,
	.map_io		= wave_map_io,
	.init_machine	= wave_machine_init,
#if	defined(CONFIG_S5P_HIGH_RES_TIMERS)
	.timer		= &s5p_systimer,
#else
	.timer		= &s3c24xx_timer,
#endif
MACHINE_END
