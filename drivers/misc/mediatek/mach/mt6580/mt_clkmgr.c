/**
* @file    mt_clkmgr.c
* @brief   Driver for Clock Manager
*
*/

/*=============================================================*/
// Include files
/*=============================================================*/

// system includes
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <asm/uaccess.h>
#include <linux/sched_clock.h>
#include <linux/seq_file.h>

#include <mach/mt_typedefs.h>
#include <mach/sync_write.h>
#include <mach/mt_dcm.h>
#include <mach/mt_spm.h>
#include <mach/mt_spm_mtcmos.h>
#include <mach/mt_spm_sleep.h>
#include <mach/mt_freqhopping.h>
#include <mach/mt_gpufreq.h>
#include <mach/mt_clkmgr.h>
// hack for gps
#include <mach/mt_clkbuf_ctl.h>


#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_address.h>

void __iomem	*clk_apmixed_base;
void __iomem	*clk_topcksys_base;
void __iomem	*clk_infracfg_ao_base;
void __iomem	*clk_audio_base;
void __iomem	*clk_mfgcfg_base;
void __iomem	*clk_mmsys_config_base;
void __iomem	*clk_imgsys_base;

#endif

/*=============================================================*/
// Macro definition
/*=============================================================*/

// #define BIT(_bit_)          (unsigned int)(1 << (_bit_))
#define BITS(_bits_, _val_) ((((unsigned) -1 >> (31 - ((1) ? _bits_))) & ~((1U << ((0) ? _bits_)) - 1)) & ((_val_)<<((0) ? _bits_)))
#define BITMASK(_bits_)     (((unsigned) -1 >> (31 - ((1) ? _bits_))) & ~((1U << ((0) ? _bits_)) - 1))



//
// CONFIG
//
// #define CONFIG_CLKMGR_EMULATION
// #define CONFIG_CLKMGR_SHOWLOG
// #define STATE_CHECK_DEBUG // TODO: disable first @ FPGA mode

//
// LOG
//
#define pr_debug printk
#define TAG     "Power/clkmgr"
#define HEX_FMT "0x%08x"

#define FUNC_LV_API         BIT(0)
#define FUNC_LV_LOCKED      BIT(1)
#define FUNC_LV_BODY        BIT(2)
#define FUNC_LV_OP          BIT(3)
#define FUNC_LV_REG_ACCESS  BIT(4)
#define FUNC_LV_DONT_CARE   BIT(5)

#define FUNC_LV_MASK        (FUNC_LV_API | FUNC_LV_LOCKED | FUNC_LV_BODY | FUNC_LV_OP | FUNC_LV_REG_ACCESS | FUNC_LV_DONT_CARE)

#if defined(CONFIG_CLKMGR_SHOWLOG)
#define ENTER_FUNC(lv)      do { if (lv & FUNC_LV_MASK) pr_debug(ANDROID_LOG_WARN, TAG, ">> %s()\n", __FUNCTION__); } while(0)
#define EXIT_FUNC(lv)       do { if (lv & FUNC_LV_MASK) pr_debug(ANDROID_LOG_WARN, TAG, "<< %s():%d\n", __FUNCTION__, __LINE__); } while(0)
#else
#define ENTER_FUNC(lv)
#define EXIT_FUNC(lv)
#endif // defined(CONFIG_CLKMGR_SHOWLOG)

//
// Register access function
//

#if defined(CONFIG_CLKMGR_SHOWLOG)
#define clk_readl(addr) \
    ((FUNC_LV_REG_ACCESS & FUNC_LV_MASK) ? pr_debug(ANDROID_LOG_WARN, TAG, "clk_readl("HEX_FMT") @ %s():%d\n", (addr), __FUNCTION__, __LINE__) : 0, DRV_Reg32(addr))

#define clk_writel(addr, val)   \
    do { unsigned int value; if (FUNC_LV_REG_ACCESS & FUNC_LV_MASK) pr_debug(ANDROID_LOG_WARN, TAG, "clk_writel("HEX_FMT", "HEX_FMT") @ %s():%d\n", (addr), (value = val), __FUNCTION__, __LINE__); mt65xx_reg_sync_writel((value), (addr)); } while(0)

#define clk_setl(addr, val) \
    do { if (FUNC_LV_REG_ACCESS & FUNC_LV_MASK) pr_debug(ANDROID_LOG_WARN, TAG, "clk_setl("HEX_FMT", "HEX_FMT") @ %s():%d\n", (addr), (val), __FUNCTION__, __LINE__); mt65xx_reg_sync_writel(clk_readl(addr) | (val), (addr)); } while(0)

#define clk_clrl(addr, val) \
    do { if (FUNC_LV_REG_ACCESS & FUNC_LV_MASK) pr_debug(ANDROID_LOG_WARN, TAG, "clk_clrl("HEX_FMT", "HEX_FMT") @ %s():%d\n", (addr), (val), __FUNCTION__, __LINE__); mt65xx_reg_sync_writel(clk_readl(addr) & ~(val), (addr)); } while(0)
#else
#define clk_readl(addr) \
    DRV_Reg32(addr)

#define clk_writel(addr, val)   \
    mt_reg_sync_writel(val, addr)

#define clk_setl(addr, val) \
    mt_reg_sync_writel(clk_readl(addr) | (val), addr)

#define clk_clrl(addr, val) \
    mt_reg_sync_writel(clk_readl(addr) & ~(val), addr)

#endif // defined(CONFIG_CLKMGR_SHOWLOG)

/************************************************
 **********      global variablies     **********
 ************************************************/
#define PWR_DOWN    0
#define PWR_ON      1



//
// INIT
//
static int initialized = 0;
static int slp_chk_mtcmos_pll_stat = 0;
//
// LOCK
//
static DEFINE_SPINLOCK(clock_lock);

#define clkmgr_lock(flags) \
do { \
    spin_lock_irqsave(&clock_lock, flags); \
} while(0)

#define clkmgr_unlock(flags) \
do { \
    spin_unlock_irqrestore(&clock_lock, flags); \
} while(0)

#define clkmgr_locked()  (spin_is_locked(&clock_lock))

int clkmgr_is_locked(void)
{
    return clkmgr_locked();
}
EXPORT_SYMBOL(clkmgr_is_locked);


//
// CG_CLK variable & function
//
static struct subsys syss[]; // NR_SYSS
static struct cg_grp grps[]; // NR_GRPS
static struct cg_clk clks[]; // NR_CLKS
static struct pll plls[];    // NR_PLLS

static struct cg_clk_ops general_gate_cg_clk_ops; // XXX: set/clr/sta addr are diff
static struct cg_clk_ops general_en_cg_clk_ops;   // XXX: set/clr/sta addr are diff
static struct cg_clk_ops rw_cg_clk_ops;       // XXX: without CLR (i.e. set/clr/sta addr are same)
static struct cg_clk_ops ao_cg_clk_ops;           // XXX: always on
static struct cg_clk_ops rw_en_cg_clk_ops;

static struct cg_clk clks[] = // NR_CLKS
{
// CG_MIXED
[MT_CG_SYS_26M]	= {
		.name = __stringify(MT_CG_SYS_26M),
		.cnt = 1,
		.mask = BIT(0),
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MIXED],
		.src = MT_CG_INVALID,
	}, // pll
[MT_CG_SYS_TEMP] = {
		.name = __stringify(MT_CG_SYS_TEMP),
		.cnt = 1,
		.mask = BIT(0),
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MIXED],
		.src = MT_CG_INVALID,
	}, // sysyem fake
[MT_CG_MEMPLL] = {
		.name = __stringify(MT_CG_MEMPLL),
		.cnt = 1,
		.mask = BIT(0),
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MIXED],
		.src = MT_CG_INVALID,
	}, // pll
[MT_CG_UNIV_48M] = {
		.name = __stringify(MT_CG_UNIV_48M),
		.cnt = 1,
		.mask = UNIV48M_EN_BIT,
		.ops = &rw_en_cg_clk_ops,
		.grp = &grps[CG_MIXED],
		.src = MT_CG_INVALID,
		.parent = &plls[UNIVPLL],
	}, // pll
[MT_CG_USB_48M] = {
		.name = __stringify(MT_CG_USB_48M),
		.cnt = 1,
		.mask = USB48M_EN_BIT,
		.ops = &rw_en_cg_clk_ops,
		.grp = &grps[CG_MIXED],
		.src = MT_CG_INVALID, 
		.parent = &plls[UNIVPLL],
	}, // pll

// CG_MPLL
[MT_CG_MPLL_D2] = {
		.name = __stringify(MT_CG_MPLL_D2),
		.cnt = 1,
		.mask = MPLL_D2_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_INVALID,
	}, // pll
[MT_CG_MPLL_D3] = {
		.name = __stringify(MT_CG_MPLL_D3),
		.cnt = 1,
		.mask = MPLL_D3_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_INVALID,
	}, // pll
[MT_CG_MPLL_D5] = {
		.name = __stringify(MT_CG_MPLL_D5),
		.cnt = 1,
		.mask = MPLL_D5_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_INVALID,
	}, // pll
[MT_CG_MPLL_D7] = {
		.name = __stringify(MT_CG_MPLL_D7),
		.cnt = 1,
		.mask = MPLL_D7_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_INVALID,
	}, // pll
[MT_CG_MPLL_D11] = {
		.name = __stringify(MT_CG_MPLL_D11),
		.cnt = 1,
		.mask = MPLL_D11_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_INVALID,
	},
[MT_CG_MPLL_D4] = {
		.name = __stringify(MT_CG_MPLL_D4),
		.cnt = 1,
		.mask = MPLL_D4_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D2,
	},
[MT_CG_MPLL_D6] = {
		.name = __stringify(MT_CG_MPLL_D6),
		.cnt = 1,
		.mask = MPLL_D6_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D3,
	},
[MT_CG_MPLL_D10] = {
		.name = __stringify(MT_CG_MPLL_D10),
		.cnt = 1,
		.mask = MPLL_D10_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D5,
	},
[MT_CG_MPLL_D14] = {
		.name = __stringify(MT_CG_MPLL_D14),
		.cnt = 1,
		.mask = MPLL_D14_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D7,
	},
[MT_CG_MPLL_D8] = {
		.name = __stringify(MT_CG_MPLL_D8),
		.cnt = 1,
		.mask = MPLL_D8_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D4,
	},
[MT_CG_MPLL_D12] = {
		.name = __stringify(MT_CG_MPLL_D12),
		.cnt = 1,
		.mask = MPLL_D12_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D6,
	},
[MT_CG_MPLL_D20] = {
		.name = __stringify(MT_CG_MPLL_D20),
		.cnt = 1,
		.mask = MPLL_D20_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D10,
	},
[MT_CG_MPLL_D28] = {
		.name = __stringify(MT_CG_MPLL_D28),
		.cnt = 1,
		.mask = MPLL_D28_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D14,
	},
[MT_CG_MPLL_D16] = {
		.name = __stringify(MT_CG_MPLL_D16),
		.cnt = 1,
		.mask = MPLL_D16_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D8,
	},
[MT_CG_MPLL_D24] = {
		.name = __stringify(MT_CG_MPLL_D24),
		.cnt = 1,
		.mask = MPLL_D24_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D12,
	},
[MT_CG_MPLL_D40] = {
		.name = __stringify(MT_CG_MPLL_D40),
		.cnt = 1,
		.mask = MPLL_D40_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_MPLL],
		.src = MT_CG_MPLL_D20,
	},
//CG_INFRA_AO
[MT_CG_PLL2_CK] = {
		.name = __stringify(MT_CG_PLL2_CK),
		.cnt = 1,
		.mask = PLL2_CK_BIT,
		.ops = &rw_cg_clk_ops,
		.grp = &grps[CG_INFRA_AO],
		.src = MT_CG_MPLL_D2,
	},
// CG_UPLL
[MT_CG_UPLL_D2] = {
		.name = __stringify(MT_CG_UPLL_D2),
		.cnt = 1,
		.mask = UPLL_D2_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_INVALID,
		.parent = &plls[UNIVPLL],
	}, // pll
[MT_CG_UPLL_D3] = {
		.name = __stringify(MT_CG_UPLL_D3),
		.cnt = 1,
		.mask = UPLL_D3_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_INVALID,
		.parent = &plls[UNIVPLL],
	}, // pll
[MT_CG_UPLL_D5] = {
		.name = __stringify(MT_CG_UPLL_D5),
		.cnt = 1,
		.mask = UPLL_D5_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_INVALID,
		.parent = &plls[UNIVPLL],
	}, // pll
[MT_CG_UPLL_D7] = {
		.name = __stringify(MT_CG_UPLL_D7),
		.cnt = 1,
		.mask = UPLL_D7_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_INVALID,
		.parent = &plls[UNIVPLL],
	}, // pll
[MT_CG_UPLL_D4] = {
		.name = __stringify(MT_CG_UPLL_D4),
		.cnt = 1,
		.mask = UPLL_D4_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D2,
	},
[MT_CG_UPLL_D6] = {
		.name = __stringify(MT_CG_UPLL_D6),
		.cnt = 1,
		.mask = UPLL_D6_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D3,
	},
[MT_CG_UPLL_D10] = {
		.name = __stringify(MT_CG_UPLL_D10),
		.cnt = 1,
		.mask = UPLL_D10_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D5,
	},
[MT_CG_UPLL_D8] = {
		.name = __stringify(MT_CG_UPLL_D8),
		.cnt = 1,
		.mask = UPLL_D8_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D4,
	},
[MT_CG_UPLL_D12] = {
		.name = __stringify(MT_CG_UPLL_D12),
		.cnt = 1,
		.mask = UPLL_D12_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D6,
	},
[MT_CG_UPLL_D20] = {
		.name = __stringify(MT_CG_UPLL_D20),
		.cnt = 1,
		.mask = UPLL_D20_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D10,
	},
[MT_CG_UPLL_D16] = {
		.name = __stringify(MT_CG_UPLL_D16),
		.cnt = 1,
		.mask = UPLL_D16_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D8,
	},
[MT_CG_UPLL_D24] = {
		.name = __stringify(MT_CG_UPLL_D24),
		.cnt = 1,
		.mask = UPLL_D24_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_UPLL],
		.src = MT_CG_UPLL_D12,
	},

