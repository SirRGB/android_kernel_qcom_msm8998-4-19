// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/phy.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/extcon.h>
#include <linux/reset.h>

enum core_ldo_levels {
	CORE_LEVEL_NONE = 0,
	CORE_LEVEL_MIN,
	CORE_LEVEL_MAX,
};

#define INIT_MAX_TIME_USEC			1000

/* MSM8998 QSERDES v3 common status registers */
#define MSM8998_QSERDES_COM_BIAS_EN_CLKBUFLR_EN	0x034
#define MSM8998_QSERDES_COM_CLK_ENABLE1		0x038
#define MSM8998_QSERDES_COM_SYS_CLK_CTRL		0x03c
#define MSM8998_QSERDES_COM_SYSCLK_BUF_ENABLE	0x040
#define MSM8998_QSERDES_COM_PLL_IVCO		0x048
#define MSM8998_QSERDES_COM_SYSCLK_DET_COMP_STATUS	0x058
#define MSM8998_QSERDES_COM_SYSCLK_EN_SEL	0x080
#define MSM8998_QSERDES_COM_RESETSM_CNTRL	0x088
#define MSM8998_QSERDES_COM_RESETSM_CNTRL2	0x08c
#define MSM8998_QSERDES_COM_LOCK_CMP_EN		0x090
#define MSM8998_QSERDES_COM_LOCK_CMP_CFG		0x094
#define MSM8998_QSERDES_COM_CMN_STATUS		0x124
#define MSM8998_QSERDES_COM_RESET_SM_STATUS	0x128
#define MSM8998_QSERDES_COM_CLK_SELECT		0x138
#define MSM8998_QSERDES_COM_C_READY_STATUS	0x158
#define MSM8998_QSERDES_COM_CMN_CONFIG		0x15c
#define MSM8998_QSERDES_COM_SVS_MODE_CLK_SEL	0x164

/* default CORE votlage and load values */
#define USB_SSPHY_1P2_VOL_MIN		1200000 /* uV */
#define USB_SSPHY_1P2_VOL_MAX		1200000 /* uV */
#define USB_SSPHY_HPM_LOAD		30000	/* uA */

/* USB3PHY_PCIE_USB3_PCS_PCS_STATUS bit */
#define PHYSTATUS				BIT(6)

/* PCIE_USB3_PHY_AUTONOMOUS_MODE_CTRL bits */
#define ARCVR_DTCT_EN		BIT(0)
#define ALFPS_DTCT_EN		BIT(1)
#define ARCVR_DTCT_EVENT_SEL	BIT(4)

/*
 * register bits
 * PCIE_USB3_PHY_PCS_MISC_TYPEC_CTRL - for QMP USB PHY
 * USB3_DP_COM_PHY_MODE_CTRL - for QMP USB DP Combo PHY
 */

/* 0 - selects Lane A. 1 - selects Lane B */
#define SW_PORTSELECT		BIT(0)
/* port select mux: 1 - sw control. 0 - HW control*/
#define SW_PORTSELECT_MX	BIT(1)

/* USB3_DP_PHY_USB3_DP_COM_SWI_CTRL bits */

/* LANE related register read/write with USB3 */
#define USB3_SWI_ACT_ACCESS_EN	BIT(0)
/* LANE related register read/write with DP */
#define DP_SWI_ACT_ACCESS_EN	BIT(1)

/* USB3_DP_COM_RESET_OVRD_CTRL bits */

/* DP PHY soft reset */
#define SW_DPPHY_RESET		BIT(0)
/* mux to select DP PHY reset control, 0:HW control, 1: software reset */
#define SW_DPPHY_RESET_MUX	BIT(1)
/* USB3 PHY soft reset */
#define SW_USB3PHY_RESET	BIT(2)
/* mux to select USB3 PHY reset control, 0:HW control, 1: software reset */
#define SW_USB3PHY_RESET_MUX	BIT(3)

/* USB3_DP_COM_PHY_MODE_CTRL bits */
#define USB3_MODE		BIT(0) /* enables USB3 mode */
#define DP_MODE			BIT(1) /* enables DP mode */
#define USB3_DP_COMBO_MODE	(USB3_MODE | DP_MODE) /*enables combo mode */

enum qmp_phy_rev_reg {
	USB3_PHY_PCS_STATUS,
	USB3_PHY_AUTONOMOUS_MODE_CTRL,
	USB3_PHY_LFPS_RXTERM_IRQ_CLEAR,
	USB3_PHY_POWER_DOWN_CONTROL,
	USB3_PHY_SW_RESET,
	USB3_PHY_START,

	/* TypeC port select configuration (optional) */
	USB3_PHY_PCS_MISC_TYPEC_CTRL,

	/* USB DP Combo PHY related */
	USB3_DP_DP_PHY_PD_CTL,
	USB3_DP_COM_POWER_DOWN_CTRL,
	USB3_DP_COM_SW_RESET,
	USB3_DP_COM_RESET_OVRD_CTRL,
	USB3_DP_COM_PHY_MODE_CTRL,
	USB3_DP_COM_TYPEC_CTRL,
	USB3_DP_COM_SWI_CTRL,
	USB3_PCS_MISC_CLAMP_ENABLE,
	USB3_DP_PCS_PCS_STATUS2,
	USB3_DP_PCS_INSIG_SW_CTRL3,
	USB3_DP_PCS_INSIG_MX_CTRL3,
	USB3_PHY_REG_MAX,
};

/* reg values to write */
struct qmp_reg_val {
	u32 offset;
	u32 val;
	u32 delay;
};

struct msm8998_qmp_snapshot {
	u32 pcs;
	u32 sysclk;
	u32 common;
	u32 reset_sm;
	u32 c_ready;
	u32 bias;
	u32 clk_enable1;
	u32 sys_clk_ctrl;
	u32 sysclk_buf_enable;
	u32 pll_ivco;
	u32 sysclk_en_sel;
	u32 resetsm_ctrl;
	u32 resetsm_ctrl2;
	u32 lock_cmp_en;
	u32 lock_cmp_cfg;
	u32 clk_select;
	u32 cmn_config;
	u32 svs_mode_clk_sel;
	u32 power;
	u32 sw_reset;
	u32 start;
	u32 typec;
	u32 tcsr_mode;
	u32 clocks_enabled;
};

struct msm_ssphy_qmp {
	struct usb_phy		phy;
	void __iomem		*base;
	void __iomem		*vls_clamp_reg;
	void __iomem		*pcs_clamp_enable_reg;
	void __iomem		*tcsr_usb3_dp_phymode;

	struct regulator	*vdd;
	int			vdd_levels[3]; /* none, low, high */
	int			vdd_max_uA;
	struct regulator	*core_ldo;
	int			core_voltage_levels[3];
	int			core_max_uA;
	struct clk		*ref_clk_src;
	struct clk		*ref_clk;
	struct clk		*aux_clk;
	struct clk		*com_aux_clk;
	struct clk		*cfg_ahb_clk;
	struct clk		*pipe_clk;
	struct clk		*pipe_clk_mux;
	struct clk		*pipe_clk_ext_src;
	struct reset_control	*phy_reset;
	struct reset_control	*phy_phy_reset;
	struct reset_control	*global_phy_reset;
	struct extcon_dev	*extcon_dp;
	struct notifier_block	dp_nb;
	bool			power_enabled;
	bool			clk_enabled;
	bool			cable_connected;
	bool			in_suspend;
	u32			*phy_reg; /* revision based offset */
	int			reg_offset_cnt;
	u32			*qmp_phy_init_seq;
	int			init_seq_len;
	unsigned int		init_attempt;
	bool			msm8998;
};

static const bool msm8998_qmp = true;

static const struct of_device_id msm_usb_id_table[] = {
	{
		.compatible = "qcom,msm8998-usb-ssphy-qmp-v2",
		.data = &msm8998_qmp,
	},
	{
		.compatible = "qcom,usb-ssphy-qmp",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-v1",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-v2",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-dp-combo",
	},
	{
		.compatible = "qcom,usb-ssphy-qmp-usb3-or-dp",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, msm_usb_id_table);

static void usb_qmp_powerup_phy(struct msm_ssphy_qmp *phy);
static void msm_ssphy_qmp_enable_clks(struct msm_ssphy_qmp *phy, bool on);
static int msm_ssphy_qmp_reset(struct usb_phy *uphy);

static bool msm8998_qmp_clock_enabled(struct clk *clk)
{
	return clk && __clk_is_enabled(clk);
}

static void msm8998_qmp_snapshot(struct msm_ssphy_qmp *phy,
				 unsigned int attempt, const char *stage)
{
	struct msm8998_qmp_snapshot snapshot;

	snapshot.pcs = readl_relaxed(phy->base +
			phy->phy_reg[USB3_PHY_PCS_STATUS]);
	snapshot.sysclk = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_SYSCLK_DET_COMP_STATUS);
	snapshot.common = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_CMN_STATUS);
	snapshot.reset_sm = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_RESET_SM_STATUS);
	snapshot.c_ready = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_C_READY_STATUS);
	snapshot.bias = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_BIAS_EN_CLKBUFLR_EN);
	snapshot.clk_enable1 = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_CLK_ENABLE1);
	snapshot.sys_clk_ctrl = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_SYS_CLK_CTRL);
	snapshot.sysclk_buf_enable = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_SYSCLK_BUF_ENABLE);
	snapshot.pll_ivco = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_PLL_IVCO);
	snapshot.sysclk_en_sel = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_SYSCLK_EN_SEL);
	snapshot.resetsm_ctrl = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_RESETSM_CNTRL);
	snapshot.resetsm_ctrl2 = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_RESETSM_CNTRL2);
	snapshot.lock_cmp_en = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_LOCK_CMP_EN);
	snapshot.lock_cmp_cfg = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_LOCK_CMP_CFG);
	snapshot.clk_select = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_CLK_SELECT);
	snapshot.cmn_config = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_CMN_CONFIG);
	snapshot.svs_mode_clk_sel = readl_relaxed(phy->base +
			MSM8998_QSERDES_COM_SVS_MODE_CLK_SEL);
	snapshot.power = readl_relaxed(phy->base +
			phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
	snapshot.sw_reset = readl_relaxed(phy->base +
			phy->phy_reg[USB3_PHY_SW_RESET]);
	snapshot.start = readl_relaxed(phy->base +
			phy->phy_reg[USB3_PHY_START]);
	snapshot.typec = readl_relaxed(phy->base +
			phy->phy_reg[USB3_PHY_PCS_MISC_TYPEC_CTRL]);
	snapshot.tcsr_mode = phy->tcsr_usb3_dp_phymode ?
		readl_relaxed(phy->tcsr_usb3_dp_phymode) : U32_MAX;
	snapshot.clocks_enabled = msm8998_qmp_clock_enabled(phy->ref_clk_src) |
		(msm8998_qmp_clock_enabled(phy->ref_clk) << 1) |
		(msm8998_qmp_clock_enabled(phy->aux_clk) << 2) |
		(msm8998_qmp_clock_enabled(phy->cfg_ahb_clk) << 3) |
		(msm8998_qmp_clock_enabled(phy->pipe_clk) << 4);

	dev_info(phy->phy.dev,
		 "QMP %u %s status: pcs=%08x sys=%08x cmn=%08x rst=%08x rdy=%08x\n",
		 attempt, stage, snapshot.pcs, snapshot.sysclk, snapshot.common,
		 snapshot.reset_sm, snapshot.c_ready);
	dev_info(phy->phy.dev,
		 "QMP %u %s pll: bias=%08x sysctl=%08x ivco=%08x rsm=%08x lock=%08x/%08x sel=%08x\n",
		 attempt, stage, snapshot.bias, snapshot.sys_clk_ctrl,
		 snapshot.pll_ivco, snapshot.resetsm_ctrl,
		 snapshot.lock_cmp_en, snapshot.lock_cmp_cfg,
		 snapshot.clk_select);
	dev_info(phy->phy.dev,
		 "QMP %u %s inputs: en1=%08x buf=%08x ensel=%08x rsm2=%08x cfg=%08x svs=%08x clks=%02x tcsr=%08x\n",
		 attempt, stage, snapshot.clk_enable1,
		 snapshot.sysclk_buf_enable, snapshot.sysclk_en_sel,
		 snapshot.resetsm_ctrl2, snapshot.cmn_config,
		 snapshot.svs_mode_clk_sel, snapshot.clocks_enabled,
		 snapshot.tcsr_mode);
	dev_info(phy->phy.dev,
		 "QMP %u %s ctl: pwr=%08x rst=%08x start=%08x typec=%08x\n",
		 attempt, stage, snapshot.power, snapshot.sw_reset,
		 snapshot.start, snapshot.typec);
}