// CG_CTRL0 (PERI/INFRA)
[MT_CG_PWM_MM_SW_CG] = {
		.name = __stringify(MT_CG_PWM_MM_SW_CG),
		.cnt = 0,
		.mask = PWM_MM_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // rg_pwm_mux_sel
[MT_CG_CAM_MM_SW_CG] = {
		.name = __stringify(MT_CG_CAM_MM_SW_CG),
		.cnt = 1,
		.mask = CAM_MM_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // rg_cam_mux_sel
[MT_CG_MFG_MM_SW_CG] = {
		.name = __stringify(MT_CG_MFG_MM_SW_CG),
		.cnt = 0,
		.mask = MFG_MM_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // rg_mfg_gfmux_sel, rg_mfg_mux_sel
[MT_CG_SPM_52M_SW_CG] = {
		.name = __stringify(MT_CG_SPM_52M_SW_CG),
		.cnt = 1,
		.mask = SPM_52M_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // rg_spm_52m_ck_sel
[MT_CG_MIPI_26M_DBG_EN] = {
		.name = __stringify(MT_CG_MIPI_26M_DBG_EN),
		.cnt = 1,
		.mask = MIPI_26M_DBG_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // feature
[MT_CG_SCAM_MM_SW_CG] = {
		.name = __stringify(MT_CG_SCAM_MM_SW_CG),
		.cnt = 1,
		.mask = SCAM_MM_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, 
[MT_CG_SC_26M_CK_SEL_EN] = {
		.name = __stringify(MT_CG_SC_26M_CK_SEL_EN),
		.cnt = 1,
		.mask = SC_26M_CK_SEL_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // feature
[MT_CG_SC_MEM_CK_OFF_EN] = {
		.name = __stringify(MT_CG_SC_MEM_CK_OFF_EN),
		.cnt = 1,
		.mask = SC_MEM_CK_OFF_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // feature
[MT_CG_SC_AXI_CK_OFF_EN] = {
		.name = __stringify(MT_CG_SC_AXI_CK_OFF_EN),
		.cnt = 1,
		.mask = SC_AXI_CK_OFF_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // feature
[MT_CG_SMI_MM_SW_CG] = {
		.name = __stringify(MT_CG_SMI_MM_SW_CG),
		.cnt = 1,
		.mask = SMI_MM_SW_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // rg_pwm_mux_sel
[MT_CG_ARM_EMICLK_WFI_GATING_DIS] = {
		.name = __stringify(MT_CG_ARM_EMICLK_WFI_GATING_DIS),
		.cnt = 1,
		.mask = ARM_EMICLK_WFI_GATING_DIS_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // feature
[MT_CG_ARMDCM_CLKOFF_EN] = {
		.name = __stringify(MT_CG_ARMDCM_CLKOFF_EN),
		.cnt = 1,
		.mask = ARMDCM_CLKOFF_EN_BIT,
		.ops = &general_en_cg_clk_ops,
		.grp = &grps[CG_CTRL0],
		.src = MT_CG_INVALID,
	}, // feature

// CG_CTRL1 (PERI/INFRA)
[MT_CG_THEM_SW_CG] = {
		.name = __stringify(MT_CG_THEM_SW_CG),
		.cnt = 1,
		.mask = THEM_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M
[MT_CG_APDMA_SW_CG] = {
		.name = __stringify(MT_CG_APDMA_SW_CG),
		.cnt = 1,
		.mask = APDMA_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_I2C0_SW_CG] = {
		.name = __stringify(MT_CG_I2C0_SW_CG),
		.cnt = 1,
		.mask = I2C0_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_I2C1_SW_CG] = {
		.name = __stringify(MT_CG_I2C1_SW_CG),
		.cnt = 1,
		.mask = I2C1_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_AUXADC1_SW_CG] = {
		.name = __stringify(MT_CG_AUXADC1_SW_CG),
		.cnt = 1,
		.mask = AUXADC1_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M
[MT_CG_NFI_SW_CG] = {
		.name = __stringify(MT_CG_NFI_SW_CG),
		.cnt = 1,
		.mask = NFI_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_NFIECC_SW_CG] = {
		.name = __stringify(MT_CG_NFIECC_SW_CG),
		.cnt = 1,
		.mask = NFIECC_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_DEBUGSYS_SW_CG] = {
		.name = __stringify(MT_CG_DEBUGSYS_SW_CG),
		.cnt = 1,
		.mask = DEBUGSYS_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_PWM_SW_CG] = {
		.name = __stringify(MT_CG_PWM_SW_CG),
		.cnt = 1,
		.mask = PWM_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_UART0_SW_CG] = {
		.name = __stringify(MT_CG_UART0_SW_CG),
		.cnt = 0,
		.mask = UART0_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // rg_uart0_gfmux_sel
[MT_CG_UART1_SW_CG] = {
		.name = __stringify(MT_CG_UART1_SW_CG),
		.cnt = 0,
		.mask = UART1_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // rg_uart1_gfmux_sel
[MT_CG_BTIF_SW_CG] = {
		.name = __stringify(MT_CG_BTIF_SW_CG),
		.cnt = 1,
		.mask = BTIF_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_USB_SW_CG] = {
		.name = __stringify(MT_CG_USB_SW_CG),
		.cnt = 1,
		.mask = USB_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_FHCTL_SW_CG] = {
		.name = __stringify(MT_CG_FHCTL_SW_CG),
		.cnt = 1,
		.mask = FHCTL_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M
[MT_CG_AUXADC2_SW_CG] = {
		.name = __stringify(MT_CG_AUXADC2_SW_CG),
		.cnt = 1,
		.mask = AUXADC2_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M
[MT_CG_I2C2_SW_CG] = {
		.name = __stringify(MT_CG_I2C2_SW_CG),
		.cnt = 1,
		.mask = I2C2_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // rg_spinfi_gfmux_sel, rg_spinfi_mux_sel
[MT_CG_MSDC0_SW_CG] = {
		.name = __stringify(MT_CG_MSDC0_SW_CG),
		.cnt = 0,
		.mask = MSDC0_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // rg_msdc0_mux_sel
[MT_CG_MSDC1_SW_CG] = {
		.name = __stringify(MT_CG_MSDC1_SW_CG),
		.cnt = 0,
		.mask = MSDC1_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // rg_msdc1_mux_sel
[MT_CG_NFI2X_SW_CG] = {
		.name = __stringify(MT_CG_NFI2X_SW_CG),
		.cnt = 1,
		.mask = NFI2X_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, 
[MT_CG_PMIC_SW_CG_AP] = {
		.name = __stringify(MT_CG_PMIC_SW_CG_AP),
		.cnt = 1,
		.mask = PMIC_SW_CG_AP_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, 
[MT_CG_SEJ_SW_CG] = {
		.name = __stringify(MT_CG_SEJ_SW_CG),
		.cnt = 1,
		.mask = SEJ_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M, AXIBUS
[MT_CG_MEMSLP_DLYER_SW_CG] = {
		.name = __stringify(MT_CG_MEMSLP_DLYER_SW_CG),
		.cnt = 1,
		.mask = MEMSLP_DLYER_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID
	}, // feature
[MT_CG_SPI_SW_CG] = {
		.name = __stringify(MT_CG_SPI_SW_CG),
		.cnt = 1,
		.mask = SPI_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID
	}, // feature
[MT_CG_APXGPT_SW_CG] = {
		.name = __stringify(MT_CG_APXGPT_SW_CG),
		.cnt = 1,
		.mask = APXGPT_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M, AXIBUS
[MT_CG_AUDIO_SW_CG] = {
		.name = __stringify(MT_CG_AUDIO_SW_CG),
		.cnt = 1,
		.mask = AUDIO_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // rg_aud_intbus_sel
[MT_CG_SPM_SW_CG] = {
		.name = __stringify(MT_CG_SPM_SW_CG),
		.cnt = 1,
		.mask = SPM_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M
[MT_CG_PMIC_SW_CG_MD] = {
		.name = __stringify(MT_CG_PMIC_SW_CG_MD),
		.cnt = 1,
		.mask = PMIC_SW_CG_MD_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	},
[MT_CG_PMIC_SW_CG_CONN] = {
		.name = __stringify(MT_CG_PMIC_SW_CG_CONN),
		.cnt = 1,
		.mask = PMIC_SW_CG_CONN_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, 
[MT_CG_PMIC_26M_SW_CG] = {
		.name = __stringify(MT_CG_PMIC_26M_SW_CG),
		.cnt = 1,
		.mask = PMIC_26M_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M
[MT_CG_AUX_SW_CG_ADC] = {
		.name = __stringify(MT_CG_AUX_SW_CG_ADC),
		.cnt = 0,
		.mask = AUX_SW_CG_ADC_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M
[MT_CG_AUX_SW_CG_TP] = {
		.name = __stringify(MT_CG_AUX_SW_CG_TP),
		.cnt = 1,
		.mask = AUX_SW_CG_TP_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL1],
		.src = MT_CG_INVALID,
	}, // MT_CG_SYS_26M

//CG_CTRL2
[MT_CG_RBIST_SW_CG] = {
		.name = __stringify(MT_CG_RBIST_SW_CG),
		.cnt = 1,
		.mask = RBIST_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
},
[MT_CG_NFI_BUS_SW_CG] = {
		.name = __stringify(MT_CG_NFI_BUS_SW_CG),
		.cnt = 1,
		.mask = NFI_BUS_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_GCE_SW_CG] = {
		.name = __stringify(MT_CG_GCE_SW_CG),
		.cnt = 1,
		.mask = GCE_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_TRNG_SW_CG] = {
		.name = __stringify(MT_CG_TRNG_SW_CG),
		.cnt = 1,
		.mask = TRNG_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_SEJ_13M_SW_CG] = {
		.name = __stringify(MT_CG_SEJ_13M_SW_CG),
		.cnt = 1,
		.mask = SEJ_13M_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_AES_SW_CG] = {
		.name = __stringify(MT_CG_AES_SW_CG),
		.cnt = 1,
		.mask = AES_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_PWM_BCLK_SW_CG] = {
		.name = __stringify(MT_CG_PWM_BCLK_SW_CG),
		.cnt = 1,
		.mask = PWM_BCLK_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_PWM1_FBCLK_SW_CG] = {
		.name = __stringify(MT_CG_PWM1_FBCLK_SW_CG),
		.cnt = 1,
		.mask = PWM1_FBCLK_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_PWM2_FBCLK_SW_CG] = {
		.name = __stringify(MT_CG_PWM2_FBCLK_SW_CG),
		.cnt = 1,
		.mask = PWM2_FBCLK_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_PWM3_FBCLK_SW_CG] = {
		.name = __stringify(MT_CG_PWM3_FBCLK_SW_CG),
		.cnt = 1,
		.mask = PWM3_FBCLK_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_PWM4_FBCLK_SW_CG] = {
		.name = __stringify(MT_CG_PWM4_FBCLK_SW_CG),
		.cnt = 1,
		.mask = PWM4_FBCLK_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},
[MT_CG_PWM5_FBCLK_SW_CG] = {
		.name = __stringify(MT_CG_PWM5_FBCLK_SW_CG),
		.cnt = 1,
		.mask = PWM5_FBCLK_SW_CG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_CTRL2],
		.src = MT_CG_INVALID,
	},

// CG_MMSYS0
[MT_CG_DISP0_SMI_COMMON] = {
		.name = __stringify(MT_CG_DISP0_SMI_COMMON),
		.cnt = 0,
		.mask = SMI_COMMON_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_SMI_LARB0] = {
		.name = __stringify(MT_CG_DISP0_SMI_LARB0),
		.cnt = 0,
		.mask = SMI_LARB0_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_CAM_MDP] = {
		.name = __stringify(MT_CG_DISP0_CAM_MDP),
		.cnt = 0,
		.mask = CAM_MDP_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_MDP_RDMA] = {
		.name = __stringify(MT_CG_DISP0_MDP_RDMA),
		.cnt = 0,
		.mask = MDP_RDMA_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_MDP_RSZ0] 	= {
		.name = __stringify(MT_CG_DISP0_MDP_RSZ0),
		.cnt = 0,
		.mask = MDP_RSZ0_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_MDP_RSZ1] = {
		.name = __stringify(MT_CG_DISP0_MDP_RSZ1),
		.cnt = 0,
		.mask = MDP_RSZ1_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_MDP_TDSHP] = {
		.name = __stringify(MT_CG_DISP0_MDP_TDSHP),
		.cnt = 0,
		.mask = MDP_TDSHP_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_MDP_WDMA] = {
		.name = __stringify(MT_CG_DISP0_MDP_WDMA),
		.cnt = 0,
		.mask = MDP_WDMA_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_MDP_WROT] = {
		.name = __stringify(MT_CG_DISP0_MDP_WROT),
		.cnt = 0,
		.mask = MDP_WROT_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_FAKE_ENG] = {
		.name = __stringify(MT_CG_DISP0_FAKE_ENG),
		.cnt = 0,
		.mask = FAKE_ENG_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_DISP_OVL0] = {
		.name = __stringify(MT_CG_DISP0_DISP_OVL0),
		.cnt = 0,
		.mask = DISP_OVL0_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_DISP_RDMA0] = {
		.name = __stringify(MT_CG_DISP0_DISP_RDMA0),
		.cnt = 0,
		.mask = DISP_RDMA0_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_DISP_WDMA0] = {
		.name = __stringify(MT_CG_DISP0_DISP_WDMA0),
		.cnt = 0,
		.mask = DISP_WDMA0_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_DISP_COLOR] = {
		.name = __stringify(MT_CG_DISP0_DISP_COLOR),
		.cnt = 0,
		.mask = DISP_COLOR_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_DISP_AAL] = {
		.name = __stringify(MT_CG_DISP0_DISP_AAL),
		.cnt = 0,
		.mask = DISP_AAL_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_DISP_GAMMA] = {
		.name = __stringify(MT_CG_DISP0_DISP_GAMMAG),
		.cnt = 0,
		.mask = DISP_GAMMA_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP0_DISP_DITHER] = {
		.name = __stringify(MT_CG_DISP0_DISP_DITHER),
		.cnt = 0,
		.mask = DISP_DITHER_BIT,
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MMSYS0],
		.src = MT_CG_INVALID,
	},

// CG_MMSYS1
[MT_CG_DISP1_DSI_ENGINE] = {
		.name = __stringify(MT_CG_DISP1_DSI_ENGINE),
		.cnt = 0,
		.mask = DSI_ENGINE_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_MMSYS1],
		.src = MT_CG_INVALID,
	},
[MT_CG_DISP1_DSI_DIGITAL] = {
		.name = __stringify(MT_CG_DISP1_DSI_DIGITAL),
		.cnt = 0,
		.mask = DSI_DIGITAL_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_MMSYS1],
		.src = MT_CG_INVALID,
	},

// CG_IMGSYS
[MT_CG_LARB1_SMI_CKPDN] = {
		.name = __stringify(MT_CG_LARB1_SMI_CKPDN),
		.cnt = 1,
		.mask = LARB1_SMI_CKPDN_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_IMGSYS],
		.src = MT_CG_INVALID,
	},
[MT_CG_CAM_SMI_CKPDN] = {
		.name = __stringify(MT_CG_CAM_SMI_CKPDN),
		.cnt = 1,
		.mask = CAM_SMI_CKPDN_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_IMGSYS],
		.src = MT_CG_INVALID,
	},
[MT_CG_CAM_CAM_CKPDN] = {
		.name = __stringify(MT_CG_CAM_CAM_CKPDN),
		.cnt = 1,
		.mask = CAM_CAM_CKPDN_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_IMGSYS],
		.src = MT_CG_INVALID,
	},
[MT_CG_SEN_TG_CKPDN] = {
		.name = __stringify(MT_CG_SEN_TG_CKPDN),
		.cnt = 1,
		.mask = SEN_TG_CKPDN_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_IMGSYS],
		.src = MT_CG_INVALID,
	},
[MT_CG_SEN_CAM_CKPDN] = {
		.name = __stringify(MT_CG_SEN_CAM_CKPDN),
		.cnt = 1,
		.mask = SEN_CAM_CKPDN_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_IMGSYS],
		.src = MT_CG_INVALID,
	},
[MT_CG_VCODEC_CKPDN] = {
		.name = __stringify(MT_CG_VCODEC_CKPDN),
		.cnt = 1,
		.mask = VCODEC_CKPDN_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_IMGSYS],
		.src = MT_CG_INVALID,
	},

// CG_MFG
[MT_CG_BG3D] = {
		.name = __stringify(MT_CG_BG3D),
		.cnt = 1,
		.mask = BG3D_BIT,
		.ops = &general_gate_cg_clk_ops,
		.grp = &grps[CG_MFGSYS],
		.src = MT_CG_MFG_MM_SW_CG,
	},

// CG_AUDIO
[MT_CG_AUD_PDN_AFE_EN] = {
		.name = __stringify(MT_CG_AUD_PDN_AFE_EN),
		.cnt = 1,
		.mask = AUD_PDN_AFE_EN_BIT,
		.ops = &rw_cg_clk_ops,
		.grp = &grps[CG_AUDIO],
		.src = MT_CG_AUDIO_SW_CG,
	},
[MT_CG_AUD_PDN_I2S_EN] = {
		.name = __stringify(MT_CG_AUD_PDN_I2S_EN),
		.cnt = 1,
		.mask = AUD_PDN_I2S_EN_BIT,
		.ops = &rw_cg_clk_ops,
		.grp = &grps[CG_AUDIO],
		.src = MT_CG_INVALID,
	}, // from connsys
[MT_CG_AUD_PDN_ADC_EN] = {
		.name = __stringify(MT_CG_AUD_PDN_ADC_EN),
		.cnt = 1,
		.mask = AUD_PDN_ADC_EN_BIT,
		.ops = &rw_cg_clk_ops,
		.grp = &grps[CG_AUDIO],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_AUD_PDN_DAC_EN] = {
		.name = __stringify(MT_CG_AUD_PDN_DAC_EN),
		.cnt = 1,
		.mask = AUD_PDN_DAC_EN_BIT,
		.ops = &rw_cg_clk_ops,
		.grp = &grps[CG_AUDIO],
		.src = MT_CG_INVALID,
	}, // AXIBUS
[MT_CG_INVALID] = {
		.name = __stringify(MT_CG_INVALID),
		.cnt = 1,
		.mask =  BIT(0),
		.ops = &ao_cg_clk_ops,
		.grp = &grps[CG_MIXED],
		.src = MT_CG_INVALID,
},
};

static struct cg_clk *id_to_clk(cg_clk_id id)
{
    return id < NR_CLKS ? &clks[id] : NULL;
}

// general_cg_clk_ops

static int general_cg_clk_check_validity_op(struct cg_clk *clk)
{
    int valid = 0;

    ENTER_FUNC(FUNC_LV_OP);

    if (clk->mask & clk->grp->mask)
    {
        valid = 1;
    }

    EXIT_FUNC(FUNC_LV_OP);
    return valid;
}

// general_gate_cg_clk_ops

static int general_gate_cg_clk_get_state_op(struct cg_clk *clk)
{
    struct subsys *sys = clk->grp->sys;

    ENTER_FUNC(FUNC_LV_OP);

    EXIT_FUNC(FUNC_LV_OP);

    if (sys && !sys->state)
    {
        return PWR_DOWN;
    }
    else
    {
        return (clk_readl(clk->grp->sta_addr) & (clk->mask)) ? PWR_DOWN : PWR_ON ; // clock gate
    }
}

static int general_gate_cg_clk_enable_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);

    clk_writel(clk->grp->clr_addr, clk->mask); // clock gate

    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static int general_gate_cg_clk_disable_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);

    clk_writel(clk->grp->set_addr, clk->mask); // clock gate

    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static struct cg_clk_ops general_gate_cg_clk_ops =
{
    .get_state      = general_gate_cg_clk_get_state_op,
    .check_validity = general_cg_clk_check_validity_op,
    .enable         = general_gate_cg_clk_enable_op,
    .disable        = general_gate_cg_clk_disable_op,
};

// general_en_cg_clk_ops

static int general_en_cg_clk_get_state_op(struct cg_clk *clk)
{
    struct subsys *sys = clk->grp->sys;

    ENTER_FUNC(FUNC_LV_OP);

    EXIT_FUNC(FUNC_LV_OP);

    if (sys && !sys->state)
    {
        return PWR_DOWN;
    }
    else
    {
        return (clk_readl(clk->grp->sta_addr) & (clk->mask)) ? PWR_ON : PWR_DOWN ; // clock enable
    }
}

static int general_en_cg_clk_enable_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);

    clk_writel(clk->grp->set_addr, clk->mask); // clock enable

    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static int general_en_cg_clk_disable_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);

    clk_writel(clk->grp->clr_addr, clk->mask); // clock enable

    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static struct cg_clk_ops general_en_cg_clk_ops =
{
    .get_state      = general_en_cg_clk_get_state_op,
    .check_validity = general_cg_clk_check_validity_op,
    .enable         = general_en_cg_clk_enable_op,
    .disable        = general_en_cg_clk_disable_op,
};

static int rw_cg_clk_enable_op(struct cg_clk *clk)
{
    volatile unsigned int val = clk_readl(clk->grp->clr_addr) & ~clk->mask;

    ENTER_FUNC(FUNC_LV_OP);
    clk_writel(clk->grp->clr_addr, val);
    if(clk->src != MT_CG_MPLL_D2)
    	pr_debug("clks[%s] enabled, TOP_CON = 0x%x, mask = 0x%x CNT = %d\n",clk->name, clk_readl(clk->grp->clr_addr),~clk->mask, clk->cnt);
    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static int rw_cg_clk_disable_op(struct cg_clk *clk)
{
    volatile unsigned int val = clk_readl(clk->grp->set_addr) | clk->mask;

    ENTER_FUNC(FUNC_LV_OP);
    clk_writel(clk->grp->clr_addr, val);
    if(clk->src != MT_CG_MPLL_D2)
    	pr_debug("clks[%s] disabled, TOP_CON = 0x%x, mask = 0x%x CNT = %d\n",clk->name, clk_readl(clk->grp->clr_addr),clk->mask, clk->cnt);
    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static struct cg_clk_ops rw_cg_clk_ops =
{
    .get_state      = general_gate_cg_clk_get_state_op,
    .check_validity = general_cg_clk_check_validity_op,
    .enable         = rw_cg_clk_enable_op,
    .disable        = rw_cg_clk_disable_op,
};

static struct cg_clk_ops rw_en_cg_clk_ops =
{
    .get_state      = general_en_cg_clk_get_state_op,
    .check_validity = general_cg_clk_check_validity_op,
    .enable         = rw_cg_clk_disable_op,
    .disable        = rw_cg_clk_enable_op,
};
// ao_clk_ops

static int ao_cg_clk_get_state_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);
    EXIT_FUNC(FUNC_LV_OP);
    return PWR_ON;
}

static int ao_cg_clk_check_validity_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);
    EXIT_FUNC(FUNC_LV_OP);
    return 1;
}

static int ao_cg_clk_enable_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);
    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static int ao_cg_clk_disable_op(struct cg_clk *clk)
{
    ENTER_FUNC(FUNC_LV_OP);
    EXIT_FUNC(FUNC_LV_OP);
    return 0;
}

static struct cg_clk_ops ao_cg_clk_ops =
{
    .get_state      = ao_cg_clk_get_state_op,
    .check_validity = ao_cg_clk_check_validity_op,
    .enable         = ao_cg_clk_enable_op,
    .disable        = ao_cg_clk_disable_op,
};

//
// CG_GRP variable & function
//
static struct cg_grp_ops general_en_cg_grp_ops;
static struct cg_grp_ops general_gate_cg_grp_ops;
static struct cg_grp_ops ctrl0_cg_grp_ops;
// static struct cg_grp_ops mfg_cg_grp_ops; /* not used */

static struct cg_grp grps[] = // NR_GRPS
{
    [CG_MIXED] =
    {
        .name       = __stringify(CG_MIXED),
        .mask       = 0, // CG_MIXED_MASK,    // TODO: set @ init is better
        .ops        = &general_en_cg_grp_ops,
        .sys        = NULL,
    },
    [CG_MPLL] =
    {
        .name       = __stringify(CG_MPLL),
        .mask       = 0, // CG_MPLL_MASK,     // TODO: set @ init is better
        .ops        = &general_en_cg_grp_ops,
        .sys        = NULL,
    },
    [CG_UPLL] =
    {
        .name       = __stringify(CG_UPLL),
        .mask       = 0, // CG_UPLL_MASK,     // TODO: set @ init is better
        .ops        = &general_en_cg_grp_ops,
        .sys        = NULL,
    },
    [CG_CTRL0] =
    {
        .name       = __stringify(CG_CTRL0),
        .mask       = 0, // CG_CTRL0_MASK,    // TODO: set @ init is better
        .ops        = &ctrl0_cg_grp_ops, // XXX: not all are clock gate (e.g. MIPI_26M_DBG_EN_BIT, SC_26M_CK_SEL_EN_BIT, SC_MEM_CK_OFF_EN_BIT, GPU_491P52M_EN_BIT, GPU_500P5M_EN_BIT, ARMDCM_CLKOFF_EN_BIT)
        .sys        = NULL,
    },
    [CG_CTRL1] =
    {
        .name       = __stringify(CG_CTRL1),
        .mask       = 0, // CG_CTRL1_MASK,    // TODO: set @ init is better
        .ops        = &general_gate_cg_grp_ops,
        .sys        = NULL,
    },
    [CG_CTRL2] =
    {
        .name       = __stringify(CG_CTRL2),
        .mask       = 0, // CG_CTRL1_MASK,    // TODO: set @ init is better
        .ops        = &general_gate_cg_grp_ops,
        .sys        = NULL,
    },
    [CG_MMSYS0] =
    {
        .name       = __stringify(CG_MMSYS0),
        .mask       = 0, // CG_MMSYS0_MASK,   // TODO: set @ init is better
        .ops        = &general_gate_cg_grp_ops,
        .sys        = &syss[SYS_DIS],
    },
    [CG_MMSYS1] =
    {
        .name       = __stringify(CG_MMSYS1),
        .mask       = 0, // CG_MMSYS1_MASK,   // TODO: set @ init is better
        .ops        = &general_gate_cg_grp_ops,
        .sys        = &syss[SYS_DIS],
    },
    [CG_MFGSYS] =
    {
        .name       = __stringify(CG_MFGSYS),
        .mask       = BG3D_BIT,      // TODO: set @ init is better
        .ops        = &general_gate_cg_grp_ops,
        .sys        = &syss[SYS_MFG],
    },
    [CG_IMGSYS] =
    {
        .name       = __stringify(CG_IMGSYS),
        .mask       = 0, // CG_IMG_MASK,      // TODO: set @ init is better
        .ops        = &general_gate_cg_grp_ops,
        .sys        = &syss[SYS_IMG],
    },
    
    [CG_AUDIO] =
    {
        .name       = __stringify(CG_AUDIO),
        .mask       = 0, // CG_AUDIO_MASK,    // TODO: set @ init is better
        .ops        = &general_gate_cg_grp_ops,
        .sys        = NULL,
    },
    [CG_INFRA_AO] =
    {
        .name       = __stringify(CG_INFRA_AO),
        .mask       = 0,
        .ops        = &general_gate_cg_grp_ops,
        .sys        = NULL,
    },   
};

static struct cg_grp *id_to_grp(cg_grp_id id)
{
    return id < NR_GRPS ? &grps[id] : NULL;
}

// general_cg_grp

static int general_cg_grp_dump_regs_op(struct cg_grp *grp, unsigned int *ptr)
{
    ENTER_FUNC(FUNC_LV_OP);

    *(ptr) = clk_readl(grp->sta_addr);

    EXIT_FUNC(FUNC_LV_OP);
    return 1; // return size
}

// general_gate_cg_grp

static unsigned int general_gate_cg_grp_get_state_op(struct cg_grp *grp)
{
    volatile unsigned int val;
    struct subsys *sys = grp->sys;

    ENTER_FUNC(FUNC_LV_OP);

    EXIT_FUNC(FUNC_LV_OP);

    if (sys && !sys->state)
    {
        return 0;
    }
    else
    {
        val = clk_readl(grp->sta_addr);
        val = (~val) & (grp->mask); // clock gate

        return val;
    }
}

static struct cg_grp_ops general_gate_cg_grp_ops =
{
    .get_state = general_gate_cg_grp_get_state_op,
    .dump_regs = general_cg_grp_dump_regs_op,
};

// general_en_cg_grp

static unsigned int general_en_cg_grp_get_state_op(struct cg_grp *grp)
{
    volatile unsigned int val;
    struct subsys *sys = grp->sys;

    ENTER_FUNC(FUNC_LV_OP);

    EXIT_FUNC(FUNC_LV_OP);

    if (sys && !sys->state)
    {
        return 0;
    }
    else
    {
        val = clk_readl(grp->sta_addr);
        val &= (grp->mask); // clock enable

        return val;
    }
}

static struct cg_grp_ops general_en_cg_grp_ops =
{
    .get_state = general_en_cg_grp_get_state_op,
    .dump_regs = general_cg_grp_dump_regs_op,
};

// ctrl0_cg_grp

static unsigned int ctrl0_cg_grp_get_state_op(struct cg_grp *grp)
{
    volatile unsigned int val;
    struct subsys *sys = grp->sys;

    ENTER_FUNC(FUNC_LV_OP);

    EXIT_FUNC(FUNC_LV_OP);

    if (sys && !sys->state)
    {
        return 0;
    }
    else
    {
        val = clk_readl(grp->sta_addr);
        val = (val ^ ~(CG_CTRL0_EN_MASK)) & (grp->mask); // XXX some are enable bit and others are gate bit

        return val;
    }
}

static struct cg_grp_ops ctrl0_cg_grp_ops =
{
    .get_state = ctrl0_cg_grp_get_state_op,
    .dump_regs = general_cg_grp_dump_regs_op,
};

// mfg_cg_grp
#if 0	/* not used */
static int get_clk_state_locked(struct cg_clk *clk);

static int mfg_cg_grp_dump_regs_op(struct cg_grp *grp, unsigned int *ptr)
{
    struct cg_clk *clk = id_to_clk(MT_CG_MFG_MM_SW_CG);

    ENTER_FUNC(FUNC_LV_OP);

    if (   NULL != clk
        && PWR_ON == get_clk_state_locked(clk) // XXX: clock_is_on() locked version
        )
    {
        *(ptr) = clk_readl(grp->sta_addr);
    }
    else
    {
        *(ptr) = 0x00000000; // XXX: default value
    }

    EXIT_FUNC(FUNC_LV_OP);
    return 1; // return size
}

static struct cg_grp_ops mfg_cg_grp_ops =
{
    .get_state = general_gate_cg_grp_get_state_op,
    .dump_regs = mfg_cg_grp_dump_regs_op,
};
#endif	/* not used */

//
// enable_clock() / disable_clock()
//
static int enable_pll_locked(struct pll *pll);
static int disable_pll_locked(struct pll *pll);

static int enable_subsys_locked(struct subsys *sys);
static int disable_subsys_locked(struct subsys *sys, int force_off);

static int power_prepare_locked(struct cg_grp *grp)
{
    int err = 0;

    ENTER_FUNC(FUNC_LV_BODY);

    if (grp->sys)
    {
        err = enable_subsys_locked(grp->sys);
    }

    EXIT_FUNC(FUNC_LV_BODY);
    return err;
}

static int power_finish_locked(struct cg_grp *grp)
{
    int err = 0;

    ENTER_FUNC(FUNC_LV_BODY);

    if (grp->sys)
    {
        err = disable_subsys_locked(grp->sys, 0); // NOT force off
    }

    EXIT_FUNC(FUNC_LV_BODY);
    return err;
}

static int enable_clock_locked(struct cg_clk *clk)
{
    struct cg_grp *grp;
    unsigned int local_state;
#ifdef STATE_CHECK_DEBUG
    unsigned int reg_state;
#endif
    int err;

    ENTER_FUNC(FUNC_LV_LOCKED);

    if (NULL == clk)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return -1;
    }

    clk->cnt++;
//    pr_debug("%s[%d]:%s\n", __FUNCTION__, clk - &clks[0], clk->name); // <-XXX
//	if((clk - &clks[0] == MT_CG_MIPI_26M_DBG_EN) | (clk - &clks[0] == MT_CG_MFG_MM_SW_CG) | (clk - &clks[0] == MT_CG_BG3D))
//		pr_debug("%s[%d]:%s enabled cnt=%d state=%d\n", __FUNCTION__, clk - &clks[0], clk->name, clk->cnt, clk->ops->get_state(clk));

    if (clk->cnt > 1)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0; // XXX: have enabled and return directly
    }

    local_state = clk->state;
    grp = clk->grp;

#ifdef STATE_CHECK_DEBUG
    reg_state = clk->ops->get_state(clk);
    BUG_ON(local_state != reg_state);
#endif
    //pll check
    if (clk->parent)
    {
        enable_pll_locked(clk->parent);
    }
    //source clock check
    if (clk->src < NR_CLKS) // TODO: please use macro VALID_CG_CLK() for this (e.g. #define VALID_CG_CLK(cg_clk_id) ((cg_clk_id) < NR_CLKS))
    {
        enable_clock_locked(id_to_clk(clk->src));
    }

    //subsys check
    err = power_prepare_locked(grp);
    BUG_ON(err);

    if (local_state == PWR_ON)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0; // XXX: assume local_state & reg_state are the same
    }

    // local clock enable
    clk->ops->enable(clk);

    clk->state = PWR_ON;
    grp->state |= clk->mask;

    EXIT_FUNC(FUNC_LV_LOCKED);
    return 0;
}

static int disable_clock_locked(struct cg_clk *clk)
{
    struct cg_grp *grp;
    unsigned int local_state;
#ifdef STATE_CHECK_DEBUG
    unsigned int reg_state;
#endif
    int err;

    ENTER_FUNC(FUNC_LV_LOCKED);

    if (NULL == clk)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return -1;
    }

    if (!clk->cnt) pr_debug(KERN_EMERG "Assert @ %s\n", clk->name);
    BUG_ON(!clk->cnt);
    clk->cnt--;

//    pr_debug("%s[%d]\n", __FUNCTION__, clk - &clks[0]); // <-XXX
//	if((clk - &clks[0] == MT_CG_MIPI_26M_DBG_EN) | (clk - &clks[0] == MT_CG_MFG_MM_SW_CG) | (clk - &clks[0] == MT_CG_BG3D))
//		pr_debug("%s[%d]:%s disabled cnt=%d state=%d\n", __FUNCTION__, clk - &clks[0], clk->name, clk->cnt, clk->ops->get_state(clk));
    if (clk->cnt > 0)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0; // XXX: not count down zero and return directly
    }

    local_state = clk->state;
    grp = clk->grp;

#ifdef STATE_CHECK_DEBUG
    reg_state = clk->ops->get_state(clk);
    BUG_ON(local_state != reg_state);
#endif

    if (local_state == PWR_DOWN)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0; // XXX: assume local_state & reg_state are the same
    }

	if (clk->force_on)
		return 0;
// step 1: local clock disable
	clk->ops->disable(clk);
	clk->state = PWR_DOWN;
	grp->state &= ~(clk->mask);

// step 3: subsys check
	err = power_finish_locked(grp);
	BUG_ON(err);

// step 2: source clock check
	if (clk->src < NR_CLKS)
	{
		disable_clock_locked(id_to_clk(clk->src));
	}
// step 4: pll check
	if (clk->parent)
	{
		disable_pll_locked(clk->parent);
	}
	EXIT_FUNC(FUNC_LV_LOCKED);
	return 0;
}

static int get_clk_state_locked(struct cg_clk *clk)
{
    if (likely(initialized))
    {
        return clk->state;
    }
    else
    {
        return clk->ops->get_state(clk);
    }
}

#if 0   // XXX: debug for MT_CG_APDMA_SW_CG
static struct
{
    int cnt;
    const char *name;
} clk_cnt_rec[] =
{
    [0] = { .cnt = 0, .name = "WLAN",        },
    [1] = { .cnt = 0, .name = "btif_driver", },
    [2] = { .cnt = 0, .name = "mt-i2c",      },
    [3] = { .cnt = 0, .name = "VFIFO",       },
    [4] = { .cnt = 0, .name = "UNKNOWN",     },
};
#endif  // XXX: debug for MT_CG_APDMA_SW_CG

int mt_enable_clock(enum cg_clk_id id, const char *name)
{
    int err;
    unsigned long flags;
    struct cg_clk *clk = id_to_clk(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!clk);
    BUG_ON(!clk->grp);
    BUG_ON(!clk->ops->check_validity(clk));

    clkmgr_lock(flags);
#if 0   // XXX: debug for MT_CG_APDMA_SW_CG
    {
        int i;

        if (MT_CG_APDMA_SW_CG == id)
        {
            for (i = 0; i < ARRAY_SIZE(clk_cnt_rec) - 1; i++)
            {
                if (0 == strcmp(clk_cnt_rec[i].name, name))
                {
                    break;
                }
            }

            clk_cnt_rec[i].cnt++;
        }
    }
#endif  // XXX: debug for MT_CG_APDMA_SW_CG
//	if((id == MT_CG_UPLL_D3) | (id == MT_CG_MFG_MM_SW_CG) | (id == MT_CG_BG3D))
//		pr_debug("%s[%d]:%s before enable by %s cnt=%d state=%d\n", __FUNCTION__, clk - &clks[0], clk->name, name , clk->cnt, clk->ops->get_state(clk));
	err = enable_clock_locked(clk);
//	if((id == MT_CG_UPLL_D3) | (id == MT_CG_MFG_MM_SW_CG) | (id == MT_CG_BG3D))
//		pr_debug("%s[%d]:%s after enable by %s cnt=%d state=%d\n", __FUNCTION__, clk - &clks[0], clk->name, name , clk->cnt, clk->ops->get_state(clk));
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(mt_enable_clock);

int mt_disable_clock(enum cg_clk_id id, const char *name)
{
    int err;
    unsigned long flags;
    struct cg_clk *clk = id_to_clk(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!clk);
    BUG_ON(!clk->grp);
    BUG_ON(!clk->ops->check_validity(clk));

    clkmgr_lock(flags);
#if 0   // XXX: debug for MT_CG_APDMA_SW_CG
    {
        int i;

        if (MT_CG_APDMA_SW_CG == id)
        {
            for (i = 0; i < ARRAY_SIZE(clk_cnt_rec) - 1; i++)
            {
                if (0 == strcmp(clk_cnt_rec[i].name, name))
                {
                    break;
                }
            }

            if (!clk_cnt_rec[i].cnt) pr_debug(KERN_EMERG "Assert @ %s\n", clk_cnt_rec[i].name);
            BUG_ON(!clk_cnt_rec[i].cnt);

            clk_cnt_rec[i].cnt--;
        }
    }
#endif  // XXX: debug for MT_CG_APDMA_SW_CG
//		pr_debug("%s[%d]:%s disable by %s \n", __FUNCTION__, clk - &clks[0], clk->name, name);
//	if((id == MT_CG_UPLL_D3) | (id == MT_CG_MFG_MM_SW_CG) | (id == MT_CG_BG3D))
//		pr_debug("%s[%d]:%s before disable by %s cnt=%d state=%d\n", __FUNCTION__, clk - &clks[0], clk->name, name , clk->cnt, clk->ops->get_state(clk));
	err = disable_clock_locked(clk);
//	if((id == MT_CG_UPLL_D3) | (id == MT_CG_MFG_MM_SW_CG) | (id == MT_CG_BG3D))
//		pr_debug("%s[%d]:%s after disable by %s cnt=%d state=%d\n", __FUNCTION__, clk - &clks[0], clk->name, name , clk->cnt, clk->ops->get_state(clk));
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(mt_disable_clock);

int clock_is_on(cg_clk_id id)
{
    int state;
    unsigned long flags;
    struct cg_clk *clk = id_to_clk(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 1;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!clk);
    BUG_ON(!clk->grp);
    BUG_ON(!clk->ops->check_validity(clk));

    clkmgr_lock(flags);
    state = get_clk_state_locked(clk);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return state;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(clock_is_on);

int grp_dump_regs(cg_grp_id id, unsigned int *ptr)
{
    struct cg_grp *grp = id_to_grp(id);

    ENTER_FUNC(FUNC_LV_DONT_CARE);

    // BUG_ON(!initialized);
    BUG_ON(!grp);

    EXIT_FUNC(FUNC_LV_DONT_CARE);
    return grp->ops->dump_regs(grp, ptr);
}
EXPORT_SYMBOL(grp_dump_regs);

const char *grp_get_name(cg_grp_id id)
{
    struct cg_grp *grp = id_to_grp(id);

    ENTER_FUNC(FUNC_LV_DONT_CARE);

    // BUG_ON(!initialized);
    BUG_ON(!grp);

    EXIT_FUNC(FUNC_LV_DONT_CARE);
    return grp->name;
}

cg_grp_id clk_id_to_grp_id(cg_clk_id id)
{
    struct cg_clk *clk;

    clk = id_to_clk(id);

    return (NULL == clk) ? NR_GRPS : (clk->grp - &grps[0]);
}

unsigned int clk_id_to_mask(cg_clk_id id)
{
    struct cg_clk *clk;

    clk = id_to_clk(id);

    return (NULL == clk) ? 0 : clk->mask;
}

//
// CLKMUX variable & function
//
static struct clkmux_ops non_gf_clkmux_ops;
static struct clkmux_ops glitch_free_clkmux_ops;
static struct clkmux_ops mfg_glitch_free_clkmux_ops;
static struct clkmux_ops glitch_free_wo_drain_cg_clkmux_ops;
static struct clkmux_ops pmicspi_clkmux_ops;
static struct clkmux_ops non_gf_value_clkmux_ops;
static struct clkmux_ops mfg_non_gf_clkmux_ops;

//Clock Mux Selection0
static const struct clkmux_map _mt_clkmux_uart0_gfmux_sel_map[] =
{
	{ .val = 0x0,		.id = MT_CG_SYS_26M,	.mask = 0x1, }, // default
	{ .val = 0x1,		.id = MT_CG_UPLL_D24,	.mask = 0x1, },
};
static const struct clkmux_map _mt_clkmux_emi1x_gfmux_sel_map[] =
{
	{ .val = 0x0,		.id = MT_CG_SYS_26M,	.mask = 0x2, }, // default
	{ .val = 0x2,		.id = MT_CG_MEMPLL,	.mask = 0x2, },	//?
};
static const struct clkmux_map _mt_clkmux_axibus_gfmux_sel_map[] =
{
	{ .val = 0x1 << 4,	.id = MT_CG_SYS_26M,	.mask = 0xF << 4, }, // default
	{ .val = 0x2 << 4,	.id = MT_CG_MPLL_D11,	.mask = 0xF << 4, },
	{ .val = 0x4 << 4,	.id = MT_CG_MPLL_D12,	.mask = 0xF << 4, },
	{ .val = 0xc << 4,	.id = MT_CG_MEMPLL,	.mask = 0xF << 4, },
};
static const struct clkmux_map _mt_clkmux_mfg_mux_sel_map[] =
{
//	{ .val = 0x0 << 8,	.id = MT_CG_INVALID,    .mask = 0x7 << 8, },
//	{ .val = 0x1 << 8,	.id = MT_CG_INVALID,    .mask = 0x7 << 8, },
	{ .val = 0x2 << 8,	.id = MT_CG_UPLL_D3,    .mask = 0x7 << 8, },	//416mhz
//	{ .val = 0x3 << 8,	.id = MT_CG_UPLL_D2,    .mask = 0x7 << 8, },
	{ .val = 0x4 << 8,	.id = MT_CG_SYS_26M,    .mask = 0x5 << 8, },
//	{ .val = 0x5 << 8,	.id = MT_CG_MPLL_D4,    .mask = 0x5 << 8, },
};
static const struct clkmux_map _mt_clkmux_msdc0_mux_sel_map[] =
{
	{ .val = 0x0 << 11,	.id = MT_CG_MPLL_D12,	.mask = 0x7 << 11, },
	{ .val = 0x1 << 11,	.id = MT_CG_MPLL_D10,	.mask = 0x7 << 11, },
	{ .val = 0x2 << 11,	.id = MT_CG_MPLL_D8,	.mask = 0x7 << 11, },
	{ .val = 0x3 << 11,	.id = MT_CG_UPLL_D7,	.mask = 0x7 << 11, },
	{ .val = 0x4 << 11,	.id = MT_CG_MPLL_D7,	.mask = 0x7 << 11, },
	{ .val = 0x5 << 11,	.id = MT_CG_MPLL_D8,	.mask = 0x7 << 11, },
	{ .val = 0x6 << 11,	.id = MT_CG_SYS_26M,	.mask = 0x7 << 11, }, // default
	{ .val = 0x7 << 11,	.id = MT_CG_UPLL_D6,	.mask = 0x7 << 11, },
};
static const struct clkmux_map _mt_clkmux_cam_mux_sel_map[] =
{
	{ .val = 0x1 << 15,	.id = MT_CG_SYS_26M,	.mask = 0x7 << 15, },
	{ .val = 0x2 << 15,	.id = MT_CG_USB_48M,	.mask = 0x7 << 15, },
	{ .val = 0x4 << 15,	.id = MT_CG_UPLL_D6,	.mask = 0x7 << 15, },
};
static const struct clkmux_map _mt_clkmux_pwm_mm_mux_sel_map[] =
{
	{ .val = 0x0 << 18,	.id = MT_CG_SYS_26M,	.mask = 0x1 << 18, }, // default
	{ .val = 0x1 << 18,	.id = MT_CG_UPLL_D12,	.mask = 0x1 << 18, },
};

static const struct clkmux_map _mt_clkmux_uart1_gfmux_sel_map[] =
{
	{ .val = 0x0 << 19,	.id = MT_CG_SYS_26M,	.mask = 0x1 << 19, }, // default
	{ .val = 0x1 << 19,	.id = MT_CG_UPLL_D24,	.mask = 0x1 << 19, },
};
static const struct clkmux_map _mt_clkmux_msdc1_mux_sel_map[] =
{
	{ .val = 0x0 << 20,	.id = MT_CG_MPLL_D12,	.mask = 0x7 << 20, },
	{ .val = 0x1 << 20,	.id = MT_CG_MPLL_D10,	.mask = 0x7 << 20, },
	{ .val = 0x2 << 20,	.id = MT_CG_MPLL_D8,	.mask = 0x7 << 20, },
	{ .val = 0x3 << 20,	.id = MT_CG_UPLL_D7,	.mask = 0x7 << 20, },
	{ .val = 0x4 << 20,	.id = MT_CG_MPLL_D7,	.mask = 0x7 << 20, },
	{ .val = 0x5 << 20,	.id = MT_CG_MPLL_D8,	.mask = 0x7 << 20, },
	{ .val = 0x6 << 20,	.id = MT_CG_SYS_26M,	.mask = 0x7 << 20, }, // default
	{ .val = 0x7 << 20,	.id = MT_CG_UPLL_D6,	.mask = 0x7 << 20, },
};
#if 0
static const struct clkmux_map _mt_clkmux_spm_52m_ck_sel_map[] =
{
	{ .val = 0x0,	.id = MT_CG_SYS_26M,	.mask = 0x1 << 23, }, // default
	{ .val = 0x1,	.id = MT_CG_UPLL_D24,	.mask = 0x1 << 23, },
};
#endif
static const struct clkmux_map _mt_clkmux_pmicspi_mux_sel_map[] =
{
	{ .val = 0x0 << 24,	.id = MT_CG_UPLL_D20,	.mask = 0x3 << 24, }, // default
	{ .val = 0x1 << 24,	.id = MT_CG_USB_48M,	.mask = 0x3 << 24, },
	{ .val = 0x2 << 24,	.id = MT_CG_UPLL_D16,	.mask = 0x3 << 24, },
	{ .val = 0x3 << 24,	.id = MT_CG_SYS_26M,	.mask = 0x3 << 24, },
};

static const struct clkmux_map _mt_clkmux_aud_hf_26m_sel_map[] =
{
	{ .val = 0x0 << 26,	.id = MT_CG_SYS_26M,	.mask = 0x1 << 26, }, // default
	{ .val = 0x1 << 26,	.id = MT_CG_SYS_TEMP,	.mask = 0x1 << 26, }, // hopping 33M
};

// just alias and would replace @ aud_intbus_clkmux_ops
static const struct clkmux_map _mt_clkmux_aud_intbus_sel_map[] =
{
	{ .val = 0x1 << 27,	.id = MT_CG_SYS_26M,	.mask = 0x7 << 27, }, // default
	{ .val = 0x2 << 27,	.id = MT_CG_MPLL_D24,	.mask = 0x7 << 27, }, //
	{ .val = 0x4 << 27,	.id = MT_CG_MPLL_D12,	.mask = 0x7 << 27, }, //
};

//Clock Mux Selection1
static const struct clkmux_map _mt_clkmux_nfi2x_gfmux_sel_map[] =
{
	{ .val = 0x8,	.id = MT_CG_SYS_26M,	.mask = 0x78, },
	{ .val = 0x11,	.id = MT_CG_MPLL_D8,	.mask = 0x7F, },
	{ .val = 0x12,	.id = MT_CG_MPLL_D10,	.mask = 0x7F, },
	{ .val = 0x14,	.id = MT_CG_MPLL_D14,	.mask = 0x7F, },
	{ .val = 0x21,	.id = MT_CG_MPLL_D16,	.mask = 0x7F, },
	{ .val = 0x22,	.id = MT_CG_MPLL_D28,	.mask = 0x7F, },
	{ .val = 0x24,	.id = MT_CG_MPLL_D40,	.mask = 0x7F, },
	{ .val = 0x61,	.id = MT_CG_MPLL_D14,	.mask = 0x7F, },
	{ .val = 0x62,	.id = MT_CG_MPLL_D24,	.mask = 0x7F, },
	{ .val = 0x51,	.id = MT_CG_MPLL_D7,	.mask = 0x7F, },
	{ .val = 0x52,	.id = MT_CG_MPLL_D8,	.mask = 0x7F, },
	{ .val = 0x54,	.id = MT_CG_MPLL_D12,	.mask = 0x7F, },
};

static const struct clkmux_map _mt_clkmux_nfi1x_infra_sel_map[] =
{
	{ .val = 0x0 << 7,	.id = MT_CG_SYS_26M,	.mask = 0x1 << 7, },
	{ .val = 0x1 << 7,	.id = MT_CG_SYS_TEMP,	.mask = 0x1 << 7, },
};

static const struct clkmux_map _mt_clkmux_mfg_gfmux_sel_map[] =
{
	{ .val = 0x8  << 8,	.id = MT_CG_UPLL_D3,	.mask = 0x38 << 8, },
	{ .val = 0x10 << 8,	.id = MT_CG_MPLL_D3,	.mask = 0x38 << 8, },
	{ .val = 0x21 << 8,	.id = MT_CG_MPLL_D5,	.mask = 0x3F << 8, },
	{ .val = 0x22 << 8,	.id = MT_CG_MPLL_D7,	.mask = 0x3F << 8, },
	{ .val = 0x24 << 8,	.id = MT_CG_MPLL_D14,	.mask = 0x3F << 8, },
};

static const struct clkmux_map _mt_clkmux_spi_gfmux_sel_map[] =
{
	{ .val = 0x0 << 14,	.id = MT_CG_SYS_26M,	.mask = 0x1 << 14, }, // default
	{ .val = 0x1 << 14,	.id = MT_CG_UPLL_D12,	.mask = 0x1 << 14, },
};

static const struct clkmux_map _mt_clkmux_ddrphycfg_gfmux_sel_map[] =
{
	{ .val = 0x0 << 15,	.id = MT_CG_SYS_26M,	.mask = 0x1 << 15, }, // default
	{ .val = 0x1 << 15,	.id = MT_CG_MPLL_D16,	.mask = 0x1 << 15, },
};

static const struct clkmux_map _mt_clkmux_smi_gfmux_sel_map[] =
{
	{ .val = 0x0 << 16,	.id = MT_CG_SYS_26M,	.mask = 0x8 << 16, }, //
	{ .val = 0x9 << 16,	.id = MT_CG_MPLL_D5,	.mask = 0xF << 16, }, //
	{ .val = 0xa << 16,	.id = MT_CG_MPLL_D7,	.mask = 0xF << 16, }, //
	{ .val = 0xc << 16,	.id = MT_CG_MPLL_D14,	.mask = 0xF << 16, }, //
};

static const struct clkmux_map _mt_clkmux_usb_gfmux_sel_map[] =
{
	{ .val = 0x1 << 20,	.id = MT_CG_SYS_26M,	.mask = 0x7 << 20, }, //
	{ .val = 0x2 << 20,	.id = MT_CG_UPLL_D16,	.mask = 0x7 << 20, }, //
	{ .val = 0x4 << 20,	.id = MT_CG_MPLL_D20,	.mask = 0x7 << 20, }, //
};

static const struct clkmux_map _mt_clkmux_scam_mux_sel_map[] =
{
	{ .val = 0x1 << 23,	.id = MT_CG_SYS_26M,	.mask = 0x7 << 23, }, //
	{ .val = 0x2 << 23,	.id = MT_CG_MPLL_D14,	.mask = 0x7 << 23, }, //
	{ .val = 0x4 << 23,	.id = MT_CG_MPLL_D12,	.mask = 0x7 << 23, }, //
};

static struct clkmux muxs[] = // NR_CLKMUXS
{
    [MT_CLKMUX_UART0_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_UART0_GFMUX_SEL),
	.offset     = 0,
        .ops        = &glitch_free_clkmux_ops,
        .map        = _mt_clkmux_uart0_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_uart0_gfmux_sel_map),
        .drain      = MT_CG_UART0_SW_CG,
    },
    [MT_CLKMUX_EMI1X_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_EMI1X_GFMUX_SEL),
	.offset     = 1,
        .ops        = &glitch_free_wo_drain_cg_clkmux_ops,
        .map        = _mt_clkmux_emi1x_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_emi1x_gfmux_sel_map),
        .drain      = MT_CG_INVALID,
    },

    [MT_CLKMUX_AXIBUS_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_AXIBUS_GFMUX_SEL),
	.offset     = 4,
        .ops        = &glitch_free_wo_drain_cg_clkmux_ops,
        .map        = _mt_clkmux_axibus_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_axibus_gfmux_sel_map),
        .drain      = MT_CG_INVALID,
    },
    [MT_CLKMUX_MFG_MUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_MFG_MUX_SEL),
	.offset     = 8,
        .ops        = &mfg_non_gf_clkmux_ops,
        .map        = _mt_clkmux_mfg_mux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_mfg_mux_sel_map),
        .drain      = MT_CG_INVALID,
    },
    [MT_CLKMUX_MSDC0_MUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_MSDC0_MUX_SEL),
	.offset     = 11,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_msdc0_mux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_msdc0_mux_sel_map),
        .drain      = MT_CG_MSDC0_SW_CG,
    },
    [MT_CLKMUX_CAM_MUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_CAM_MUX_SEL),
	.offset     = 15,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_cam_mux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_cam_mux_sel_map),
        .drain      = MT_CG_CAM_MM_SW_CG,
    },
    [MT_CLKMUX_PWM_MM_MUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_PWM_MM_MUX_SEL),
	.offset     = 18,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_pwm_mm_mux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_pwm_mm_mux_sel_map),
        .drain      = MT_CG_PWM_MM_SW_CG,
    },
    [MT_CLKMUX_UART1_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_UART1_GFMUX_SEL),
	.offset     = 19,
        .ops        = &glitch_free_clkmux_ops,
        .map        = _mt_clkmux_uart1_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_uart1_gfmux_sel_map),
        .drain      = MT_CG_UART1_SW_CG,
    },
    [MT_CLKMUX_MSDC1_MUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_MSDC1_MUX_SEL),
	.offset     = 20,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_msdc1_mux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_msdc1_mux_sel_map),
        .drain      = MT_CG_MSDC1_SW_CG,
    },
#if 0
    [MT_CLKMUX_SPM_52M_CK_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_SPM_52M_CK_SEL),
        // .sel_mask   = BITMASK(23: 23),
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_spm_52m_ck_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_spm_52m_ck_sel_map),
        .drain      = MT_CG_SPM_52M_SW_CG,
    },
#endif
    [MT_CLKMUX_PMICSPI_MUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_PMICSPI_MUX_SEL),
	.offset     = 24,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_pmicspi_mux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_pmicspi_mux_sel_map),
        .drain      = MT_CG_PMIC_SW_CG_AP,
    },
    [MT_CLKMUX_AUD_HF_26M_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_AUD_HF_26M_SEL),
	.offset     = 26,
        .ops        = &glitch_free_wo_drain_cg_clkmux_ops,
        .map        = _mt_clkmux_aud_hf_26m_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_aud_hf_26m_sel_map),
        .drain      = MT_CG_INVALID,
    },
    [MT_CLKMUX_AUD_INTBUS_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_AUD_INTBUS_SEL),
	.offset     = 27,
        .ops        = &glitch_free_clkmux_ops,
        .map        = _mt_clkmux_aud_intbus_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_aud_intbus_sel_map),
        .drain      = MT_CG_AUDIO_SW_CG,
    },

    [MT_CLKMUX_NFI2X_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_NFI2X_GFMUX_SEL),
	.offset     = 0,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_nfi2x_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_nfi2x_gfmux_sel_map),
        .drain      = MT_CG_NFI2X_SW_CG,
    },

    [MT_CLKMUX_NFI1X_INFRA_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_NFI1X_INFRA_SEL),
	.offset     = 7,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_nfi1x_infra_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_nfi1x_infra_sel_map),
        .drain      = MT_CG_NFI2X_SW_CG,
    },

    [MT_CLKMUX_MFG_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_MFG_GFMUX_SEL),
	.offset     = 8,
        .ops        = &mfg_glitch_free_clkmux_ops,
        .map        = _mt_clkmux_mfg_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_mfg_gfmux_sel_map),
        .drain      = MT_CG_MFG_MM_SW_CG,
    },

    [MT_CLKMUX_SPI_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_SPI_GFMUX_SEL),
	.offset     = 14,
        .ops        = &glitch_free_clkmux_ops,
        .map        = _mt_clkmux_spi_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_spi_gfmux_sel_map),
        .drain      = MT_CG_SPI_SW_CG,
    },

    [MT_CLKMUX_DDRPHYCFG_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_DDRPHYCFG_GFMUX_SEL),
	.offset     = 15,
        .ops        = &glitch_free_wo_drain_cg_clkmux_ops,
        .map        = _mt_clkmux_ddrphycfg_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_ddrphycfg_gfmux_sel_map),
        .drain      = MT_CG_INVALID,
    },

    [MT_CLKMUX_SMI_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_SMI_GFMUX_SEL),
	.offset     = 16,
        .ops        = &glitch_free_clkmux_ops,
        .map        = _mt_clkmux_smi_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_smi_gfmux_sel_map),
        .drain      = MT_CG_SMI_MM_SW_CG,
    },

    [MT_CLKMUX_USB_GFMUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_USB_GFMUX_SEL),
	.offset     = 20,
        .ops        = &glitch_free_clkmux_ops,
        .map        = _mt_clkmux_usb_gfmux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_usb_gfmux_sel_map),
        .drain      = MT_CG_USB_SW_CG,
    },

    [MT_CLKMUX_SCAM_MUX_SEL] =
    {
        .name       = __stringify(MT_CLKMUX_SCAM_MUX_SEL),
	.offset     = 23,
        .ops        = &non_gf_clkmux_ops,
        .map        = _mt_clkmux_scam_mux_sel_map,
        .nr_map     = ARRAY_SIZE(_mt_clkmux_scam_mux_sel_map),
        .drain      = MT_CG_SCAM_MM_SW_CG,
    },
};