static bool msm_ssphy_qmp_start(struct msm_ssphy_qmp *phy,
				unsigned int attempt)
{
	unsigned int i;
	u32 status;

	writel_relaxed(0x00, phy->base +
			phy->phy_reg[USB3_PHY_SW_RESET]);
	if (phy->msm8998) {
		/* Make the reset release visible before capturing its state. */
		mb();
		msm8998_qmp_snapshot(phy, attempt, "post-reset");
	}
	writel_relaxed(0x03, phy->base +
			phy->phy_reg[USB3_PHY_START]);

	/* Make sure the release sequence completed before polling the PHY. */
	mb();
	if (phy->msm8998)
		msm8998_qmp_snapshot(phy, attempt, "post-start");

	for (i = 0; i < INIT_MAX_TIME_USEC; i++) {
		if (phy->msm8998 && i == 10)
			msm8998_qmp_snapshot(phy, attempt, "poll-10");
		if (phy->msm8998 && i == 100)
			msm8998_qmp_snapshot(phy, attempt, "poll-100");

		status = readl_relaxed(phy->base +
				phy->phy_reg[USB3_PHY_PCS_STATUS]);
		if (!(status & PHYSTATUS))
			break;
		usleep_range(1, 2);
	}

	if (phy->msm8998)
		msm8998_qmp_snapshot(phy, attempt, "final");

	return i < INIT_MAX_TIME_USEC;
}

static inline char *get_cable_status_str(struct msm_ssphy_qmp *phy)
{
	return phy->cable_connected ? "connected" : "disconnected";
}

static void msm_ssusb_qmp_clr_lfps_rxterm_int(struct msm_ssphy_qmp *phy)
{
	writel_relaxed(1, phy->base +
			phy->phy_reg[USB3_PHY_LFPS_RXTERM_IRQ_CLEAR]);
	/* flush the previous write before next write */
	wmb();
	writel_relaxed(0, phy->base +
			phy->phy_reg[USB3_PHY_LFPS_RXTERM_IRQ_CLEAR]);
}

static void msm_ssusb_qmp_clamp_enable(struct msm_ssphy_qmp *phy, bool val)
{
	switch (phy->phy.type) {
	case USB_PHY_TYPE_USB3_AND_DP:
		writel_relaxed(!val, phy->base +
			phy->phy_reg[USB3_PCS_MISC_CLAMP_ENABLE]);
		break;
	case USB_PHY_TYPE_USB3_OR_DP:
	case USB_PHY_TYPE_USB3:
		if (phy->vls_clamp_reg)
			writel_relaxed(!!val, phy->vls_clamp_reg);
		if (phy->pcs_clamp_enable_reg)
			writel_relaxed(!val, phy->pcs_clamp_enable_reg);
		break;
	default:
		break;
	}
}

static void msm_ssusb_qmp_enable_autonomous(struct msm_ssphy_qmp *phy,
		int enable)
{
	u32 val;
	unsigned int autonomous_mode_offset =
			phy->phy_reg[USB3_PHY_AUTONOMOUS_MODE_CTRL];

	dev_dbg(phy->phy.dev, "enabling QMP autonomous mode with cable %s\n",
			get_cable_status_str(phy));

	if (enable) {
		msm_ssusb_qmp_clr_lfps_rxterm_int(phy);
		val = readl_relaxed(phy->base + autonomous_mode_offset);
		val |= ARCVR_DTCT_EN;
		if (phy->phy.flags & DEVICE_IN_SS_MODE) {
			val |= ALFPS_DTCT_EN;
			val &= ~ARCVR_DTCT_EVENT_SEL;
		} else {
			val &= ~ALFPS_DTCT_EN;
			val |= ARCVR_DTCT_EVENT_SEL;
		}
		writel_relaxed(val, phy->base + autonomous_mode_offset);
		msm_ssusb_qmp_clamp_enable(phy, true);
	} else {
		msm_ssusb_qmp_clamp_enable(phy, false);
		writel_relaxed(0, phy->base + autonomous_mode_offset);
		msm_ssusb_qmp_clr_lfps_rxterm_int(phy);
	}
}

static int msm_ssusb_qmp_ldo_enable(struct msm_ssphy_qmp *phy, int on)
{
	int min, rc = 0;

	dev_dbg(phy->phy.dev, "reg (%s)\n", on ? "HPM" : "LPM");

	if (phy->power_enabled == on) {
		dev_dbg(phy->phy.dev, "PHYs' regulators status %d\n",
			phy->power_enabled);
		return 0;
	}

	phy->power_enabled = on;

	min = on ? 1 : 0; /* low or none? */

	if (!on)
		goto disable_regulators;

	rc = regulator_set_load(phy->vdd, phy->vdd_max_uA);
	if (rc < 0) {
		dev_err(phy->phy.dev, "Unable to set HPM of %s\n", "vdd");
		return rc;
	}

	rc = regulator_set_voltage(phy->vdd, phy->vdd_levels[min],
				    phy->vdd_levels[2]);
	if (rc) {
		dev_err(phy->phy.dev, "Unable to set voltage for %s\n", "vdd");
		goto put_vdd_lpm;
	}

	dev_dbg(phy->phy.dev, "min_vol:%d max_vol:%d\n",
		phy->vdd_levels[min], phy->vdd_levels[2]);

	rc = regulator_enable(phy->vdd);
	if (rc) {
		dev_err(phy->phy.dev, "Unable to enable %s\n", "vdd");
		goto unconfig_vdd;
	}

	rc = regulator_set_load(phy->core_ldo, phy->core_max_uA);
	if (rc < 0) {
		dev_err(phy->phy.dev, "Unable to set HPM of %s\n", "core_ldo");
		goto disable_vdd;
	}

	rc = regulator_set_voltage(phy->core_ldo,
			phy->core_voltage_levels[CORE_LEVEL_MIN],
			phy->core_voltage_levels[CORE_LEVEL_MAX]);
	if (rc) {
		dev_err(phy->phy.dev, "Unable to set voltage for %s\n",
				"core_ldo");
		goto put_core_ldo_lpm;
	}

	rc = regulator_enable(phy->core_ldo);
	if (rc) {
		dev_err(phy->phy.dev, "Unable to enable %s\n", "core_ldo");
		goto unset_core_ldo;
	}

	return 0;

disable_regulators:
	rc = regulator_disable(phy->core_ldo);
	if (rc)
		dev_err(phy->phy.dev, "Unable to disable %s\n", "core_ldo");

unset_core_ldo:
	rc = regulator_set_voltage(phy->core_ldo,
			phy->core_voltage_levels[CORE_LEVEL_NONE],
			phy->core_voltage_levels[CORE_LEVEL_MAX]);
	if (rc)
		dev_err(phy->phy.dev, "Unable to set voltage for %s\n",
				"core_ldo");

put_core_ldo_lpm:
	rc = regulator_set_load(phy->core_ldo, 0);
	if (rc < 0)
		dev_err(phy->phy.dev, "Unable to set LPM of %s\n", "core_ldo");

disable_vdd:
	rc = regulator_disable(phy->vdd);
	if (rc)
		dev_err(phy->phy.dev, "Unable to disable %s\n", "vdd");

unconfig_vdd:
	rc = regulator_set_voltage(phy->vdd, phy->vdd_levels[min],
				    phy->vdd_levels[2]);
	if (rc)
		dev_err(phy->phy.dev, "Unable to set voltage for %s\n", "vdd");

put_vdd_lpm:
	rc = regulator_set_load(phy->vdd, 0);
	if (rc < 0)
		dev_err(phy->phy.dev, "Unable to set LPM of %s\n", "vdd");

	return rc < 0 ? rc : 0;
}