static struct clkmux *id_to_mux(clkmux_id id)
{
    return id < NR_CLKMUXS ? &muxs[id] : NULL;
}
// general clkmux get
static cg_clk_id general_clkmux_get_op(struct clkmux *mux)
{
    volatile unsigned int reg_val;
    int i;

	ENTER_FUNC(FUNC_LV_OP);

	reg_val = clk_readl(mux->base_addr);

//	pr_debug("MUX = %s nr_map = %d reg = %x\n", mux->name, mux->nr_map, reg_val);

	for (i = 0; i < mux->nr_map; i++)
	{
//		pr_debug("%s "HEX_FMT" >> "HEX_FMT", "HEX_FMT", "HEX_FMT"\n",clks[mux->map[i].id].name, mux->map[i].id, mux->map[i].val, mux->map[i].mask, reg_val);
		if ((mux->map[i].val & mux->map[i].mask) == (reg_val & mux->map[i].mask))
		{
			break;
		}
	}

    #if 0 // XXX: just for FPGA
    if (i == mux->nr_map)
    {
        return MT_CG_SYS_26M;
    }
    #else
    BUG_ON(i == mux->nr_map);
    #endif

    EXIT_FUNC(FUNC_LV_OP);

    return mux->map[i].id;
}
static cg_clk_id non_gf_value_clkmux_get_op(struct clkmux *mux)
{
    volatile unsigned int reg_val;
    int i;

	ENTER_FUNC(FUNC_LV_OP);

	reg_val = clk_readl(mux->base_addr);

//	pr_debug("MUX = %s nr_map = %d reg = %x\n", mux->name, mux->nr_map, reg_val);

	for (i = 0; i < mux->nr_map; i++)
	{
//		pr_debug("%s "HEX_FMT" >> "HEX_FMT", "HEX_FMT", "HEX_FMT"\n",clks[mux->map[i].id].name, mux->map[i].id, mux->map[i].val, mux->map[i].mask, reg_val);
		if ((mux->map[i].val & mux->map[i].mask) == (reg_val & mux->map[i].mask))
		{
			break;
		}
	}

    #if 0 // XXX: just for FPGA
    if (i == mux->nr_map)
    {
        return MT_CG_SYS_26M;
    }
    #else
    BUG_ON(i == mux->nr_map);
    #endif

    EXIT_FUNC(FUNC_LV_OP);

    return (mux->map[i].val >> mux->offset);
}
// non glitch free clkmux sel (disable drain cg first (i.e. with drain cg))

static int non_gf_clkmux_sel_op(struct clkmux *mux, cg_clk_id clksrc)
{
    struct cg_clk *drain_clk = id_to_clk(mux->drain);
    int err = 0;
    int i;

    ENTER_FUNC(FUNC_LV_OP);

    // 0. all CLKMUX use this op need drain clk (to disable)
    BUG_ON(NULL == drain_clk); // map error @ muxs[]->ops & muxs[]->drain (use non_gf_clkmux_sel_op with "wrong drain cg")

    if (clksrc == drain_clk->src) // switch to the same source (i.e. don't need clk switch actually)
    {
        EXIT_FUNC(FUNC_LV_OP);
        return 0;
    }

    // look up the clk map
    for (i = 0; i < mux->nr_map; i++)
    {
        if (mux->map[i].id == clksrc)
        {
            break;
        }
    }

    // clk sel match
    if (i < mux->nr_map)
    {
        bool orig_drain_cg_is_on_flag = FALSE;

        // 1. turn off first for glitch free if drain CG is on
        if (PWR_ON == drain_clk->ops->get_state(drain_clk))
        {
            drain_clk->ops->disable(drain_clk);
            orig_drain_cg_is_on_flag = TRUE;
        }

        // 2. switch clkmux
        {
            volatile unsigned int reg_val;

            // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
            if (TRUE == orig_drain_cg_is_on_flag)
            {
                err = enable_clock_locked(id_to_clk(clksrc)); // XXX: enable first for seemless transition
                BUG_ON(err);
            }

            // set clkmux reg (non glitch free)
            reg_val = (clk_readl(mux->base_addr) & ~mux->map[i].mask) | mux->map[i].val;
            clk_writel(mux->base_addr, reg_val);

            // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
            if (TRUE == orig_drain_cg_is_on_flag)
            {
                err = disable_clock_locked(id_to_clk(drain_clk->src));
                BUG_ON(err);
            }

            drain_clk->src = clksrc;
        }

        // 3. turn on drain CG if necessary
        if (TRUE == orig_drain_cg_is_on_flag)
        {
            drain_clk->ops->enable(drain_clk);
        }
    }
    else
    {
        err = -1; // ERROR: clk sel not match
    }
    BUG_ON(err); // clk sel not match

    EXIT_FUNC(FUNC_LV_OP);
    return err;
}

static struct clkmux_ops non_gf_clkmux_ops =
{
    .sel = non_gf_clkmux_sel_op,
    .get = general_clkmux_get_op,
};

// (half) glitch free clkmux sel (with drain cg)

//#define GFMUX_BIT_MASK_FOR_HALF_GF_OP BITMASK(31:30) // rg_mfg_gfmux_sel, rg_spinfi_gfmux_sel

static cg_clk_id clkmux_get_locked(struct clkmux *mux);

static int glitch_free_clkmux_sel_op(struct clkmux *mux, cg_clk_id clksrc)
{
    struct cg_clk *drain_clk = id_to_clk(mux->drain);
//	struct cg_clk *to_clk = id_to_clk(clksrc);
	struct cg_clk *from_clk;
	
	struct clkmux *clkmuxs;
    int err = 0;
    int i,j;

    ENTER_FUNC(FUNC_LV_OP);

    BUG_ON(NULL == drain_clk); // map error @ muxs[]->ops & muxs[]->drain (use glitch_free_clkmux_sel_op with "wrong drain cg")

    if (clksrc == drain_clk->src) // switch to the same source (i.e. don't need clk switch actually)
    {
        EXIT_FUNC(FUNC_LV_OP);
        return 0;
    }

    // look up the clk map
    for (i = 0; i < mux->nr_map; i++)
    {
        if (mux->map[i].id == clksrc)
        {
            break;
        }
    }

    // clk sel match
    if (i < mux->nr_map)
    {
        bool disable_src_cg_flag = FALSE;
        volatile unsigned int reg_val;
//	from_clk = id_to_clk(clkmux_get_locked(mux));

        // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
        if (PWR_ON == drain_clk->ops->get_state(drain_clk))
        {
//		pr_debug("mux[%s]:  clock from:%s, cnt = %d state = %d \n", __func__, from_clk->name, from_clk->cnt, from_clk->ops->get_state(from_clk));
//		pr_debug("mux[%s]:  clock to  :%s, cnt = %d state = %d \n", __func__, to_clk->name, to_clk->cnt, to_clk->ops->get_state(to_clk));
		err = enable_clock_locked(id_to_clk(clksrc)); // XXX: enable first for seemless transition
		
		BUG_ON(err);	
		disable_src_cg_flag = TRUE;
        }
        else
        {
            err = enable_clock_locked(id_to_clk(clksrc));
            BUG_ON(err);
//	pr_debug("[%s]: enable_clock_locked mux change to   :%s, cnt = %d state = %d \n", __func__, to_clk->name, to_clk->cnt ,to_clk->ops->get_state(to_clk));
            from_clk = id_to_clk(clkmux_get_locked(mux));
            err = enable_clock_locked(from_clk);
            BUG_ON(err);
//	pr_debug("[%s]: enable_clock_locked mux change from :%s, cnt = %d state = %d \n", __func__, from_clk->name, from_clk->cnt, from_clk->ops->get_state(from_clk));
        }

        // set clkmux reg (inc. non glitch free / half glitch free / glitch free case)
        reg_val = (clk_readl(mux->base_addr) & ~mux->map[i].mask) | mux->map[i].val;

        clk_writel(mux->base_addr, reg_val);

        // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
        if (TRUE == disable_src_cg_flag)
        {
            err = disable_clock_locked(id_to_clk(drain_clk->src));
            BUG_ON(err);
//		pr_debug("[%s]: mux source from:%s, cnt = %d state = %d \n", __func__, from_clk->name, from_clk->cnt, from_clk->ops->get_state(from_clk));
//		pr_debug("[%s]: mux source to  :%s, cnt = %d state = %d \n", __func__, to_clk->name, to_clk->cnt, to_clk->ops->get_state(to_clk));
        }
        else
        {
            err = disable_clock_locked(id_to_clk(clksrc));
            BUG_ON(err);
//		pr_debug("[%s]: disable_clock_locked mux :%s, cnt = %d state = %d \n", __func__, to_clk->name, to_clk->cnt ,to_clk->ops->get_state(to_clk));
            err = disable_clock_locked(from_clk);
            BUG_ON(err);
//		pr_debug("[%s]: disable_clock_locked mux :%s, cnt = %d state = %d \n", __func__, from_clk->name, from_clk->cnt, from_clk->ops->get_state(from_clk));
        }
        drain_clk->src = clksrc;
#if 0
	for (j = 0; j < NR_CLKMUXS; j++)
	{
		cg_clk_id src_id;
		clkmuxs = &muxs[j];
		src_id = clkmuxs->ops->get(clkmuxs);
		pr_debug("clkmux[%s]: source = %s, Drain = %s\n", clkmuxs->name, clks[src_id].name, clks[clkmuxs->drain].name); // <-XXX
	}
#endif
    }
    else
    {
        err = -1; // ERROR: clk sel not match
    }
    BUG_ON(err); // clk sel not match

    EXIT_FUNC(FUNC_LV_OP);
    return err;
}

static struct clkmux_ops glitch_free_clkmux_ops =
{
    .sel = glitch_free_clkmux_sel_op,
    .get = general_clkmux_get_op,
};

static int mfg_non_gf_clkmux_sel_op(cg_clk_id clksrc)
{
    struct cg_clk *drain_clk = id_to_clk(MT_CG_MFG_MM_SW_CG);
    struct clkmux *mux = id_to_mux(MT_CLKMUX_MFG_MUX_SEL);
    int err = 0;
    int i,clk;

    ENTER_FUNC(FUNC_LV_OP);
	clk = general_clkmux_get_op(mux);
    if (clksrc == clk) // switch to the same source (i.e. don't need clk switch actually)
    {
        EXIT_FUNC(FUNC_LV_OP);
        return 0;
    }

    // look up the clk map
    for (i = 0; i < mux->nr_map; i++)
    {
        if (mux->map[i].id == clksrc)
        {
            break;
        }
    }

    // clk sel match
    if (i < mux->nr_map)
    {
        bool orig_drain_cg_is_on_flag = FALSE;

        // 1. turn off first for glitch free if drain CG is on
        if (PWR_ON == drain_clk->ops->get_state(drain_clk))
        {
 //           drain_clk->ops->disable(drain_clk);
            orig_drain_cg_is_on_flag = TRUE;
        }

        // 2. switch clkmux
        {
            volatile unsigned int reg_val;

            // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
            if (TRUE == orig_drain_cg_is_on_flag)
            {
                err = enable_clock_locked(id_to_clk(clksrc)); // XXX: enable first for seemless transition
                BUG_ON(err);
            }

            // set clkmux reg (non glitch free)
            reg_val = (clk_readl(mux->base_addr) & ~mux->map[i].mask) | mux->map[i].val;
            clk_writel(mux->base_addr, reg_val);

            // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
            if (TRUE == orig_drain_cg_is_on_flag)
            {
                err = disable_clock_locked(id_to_clk(clk));
                BUG_ON(err);
            }
        }

        // 3. turn on drain CG if necessary
        if (TRUE == orig_drain_cg_is_on_flag)
        {
//            drain_clk->ops->enable(drain_clk);
        }
    }
    else
    {
        err = -1; // ERROR: clk sel not match
    }
    BUG_ON(err); // clk sel not match

    EXIT_FUNC(FUNC_LV_OP);
    return err;
}
static struct clkmux_ops mfg_non_gf_clkmux_ops =
{
	.sel = mfg_non_gf_clkmux_sel_op,
	.get = general_clkmux_get_op,
};
static int mfg_glitch_free_clkmux_sel_op(struct clkmux *mux, cg_clk_id clksrc)
{
	int err = 0;
#if 1
	struct cg_clk *to_clk = id_to_clk(clksrc);
	if (clksrc != MT_CG_UPLL_D3) {
		err |= mfg_non_gf_clkmux_sel_op(MT_CG_SYS_26M);
		err |= glitch_free_clkmux_sel_op(mux, clksrc);
//		pr_debug("not MT_CG_UPLL_D3 clks[%s] CON = 0x%x, mask = 0x%x CNT = %d\n",to_clk->name, clk_readl(to_clk->grp->sta_addr),~to_clk->mask, to_clk->cnt);

	} else {
		err |= mfg_non_gf_clkmux_sel_op(MT_CG_UPLL_D3);
		err |= glitch_free_clkmux_sel_op(mux, clksrc);
//		pr_debug("    MT_CG_UPLL_D3 clks[%s] CON = 0x%x, mask = 0x%x CNT = %d\n",to_clk->name, clk_readl(to_clk->grp->sta_addr),~to_clk->mask, to_clk->cnt);
	}
#endif
	return err;
}

static cg_clk_id mfg_glitch_free_clkmux_get_op(struct clkmux *mux)
{
	cg_clk_id clksrc = 0;
	clksrc = general_clkmux_get_op(mux);
	return clksrc;
}

static struct clkmux_ops mfg_glitch_free_clkmux_ops =
{
	.sel = mfg_glitch_free_clkmux_sel_op,
	.get = general_clkmux_get_op,
};

// glitch free clkmux without drain cg

static int glitch_free_wo_drain_cg_clkmux_sel_op(struct clkmux *mux, cg_clk_id clksrc)
{
    volatile unsigned int reg_val;
    struct cg_clk *src_clk;
    int err;
    int i;

    ENTER_FUNC(FUNC_LV_OP);

    // search current src cg first
    {
        reg_val = clk_readl(mux->base_addr);

        // look up current src cg
        for (i = 0; i < mux->nr_map; i++)
        {
            if ((mux->map[i].val & mux->map[i].mask) == (reg_val & mux->map[i].mask))
            {
                break;
            }
        }

        BUG_ON(i >= mux->nr_map); // map error @ muxs[]->map

        src_clk = id_to_clk(mux->map[i].id);

        BUG_ON(NULL == src_clk); // map error @ muxs[]->map

        if (clksrc == mux->map[i].id) // switch to the same source (i.e. don't need clk switch actually)
        {
            EXIT_FUNC(FUNC_LV_OP);
            return 0;
        }
    }

    // look up the clk map
    for (i = 0; i < mux->nr_map; i++)
    {
        if (mux->map[i].id == clksrc)
        {
            break;
        }
    }

    // clk sel match
    if (i < mux->nr_map)
    {
        err = enable_clock_locked(id_to_clk(clksrc)); // XXX: enable first for seemless transition
        BUG_ON(err);

        // set clkmux reg (inc. non glitch free / half glitch free / glitch free case)
        reg_val = (clk_readl(mux->base_addr) & ~mux->map[i].mask) | mux->map[i].val;
//        clk_writel(mux->base_addr, reg_val & ~(mux->map[i].mask & GFMUX_BIT_MASK_FOR_HALF_GF_OP)); // XXX: tricky for half glitch free case (set rg_xxx_gfmux_sel 0 first)
        clk_writel(mux->base_addr, reg_val);

        err = disable_clock_locked(src_clk); // TODO: please check init case (should setup well (i.e. enable) @ init state because of no drain CG)
        BUG_ON(err);
    }
    else
    {
        err = -1; // ERROR: clk sel not match
    }
    BUG_ON(err); // clk sel not match

    EXIT_FUNC(FUNC_LV_OP);
    return err;
}

static struct clkmux_ops glitch_free_wo_drain_cg_clkmux_ops =
{
    .sel = glitch_free_wo_drain_cg_clkmux_sel_op,
    .get = general_clkmux_get_op,
};

// pmicspi clkmux sel (non glitch free & with drain cg)

static int pmicspi_clkmux_sel_op(struct clkmux *mux, cg_clk_id clksrc) // almost the same with non_gf_clkmux_sel_op()
{
    struct cg_clk *drain_clk = id_to_clk(mux->drain);
    int err = 0;
    int i;

    ENTER_FUNC(FUNC_LV_OP);

    // 0. all CLKMUX use this op need drain clk (to disable)
    BUG_ON(NULL == drain_clk); // map error @ muxs[]->ops & muxs[]->drain (use pmicspi_clkmux_sel_op with "wrong drain cg")

    // look up the clk map
    for (i = 0; i < mux->nr_map; i++)
    {
        if (mux->map[i].id == clksrc)
        {
            break;
        }
    }

    if (i < mux->nr_map)
    {
        bool orig_drain_cg_is_on_flag = FALSE;

//        if (MT_CG_MPLL_D24 == clksrc)
//        {
//            clksrc = is_ddr3() ? MT_CG_MPLL_D20 : MT_CG_MPLL_D24;
//        }

        if (clksrc == drain_clk->src) // switch to the same source (i.e. don't need clk switch actually)
        {
            EXIT_FUNC(FUNC_LV_OP);
            return 0;
        }

        // 1. turn off first for glitch free if drain CG is on
        if (PWR_ON == drain_clk->ops->get_state(drain_clk))
        {
            drain_clk->ops->disable(drain_clk);
            orig_drain_cg_is_on_flag = TRUE;
        }

        // 2. switch clkmux
        {
            volatile unsigned int reg_val;

            // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
            if (TRUE == orig_drain_cg_is_on_flag)
            {
                err = enable_clock_locked(id_to_clk(clksrc)); // XXX: enable first for seemless transition
                BUG_ON(err);
            }

            // set clkmux reg (non glitch free)
            reg_val = (clk_readl(mux->base_addr) & ~mux->map[i].mask) | mux->map[i].val;
            clk_writel(mux->base_addr, reg_val);

            // (just) help to enable/disable src clock if necessary (i.e. drain cg is on original)
            if (TRUE == orig_drain_cg_is_on_flag)
            {
                err = disable_clock_locked(id_to_clk(drain_clk->src));
                BUG_ON(err);
            }

            drain_clk->src = clksrc;
        }

        // 3. turn on drain CG if necessary
        if (TRUE == orig_drain_cg_is_on_flag)
        {
            drain_clk->ops->enable(drain_clk);
        }
    }
    else
    {
        err = -1; // ERROR: clk sel not match
    }
    BUG_ON(err); // clk sel not match

    EXIT_FUNC(FUNC_LV_OP);
    return err;
}

static struct clkmux_ops pmicspi_clkmux_ops =
{
    .sel = pmicspi_clkmux_sel_op,
    .get = general_clkmux_get_op,
};

// aud_intbus clkmux sel (glitch free & with drain cg)

static int non_gf_value_clkmux_sel_op(struct clkmux *mux, cg_clk_id clksrc) //  almost the same with glitch_free_clkmux_sel_op()
{
	int i;
	int err = 0;
	ENTER_FUNC(FUNC_LV_OP);
	// look up the clk map
	for (i = 0; i < mux->nr_map; i++)
	{
		if ((mux->map[i].val >> mux->offset) == clksrc)
		{
			clksrc = mux->map[i].id;
			break;
		}
	}
	// clk sel match
	if (i < mux->nr_map)
		err = non_gf_clkmux_sel_op(mux, clksrc);
	else
		err = -1; // ERROR: clk sel not match
	BUG_ON(err); // clk sel not match
	EXIT_FUNC(FUNC_LV_OP);
	return err;
}

static struct clkmux_ops non_gf_value_clkmux_ops =
{
    .sel = non_gf_value_clkmux_sel_op,
    .get = non_gf_value_clkmux_get_op,
};

static void clkmux_sel_locked(struct clkmux *mux, cg_clk_id clksrc)
{
    ENTER_FUNC(FUNC_LV_LOCKED);
    mux->ops->sel(mux, clksrc);
    EXIT_FUNC(FUNC_LV_LOCKED);
}

static cg_clk_id clkmux_get_locked(struct clkmux *mux)
{
    ENTER_FUNC(FUNC_LV_LOCKED);

    EXIT_FUNC(FUNC_LV_LOCKED);
    return mux->ops->get(mux);
}

int clkmux_sel(clkmux_id id, cg_clk_id clksrc, const char *name)
{
	unsigned long flags;
	struct clkmux *mux = id_to_mux(id);
	ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

	return 0;

#else   // CONFIG_CLKMGR_EMULATION

	BUG_ON(!initialized);
	BUG_ON(!mux);
	if((id == MT_CLKMUX_MSDC0_MUX_SEL) | (id == MT_CLKMUX_MSDC1_MUX_SEL) | (id == MT_CLKMUX_CAM_MUX_SEL) | (id == MT_CLKMUX_SCAM_MUX_SEL))
	{
		clkmgr_lock(flags);
		non_gf_value_clkmux_ops.sel(mux, clksrc);
		clkmgr_unlock(flags);
	}
	else
	{
		clkmgr_lock(flags);
		clkmux_sel_locked(mux, clksrc);
		clkmgr_unlock(flags);
	}
	EXIT_FUNC(FUNC_LV_API);
	return 0;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(clkmux_sel);

int clkmux_get(clkmux_id id, const char *name)
{
	cg_clk_id clksrc;
	unsigned long flags;
	struct clkmux *mux = id_to_mux(id);

	ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

	return 0;

#else   // CONFIG_CLKMGR_EMULATION

	BUG_ON(!initialized);
	BUG_ON(!mux);
    // BUG_ON(clksrc >= mux->nr_inputs); // XXX: clksrc match is @ clkmux_sel_op()
	if((id == MT_CLKMUX_MSDC0_MUX_SEL) | (id == MT_CLKMUX_MSDC1_MUX_SEL))
	{
		clkmgr_lock(flags);
		clksrc = non_gf_value_clkmux_ops.get(mux);
		clkmgr_unlock(flags);
	}
	else
	{
		clkmgr_lock(flags);
		clksrc = clkmux_get_locked(mux);
		clkmgr_unlock(flags);
	}
	EXIT_FUNC(FUNC_LV_API);
	return clksrc;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(clkmux_get);

void enable_mux(int id, char *name)
{
    unsigned long flags;
    struct clkmux *mux = id_to_mux(id);
    struct cg_clk *drain_clk = id_to_clk(mux->drain);
#if defined(CONFIG_CLKMGR_EMULATION)
return;
#endif

    BUG_ON(!initialized);
    BUG_ON(!mux);
    BUG_ON(!name);
#ifdef MUX_LOG_TOP
    pr_debug("[%s]: id=%d, name=%s\n", __func__, id, name);
#endif
    clkmgr_lock(flags);
    enable_clock_locked(drain_clk);
    clkmgr_unlock(flags);

    return;
}
EXPORT_SYMBOL(enable_mux);

void disable_mux(int id, char *name)
{
    unsigned long flags;
    struct clkmux *mux = id_to_mux(id);
    struct cg_clk *drain_clk = id_to_clk(mux->drain);
#if defined(CONFIG_CLKMGR_EMULATION)
return;
#endif
    BUG_ON(!initialized);
    BUG_ON(!mux);
    BUG_ON(!name);
#ifdef MUX_LOG_TOP
    pr_debug("[%s]: id=%d, name=%s\n", __func__, id, name);
#endif
    clkmgr_lock(flags);
    disable_clock_locked(drain_clk);
    clkmgr_unlock(flags);

    return;
}
EXPORT_SYMBOL(disable_mux);

static unsigned int MT_CLKMUX_AUD_26M = 0, MT_CLKMUX_AUD_INTBUS = 0;
void clkmgr_faudintbus_pll2sq(void)
{
	MT_CLKMUX_AUD_26M = clkmux_get(MT_CLKMUX_AUD_HF_26M_SEL,"MP3");
	MT_CLKMUX_AUD_INTBUS = clkmux_get(MT_CLKMUX_AUD_INTBUS_SEL,"MP3");
	clkmux_sel(MT_CLKMUX_AUD_HF_26M_SEL, MT_CG_SYS_26M, "MP3");
	clkmux_sel(MT_CLKMUX_AUD_INTBUS_SEL, MT_CG_SYS_26M, "MP3");
	return;
}

void clkmgr_faudintbus_sq2pll(void)
{
	clkmux_sel(MT_CLKMUX_AUD_HF_26M_SEL, MT_CLKMUX_AUD_26M, "MP3");
	clkmux_sel(MT_CLKMUX_AUD_INTBUS_SEL, MT_CLKMUX_AUD_INTBUS, "MP3");
	return;
}
//
// PLL variable & function
//
#define PLL_TYPE_SDM        0
#define PLL_TYPE_LC         1

#define HAVE_RST_BAR        (0x1 << 0)
#define HAVE_PLL_HP         (0x1 << 1)
#define HAVE_FIX_FRQ        (0x1 << 2)

#define PLL_EN              BIT(0)          // @ XPLL_CON0
#define PLL_SDM_FRA_EN      BIT(4)          // @ XPLL_CON0
#define RST_BAR_MASK        BITMASK(27:27)  // @ XPLL_CON0

#define SDM_PLL_N_INFO_MASK BITMASK(20:0)   // @ XPLL_CON1 (XPLL_SDM_PCW)
#define SDM_PLL_POSDIV_MASK BITMASK(26:24)  // @ XPLL_CON1 (XPLL_POSDIV)
#define SDM_PLL_N_INFO_CHG  BITMASK(31:31)  // @ XPLL_CON1 (XPLL_SDM_PCW_CHG)

#define PLL_FBKDIV_MASK     BITMASK(6:0)    // @ UNIVPLL_CON1 (UNIVPLL_SDM_PCW)

#define PLL_PWR_ON          BIT(0)          // @ XPLL_PWR
#define PLL_ISO_EN          BIT(1)          // @ XPLL_PWR

static struct pll_ops sdm_pll_ops;
static struct pll_ops univpll_ops;
static struct pll_ops whpll_ops;

static struct pll plls[NR_PLLS] =
{
    [ARMPLL] =
    {
        .name = __stringify(ARMPLL),
	.cnt = 1,
        .type = PLL_TYPE_SDM,
        .feat = HAVE_PLL_HP,
        .en_mask = PLL_EN | PLL_SDM_FRA_EN,
        .ops = &sdm_pll_ops,
//        .hp_id = MT658X_FH_ARM_PLL,
        .hp_switch = 0,
    },
    [MAINPLL] =
    {
        .name = __stringify(MAINPLL),
	.cnt = 1,
        .type = PLL_TYPE_SDM,
        .feat = HAVE_PLL_HP | HAVE_RST_BAR,
        .en_mask = PLL_EN | PLL_SDM_FRA_EN,
        .ops = &sdm_pll_ops,
//        .hp_id = MT658X_FH_MAIN_PLL,
        .hp_switch = 0,
    },
    [UNIVPLL] =
    {
        .name = __stringify(UNIVPLL),
	.cnt = 0,
        .type = PLL_TYPE_SDM,
        .feat = HAVE_RST_BAR | HAVE_FIX_FRQ,
        .en_mask = PLL_EN | PLL_SDM_FRA_EN, // | USB48M_EN_BIT | UNIV48M_EN_BIT, // TODO: check with USB owner
        .ops = &univpll_ops,
    },
};

static struct pll *id_to_pll(unsigned int id)
{
    return id < NR_PLLS ? &plls[id] : NULL;
}

static int pll_get_state_op(struct pll *pll)
{
    ENTER_FUNC(FUNC_LV_OP);

    EXIT_FUNC(FUNC_LV_OP);
    return clk_readl(pll->base_addr) & PLL_EN;
}

static void sdm_pll_enable_op(struct pll *pll)
{
    // PWRON:1 -> ISOEN:0 -> EN:1 -> RSTB:1

    ENTER_FUNC(FUNC_LV_OP);

    clk_setl(pll->pwr_addr, PLL_PWR_ON);
    udelay(1); // XXX: 30ns is enough (diff from 89)
    clk_clrl(pll->pwr_addr, PLL_ISO_EN);
    udelay(1); // XXX: 30ns is enough (diff from 89)

    clk_setl(pll->base_addr, pll->en_mask);
    udelay(20);

    if (pll->feat & HAVE_RST_BAR)
    {
        clk_setl(pll->base_addr, RST_BAR_MASK);
    }

    EXIT_FUNC(FUNC_LV_OP);
}

static void sdm_pll_disable_op(struct pll *pll)
{
    // RSTB:0 -> EN:0 -> ISOEN:1 -> PWRON:0

    ENTER_FUNC(FUNC_LV_OP);

    if (pll->feat & HAVE_RST_BAR)
    {
        clk_clrl(pll->base_addr, RST_BAR_MASK);
    }

    clk_clrl(pll->base_addr, PLL_EN);

    clk_setl(pll->pwr_addr, PLL_ISO_EN);
    clk_clrl(pll->pwr_addr, PLL_PWR_ON);

    EXIT_FUNC(FUNC_LV_OP);
}

static void sdm_pll_fsel_op(struct pll *pll, unsigned int value)
{
    unsigned int ctrl_value;
    unsigned int pll_en;

    ENTER_FUNC(FUNC_LV_OP);

    pll_en = clk_readl(pll->base_addr) & PLL_EN;
    ctrl_value = clk_readl(pll->base_addr + 4); // XPLL_CON1
    ctrl_value &= ~(SDM_PLL_N_INFO_MASK | SDM_PLL_POSDIV_MASK);
    ctrl_value |= value & (SDM_PLL_N_INFO_MASK | SDM_PLL_POSDIV_MASK);
    if (pll_en) ctrl_value |= SDM_PLL_N_INFO_CHG;

    clk_writel(pll->base_addr + 4, ctrl_value); // XPLL_CON1
    if (pll_en) udelay(20);

    EXIT_FUNC(FUNC_LV_OP);
}

static int sdm_pll_dump_regs_op(struct pll *pll, unsigned int *ptr)
{
    ENTER_FUNC(FUNC_LV_OP);

    *(ptr) = clk_readl(pll->base_addr);         // XPLL_CON0
    *(++ptr) = clk_readl(pll->base_addr + 4);   // XPLL_CON1
    *(++ptr) = clk_readl(pll->pwr_addr);        // XPLL_PWR

    EXIT_FUNC(FUNC_LV_OP);
    return 3; // return size
}

static const unsigned int pll_vcodivsel_map[2] = {1, 2};
static const unsigned int pll_prediv_map[4] = {1, 2, 4, 4};
static const unsigned int pll_posdiv_map[8] = {1, 2, 4, 8, 16, 16, 16, 16};
static const unsigned int pll_fbksel_map[4] = {1, 2, 4, 4};
static const unsigned int pll_n_info_map[14] = // assume fin = 26MHz
{
    13000000,
    6500000,
    3250000,
    1625000,
    812500,
    406250,
    203125,
    101563,
    50782,
    25391,
    12696,
    6348,
    3174,
    1587,
};

// vco_freq = fin (i.e. 26MHz) x n_info x vco_div_sel / prediv
// freq = vco_freq / posdiv
static unsigned int sdm_pll_vco_calc_op(struct pll *pll)
{
    int i;
    unsigned int mask;
    unsigned int vco_i = 0;
    unsigned int vco_f = 0;
    unsigned int vco = 0;

    // volatile unsigned int con0 = clk_readl(pll->base_addr);     // XPLL_CON0 /* not used */
    volatile unsigned int con1 = clk_readl(pll->base_addr + 4); // XPLL_CON1

    unsigned int vcodivsel  = 0; // (con0 >> 19) & 0x1; // bit[19]  // XXX: always zero
    unsigned int prediv     = 0; // (con0 >> 4) & 0x3;  // bit[5:4] // XXX: always zero
    unsigned int n_info_i = (con1 >> 14) & 0x7F;    // bit[20:14]
    unsigned int n_info_f = (con1 >> 0)  & 0x3FFF;  // bit[13:0]

    ENTER_FUNC(FUNC_LV_OP);

    vcodivsel = pll_vcodivsel_map[vcodivsel];
    prediv = pll_prediv_map[prediv];

    vco_i = 26 * n_info_i;

    for (i = 0; i < 14; i++)
    {
        mask = 1U << (13 - i);

        if (n_info_f & mask)
        {
            vco_f += pll_n_info_map[i];

            if (!(n_info_f & (mask-1))) // could break early if remaining bits are 0
            {
                break;
            }
        }
    }

    vco_f = (vco_f + 1000000 / 2) / 1000000; // round up

    vco = (vco_i + vco_f) * 1000 * vcodivsel / prediv; // KHz

#if 0
    pr_debug("[%s]%s: ["HEX_FMT", "HEX_FMT"] vco_i=%uMHz, vco_f=%uMHz, vco=%uKHz\n",
             __func__, pll->name, con0, con1, vco_i, vco_f, vco);
#endif

    EXIT_FUNC(FUNC_LV_OP);
    return vco;
}

static int sdm_pll_hp_enable_op(struct pll *pll)
{
    int err =0;
#if 1    
    unsigned int vco;

    ENTER_FUNC(FUNC_LV_OP);

    if (!pll->hp_switch || (pll->state == PWR_DOWN))
    {
        EXIT_FUNC(FUNC_LV_OP);
        return 0;
    }

    vco = pll->ops->vco_calc(pll);
    err = freqhopping_config(pll->hp_id, vco, 1);

    EXIT_FUNC(FUNC_LV_OP);
#endif
    return err;
}

static int sdm_pll_hp_disable_op(struct pll *pll)
{
    int err = 0;
#if 1    
    unsigned int vco;


    ENTER_FUNC(FUNC_LV_OP);

    if (!pll->hp_switch || (pll->state == PWR_ON)) // TODO: why PWR_ON
    {
        EXIT_FUNC(FUNC_LV_OP);
        return 0;
    }

    vco = pll->ops->vco_calc(pll);
    err = freqhopping_config(pll->hp_id, vco, 0);

    EXIT_FUNC(FUNC_LV_OP);
#endif    
    return err;
}

static struct pll_ops sdm_pll_ops =
{
    .get_state  = pll_get_state_op,
    .enable     = sdm_pll_enable_op,
    .disable    = sdm_pll_disable_op,
    .fsel       = sdm_pll_fsel_op,
    .dump_regs  = sdm_pll_dump_regs_op,
    .vco_calc   = sdm_pll_vco_calc_op,
    .hp_enable  = sdm_pll_hp_enable_op,
    .hp_disable = sdm_pll_hp_disable_op,
};

static void univ_pll_fsel_op(struct pll *pll, unsigned int value)
{
    unsigned int ctrl_value;
    unsigned int pll_en;

    ENTER_FUNC(FUNC_LV_OP);

    if (pll->feat & HAVE_FIX_FRQ) // XXX: UNIVPLL is HAVE_FIX_FRQ i.e. it would return
    {
        EXIT_FUNC(FUNC_LV_OP);
        return;
    }

    pll_en = clk_readl(pll->base_addr) & PLL_EN;
    ctrl_value = clk_readl(pll->base_addr + 4); // UNIVPLL_CON1
    ctrl_value &= ~PLL_FBKDIV_MASK;
    ctrl_value |= value & PLL_FBKDIV_MASK;
    if (pll_en) ctrl_value |= SDM_PLL_N_INFO_CHG;

    clk_writel(pll->base_addr + 4, ctrl_value); // UNIVPLL_CON1
    if (pll_en) udelay(20);

    EXIT_FUNC(FUNC_LV_OP);
}

static unsigned int univ_pll_vco_calc_op(struct pll *pll)
{
    unsigned int vco = 0;

    // volatile unsigned int con0 = clk_readl(pll->base_addr);     // UNIVPLL_CON0 /* not used */
    volatile unsigned int con1 = clk_readl(pll->base_addr + 4); // UNIVPLL_CON1

    unsigned int vcodivsel  = 0; // (con0 >> 19) & 0x1; // bit[19]      // XXX: always zero
    unsigned int prediv     = 0; // (con0 >> 4) & 0x3;  // bit[5:4]     // XXX: always zero
    unsigned int fbksel     = 0; // (con0 >> 20) & 0x3; // bit[21:20]   // XXX: always zero
    unsigned int fbkdiv     = con1 & PLL_FBKDIV_MASK;   // bit[6:0]

    ENTER_FUNC(FUNC_LV_OP);

    vcodivsel = pll_vcodivsel_map[vcodivsel];
    fbksel = pll_fbksel_map[fbksel];
    prediv = pll_prediv_map[prediv];

    vco = 26000 * fbkdiv * fbksel * vcodivsel / prediv;

#if 0
    pr_debug("[%s]%s: ["HEX_FMT"] vco=%uKHz\n", __func__, pll->name, con0, vco);
#endif

    EXIT_FUNC(FUNC_LV_OP);
    return vco;
}

static struct pll_ops univpll_ops =
{
    .get_state  = pll_get_state_op,
    .enable     = sdm_pll_enable_op,
    .disable    = sdm_pll_disable_op,
    .fsel       = univ_pll_fsel_op,
    .dump_regs  = sdm_pll_dump_regs_op,
    .vco_calc   = univ_pll_vco_calc_op,
};

static unsigned int pll_freq_calc_op(struct pll *pll)
{
    volatile unsigned int con1 = clk_readl(pll->base_addr + 4); // XPLL_CON1
    unsigned int posdiv = (con1 >> 24) & 0x7; // bit[26:24]

    posdiv = pll_posdiv_map[posdiv];

    return pll->ops->vco_calc(pll) / posdiv;
}

static int get_pll_state_locked(struct pll *pll)
{
    ENTER_FUNC(FUNC_LV_LOCKED);

    if (likely(initialized))
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return pll->state;                  // after init, get from local_state
    }
    else
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return pll->ops->get_state(pll);    // before init, get from reg_val
    }
}