static int configure_phy_regs(struct usb_phy *uphy,
				const struct qmp_reg_val *reg)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);

	if (!reg) {
		dev_err(uphy->dev, "NULL PHY configuration\n");
		return -EINVAL;
	}

	while (reg->offset != -1) {
		writel_relaxed(reg->val, phy->base + reg->offset);
		if (reg->delay)
			usleep_range(reg->delay, reg->delay + 10);
		reg++;
	}
	return 0;
}

static void msm_ssphy_qmp_setmode(struct msm_ssphy_qmp *phy, u32 mode)
{
	mode = mode & USB3_DP_COMBO_MODE;

	writel_relaxed(mode,
			phy->base + phy->phy_reg[USB3_DP_COM_PHY_MODE_CTRL]);

	/* flush the write by reading it */
	readl_relaxed(phy->base + phy->phy_reg[USB3_DP_COM_PHY_MODE_CTRL]);
}


static void usb_qmp_update_portselect_phymode(struct msm_ssphy_qmp *phy)
{
	int val;

	/* perform lane selection */
	val = -EINVAL;
	if (phy->phy.flags & PHY_LANE_A)
		val = SW_PORTSELECT_MX;
	else if (phy->phy.flags & PHY_LANE_B)
		val = SW_PORTSELECT | SW_PORTSELECT_MX;

	/* PHY must be powered up before updating portselect and phymode. */
	usb_qmp_powerup_phy(phy);

	switch (phy->phy.type) {
	case USB_PHY_TYPE_USB3_AND_DP:
		/* override hardware control for reset of qmp phy */
		if (!(phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE))
			writel_relaxed(SW_DPPHY_RESET_MUX | SW_DPPHY_RESET |
				SW_USB3PHY_RESET_MUX | SW_USB3PHY_RESET,
				phy->base +
				phy->phy_reg[USB3_DP_COM_RESET_OVRD_CTRL]);

		/* update port select */
		if (val > 0) {
			dev_err(phy->phy.dev,
				"USB DP QMP PHY: Update TYPEC CTRL(%d)\n", val);
			writel_relaxed(val, phy->base +
				phy->phy_reg[USB3_DP_COM_TYPEC_CTRL]);
		}

		if (!(phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE)) {
			msm_ssphy_qmp_setmode(phy, USB3_DP_COMBO_MODE);

			/* bring both USB and DP PHYs PCS block out of reset */
			writel_relaxed(0x00, phy->base +
				phy->phy_reg[USB3_DP_COM_RESET_OVRD_CTRL]);
		}
		break;
	case  USB_PHY_TYPE_USB3_OR_DP:
		if (val > 0) {
			dev_err(phy->phy.dev,
				"USB QMP PHY: Update TYPEC CTRL(%d)\n", val);
			writel_relaxed(val, phy->base +
				phy->phy_reg[USB3_PHY_PCS_MISC_TYPEC_CTRL]);
		}
		break;
	default:
		dev_dbg(phy->phy.dev, "no portselect for phy type %d\n",
					phy->phy.type);
		break;
	}

	/* Make sure above selection and reset sequence is gone through */
	mb();
}

static void usb_qmp_powerup_phy(struct msm_ssphy_qmp *phy)
{
	switch (phy->phy.type) {
	case USB_PHY_TYPE_USB3_AND_DP:
		/* power up USB3 and DP common logic block */
		writel_relaxed(0x01,
			phy->base + phy->phy_reg[USB3_DP_COM_POWER_DOWN_CTRL]);

		/*
		 * Don't write 0x0 to DP_COM_SW_RESET here as portselect and
		 * phymode operation needs DP_COM_SW_RESET as 0x1.
		 * msm_ssphy_qmp_init() writes 0x0 to DP_COM_SW_RESET before
		 * initializing PHY.
		 */

		/* intentional fall-through */
	case USB_PHY_TYPE_USB3_OR_DP:
	case USB_PHY_TYPE_USB3:
		/* power up USB3 PHY */
		writel_relaxed(0x01,
			phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
		break;
	default:
		dev_err(phy->phy.dev, "phy_powerup: Unknown USB QMP PHY type\n");
		break;
	}

	/* Make sure that above write completed to power up PHY */
	mb();
}

/* SSPHY Initialization */
static int msm_ssphy_qmp_init(struct usb_phy *uphy)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int ret;
	unsigned int attempt = ++phy->init_attempt;
	const struct qmp_reg_val *reg = NULL;
	bool diagnose = phy->msm8998;

	dev_dbg(uphy->dev, "Initializing QMP phy\n");

	ret = msm_ssusb_qmp_ldo_enable(phy, 1);
	if (ret) {
		dev_err(phy->phy.dev,
		"msm_ssusb_qmp_ldo_enable(1) failed, ret=%d\n",
		ret);
		return ret;
	}

	msm_ssphy_qmp_enable_clks(phy, true);
	if (diagnose)
		dev_info(uphy->dev,
			 "QMP init %u regulators: vdd=%d/%d core=%d/%d\n",
			 attempt, regulator_is_enabled(phy->vdd),
			 regulator_get_voltage(phy->vdd),
			 regulator_is_enabled(phy->core_ldo),
			 regulator_get_voltage(phy->core_ldo));

	/* select appropriate port select and PHY mode if applicable */
	usb_qmp_update_portselect_phymode(phy);

	/* power up PHY */
	usb_qmp_powerup_phy(phy);
	if (diagnose)
		dev_info(uphy->dev,
			 "QMP init %u power: ctrl=%#x status=%#x\n", attempt,
			 readl_relaxed(phy->base +
				phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]),
			 readl_relaxed(phy->base +
				phy->phy_reg[USB3_PHY_PCS_STATUS]));

	reg = (struct qmp_reg_val *)phy->qmp_phy_init_seq;
	if (diagnose)
		msm8998_qmp_snapshot(phy, attempt, "pre-table");

	/* Main configuration */
	ret = configure_phy_regs(uphy, reg);
	if (ret) {
		dev_err(uphy->dev, "Failed the main PHY configuration\n");
		goto fail;
	}
	if (diagnose)
		msm8998_qmp_snapshot(phy, attempt, "post-table");

	/* perform software reset of PHY common logic */
	if (phy->phy.type == USB_PHY_TYPE_USB3_AND_DP &&
				!(phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE))
		writel_relaxed(0x00,
			phy->base + phy->phy_reg[USB3_DP_COM_SW_RESET]);

	if (msm_ssphy_qmp_start(phy, attempt))
		return 0;
	dev_err(uphy->dev, "QMP PHY initialization timeout\n");
	dev_err(uphy->dev, "USB3_PHY_PCS_STATUS:%x\n",
		readl_relaxed(phy->base +
			      phy->phy_reg[USB3_PHY_PCS_STATUS]));
	ret = -EBUSY;
	goto fail;