static int enable_pll_locked(struct pll *pll)
{
    ENTER_FUNC(FUNC_LV_LOCKED);

    pll->cnt++;

    if (pll->cnt > 1)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0;
    }

    pll->ops->enable(pll);
    pll->state = PWR_ON;

    if (pll->ops->hp_enable && pll->feat & HAVE_PLL_HP) // enable freqnency hopping automatically
    {
        pll->ops->hp_enable(pll);
    }

    EXIT_FUNC(FUNC_LV_LOCKED);
    return 0;
}

static int disable_pll_locked(struct pll *pll)
{
    ENTER_FUNC(FUNC_LV_LOCKED);

    BUG_ON(!pll->cnt);
    pll->cnt--;

    if (pll->cnt > 0)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0;
    }

    pll->ops->disable(pll);
    pll->state = PWR_DOWN;

    if (pll->ops->hp_disable && pll->feat & HAVE_PLL_HP) // disable freqnency hopping automatically
    {
        pll->ops->hp_disable(pll);
    }

    EXIT_FUNC(FUNC_LV_LOCKED);
    return 0;
}

static int pll_fsel_locked(struct pll *pll, unsigned int value)
{
    ENTER_FUNC(FUNC_LV_LOCKED);

    pll->ops->fsel(pll, value);

    if (pll->ops->hp_enable && pll->feat & HAVE_PLL_HP)
    {
        pll->ops->hp_enable(pll);
    }

    EXIT_FUNC(FUNC_LV_LOCKED);
    return 0;
}