fail:
	phy->in_suspend = true;
	writel_relaxed(0x00,
		phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
	msm_ssphy_qmp_enable_clks(phy, false);
	msm_ssusb_qmp_ldo_enable(phy, 0);

	return ret;
}

static int msm_ssphy_qmp_dp_combo_reset(struct usb_phy *uphy)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int ret = 0;

	if (phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE) {
		dev_dbg(uphy->dev, "Resetting USB part of QMP phy\n");

		/* Assert USB3 PHY CSR reset */
		ret = reset_control_assert(phy->phy_reset);
		if (ret) {
			dev_err(uphy->dev, "phy_reset assert failed\n");
			goto exit;
		}

		/* Deassert USB3 PHY CSR reset */
		ret = reset_control_deassert(phy->phy_reset);
		if (ret) {
			dev_err(uphy->dev, "phy_reset deassert failed\n");
			goto exit;
		}
		return 0;
	}

	dev_dbg(uphy->dev, "Global reset of QMP DP combo phy\n");
	/* Assert global PHY reset */
	ret = reset_control_assert(phy->global_phy_reset);
	if (ret) {
		dev_err(uphy->dev, "global_phy_reset assert failed\n");
		goto exit;
	}

	/* Assert QMP USB PHY reset */
	ret = reset_control_assert(phy->phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_reset assert failed\n");
		goto exit;
	}

	/* De-Assert QMP USB PHY reset */
	ret = reset_control_deassert(phy->phy_reset);
	if (ret)
		dev_err(uphy->dev, "phy_reset deassert failed\n");

	/* De-Assert global PHY reset */
	ret = reset_control_deassert(phy->global_phy_reset);
	if (ret)
		dev_err(uphy->dev, "global_phy_reset deassert failed\n");

exit:
	return ret;
}

static int msm_ssphy_qmp_reset(struct usb_phy *uphy)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);
	int ret;

	dev_dbg(uphy->dev, "Resetting QMP phy\n");

	/* Assert USB3 PHY reset */
	ret = reset_control_assert(phy->phy_phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_phy_reset assert failed\n");
		goto exit;
	}

	/* Assert USB3 PHY CSR reset */
	ret = reset_control_assert(phy->phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_reset assert failed\n");
		goto deassert_phy_phy_reset;
	}

	/* select usb3 phy mode */
	if (phy->tcsr_usb3_dp_phymode)
		writel_relaxed(0x0, phy->tcsr_usb3_dp_phymode);

	/* Deassert USB3 PHY CSR reset */
	ret = reset_control_deassert(phy->phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_reset deassert failed\n");
		goto deassert_phy_phy_reset;
	}

	/* Deassert USB3 PHY reset */
	ret = reset_control_deassert(phy->phy_phy_reset);
	if (ret) {
		dev_err(uphy->dev, "phy_phy_reset deassert failed\n");
		goto exit;
	}
	if (phy->msm8998)
		dev_info(uphy->dev,
			 "QMP reset: phy_phy and phy assert/deassert succeeded\n");

	return 0;

deassert_phy_phy_reset:
	ret = reset_control_deassert(phy->phy_phy_reset);
	if (ret)
		dev_err(uphy->dev, "phy_phy_reset deassert failed\n");
exit:
	phy->in_suspend = false;

	return ret;
}

static int msm_ssphy_power_enable(struct msm_ssphy_qmp *phy, bool on)
{
	bool host = phy->phy.flags & PHY_HOST_MODE;
	int ret = 0;

	/*
	 * Turn off the phy's LDOs when cable is disconnected for device mode
	 * with external vbus_id indication.
	 */
	if (!host && !phy->cable_connected) {
		if (on) {
			ret = msm_ssusb_qmp_ldo_enable(phy, 1);
			if (ret)
				dev_err(phy->phy.dev,
				"msm_ssusb_qmp_ldo_enable(1) failed, ret=%d\n",
				ret);
		} else {
			ret = msm_ssusb_qmp_ldo_enable(phy, 0);
			if (ret)
				dev_err(phy->phy.dev,
					"msm_ssusb_qmp_ldo_enable(0) failed, ret=%d\n",
					ret);
		}
	}

	return ret;
}

/**
 * Performs QMP PHY suspend/resume functionality.
 *
 * @uphy - usb phy pointer.
 * @suspend - to enable suspend or not. 1 - suspend, 0 - resume
 *
 */