int pll_is_on(pll_id id)
{
    int state;
    unsigned long flags;
    struct pll *pll = id_to_pll(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!pll);

    clkmgr_lock(flags);
    state = get_pll_state_locked(pll);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return state;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(pll_is_on);

int enable_pll(pll_id id, const char *name)
{
    int err;
    unsigned long flags;
    struct pll *pll = id_to_pll(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    if (UNIVPLL != id)
    {
        // WARN_ON(UNIVPLL != id); // XXX: ARMPLL / MAINPLL are not controlled by AP side
        return 0;
    }

    BUG_ON(!initialized);
    BUG_ON(!pll);

    clkmgr_lock(flags);
    err = enable_pll_locked(pll);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(enable_pll);

int disable_pll(pll_id id, const char *name)
{
    int err;
    unsigned long flags;
    struct pll *pll = id_to_pll(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    if (UNIVPLL != id)
    {
        // WARN_ON(UNIVPLL != id); // XXX: ARMPLL / MAINPLL are not controlled by AP side
        return 0;
    }

    BUG_ON(!initialized);
    BUG_ON(!pll);

    clkmgr_lock(flags);
    err = disable_pll_locked(pll);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(disable_pll);

int pll_fsel(pll_id id, unsigned int value)
{
    int err;
    unsigned long flags;
    struct pll *pll = id_to_pll(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    if (UNIVPLL != id)
    {
        // WARN_ON(UNIVPLL != id); // XXX: ARMPLL / MAINPLL are not controlled by AP side
        return 0;
    }

    BUG_ON(!initialized);
    BUG_ON(!pll);

    clkmgr_lock(flags);
    err = pll_fsel_locked(pll, value);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(pll_fsel);

int pll_dump_regs(pll_id id, unsigned int *ptr)
{
    struct pll *pll = id_to_pll(id);

    ENTER_FUNC(FUNC_LV_DONT_CARE);

    BUG_ON(!initialized);
    BUG_ON(!pll);

    EXIT_FUNC(FUNC_LV_DONT_CARE);
    return pll->ops->dump_regs(pll, ptr);
}
EXPORT_SYMBOL(pll_dump_regs);

const char *pll_get_name(pll_id id)
{
    struct pll *pll = id_to_pll(id);

    ENTER_FUNC(FUNC_LV_DONT_CARE);

    BUG_ON(!initialized);
    BUG_ON(!pll);

    EXIT_FUNC(FUNC_LV_DONT_CARE);
    return pll->name;
}

#if 0   // XXX: NOT USED @ MT6572
#define CLKSQ1_EN           BIT(0)
#define CLKSQ1_LPF_EN       BIT(1)
#define CLKSQ1_EN_SEL       BIT(0)
#define CLKSQ1_LPF_EN_SEL   BIT(1)

void enable_clksq1(void)
{
    unsigned long flags;

    clkmgr_lock(flags);
    clk_setl(AP_PLL_CON0, CLKSQ1_EN);
    udelay(200);
    clk_setl(AP_PLL_CON0, CLKSQ1_LPF_EN);
    clkmgr_unlock(flags);
}
EXPORT_SYMBOL(enable_clksq1);

void disable_clksq1(void)
{
    unsigned long flags;

    clkmgr_lock(flags);
    clk_clrl(AP_PLL_CON0, CLKSQ1_LPF_EN | CLKSQ1_EN);
    clkmgr_unlock(flags);
}
EXPORT_SYMBOL(disable_clksq1);

void clksq1_sw2hw(void)
{
    unsigned long flags;

    clkmgr_lock(flags);
    clk_clrl(AP_PLL_CON1, CLKSQ1_LPF_EN_SEL | CLKSQ1_EN_SEL);
    clkmgr_unlock(flags);
}
EXPORT_SYMBOL(clksq1_sw2hw);

void clksq2_hw2sw(void)
{
    unsigned long flags;

    clkmgr_lock(flags);
    clk_setl(AP_PLL_CON1, CLKSQ1_LPF_EN_SEL | CLKSQ1_EN_SEL);
    clkmgr_unlock(flags);
}
EXPORT_SYMBOL(clksq1_hw2sw);
#endif  // XXX: NOT USED @ MT6572

//
// LARB related
//
static DEFINE_MUTEX(larb_monitor_lock);
static LIST_HEAD(larb_monitor_handlers);

void register_larb_monitor(struct larb_monitor *handler)
{
    struct list_head *pos;

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

#else   // CONFIG_CLKMGR_EMULATION

    mutex_lock(&larb_monitor_lock);
    list_for_each(pos, &larb_monitor_handlers)
    {
        struct larb_monitor *l;
        l = list_entry(pos, struct larb_monitor, link);

        if (l->level > handler->level)
        {
            break;
        }
    }
    list_add_tail(&handler->link, pos);
    mutex_unlock(&larb_monitor_lock);

    EXIT_FUNC(FUNC_LV_API);

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(register_larb_monitor);


void unregister_larb_monitor(struct larb_monitor *handler)
{
    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

#else   // CONFIG_CLKMGR_EMULATION

    mutex_lock(&larb_monitor_lock);
    list_del(&handler->link);
    mutex_unlock(&larb_monitor_lock);

    EXIT_FUNC(FUNC_LV_API);

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(unregister_larb_monitor);

static void larb_clk_prepare(int larb_idx)
{
	ENTER_FUNC(FUNC_LV_BODY);

	switch (larb_idx)
	{
		case MT_LARB0:
			clk_writel(MMSYS_CG_CLR0, SMI_COMMON_BIT | SMI_LARB0_BIT);
			break;
		case MT_LARB1:
			clk_writel(IMG_CG_CLR, LARB1_SMI_CKPDN_BIT);
			break;
		default:
			BUG();
	}

	EXIT_FUNC(FUNC_LV_BODY);
}

static void larb_clk_finish(int larb_idx)
{
	ENTER_FUNC(FUNC_LV_BODY);

	switch (larb_idx)
	{
		case MT_LARB0:
			clk_writel(MMSYS_CG_SET0, SMI_COMMON_BIT | SMI_LARB0_BIT);
			break;
		case MT_LARB1:
			clk_writel(IMG_CG_SET, LARB1_SMI_CKPDN_BIT);
			break;
		default:
			BUG();
	}
	EXIT_FUNC(FUNC_LV_BODY);
}

static void larb_backup(int larb_idx)
{
    struct larb_monitor *pos;

    ENTER_FUNC(FUNC_LV_BODY);

    // pr_debug("[%s]: start to backup larb%d\n", __func__, larb_idx);
    larb_clk_prepare(larb_idx);

    list_for_each_entry(pos, &larb_monitor_handlers, link)
    {
        if (pos->backup != NULL)
        {
            pos->backup(pos, larb_idx);
        }
    }

    larb_clk_finish(larb_idx);

    EXIT_FUNC(FUNC_LV_BODY);
}

static void larb_restore(int larb_idx)
{
    struct larb_monitor *pos;

    ENTER_FUNC(FUNC_LV_BODY);

    // pr_debug("[%s]: start to restore larb%d\n", __func__, larb_idx);
    larb_clk_prepare(larb_idx);

    list_for_each_entry(pos, &larb_monitor_handlers, link)
    {
        if (pos->restore != NULL)
        {
            pos->restore(pos, larb_idx);
        }
    }

    larb_clk_finish(larb_idx);

    EXIT_FUNC(FUNC_LV_BODY);
}

//
// SUBSYS variable & function
//
#define SYS_TYPE_MODEM    0
#define SYS_TYPE_MEDIA    1
#define SYS_TYPE_OTHER    2

static struct subsys_ops md1_sys_ops;
static struct subsys_ops con_sys_ops;
static struct subsys_ops dis_sys_ops;
static struct subsys_ops mfg_sys_ops;
static struct subsys_ops img_sys_ops;

static struct subsys syss[] = // NR_SYSS
{
    [SYS_MD1] =
    {
        .name               = __stringify(SYS_MD1),
        .type               = SYS_TYPE_MODEM,
        .default_sta        = PWR_DOWN,
	.sta_mask           = MD1_PWR_STA_MASK,
        .ops                = &md1_sys_ops, // &md1_sys_ops
    },
    [SYS_CON] =
    {
        .name               = __stringify(SYS_CON),
        .type               = SYS_TYPE_MODEM,
        .default_sta        = PWR_DOWN,
	.sta_mask           = CONN_PWR_STA_MASK,
        .ops                = &con_sys_ops, // &con_sys_ops
    },
    [SYS_DIS] =
    {
        .name               = __stringify(SYS_DIS),
        .type               = SYS_TYPE_MEDIA,
        .default_sta        = PWR_ON,
	.sta_mask           = DIS_PWR_STA_MASK,
        .ops                = &dis_sys_ops,
        .start              = &grps[CG_MMSYS0],
        .nr_grps            = 2,
    },
    [SYS_MFG] =
    {
        .name               = __stringify(SYS_MFG),
        .type               = SYS_TYPE_MEDIA,
        .default_sta        = PWR_DOWN,
	.sta_mask           = MFG_PWR_STA_MASK,
        .ops                = &mfg_sys_ops,
        .start              = &grps[CG_MFGSYS],
        .nr_grps            = 1,
    },
    [SYS_IMG] =
    {
        .name               = __stringify(SYS_IMG),
        .type               = SYS_TYPE_MEDIA,
        .default_sta        = PWR_DOWN,
	.sta_mask           = ISP_PWR_STA_MASK,
        .ops                = &img_sys_ops,
        .start              = &grps[CG_IMGSYS],
        .nr_grps            = 1,
    },
};

static struct subsys *id_to_sys(unsigned int id)
{
    return id < NR_SYSS ? &syss[id] : NULL;
}

#define FMT7	",%d,%d,%d,%d,%d,%d,%d\n"
#define VAL7	,value[0],value[1],value[2],value[3],value[4],value[5],value[6]

#define SAMPLE_FMT	"%5lu.%06lu"
#define SAMPLE_VAL	(unsigned long)(timestamp), nano_rem/1000

void ms_mtcmos(unsigned long long timestamp, unsigned char cnt, unsigned int *value)
{
	unsigned long nano_rem = do_div(timestamp, 1000000000);

	switch (cnt) {
	case 7: trace_printk(SAMPLE_FMT FMT7, SAMPLE_VAL VAL7); break;
	}
}

static int md1_sys_enable_op(struct subsys *sys)
{
    int err; 
    err = spm_mtcmos_ctrl_mdsys1(STA_POWER_ON);
    return err;
}

static int md1_sys_disable_op(struct subsys *sys)
{
    int err;
    err = spm_mtcmos_ctrl_mdsys1(STA_POWER_DOWN);
    return err;
}

static int con_sys_enable_op(struct subsys *sys)
{
    int err; 
    err = spm_mtcmos_ctrl_connsys(STA_POWER_ON);
    return err;
}

static int con_sys_disable_op(struct subsys *sys)
{
    int err;
    err = spm_mtcmos_ctrl_connsys(STA_POWER_DOWN);
    return err;
}

static int dis_sys_enable_op(struct subsys *sys)
{
	struct cg_clk *cg_clk = id_to_clk(MT_CG_SMI_MM_SW_CG);
	int err;
#ifdef SYS_LOG	
	pr_debug("[%s]: sys->name=%s\n", __func__, sys->name);
#endif	
//return 0;
	enable_clock_locked(cg_clk);
	err = spm_mtcmos_ctrl_disp(STA_POWER_ON);
	larb_restore(MT_LARB0);
	return err;
}

static int dis_sys_disable_op(struct subsys *sys)
{
	struct cg_clk *cg_clk = id_to_clk(MT_CG_SMI_MM_SW_CG);
	int err;
#ifdef SYS_LOG	
	pr_debug("[%s]: sys->name=%s\n", __func__, sys->name);
#endif
//return 0;
	larb_backup(MT_LARB0);
	err = spm_mtcmos_ctrl_disp(STA_POWER_DOWN);
	disable_clock_locked(cg_clk);
	return err;
}

static int mfg_sys_enable_op(struct subsys *sys)
{
	struct cg_clk *mfg_clk = id_to_clk(MT_CG_MFG_MM_SW_CG);
	int err = 0;
#ifdef SYS_LOG	
	pr_debug("[%s]: sys->name=%s\n", __func__, sys->name);
#endif
	if (get_clk_state_locked(mfg_clk));
		enable_clock_locked(mfg_clk);
	err = spm_mtcmos_ctrl_mfg(STA_POWER_ON);
	return err;
}

static int mfg_sys_disable_op(struct subsys *sys)
{
	struct cg_clk *mfg_clk = id_to_clk(MT_CG_MFG_MM_SW_CG);
	int err = 0;
#ifdef SYS_LOG	
	pr_debug("[%s]: sys->name=%s\n", __func__, sys->name);
#endif
	err = spm_mtcmos_ctrl_mfg(STA_POWER_DOWN);
	if (get_clk_state_locked(mfg_clk));
		disable_clock_locked(mfg_clk);
	return err;
}

static int img_sys_enable_op(struct subsys *sys)
{
    int err;
#ifdef SYS_LOG	
    pr_debug("[%s]: sys->name=%s\n", __func__, sys->name);
#endif
    err = spm_mtcmos_ctrl_isp(STA_POWER_ON);
//    larb_restore(MT_LARB1);

    return err;
}

static int img_sys_disable_op(struct subsys *sys)
{
    int err;
#ifdef SYS_LOG	
    pr_debug("[%s]: sys->name=%s\n", __func__, sys->name);
#endif

//    larb_backup(MT_LARB1);
    err = spm_mtcmos_ctrl_isp(STA_POWER_DOWN);
    return err;
}

static int sys_get_state_op(struct subsys *sys)
{
    unsigned int sta = clk_readl(SPM_PWR_STATUS);
    unsigned int sta_s = clk_readl(SPM_PWR_STATUS_2ND);

    return (sta & sys->sta_mask) && (sta_s & sys->sta_mask);
}

static int sys_dump_regs_op(struct subsys *sys, unsigned int *ptr)
{
    *(ptr) = clk_readl(sys->ctl_addr);
    return 1;
}

static struct subsys_ops md1_sys_ops =
{
    .enable     = md1_sys_enable_op,
    .disable    = md1_sys_disable_op,
    .get_state  = sys_get_state_op,
    .dump_regs  = sys_dump_regs_op,
};

static struct subsys_ops con_sys_ops =
{
    .enable     = con_sys_enable_op,
    .disable    = con_sys_disable_op,
    .get_state  = sys_get_state_op,
    .dump_regs  = sys_dump_regs_op,
};

static struct subsys_ops mfg_sys_ops =
{
    .enable     = mfg_sys_enable_op,
    .disable    = mfg_sys_disable_op,
    .get_state  = sys_get_state_op,
    .dump_regs  = sys_dump_regs_op,
};

static struct subsys_ops img_sys_ops = {
    .enable = img_sys_enable_op,
    .disable = img_sys_disable_op,
    .get_state = sys_get_state_op,
    .dump_regs = sys_dump_regs_op,
};

static struct subsys_ops dis_sys_ops =
{
    .enable     = dis_sys_enable_op,
    .disable    = dis_sys_disable_op,
    .get_state  = sys_get_state_op,
    .dump_regs  = sys_dump_regs_op,
};

static int get_sys_state_locked(struct subsys *sys)
{
    ENTER_FUNC(FUNC_LV_LOCKED);

    if (likely(initialized))
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return sys->state;                  // after init, get from local_state
    }
    else
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return sys->ops->get_state(sys);    // before init, get from reg_val
    }
}

void set_mipi26m(int en)
{
    unsigned long flags;
	struct cg_clk *clk = id_to_clk(MT_CG_MIPI_26M_DBG_EN);
#if defined(CONFIG_CLKMGR_EMULATION)
return;
#endif

    clkmgr_lock(flags);
    
    if(en){
	if(!get_clk_state_locked(clk))
		enable_clock_locked(clk);
        clk_setl(AP_PLL_CON0, 1 << 5);
    } else {
        clk_clrl(AP_PLL_CON0, 1 << 5);
	if(get_clk_state_locked(clk))
		disable_clock_locked(clk);
    }
    clkmgr_unlock(flags);
}
EXPORT_SYMBOL(set_mipi26m);

static void clk_set_force_on_locked(struct cg_clk *clk)
{
	clk->force_on = 1;
}

static void clk_clr_force_on_locked(struct cg_clk *clk)
{
	clk->force_on = 0;
}

void clk_set_force_on(int id)
{
    unsigned long flags;
    struct cg_clk *clk = id_to_clk(id);

#ifdef Bring_Up
return;
#endif

    BUG_ON(!initialized);
    BUG_ON(!clk);
    BUG_ON(!clk->grp);
    BUG_ON(!clk->ops->check_validity(clk));

    clkmgr_lock(flags);

    clk_set_force_on_locked(clk);
    clkmgr_unlock(flags);

}
EXPORT_SYMBOL(clk_set_force_on);

void clk_clr_force_on(int id)
{
    unsigned long flags;
    struct cg_clk *clk = id_to_clk(id);

#ifdef Bring_Up
return;
#endif
    BUG_ON(!initialized);
    BUG_ON(!clk);
    BUG_ON(!clk->grp);
    BUG_ON(!clk->ops->check_validity(clk));

    clkmgr_lock(flags);
    clk_clr_force_on_locked(clk);
    clkmgr_unlock(flags);
}
EXPORT_SYMBOL(clk_clr_force_on);

int clk_is_force_on(int id)
{
    struct cg_clk *clk = id_to_clk(id);

#ifdef Bring_Up
return 0;
#endif

    BUG_ON(!initialized);
    BUG_ON(!clk);
    BUG_ON(!clk->grp);
    BUG_ON(!clk->ops->check_validity(clk));

    return clk->force_on;
}
EXPORT_SYMBOL(clk_is_force_on);


int subsys_is_on(subsys_id id)
{
    int state;
    unsigned long flags;
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!sys);

    clkmgr_lock(flags);
    state = get_sys_state_locked(sys);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return state;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(subsys_is_on);

static int enable_subsys_locked(struct subsys *sys)
{
    int err;
    int local_state = sys->state; //get_subsys_local_state(sys);

    ENTER_FUNC(FUNC_LV_LOCKED);

#ifdef STATE_CHECK_DEBUG
    int reg_state = sys->ops->get_state(sys);//get_subsys_reg_state(sys);
    BUG_ON(local_state != reg_state);
#endif

    if (local_state == PWR_ON)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0; // XXX: ??? just check local_state ???
    }

    err = sys->ops->enable(sys);
    WARN_ON(err);

    if (!err)
    {
        sys->state = PWR_ON;
    }

    EXIT_FUNC(FUNC_LV_LOCKED);
    return err;
}

static int disable_subsys_locked(struct subsys *sys, int force_off)
{
    int err;
    int local_state = sys->state;//get_subsys_local_state(sys);
    int i;
    const struct cg_grp *grp;

    ENTER_FUNC(FUNC_LV_LOCKED);

#ifdef STATE_CHECK_DEBUG
    int reg_state = sys->ops->get_state(sys);//get_subsys_reg_state(sys);
    BUG_ON(local_state != reg_state);
#endif

    if (!force_off) // XXX: check all clock gate groups related to this subsys are off
    {
        //could be power off or not
        for (i = 0; i < sys->nr_grps; i++)
        {
            grp = sys->start + i;

            if (grp->state)
            {
                EXIT_FUNC(FUNC_LV_LOCKED);
                return 0;
            }
        }
    }

    if (local_state == PWR_DOWN)
    {
        EXIT_FUNC(FUNC_LV_LOCKED);
        return 0;
    }

    err = sys->ops->disable(sys);
    WARN_ON(err);

    if (!err)
    {
        sys->state = PWR_DOWN;
    }

    EXIT_FUNC(FUNC_LV_LOCKED);
    return err;
}

int enable_subsys(subsys_id id, const char *name)
{
    int err;
    unsigned long flags;
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!sys);

    clkmgr_lock(flags);
    err = enable_subsys_locked(sys);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(enable_subsys);

int disable_subsys(subsys_id id, const char *name)
{
    int err;
    unsigned long flags;
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!sys);

    clkmgr_lock(flags);
    err = disable_subsys_locked(sys, 0);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(disable_subsys);

int disable_subsys_force(subsys_id id, const char *name)
{
    int err;
    unsigned long flags;
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!sys);

    clkmgr_lock(flags);
    err = disable_subsys_locked(sys, 1);
    clkmgr_unlock(flags);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(disable_subsys_force);

int subsys_dump_regs(subsys_id id, unsigned int *ptr)
{
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_DONT_CARE);

    BUG_ON(!initialized);
    BUG_ON(!sys);

    EXIT_FUNC(FUNC_LV_DONT_CARE);
    return sys->ops->dump_regs(sys, ptr);
}
EXPORT_SYMBOL(subsys_dump_regs);

const char *subsys_get_name(subsys_id id)
{
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_DONT_CARE);

    BUG_ON(!initialized);
    BUG_ON(!sys);

    EXIT_FUNC(FUNC_LV_DONT_CARE);
    return sys->name;
}

#define JIFFIES_PER_LOOP 10

int md_power_on(subsys_id id)
{
    int err;
    unsigned long flags;
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!sys);
    BUG_ON(sys->type != SYS_TYPE_MODEM);
    BUG_ON(id != SYS_MD1);

		err = spm_mtcmos_ctrl_mdsys1(STA_POWER_ON);
    WARN_ON(err);
    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(md_power_on);

int md_power_off(subsys_id id, unsigned int timeout)
{
    int err;
    int cnt;
    bool slept =0;
    unsigned long flags;
    struct subsys *sys = id_to_sys(id);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!sys);
    BUG_ON(sys->type != SYS_TYPE_MODEM);
    BUG_ON(id != SYS_MD1);

    // 0: not sleep, 1: sleep
    slept = 0;//fixme spm_is_md_sleep();
    cnt = (timeout + JIFFIES_PER_LOOP - 1) / JIFFIES_PER_LOOP;

    while (!slept && cnt--)
    {
        msleep(MSEC_PER_SEC / JIFFIES_PER_LOOP);
//        slept = spm_is_md_sleep();

        if (slept)
        {
            break;
        }
    }

    clkmgr_lock(flags);
    // XXX: md (abnormal) reset or flight mode fail
		err = spm_mtcmos_ctrl_mdsys1(STA_POWER_DOWN);

    clkmgr_unlock(flags);

    WARN_ON(err);

    EXIT_FUNC(FUNC_LV_API);
    return !slept;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(md_power_off);

int conn_power_on(void)
{
    int err=0;
    unsigned long flags;
    struct subsys *sys = id_to_sys(SYS_CON);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!sys);
    BUG_ON(sys->type != SYS_TYPE_MODEM);

    // hack for gps
    clk_buf_ctrl(CLK_BUF_AUDIO, 1);

    clkmgr_lock(flags);
    err = enable_subsys_locked(sys);
    clkmgr_unlock(flags);

    WARN_ON(err);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(conn_power_on);

int conn_power_off(void)
{
    int err=0;
    int cnt;
    bool slept =0;
    unsigned long flags;
    struct subsys *sys = id_to_sys(SYS_CON);

    ENTER_FUNC(FUNC_LV_API);

#if defined(CONFIG_CLKMGR_EMULATION)

    return 0;

#else   // CONFIG_CLKMGR_EMULATION

    BUG_ON(!initialized);
    BUG_ON(!sys);
    BUG_ON(sys->type != SYS_TYPE_MODEM);

    clkmgr_lock(flags);
    err = disable_subsys_locked(sys, 0);
    clkmgr_unlock(flags);
    WARN_ON(err);

    // hack for gps
    clk_buf_ctrl(CLK_BUF_AUDIO, 0);

    EXIT_FUNC(FUNC_LV_API);
    return err;

#endif  // CONFIG_CLKMGR_EMULATION
}
EXPORT_SYMBOL(conn_power_off);

//
// INIT related
//
static void dump_clk_info(void);

#if 0	/* XXX: for bring up */
static void subsys_all_force_on(void)
{
    struct subsys *sys;
    int i;

    for (i = 0; i < NR_SYSS; i++)
    {
        sys = &syss[i];

        if (PWR_DOWN == sys->ops->get_state(sys))
        {
            spm_mtcmos_ctrl_general_locked(sys, STA_POWER_ON);
        }
    }
}
#endif
static void clk_stat_bug(void)
{
	struct cg_clk *clk;
	struct clkmux *clkmuxs;
	int i;
	int skip;
	for (i = CG_MPLL_FROM; i < CG_AUDIO_TO; i++) {
		clk = id_to_clk(i);
		if (!clk || !clk->grp || !clk->ops->check_validity(clk))
			continue;
		skip = (clk->cnt == 0) && (clk->state == 0);
		if (skip)
			continue;
		pr_debug(" [%s]state=%u, cnt=%u \n", clk->name, clk->state, clk->cnt);
        }

	for (i = 0; i < NR_CLKMUXS; i++)
	{
		cg_clk_id src_id;
		
		clkmuxs = &muxs[i];
		src_id = clkmuxs->ops->get(clkmuxs);
		pr_debug("clkmux[%s]: source = %s,source cnt = %d, Drain = %s, Drain cnt = %d \n", 
		clkmuxs->name, clks[src_id].name,clks[src_id].cnt, clks[clkmuxs->drain].name, clks[clkmuxs->drain].cnt); // <-XXX
	}
}

static void mtcmos_force_off(void)
{
	struct cg_clk *cg_clk = id_to_clk(MT_CG_MFG_MM_SW_CG);
	int err = 0;
	int i;
        for (i = SYS_MD1; i < NR_SYSS; i++)
		disable_subsys_force(i,"forceoff");
	enable_clock_locked(cg_clk);
	err = spm_mtcmos_ctrl_mfg(STA_POWER_DOWN);
	disable_clock_locked(cg_clk);
	disable_pll_locked(id_to_pll(UNIVPLL));
}
void slp_check_pm_mtcmos_pll(void)
{
	int i,src_id;
	struct cg_grp *grp;
	slp_chk_mtcmos_pll_stat = 1;
	pr_debug("[%s]\n", __func__);

	if (pll_is_on(UNIVPLL)) {
		slp_chk_mtcmos_pll_stat = -1;
		pr_debug("%s: on\n", plls[UNIVPLL].name);
		pr_debug("suspend warning: %s is on!!!\n", plls[UNIVPLL].name);
		pr_debug("warning! warning! warning! it may cause resume fail\n");
		clk_stat_bug();
	}

	for (i = 0; i < NR_SYSS; i++) {
		if (subsys_is_on(i)) {
			pr_debug("%s: on\n", syss[i].name);
			if (i > SYS_CON) {
				//aee_kernel_warning("Suspend Warning","%s is on", subsyss[i].name);
				slp_chk_mtcmos_pll_stat = -1;
				pr_debug("suspend warning: %s is on!!!\n", syss[i].name);
				pr_debug("warning! warning! warning! it may cause resume fail\n");
				clk_stat_bug();
			}
		}
	}

	for (i = MT_CG_UNIV_48M; i <= CG_AUDIO; i++) {
		grp = id_to_grp(i);
		pr_debug("[%s] sta_addr = 0x%x, mask = 0x%x, => 0x%x \n", grp->name, clk_readl(grp->sta_addr), grp->mask, clk_readl(grp->sta_addr) & (grp->mask));
	}
	//mtcmos_force_off();
}
EXPORT_SYMBOL(slp_check_pm_mtcmos_pll);

static void cg_all_force_on(void)
{
    ENTER_FUNC(FUNC_LV_BODY);

    clk_writel(SET_MPLL_FREDIV_EN, CG_MPLL_MASK);
    clk_writel(SET_UPLL_FREDIV_EN, CG_UPLL_MASK);
    clk_writel(CLR_CLK_GATING_CTRL0, (CG_CTRL0_MASK & (~CG_CTRL0_EN_MASK)));
    // clk_writel(SET_CLK_GATING_CTRL0, CG_CTRL0_EN_MASK); // TODO: feature enable?
    clk_writel(CLR_CLK_GATING_CTRL1, CG_CTRL1_MASK);
    clk_writel(MMSYS_CG_CLR0, CG_MMSYS0_MASK);
    clk_writel(MMSYS_CG_CLR1, CG_MMSYS1_MASK);
    clk_writel(MFG_CG_CLR, CG_MFGSYS_MASK);
    clk_writel(IMG_CG_CON, CG_IMGSYS_MASK);
    clk_writel(AUDIO_TOP_CON0, CG_AUDIO_MASK);
	

    EXIT_FUNC(FUNC_LV_BODY);
}
	/* XXX: for bring up */

static void mt_subsys_init(void)
{
	int i;
	struct subsys *sys;

	ENTER_FUNC(FUNC_LV_API);

    for (i = 0; i < NR_SYSS; i++)
    {
        sys = &syss[i];
        sys->state = sys->ops->get_state(sys);

        if (sys->state != sys->default_sta) // XXX: state not match and use default_state (sync with cg_clk[]?)
        {
            pr_debug("[%s]%s, change state: (%u->%u)\n", __func__,
                     sys->name, sys->state, sys->default_sta);

            if (sys->default_sta == PWR_DOWN)
            {
                disable_subsys_locked(sys, 1);
            }
            else
            {
                enable_subsys_locked(sys);
            }
        }
    }

    EXIT_FUNC(FUNC_LV_API);
}

static void mt_plls_init(void)
{
    int i;
    struct pll *pll;

    ENTER_FUNC(FUNC_LV_API);

    for (i = 0; i < NR_PLLS; i++)
    {
        pll = &plls[i];
        pll->state = pll->ops->get_state(pll);
//	if (pll->state)
//		pll->cnt = 1;
    }

    EXIT_FUNC(FUNC_LV_API);
}

static void mt_clks_init(void)
{
    int i;
    struct cg_grp *grp;
    struct cg_clk *clk;
    struct clkmux *clkmux;

    ENTER_FUNC(FUNC_LV_API);

    // TODO: preset

    //clk_writel(CLR_CLK_GATING_CTRL0, MFG_MM_SW_CG_BIT); // XXX: patch for MFG clock access @ init mode

#if 0
    clk_writel(CLR_CLK_GATING_CTRL0, (CG_CTRL0_MASK & (~CG_CTRL0_EN_MASK)));
    // clk_writel(SET_CLK_GATING_CTRL0, CG_CTRL0_EN_MASK); // TODO: feature enable?
    clk_writel(CLR_CLK_GATING_CTRL1, CG_CTRL1_MASK);
    clk_writel(MMSYS_CG_CLR0, CG_MMSYS0_MASK);
    clk_writel(MMSYS_CG_CLR1, CG_MMSYS1_MASK);
    clk_writel(MFG_CG_CLR, CG_MFG_MASK);
    clk_writel(AUDIO_TOP_CON0, CG_AUDIO_MASK);
#else

#endif

	clk_writel(CLK_GATING_CTRL1, (clk_readl(CLK_GATING_CTRL1) | USB_SW_CG_BIT ));
	clk_writel(CLK_GATING_CTRL2, (clk_readl(CLK_GATING_CTRL2) | RBIST_SW_CG_BIT));
	clk_writel(INFRA_RSVD1, (clk_readl(INFRA_RSVD1) | PLL1_CK_BIT | PLL2_CK_BIT));

#ifndef CONFIG_MTK_MTD_NAND
	clk_writel(CLK_GATING_CTRL2, (clk_readl(CLK_GATING_CTRL2) | NFI_BUS_SW_CG_BIT));
	clk_writel(CLK_GATING_CTRL1, (clk_readl(CLK_GATING_CTRL1) | NFI_SW_CG_BIT | NFIECC_SW_CG_BIT | NFI2X_SW_CG_BIT));
#endif
    //clk_writel(INFRABUS_DCMCTL1, clk_readl(INFRABUS_DCMCTL1) | BIT(29)); // XXX: for SPM

    // TODO: patch clk_ops if necessary

    // TODO: set parent (i.e. pll)

    // init CG_CLK
    for (i = 0; i < NR_CLKS; i++)
    {
        clk = &clks[i];
        grp = clk->grp;

        if (NULL != grp) // XXX: CG_CLK always map one of CL_GRP
        {
		grp->mask |= clk->mask; // XXX: init cg_grp mask by cg_clk mask
		clk->state = clk->ops->get_state(clk);
		if(!clk->cnt)
			clk->cnt = 0;
		else
			clk->cnt = (PWR_ON == clk->state) ? 1 : 0;
		clk->force_on = 0;
		pr_debug("clks[%d] %s	CNT = %d,	STATE = %d\n", i, clks[i].name, clk->cnt, clk->state);
        }
        else
        {
            BUG(); // XXX: CG_CLK always map one of CL_GRP
        }
    }

    // init CG_GRP
    for (i = 0; i < NR_GRPS; i++)
    {
        grp = &grps[i];
        grp->state = grp->ops->get_state(grp);
    }

    // init CLKMUX (link clk src + clkmux + clk drain)
    for (i = 0; i < NR_CLKMUXS; i++)
    {
        cg_clk_id src_id;

        clkmux = &muxs[i];

        src_id = clkmux->ops->get(clkmux);

        pr_debug("clkmux[%d]: source = %s source cnt = %d, Drain = %s Drain cnt = %d\n", i, clks[src_id].name, clks[src_id].cnt, clks[clkmux->drain].name, clks[clkmux->drain].cnt); // <-XXX

        clk = id_to_clk(clkmux->drain); // clk (drain)

        if (NULL != clk) // clk (drain)
        {
            clk->src = src_id; // clk (drain)
        }
        else // wo drain
        {
            clk = id_to_clk(src_id); // clk (source)

            if (NULL != clk) // clk (source)
            {
                enable_clock_locked(clk); // clk (source)
            }
        }
    }

    // init CG_CLK again (construct clock tree dependency)
	for (i = 0; i < NR_CLKS; i++)
	{
		clk = &clks[i];
		pr_debug("%s[%d]:%s cnt=%d state=%d\n", __FUNCTION__, clk - &clks[0], clk->name, clk->cnt, clk->ops->get_state(clk));
		if (PWR_ON == clk->state)
		{
			BUG_ON((MT_CG_INVALID != clk->src) && (NULL != clk->parent));
			if (MT_CG_INVALID != clk->src)
			{
				clk = id_to_clk(clk->src); // clk (source)
				if (NULL != clk)
				{
					enable_clock_locked(clk); // clk (source)
				}
			} 
			else if (NULL != clk->parent)
			{
				enable_pll_locked(clk->parent);
			}
		}
        }

	for (i = CG_UPLL_TO; i >= MT_CG_UNIV_48M; i--) // XXX: tricky (down to up)
	{
		clk = &clks[i];
		if ((i != MT_CG_MPLL_D14))
			clk->cnt--;
		if (0 == clk->cnt)
		{
			clk->state = PWR_DOWN;
			clk->ops->disable(clk);
			if (MT_CG_INVALID != clk->src)
			{
				clk = id_to_clk(clk->src); // clk (source)
				if (NULL != clk) // clk (source)
				{
					disable_clock_locked(clk); // clk (source)
				}
			}
			else if (NULL != clk->parent)
			{
				disable_pll_locked(clk->parent);
			}
		}
	}
	for (i = MT_CG_VCODEC_CKPDN; i >= CG_MPLL_FROM; i--) {
		clk = &clks[i];
		if ((0 == clk->cnt) && (PWR_ON == clk->state)) {
			if (MT_CG_INVALID != clk->src) {
				clk = id_to_clk(clk->src);	// clk (source)
				if (NULL != clk) {
					clk->cnt --;		// clk (source)
				}
			}
		}
	}
	disable_clock_locked(&clks[MT_CG_AUDIO_SW_CG]);
	// Don't disable these clock until it's clk_clr_force_on() is called
	clk_set_force_on_locked(&clks[MT_CG_DISP0_SMI_LARB0]);
	clk_set_force_on_locked(&clks[MT_CG_DISP0_SMI_COMMON]);
	EXIT_FUNC(FUNC_LV_API);
}

//#endif //#ifndef Bring_Up
#ifdef CONFIG_OF
void iomap(void)
{
    struct device_node *node;

//apmixed
    node = of_find_compatible_node(NULL, NULL, "mediatek,APMIXED");
    if (!node) {
        pr_debug("[CLK_APMIXED] find node failed\n");
    }
    clk_apmixed_base = of_iomap(node, 0);
    if (!clk_apmixed_base)
        pr_debug("[CLK_APMIXED] base failed\n");
//cksys_base
    node = of_find_compatible_node(NULL, NULL, "mediatek,TOPCKGEN");
    if (!node) {
        pr_debug("[CLK_CKSYS] find node failed\n");
    }
    clk_topcksys_base = of_iomap(node, 0);
    if (!clk_topcksys_base)
        pr_debug("[CLK_CKSYS] base failed\n");
//infracfg_ao        
    node = of_find_compatible_node(NULL, NULL, "mediatek,INFRACFG_AO");
    if (!node) {
        pr_debug("[CLK_INFRACFG_AO] find node failed\n");
    }
    clk_infracfg_ao_base = of_iomap(node, 0);
    if (!clk_infracfg_ao_base)
        pr_debug("[CLK_INFRACFG_AO] base failed\n");
//audio
    node = of_find_compatible_node(NULL, NULL, "mediatek,AUDIO");
    if (!node) {
        pr_debug("[CLK_AUDIO] find node failed\n");
    }
    clk_audio_base = of_iomap(node, 0);
    if (!clk_audio_base)
        pr_debug("[CLK_AUDIO] base failed\n");
//mfgcfg
    node = of_find_compatible_node(NULL, NULL, "mediatek,G3D_CONFIG");
    if (!node) {
        pr_debug("[CLK_G3D_CONFIG] find node failed\n");
    }
    clk_mfgcfg_base = of_iomap(node, 0);
    if (!clk_mfgcfg_base)
        pr_debug("[CLK_G3D_CONFIG] base failed\n");
//mmsys_config
    node = of_find_compatible_node(NULL, NULL, "mediatek,MMSYS_CONFIG");
    if (!node) {
        pr_debug("[CLK_MMSYS_CONFIG] find node failed\n");
    }
    clk_mmsys_config_base = of_iomap(node, 0);
    if (!clk_mmsys_config_base)
        pr_debug("[CLK_MMSYS_CONFIG] base failed\n");
//imgsys
    node = of_find_compatible_node(NULL, NULL, "mediatek,IMGSYS_CONFIG");
    if (!node) {
        pr_debug("[CLK_IMGSYS_CONFIG] find node failed\n");
    }
    clk_imgsys_base = of_iomap(node, 0);
    if (!clk_imgsys_base)
        pr_debug("[CLK_IMGSYS_CONFIG] base failed\n");

	syss[SYS_MD1].ctl_addr = SPM_MD_PWR_CON;
	syss[SYS_CON].ctl_addr = SPM_CONN_PWR_CON;
	syss[SYS_DIS].ctl_addr = SPM_DIS_PWR_CON;
	syss[SYS_MFG].ctl_addr = SPM_MFG_PWR_CON;
	syss[SYS_IMG].ctl_addr = SPM_ISP_PWR_CON;

	grps[CG_MIXED].set_addr = UNIVPLL_CON0;
	grps[CG_MIXED].clr_addr = UNIVPLL_CON0;
	grps[CG_MIXED].sta_addr = UNIVPLL_CON0;
	grps[CG_MPLL].set_addr = SET_MPLL_FREDIV_EN;
	grps[CG_MPLL].clr_addr = CLR_MPLL_FREDIV_EN;
	grps[CG_MPLL].sta_addr = MPLL_FREDIV_EN;
	grps[CG_UPLL].set_addr = SET_UPLL_FREDIV_EN;
	grps[CG_UPLL].clr_addr = CLR_UPLL_FREDIV_EN;
	grps[CG_UPLL].sta_addr = UPLL_FREDIV_EN;
	grps[CG_CTRL0].set_addr = SET_CLK_GATING_CTRL0;
	grps[CG_CTRL0].clr_addr = CLR_CLK_GATING_CTRL0;
	grps[CG_CTRL0].sta_addr = CLK_GATING_CTRL0;
	grps[CG_CTRL1].set_addr = SET_CLK_GATING_CTRL1;
	grps[CG_CTRL1].clr_addr = CLR_CLK_GATING_CTRL1;
	grps[CG_CTRL1].sta_addr = CLK_GATING_CTRL1;	
	grps[CG_CTRL2].set_addr = SET_CLK_GATING_CTRL2;
	grps[CG_CTRL2].clr_addr = CLR_CLK_GATING_CTRL2;
	grps[CG_CTRL2].sta_addr = CLK_GATING_CTRL2;			
	grps[CG_MMSYS0].set_addr = MMSYS_CG_SET0;
	grps[CG_MMSYS0].clr_addr = MMSYS_CG_CLR0;
	grps[CG_MMSYS0].sta_addr = MMSYS_CG_CON0;	
	grps[CG_MMSYS1].set_addr = MMSYS_CG_SET1;
	grps[CG_MMSYS1].clr_addr = MMSYS_CG_CLR1;
	grps[CG_MMSYS1].sta_addr = MMSYS_CG_CON1;
	grps[CG_IMGSYS].set_addr = IMG_CG_SET;
	grps[CG_IMGSYS].clr_addr = IMG_CG_CLR;
	grps[CG_IMGSYS].sta_addr = IMG_CG_CON;
	grps[CG_MFGSYS].set_addr = MFG_CG_SET;
	grps[CG_MFGSYS].clr_addr = MFG_CG_CLR;
	grps[CG_MFGSYS].sta_addr = MFG_CG_STA;
	grps[CG_AUDIO].set_addr = AUDIO_TOP_CON0;
	grps[CG_AUDIO].clr_addr = AUDIO_TOP_CON0;
	grps[CG_AUDIO].sta_addr = AUDIO_TOP_CON0;
	grps[CG_INFRA_AO].set_addr = INFRA_RSVD1;
	grps[CG_INFRA_AO].clr_addr = INFRA_RSVD1;
	grps[CG_INFRA_AO].sta_addr = INFRA_RSVD1;	

	muxs[MT_CLKMUX_UART0_GFMUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_EMI1X_GFMUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_AXIBUS_GFMUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_MFG_MUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_MSDC0_MUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_CAM_MUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_PWM_MM_MUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_UART1_GFMUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_MSDC1_MUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_PMICSPI_MUX_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_AUD_HF_26M_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_AUD_INTBUS_SEL].base_addr = CLK_MUX_SEL0;
	muxs[MT_CLKMUX_NFI2X_GFMUX_SEL].base_addr = CLK_MUX_SEL1;
	muxs[MT_CLKMUX_NFI1X_INFRA_SEL].base_addr = CLK_MUX_SEL1;
	muxs[MT_CLKMUX_MFG_GFMUX_SEL].base_addr = CLK_MUX_SEL1;
	muxs[MT_CLKMUX_SPI_GFMUX_SEL].base_addr = CLK_MUX_SEL1;
	muxs[MT_CLKMUX_DDRPHYCFG_GFMUX_SEL].base_addr = CLK_MUX_SEL1;
	muxs[MT_CLKMUX_SMI_GFMUX_SEL].base_addr = CLK_MUX_SEL1;
	muxs[MT_CLKMUX_USB_GFMUX_SEL].base_addr = CLK_MUX_SEL1;
	muxs[MT_CLKMUX_SCAM_MUX_SEL].base_addr = CLK_MUX_SEL1;

	plls[ARMPLL].base_addr = ARMPLL_CON0;
	plls[ARMPLL].pwr_addr = ARMPLL_PWR_CON0;
	plls[MAINPLL].base_addr = MAINPLL_CON0;
	plls[MAINPLL].pwr_addr = MAINPLL_PWR_CON0;
	plls[UNIVPLL].base_addr = UNIVPLL_CON0;
	plls[UNIVPLL].pwr_addr = UNIVPLL_PWR_CON0;
}
#endif



int mt_clkmgr_init(void)
{
	int i =0;
	ENTER_FUNC(FUNC_LV_API);
	iomap();
	BUG_ON(initialized);

// XXX: force MT_CG_SPM_SW_CG on for mt_subsys_init()
//        struct cg_clk *cg_clk = id_to_clk(MT_CG_SPM_SW_CG);
//        cg_clk->ops->enable(cg_clk);
	mt_subsys_init();
//	cg_all_force_on(); // XXX: for bring up

	mt_clks_init();
	mt_plls_init();

#if 1	//defined(CONFIG_CLKMGR_SHOWLOG)
	dump_clk_info();
#endif
	initialized = 1;
	mt_freqhopping_init();
	EXIT_FUNC(FUNC_LV_API);
	return 0;
}

#ifdef CONFIG_MTK_MMC
extern void msdc_clk_status(int *status);
#else
void msdc_clk_status(int *status)
{
    *status = 0;
}
#endif

//
// IDLE related
//
bool clkmgr_idle_can_enter(unsigned int *condition_mask, unsigned int *block_mask)
{
    int i;
    unsigned int sd_mask = 0;
    unsigned int cg_mask = 0;
    bool cg_fail = 0;

    ENTER_FUNC(FUNC_LV_API);

    msdc_clk_status(&sd_mask);

    if (sd_mask)
    {
        // block_mask[CG_PERI0] |= sd_mask; // TODO: review it latter
        EXIT_FUNC(FUNC_LV_API);
        return false;
    }

    for (i = 0; i < NR_GRPS; i++)
    {
        cg_mask = grps[i].state & condition_mask[i];

        if (cg_mask)
        {
            block_mask[i] |= cg_mask;
            EXIT_FUNC(FUNC_LV_API);
            cg_fail = 1;
        }
    }

    if(cg_fail)
        return false;

    EXIT_FUNC(FUNC_LV_API);
    return true;
}
//
// DEBUG related
//
void dump_clk_info_by_id(cg_clk_id id)
{
    struct cg_clk *clk = id_to_clk(id);

    if (NULL != clk)
    {
        pr_debug("[%d,\t%d,\t%s,\t"HEX_FMT",\t%s,\t%s,\t%s]\n",
               id,
               clk->cnt,
               (PWR_ON == clk->state) ? "ON" : "OFF",
               clk->mask,
               (clk->grp) ? clk->grp->name : "NULL",
               clk->name,
               (clk->src < NR_CLKS) ? clks[clk->src].name : "MT_CG_INVALID"
               );
    }

    return;
}

static void dump_clk_info(void)
{
    int i;

    for (i = CG_MPLL_FROM; i < NR_CLKS; i++)
    {
        dump_clk_info_by_id(i);
    }

#if 0   // XXX: dump for control table
    for (i = 0; i < NR_CLKS; i++)
    {
        struct cg_clk *clk;

        clk = &clks[i];

        pr_debug("[%s] set:%d clr:%d sta:%d mask:%x %s]\n",
                 clk->name,
                 (clk->grp->set_addr) ? 1 : 0,
                 (clk->grp->clr_addr) ? 1 : 0,
                 (clk->grp->sta_addr) ? 1 : 0,
                 (clk->mask),
                 (clk->ops->enable == general_en_cg_clk_enable_op) ? "EN" : "CG"
                 );
    }
#endif  // XXX: dump for control table
}

static int clk_test_read(struct seq_file *m, void *v)
{
	int i;
	int cnt;
	unsigned int value[2];
	const char *name;
	ENTER_FUNC(FUNC_LV_BODY);

	seq_printf(m, "********** clk register dump *********\n");
	for (i = 0; i < NR_GRPS; i++)
	{
		name = grp_get_name(i);
		//seq_printf(m, "[%d][%s] = "HEX_FMT", "HEX_FMT"\n", i, name, grps[i].ops->get_state());
		//seq_printf(m, "[%d][%s]="HEX_FMT"\n", i, name, grps[i].state);
		cnt = grp_dump_regs(i, value);

		if (cnt == 1)
		{
			seq_printf(m, "[%02d][%-8s] =\t["HEX_FMT"]\n", i, name, value[0]);
		}
		else
		{
			seq_printf(m, "[%02d][%-8s] =\t["HEX_FMT"]["HEX_FMT"]\n", i, name, value[0], value[1]);
		}
	}

	seq_printf(m, "[CLK_MUX_SEL0]="HEX_FMT"\n", clk_readl(CLK_MUX_SEL0));
	seq_printf(m, "[CLK_MUX_SEL1]="HEX_FMT"\n", clk_readl(CLK_MUX_SEL1));
	seq_printf(m, "\n********** clk_test help *********\n");
	seq_printf(m, "clkmux     : echo clkmux mux_id cg_id [mod_name] > /proc/clkmgr/clk_test\n");
	seq_printf(m, "enable  clk: echo enable  cg_id [mod_name] > /proc/clkmgr/clk_test\n");
	seq_printf(m, "disable clk: echo disable cg_id [mod_name] > /proc/clkmgr/clk_test\n");
	seq_printf(m, "dump clk   : echo dump cg_id > /proc/clkmgr/clk_test\n");
	seq_printf(m, "dump clk   : echo dump MT_CG_xxx > /proc/clkmgr/clk_test\n");
	dump_clk_info();
	EXIT_FUNC(FUNC_LV_BODY);
	return 0;
}

static int clk_test_write(struct file *file, const char *buffer,
                          unsigned long count, void *data)
{
    char desc[32];
    int len = 0;

    char cmd[10];
    char mod_name[10];
    int mux_id;
    int cg_id;

    ENTER_FUNC(FUNC_LV_BODY);

    len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);

    if (copy_from_user(desc, buffer, len))
    {
        EXIT_FUNC(FUNC_LV_BODY);
        return 0;
    }

    desc[len] = '\0';

    if  (sscanf(desc, "%9s %d %d %9s", cmd, &mux_id, &cg_id, mod_name) == 4) // XXX: 9 = sizeof(cmd) - 1, sizeof(mod_name) - 1
    {
        if (!strcmp(cmd, "clkmux"))
        {
            clkmux_sel(mux_id, cg_id, mod_name);
        }
    }
    else if (sscanf(desc, "%9s %d %9s", cmd, &cg_id, mod_name) == 3) // XXX: 9 = sizeof(cmd) - 1, sizeof(mod_name) - 1
    {
        if (!strcmp(cmd, "enable"))
        {
            enable_clock(cg_id, mod_name);
        }
        else if (!strcmp(cmd, "disable"))
        {
            disable_clock(cg_id, mod_name);
        }
    }
    else if (sscanf(desc, "%9s %d", cmd, &cg_id) == 2) // XXX: 9 = sizeof(cmd) - 1
    {
        if (!strcmp(cmd, "enable"))
        {
            enable_clock(cg_id, "pll_test");
        }
        else if (!strcmp(cmd, "disable"))
        {
            disable_clock(cg_id, "pll_test");
        }
        else if (!strcmp(cmd, "dump"))
        {
            dump_clk_info_by_id(cg_id);
        }
    }
    else if (sscanf(desc, "%9s %9s", cmd, mod_name) == 2) // XXX: 9 = sizeof(cmd) - 1, sizeof(mod_name) - 1
    {
        if (!strcmp(cmd, "dump"))
        {
            int i;

            for (i = 0; i < ARRAY_SIZE(clks); i++)
            {
                if (0 == strcmp(clks[i].name, mod_name))
                {
                    dump_clk_info_by_id(i);
                    break;
                }
            }
        }
    }

    EXIT_FUNC(FUNC_LV_BODY);
    return count;
}


static int pll_test_read(struct seq_file *m, void *v)
{
    int i, j;
    int cnt;
    unsigned int value[3];
    const char *name;

    ENTER_FUNC(FUNC_LV_BODY);

    seq_printf(m, "********** pll register dump *********\n");

    for (i = 0; i < NR_PLLS; i++)
    {
        name = pll_get_name(i);
        cnt = pll_dump_regs(i, value);

        for (j = 0; j < cnt; j++)
        {
            seq_printf(m, "[%d][%-7s reg%d]=["HEX_FMT"]\n", i, name, j, value[j]);
        }
    }

    seq_printf(m, "\n********** pll_test help *********\n");
    seq_printf(m, "enable  pll: echo enable  id [mod_name] > /proc/clkmgr/pll_test\n");
    seq_printf(m, "disable pll: echo disable id [mod_name] > /proc/clkmgr/pll_test\n");

    EXIT_FUNC(FUNC_LV_BODY);
    return 0;
}

static int pll_test_write(struct file *file, const char *buffer,
                          unsigned long count, void *data)
{
    char desc[32];
    int len = 0;

    char cmd[10];
    char mod_name[10];
    int id;

    ENTER_FUNC(FUNC_LV_BODY);

    len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);

    if (copy_from_user(desc, buffer, len))
    {
        EXIT_FUNC(FUNC_LV_BODY);
        return 0;
    }

    desc[len] = '\0';

    if (sscanf(desc, "%9s %d %9s", cmd, &id, mod_name) == 3) // XXX: 9 = sizeof(cmd) - 1, sizeof(mod_name) - 1
    {
        if (!strcmp(cmd, "enable"))
        {
            enable_pll(id, mod_name);
        }
        else if (!strcmp(cmd, "disable"))
        {
            disable_pll(id, mod_name);
        }
    }
    else if (sscanf(desc, "%9s %d", cmd, &id) == 2) // XXX: 9 = sizeof(cmd) - 1
    {
        if (!strcmp(cmd, "enable"))
        {
            enable_pll(id, "pll_test");
        }
        else if (!strcmp(cmd, "disable"))
        {
            disable_pll(id, "pll_test");
        }
    }

    EXIT_FUNC(FUNC_LV_BODY);
    return count;
}

static int pll_fsel_read(struct seq_file *m, void *v)
{
    int i;
    int cnt;
    unsigned int value[3];
    const char *name;

    ENTER_FUNC(FUNC_LV_BODY);

    for (i = 0; i < NR_PLLS; i++)
    {
        name = pll_get_name(i);

        if (pll_is_on(i))
        {
            cnt = pll_dump_regs(i, value);

            if (cnt >= 2)
            {
                seq_printf(m, "[%d][%-7s]=["HEX_FMT" "HEX_FMT"]\n", i, name, value[0], value[1]);
            }
            else
            {
                seq_printf(m, "[%d][%-7s]=["HEX_FMT"]\n", i, name, value[0]);
            }
        }
        else
        {
            seq_printf(m, "[%d][%-7s]=[-1]\n", i, name);
        }
    }

    seq_printf(m, "\n********** pll_fsel help *********\n");
    seq_printf(m, "adjust pll frequency:  echo id freq > /proc/clkmgr/pll_fsel\n");

    EXIT_FUNC(FUNC_LV_BODY);
    return 0;
}

static int pll_fsel_write(struct file *file, const char *buffer,
                          unsigned long count, void *data)
{
    char desc[32];
    int len = 0;

    int id;
    unsigned int value;

    ENTER_FUNC(FUNC_LV_BODY);

    len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);

    if (copy_from_user(desc, buffer, len))
    {
        EXIT_FUNC(FUNC_LV_BODY);
        return 0;
    }

    desc[len] = '\0';

    if (sscanf(desc, "%d %x", &id, &value) == 2)
    {
        pll_fsel(id, value);
    }

    EXIT_FUNC(FUNC_LV_BODY);
    return count;
}


static int subsys_test_read(struct seq_file *m, void *v)
{
    int i;
    int state;
    unsigned int value, sta, sta_s;
    const char *name;

    ENTER_FUNC(FUNC_LV_BODY);

//    sta = clk_readl(SPM_PWR_STATUS);
//    sta_s = clk_readl(SPM_PWR_STATUS_S);

    seq_printf(m, "********** subsys register dump *********\n");

    for (i = 0; i < NR_SYSS; i++)
    {
        name = subsys_get_name(i);
        state = subsys_is_on(i);
        subsys_dump_regs(i, &value);
        seq_printf(m, "[%d][%-7s]=["HEX_FMT"], state(%u)\n", i, name, value, state);
    }

//    seq_printf(m, "SPM_PWR_STATUS="HEX_FMT", SPM_PWR_STATUS_S="HEX_FMT"\n", sta, sta_s);

    seq_printf(m, "\n********** subsys_test help *********\n");
    seq_printf(m, "enable subsys:  echo enable id > /proc/clkmgr/subsys_test\n");
    seq_printf(m, "disable subsys: echo disable id [force_off] > /proc/clkmgr/subsys_test\n");

    EXIT_FUNC(FUNC_LV_BODY);
    return 0;
}

static int subsys_test_write(struct file *file, const char *buffer,
                             unsigned long count, void *data)
{
    char desc[32];
    int len = 0;

    char cmd[10];
    subsys_id id;
    int force_off;
    int err = 0;

    ENTER_FUNC(FUNC_LV_BODY);

    len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);

    if (copy_from_user(desc, buffer, len))
    {
        EXIT_FUNC(FUNC_LV_BODY);
        return 0;
    }

    desc[len] = '\0';

    if (sscanf(desc, "%9s %d %d", cmd, (int *)&id, &force_off) == 3) // XXX: 9 = sizeof(cmd) - 1
    {
        if (!strcmp(cmd, "disable"))
        {
            err = disable_subsys_force(id, "test");
        }
    }
    else if (sscanf(desc, "%9s %d", cmd, (int *)&id) == 2) // XXX: 9 = sizeof(cmd) - 1
    {
        if (!strcmp(cmd, "enable"))
        {
            err = enable_subsys(id, "test");
        }
        else if (!strcmp(cmd, "disable"))
        {
            err = disable_subsys(id, "test");
        }
    }

    pr_debug("[%s]%s subsys %d: result is %d\n", __func__, cmd, id, err);

    EXIT_FUNC(FUNC_LV_BODY);
    return count;
}