static int msm_ssphy_qmp_set_suspend(struct usb_phy *uphy, int suspend)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);

	dev_dbg(uphy->dev, "QMP PHY set_suspend for %s called with cable %s\n",
			(suspend ? "suspend" : "resume"),
			get_cable_status_str(phy));

	if (phy->in_suspend == suspend) {
		dev_dbg(uphy->dev, "%s: USB PHY is already %s.\n",
			__func__, (suspend ? "suspended" : "resumed"));
		return 0;
	}

	if (suspend) {
		if (phy->cable_connected) {
			msm_ssusb_qmp_enable_autonomous(phy, 1);
		} else {
			/* Reset phy mode to USB only if DP not connected */
			if (uphy->type  == USB_PHY_TYPE_USB3_AND_DP &&
				!(phy->phy.flags & PHY_USB_DP_CONCURRENT_MODE))
				msm_ssphy_qmp_setmode(phy, USB3_MODE);
			writel_relaxed(0x00,
			phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
		}

		/* Make sure above write completed with PHY */
		wmb();

		msm_ssphy_qmp_enable_clks(phy, false);
		phy->in_suspend = true;
		msm_ssphy_power_enable(phy, 0);
		dev_dbg(uphy->dev, "QMP PHY is suspend\n");
	} else {
		msm_ssphy_power_enable(phy, 1);
		msm_ssphy_qmp_enable_clks(phy, true);
		if (!phy->cable_connected) {
			writel_relaxed(0x01,
			phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
		} else  {
			msm_ssusb_qmp_enable_autonomous(phy, 0);
		}

		/* Make sure that above write completed with PHY */
		wmb();

		phy->in_suspend = false;
		dev_dbg(uphy->dev, "QMP PHY is resumed\n");
	}

	return 0;
}

static int msm_ssphy_qmp_notify_connect(struct usb_phy *uphy,
				       enum usb_device_speed speed)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);

	dev_dbg(uphy->dev, "QMP phy connect notification\n");
	phy->cable_connected = true;
	dev_dbg(uphy->dev, "cable_connected=%d\n", phy->cable_connected);
	return 0;
}

static int msm_ssphy_qmp_notify_disconnect(struct usb_phy *uphy,
				       enum usb_device_speed speed)
{
	struct msm_ssphy_qmp *phy = container_of(uphy, struct msm_ssphy_qmp,
					phy);

	writel_relaxed(0x00,
		phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);
	readl_relaxed(phy->base + phy->phy_reg[USB3_PHY_POWER_DOWN_CONTROL]);

	dev_dbg(uphy->dev, "QMP phy disconnect notification\n");
	dev_dbg(uphy->dev, " cable_connected=%d\n", phy->cable_connected);
	phy->cable_connected = false;
	return 0;
}

static int msm_ssphy_qmp_vbus_notifier(struct notifier_block *nb,
		unsigned long event, void *ptr)
{
	return 0;
}

static int msm_ssphy_qmp_dp_notifier(struct notifier_block *nb,
		unsigned long dp_lane, void *ptr)
{
	struct msm_ssphy_qmp *phy = container_of(nb,
			struct msm_ssphy_qmp, dp_nb);

	if (dp_lane == 2 || dp_lane == 4)
		phy->phy.flags |= PHY_USB_DP_CONCURRENT_MODE;
	else
		phy->phy.flags &= ~PHY_USB_DP_CONCURRENT_MODE;

	return 0;

}

static int msm_ssphy_qmp_extcon_register(struct msm_ssphy_qmp *phy,
				struct device *dev)
{
	struct device_node *node = dev->of_node;
	struct extcon_dev *edev;
	int ret = 0;

	if (!of_property_read_bool(node, "extcon"))
		return 0;

	edev = extcon_get_edev_by_phandle(dev, 0);
	if (IS_ERR(edev)) {
		dev_err(dev, "failed to get phandle for msm_ssphy_qmp\n");
		return PTR_ERR(edev);
	}

	phy->extcon_dp = edev;
	phy->phy.vbus_nb.notifier_call = msm_ssphy_qmp_vbus_notifier;
	phy->dp_nb.notifier_call = msm_ssphy_qmp_dp_notifier;
	ret = extcon_register_blocking_notifier(edev, EXTCON_DISP_DP,
								&phy->dp_nb);
	if (ret < 0) {
		dev_err(dev, "failed to register blocking notifier\n");
		return ret;
	}

	return 0;
}

static int msm_ssphy_qmp_get_clks(struct msm_ssphy_qmp *phy, struct device *dev)
{
	int ret = 0;

	phy->aux_clk = devm_clk_get(dev, "aux_clk");
	if (IS_ERR(phy->aux_clk)) {
		ret = PTR_ERR(phy->aux_clk);
		phy->aux_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get aux_clk\n");
		goto err;
	}
	clk_set_rate(phy->aux_clk, clk_round_rate(phy->aux_clk, ULONG_MAX));

	if (of_property_match_string(dev->of_node,
			"clock-names", "cfg_ahb_clk") >= 0) {
		phy->cfg_ahb_clk = devm_clk_get(dev, "cfg_ahb_clk");
		if (IS_ERR(phy->cfg_ahb_clk)) {
			ret = PTR_ERR(phy->cfg_ahb_clk);
			if (ret != -EPROBE_DEFER)
				dev_err(dev,
				"failed to get cfg_ahb_clk ret %d\n", ret);
			goto err;
		}
	}

	phy->pipe_clk = devm_clk_get(dev, "pipe_clk");
	if (IS_ERR(phy->pipe_clk)) {
		ret = PTR_ERR(phy->pipe_clk);
		phy->pipe_clk = NULL;
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to get pipe_clk\n");
		goto err;
	}

	phy->pipe_clk_mux = devm_clk_get(dev, "pipe_clk_mux");
	if (IS_ERR(phy->pipe_clk_mux))
		phy->pipe_clk_mux = NULL;

	phy->pipe_clk_ext_src = devm_clk_get(dev, "pipe_clk_ext_src");
	if (IS_ERR(phy->pipe_clk_ext_src))
		phy->pipe_clk_ext_src = NULL;

	phy->ref_clk_src = devm_clk_get(dev, "ref_clk_src");
	if (IS_ERR(phy->ref_clk_src))
		phy->ref_clk_src = NULL;

	phy->ref_clk = devm_clk_get(dev, "ref_clk");
	if (IS_ERR(phy->ref_clk))
		phy->ref_clk = NULL;

	if (of_property_match_string(dev->of_node,
			"clock-names", "com_aux_clk") >= 0) {
		phy->com_aux_clk = devm_clk_get(dev, "com_aux_clk");
		if (IS_ERR(phy->com_aux_clk)) {
			ret = PTR_ERR(phy->com_aux_clk);
			if (ret != -EPROBE_DEFER)
				dev_err(dev,
				"failed to get com_aux_clk ret %d\n", ret);
			goto err;
		}
	}

err:
	return ret;
}

static void msm_ssphy_qmp_enable_clks(struct msm_ssphy_qmp *phy, bool on)
{
	int aux_ret = 0, cfg_ret = 0, pipe_ret = 0;
	int ref_src_ret = 0, ref_ret = 0, com_aux_ret = 0, parent_ret = 0;
	bool diagnose = phy->msm8998;

	dev_dbg(phy->phy.dev, "%s(): clk_enabled:%d on:%d\n", __func__,
					phy->clk_enabled, on);

	if (!phy->clk_enabled && on) {
		if (phy->ref_clk_src)
			ref_src_ret = clk_prepare_enable(phy->ref_clk_src);

		if (phy->ref_clk)
			ref_ret = clk_prepare_enable(phy->ref_clk);

		if (phy->com_aux_clk)
			com_aux_ret = clk_prepare_enable(phy->com_aux_clk);

		aux_ret = clk_prepare_enable(phy->aux_clk);
		if (phy->cfg_ahb_clk)
			cfg_ret = clk_prepare_enable(phy->cfg_ahb_clk);

		//select PHY pipe clock
		parent_ret = clk_set_parent(phy->pipe_clk_mux,
					    phy->pipe_clk_ext_src);
		clk_set_rate(phy->pipe_clk, 125000000);
		pipe_ret = clk_prepare_enable(phy->pipe_clk);
		if (diagnose)
			dev_info(phy->phy.dev,
				 "QMP clocks: aux=%d/%lu pipe=%d/%lu "
				 "cfg=%d/%lu refsrc=%d/%lu ref=%d/%lu "
				 "comaux=%d/%lu mux=%d/%d/%d\n",
				 aux_ret, clk_get_rate(phy->aux_clk), pipe_ret,
				 clk_get_rate(phy->pipe_clk), cfg_ret,
				 phy->cfg_ahb_clk ?
					clk_get_rate(phy->cfg_ahb_clk) : 0UL,
				 ref_src_ret, phy->ref_clk_src ?
					clk_get_rate(phy->ref_clk_src) : 0UL,
				 ref_ret, phy->ref_clk ?
					clk_get_rate(phy->ref_clk) : 0UL,
				 com_aux_ret, phy->com_aux_clk ?
					clk_get_rate(phy->com_aux_clk) : 0UL,
				 !!phy->pipe_clk_mux, !!phy->pipe_clk_ext_src,
				 parent_ret);
		phy->clk_enabled = true;
	}

	if (phy->clk_enabled && !on) {
		clk_disable_unprepare(phy->pipe_clk);

		//select XO instead of PHY pipe clock
		clk_set_parent(phy->pipe_clk_mux, phy->ref_clk_src);

		if (phy->cfg_ahb_clk)
			clk_disable_unprepare(phy->cfg_ahb_clk);

		clk_disable_unprepare(phy->aux_clk);
		if (phy->com_aux_clk)
			clk_disable_unprepare(phy->com_aux_clk);

		if (phy->ref_clk)
			clk_disable_unprepare(phy->ref_clk);

		if (phy->ref_clk_src)
			clk_disable_unprepare(phy->ref_clk_src);

		phy->clk_enabled = false;
	}
}