static int clk_force_on_read(struct seq_file *m, void *v)
{
    int i;
    struct cg_clk *clk;

    seq_printf(m, "********** clk force on info dump **********\n");
    for (i = 0; i < NR_CLKS; i++) {
        clk = &clks[i];
        if (clk->force_on) {
            seq_printf(m, "clock %d (0x%08x @ %s) is force on\n", i,
                    clk->mask, clk->grp->name);
        }
    }

    seq_printf(m, "\n********** clk_force_on help **********\n");
    seq_printf(m, "set clk force on: echo set id > /proc/clkmgr/clk_force_on\n");
    seq_printf(m, "clr clk force on: echo clr id > /proc/clkmgr/clk_force_on\n");

    return 0;
}

static ssize_t clk_force_on_write(struct file *file, const char __user *buffer,
                size_t count, loff_t *data)
{
    char desc[32];
    int len = 0;

    char cmd[10];
    int id;

    len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
    if (copy_from_user(desc, buffer, len)) {
        return 0;
    }
    desc[len] = '\0';

    if (sscanf(desc, "%9s %d", cmd, &id) == 2) {
        if (!strcmp(cmd, "set")) {
            clk_set_force_on(id);
        } else if (!strcmp(cmd, "clr")) {
            clk_clr_force_on(id);
        }
    }

    return count;
}

static int slp_chk_mtcmos_pll_stat_read(struct seq_file *m, void *v)
{
    seq_printf(m, "%d\n", slp_chk_mtcmos_pll_stat);
    return 0;
}

#if 0   // XXX: for debugfs
static ssize_t debug_read(struct file *file,
                          char __user *ubuf, size_t count, loff_t *ppos)
{
    char *page;
    char *start;
    int eof;
    int data = 0;
    int len = 0;

    if (page = (char*) __get_free_page(GFP_TEMPORARY))
    {
        len = golden_test_read(page, &start, *ppos, count, &eof, &data);
        len = simple_read_from_buffer(ubuf, count, ppos, page, len);
        free_page((unsigned long) page);
    }

    return len;
}

static ssize_t debug_open(struct inode *inode, struct file *file)
{
    file->private_data = inode->i_private;

    return 0;
}

static struct file_operations debug_fops =
{
    .read   = debug_read,
    .write  = golden_test_write,
    .open   = debug_open,
};
#endif  // XXX: for debugfs

static int proc_clk_test_open(struct inode *inode, struct file *file)
{
    return single_open(file, clk_test_read, NULL);
}
static const struct file_operations clk_test_proc_fops = { 
    .owner = THIS_MODULE,
    .open  = proc_clk_test_open,
    .read  = seq_read,
    .write = clk_test_write,
};

static int proc_pll_test_open(struct inode *inode, struct file *file)
{
    return single_open(file, pll_test_read, NULL);
}
static const struct file_operations pll_test_proc_fops = { 
    .owner = THIS_MODULE,
    .open  = proc_pll_test_open,
    .read  = seq_read,
    .write = pll_test_write,
};

static int proc_pll_fsel_open(struct inode *inode, struct file *file)
{
    return single_open(file, pll_fsel_read, NULL);
}
static const struct file_operations pll_fsel_proc_fops = { 
    .owner = THIS_MODULE,
    .open  = proc_pll_fsel_open,
    .read  = seq_read,
    .write = pll_fsel_write,
};

static int proc_subsys_test_open(struct inode *inode, struct file *file)
{
    return single_open(file, subsys_test_read, NULL);
}
static const struct file_operations subsys_test_proc_fops = { 
    .owner = THIS_MODULE,
    .open  = proc_subsys_test_open,
    .read  = seq_read,
    .write = subsys_test_write,
};

static int proc_clk_force_on_open(struct inode *inode, struct file *file)
{
    return single_open(file, clk_force_on_read, NULL);
}
static const struct file_operations clk_force_on_proc_fops = {
    .owner = THIS_MODULE,
    .open  = proc_clk_force_on_open,
    .read  = seq_read,
    .write = clk_force_on_write,
};

static int proc_slp_chk_mtcmos_pll_stat_open(struct inode *inode, struct file *file)
{
    return single_open(file, slp_chk_mtcmos_pll_stat_read, NULL);
}
static const struct file_operations slp_chk_mtcmos_pll_stat_proc_fops = { 
    .owner = THIS_MODULE,
    .open  = proc_slp_chk_mtcmos_pll_stat_open,
    .read  = seq_read,
};

void mt_clkmgr_debug_init(void)
{
    struct proc_dir_entry *entry;
    struct proc_dir_entry *clkmgr_dir;

	ENTER_FUNC(FUNC_LV_API);
	clkmgr_dir = proc_mkdir("clkmgr", NULL);
	if (!clkmgr_dir)
	{
		pr_debug("[%s]: fail to mkdir /proc/clkmgr\n", __func__);
		EXIT_FUNC(FUNC_LV_API);
		return;
	}
	entry = proc_create("clk_test", S_IRUGO | S_IWUSR, clkmgr_dir, &clk_test_proc_fops);
	entry = proc_create("pll_test", S_IRUGO | S_IWUSR, clkmgr_dir, &pll_test_proc_fops);
	entry = proc_create("pll_fsel", S_IRUGO | S_IWUSR, clkmgr_dir, &pll_fsel_proc_fops);
	entry = proc_create("subsys_test", S_IRUGO | S_IWUSR, clkmgr_dir, &subsys_test_proc_fops);
	entry = proc_create("clk_force_on", S_IRUGO | S_IWUSR, clkmgr_dir, &clk_force_on_proc_fops);
	entry = proc_create("slp_chk_mtcmos_pll_stat", S_IRUGO, clkmgr_dir, &slp_chk_mtcmos_pll_stat_proc_fops);
#if defined(CONFIG_CLKMGR_EMULATION)
    {
        #define GOLDEN_SETTING_BUF_SIZE (2 * PAGE_SIZE)

        unsigned int *buf;

        buf = kmalloc(GOLDEN_SETTING_BUF_SIZE, GFP_KERNEL);

        if (NULL != buf)
        {
            _golden_setting_init(&_golden, buf, GOLDEN_SETTING_BUF_SIZE);

            entry = proc_create("golden_test", S_IRUGO | S_IWUSR, clkmgr_dir, &golden_test_proc_fops);
        }
    }
#endif /* CONFIG_CLKMGR_EMULATION */

#if 0   // XXX: for debugfs
    {
        struct dentry *dir, *file;

        dir = debugfs_create_dir("clkmgr", NULL);
        file = debugfs_create_file("golden_test", 0644, dir, (void *)0, &debug_fops);
    }
#endif  // XXX: for debugfs

    EXIT_FUNC(FUNC_LV_API);
}

static int mt_clkmgr_debug_bringup_init(void)
{
	ENTER_FUNC(FUNC_LV_API);
	mt_clkmgr_debug_init();
	EXIT_FUNC(FUNC_LV_API);
	return 0;
}
#ifndef CONFIG_MTK_FPGA
module_init(mt_clkmgr_debug_bringup_init);
#endif