static int msm_ssphy_qmp_probe(struct platform_device *pdev)
{
	struct msm_ssphy_qmp *phy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret = 0, size = 0, len;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;
	phy->msm8998 = !!of_device_get_match_data(dev);

	phy->phy.type = USB_PHY_TYPE_USB3;
	if (of_device_is_compatible(dev->of_node,
			"qcom,usb-ssphy-qmp-dp-combo"))
		phy->phy.type = USB_PHY_TYPE_USB3_AND_DP;

	if (of_device_is_compatible(dev->of_node,
			"qcom,usb-ssphy-qmp-usb3-or-dp"))
		phy->phy.type = USB_PHY_TYPE_USB3_OR_DP;

	ret = msm_ssphy_qmp_get_clks(phy, dev);
	if (ret)
		goto err;

	phy->phy_reset = devm_reset_control_get(dev, "phy_reset");
	if (IS_ERR(phy->phy_reset)) {
		ret = PTR_ERR(phy->phy_reset);
		dev_dbg(dev, "failed to get phy_reset\n");
		goto err;
	}

	if (phy->phy.type == USB_PHY_TYPE_USB3_AND_DP) {
		phy->global_phy_reset = devm_reset_control_get(dev,
						"global_phy_reset");
		if (IS_ERR(phy->global_phy_reset)) {
			ret = PTR_ERR(phy->global_phy_reset);
			dev_dbg(dev, "failed to get global_phy_reset\n");
			goto err;
		}
	} else {
		phy->phy_phy_reset = devm_reset_control_get(dev,
						"phy_phy_reset");
		if (IS_ERR(phy->phy_phy_reset)) {
			ret = PTR_ERR(phy->phy_phy_reset);
			dev_dbg(dev, "failed to get phy_phy_reset\n");
			goto err;
		}
	}

	of_get_property(dev->of_node, "qcom,qmp-phy-reg-offset", &size);
	if (size) {
		phy->phy_reg = devm_kzalloc(dev, size, GFP_KERNEL);
		if (phy->phy_reg) {
			phy->reg_offset_cnt = (size / sizeof(*phy->phy_reg));
			if (phy->reg_offset_cnt > USB3_PHY_REG_MAX) {
				dev_err(dev, "invalid reg offset count\n");
				return -EINVAL;
			}

			of_property_read_u32_array(dev->of_node,
				"qcom,qmp-phy-reg-offset",
				phy->phy_reg, phy->reg_offset_cnt);
		} else {
			dev_err(dev, "err mem alloc for qmp_phy_reg_offset\n");
			return -ENOMEM;
		}
	} else {
		dev_err(dev, "err provide qcom,qmp-phy-reg-offset\n");
		return -EINVAL;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						"qmp_phy_base");
	if (!res) {
		dev_err(dev, "failed getting qmp_phy_base\n");
		return -ENODEV;
	}

	/*
	 * For USB QMP DP combo PHY, common set of registers shall be accessed
	 * by DP driver as well.
	 */
	phy->base = devm_ioremap_nocache(dev, res->start, resource_size(res));
	if (IS_ERR_OR_NULL(phy->base)) {
		ret = PTR_ERR(phy->base);
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"vls_clamp_reg");
	if (res) {
		phy->vls_clamp_reg = devm_ioremap_resource(dev, res);
		if (IS_ERR(phy->vls_clamp_reg)) {
			dev_err(dev, "err getting vls_clamp_reg address\n");
			return PTR_ERR(phy->vls_clamp_reg);
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"pcs_clamp_enable_reg");
	if (res) {
		phy->pcs_clamp_enable_reg = devm_ioremap_resource(dev, res);
		if (IS_ERR(phy->pcs_clamp_enable_reg)) {
			dev_err(dev, "err getting pcs_clamp_enable_reg address.\n");
			return PTR_ERR(phy->pcs_clamp_enable_reg);
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"tcsr_usb3_dp_phymode");
	if (res) {
		phy->tcsr_usb3_dp_phymode = devm_ioremap_resource(dev, res);
		if (IS_ERR(phy->tcsr_usb3_dp_phymode)) {
			dev_err(dev, "err getting tcsr_usb3_dp_phymode addr\n");
			return PTR_ERR(phy->tcsr_usb3_dp_phymode);
		}
	}

	of_get_property(dev->of_node, "qcom,qmp-phy-init-seq", &size);
	if (size) {
		if (size % sizeof(*phy->qmp_phy_init_seq)) {
			dev_err(dev, "invalid init_seq_len\n");
			return -EINVAL;
		}

		phy->qmp_phy_init_seq = devm_kzalloc(dev, size, GFP_KERNEL);
		if (!phy->qmp_phy_init_seq)
			return -ENOMEM;

		phy->init_seq_len = (size / sizeof(*phy->qmp_phy_init_seq));
		of_property_read_u32_array(dev->of_node,
				"qcom,qmp-phy-init-seq",
				phy->qmp_phy_init_seq,
				phy->init_seq_len);
	} else {
		dev_err(dev, "error need qmp-phy-init-seq\n");
		return -EINVAL;
	}

	/* Set default core voltage values */
	phy->core_voltage_levels[CORE_LEVEL_NONE] = 0;
	phy->core_voltage_levels[CORE_LEVEL_MIN] = USB_SSPHY_1P2_VOL_MIN;
	phy->core_voltage_levels[CORE_LEVEL_MAX] = USB_SSPHY_1P2_VOL_MAX;

	if (of_get_property(dev->of_node, "qcom,core-voltage-level", &len) &&
		len == sizeof(phy->core_voltage_levels)) {
		ret = of_property_read_u32_array(dev->of_node,
				"qcom,core-voltage-level",
				(u32 *)phy->core_voltage_levels,
				len / sizeof(u32));
		if (ret) {
			dev_err(dev, "err qcom,core-voltage-level property\n");
			goto err;
		}
	}

	if (of_property_read_s32(dev->of_node, "qcom,core-max-load-uA",
				&phy->core_max_uA) || !phy->core_max_uA)
		phy->core_max_uA = USB_SSPHY_HPM_LOAD;

	if (of_get_property(dev->of_node, "qcom,vdd-voltage-level", &len) &&
		len == sizeof(phy->vdd_levels)) {
		ret = of_property_read_u32_array(dev->of_node,
				"qcom,vdd-voltage-level",
				(u32 *) phy->vdd_levels,
				len / sizeof(u32));
		if (ret) {
			dev_err(dev, "err qcom,vdd-voltage-level property\n");
			goto err;
		}
	} else {
		ret = -EINVAL;
		dev_err(dev, "error invalid inputs for vdd-voltage-level\n");
		goto err;
	}

	if (of_property_read_s32(dev->of_node, "qcom,vdd-max-load-uA",
				&phy->vdd_max_uA) || !phy->vdd_max_uA)
		phy->vdd_max_uA = USB_SSPHY_HPM_LOAD;

	phy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(phy->vdd)) {
		dev_err(dev, "unable to get vdd supply\n");
		ret = PTR_ERR(phy->vdd);
		goto err;
	}

	phy->core_ldo = devm_regulator_get(dev, "core");
	if (IS_ERR(phy->core_ldo)) {
		dev_err(dev, "unable to get core ldo supply\n");
		ret = PTR_ERR(phy->core_ldo);
		goto err;
	}

	platform_set_drvdata(pdev, phy);

	if (of_property_read_bool(dev->of_node, "qcom,vbus-valid-override"))
		phy->phy.flags |= PHY_VBUS_VALID_OVERRIDE;

	phy->phy.dev			= dev;
	phy->phy.init			= msm_ssphy_qmp_init;
	phy->phy.set_suspend		= msm_ssphy_qmp_set_suspend;
	phy->phy.notify_connect		= msm_ssphy_qmp_notify_connect;
	phy->phy.notify_disconnect	= msm_ssphy_qmp_notify_disconnect;

	if (phy->phy.type == USB_PHY_TYPE_USB3_AND_DP)
		phy->phy.reset		= msm_ssphy_qmp_dp_combo_reset;
	else
		phy->phy.reset		= msm_ssphy_qmp_reset;

	ret = msm_ssphy_qmp_extcon_register(phy, dev);
	if (ret)
		goto err;

	ret = usb_add_phy_dev(&phy->phy);

err:
	return ret;
}

static int msm_ssphy_qmp_remove(struct platform_device *pdev)
{
	struct msm_ssphy_qmp *phy = platform_get_drvdata(pdev);

	if (!phy)
		return 0;

	usb_remove_phy(&phy->phy);
	msm_ssphy_qmp_enable_clks(phy, false);
	msm_ssusb_qmp_ldo_enable(phy, 0);
	return 0;
}

static struct platform_driver msm_ssphy_qmp_driver = {
	.probe		= msm_ssphy_qmp_probe,
	.remove		= msm_ssphy_qmp_remove,
	.driver = {
		.name	= "msm-usb-ssphy-qmp",
		.of_match_table = of_match_ptr(msm_usb_id_table),
	},
};

module_platform_driver(msm_ssphy_qmp_driver);

MODULE_DESCRIPTION("MSM USB SS QMP PHY driver");
MODULE_LICENSE("GPL v2");
