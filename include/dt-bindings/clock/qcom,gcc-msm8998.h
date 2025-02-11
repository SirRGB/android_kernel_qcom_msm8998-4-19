/*
 * Linux mainline API clocks implementation for MSM8998
 * Copyright (C) 2018, AngeloGioacchino Del Regno <kholk11@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* HWs */
#define GCC_XO					0
#define GCC_BI_TCXO				1
#define GCC_CE1_AHB_M_CLK			2
#define GCC_CE1_AXI_M_CLK			3
#define GCC_GPLL0_EARLY_DIV			4

/* PLLs */
#define GPLL0					0
#define GPLL0_AO				1
#define GPLL0_OUT_MAIN				2
#define GPLL1					3
#define GPLL1_OUT_MAIN				4
#define GPLL2					5
#define GPLL2_OUT_MAIN				6
#define GPLL3					7
#define GPLL3_OUT_MAIN				8
#define GPLL4					9
#define GPLL4_OUT_MAIN				10

/* GCC Clocks */
#define USB30_MASTER_CLK_SRC			20
#define PCIE_AUX_CLK_SRC			21
#define UFS_AXI_CLK_SRC				22
#define BLSP1_QUP1_I2C_APPS_CLK_SRC		23
#define BLSP1_QUP1_SPI_APPS_CLK_SRC		24
#define BLSP1_QUP2_I2C_APPS_CLK_SRC		25
#define BLSP1_QUP2_SPI_APPS_CLK_SRC		26
#define BLSP1_QUP3_I2C_APPS_CLK_SRC		27
#define BLSP1_QUP3_SPI_APPS_CLK_SRC		28
#define BLSP1_QUP4_I2C_APPS_CLK_SRC		29
#define BLSP1_QUP4_SPI_APPS_CLK_SRC		30
#define BLSP1_QUP5_I2C_APPS_CLK_SRC		31
#define BLSP1_QUP5_SPI_APPS_CLK_SRC		32
#define BLSP1_QUP6_I2C_APPS_CLK_SRC		33
#define BLSP1_QUP6_SPI_APPS_CLK_SRC		34
#define BLSP1_UART1_APPS_CLK_SRC		35
#define BLSP1_UART2_APPS_CLK_SRC		36
#define BLSP1_UART3_APPS_CLK_SRC		37
#define BLSP2_QUP1_I2C_APPS_CLK_SRC		38
#define BLSP2_QUP1_SPI_APPS_CLK_SRC		39
#define BLSP2_QUP2_I2C_APPS_CLK_SRC		40
#define BLSP2_QUP2_SPI_APPS_CLK_SRC		41
#define BLSP2_QUP3_I2C_APPS_CLK_SRC		42
#define BLSP2_QUP3_SPI_APPS_CLK_SRC		43
#define BLSP2_QUP4_I2C_APPS_CLK_SRC		44
#define BLSP2_QUP4_SPI_APPS_CLK_SRC		45
#define BLSP2_QUP5_I2C_APPS_CLK_SRC		46
#define BLSP2_QUP5_SPI_APPS_CLK_SRC		47
#define BLSP2_QUP6_I2C_APPS_CLK_SRC		48
#define BLSP2_QUP6_SPI_APPS_CLK_SRC		49
#define BLSP2_UART1_APPS_CLK_SRC		50
#define BLSP2_UART2_APPS_CLK_SRC		51
#define BLSP2_UART3_APPS_CLK_SRC		52
#define GP1_CLK_SRC				53
#define GP2_CLK_SRC				54
#define GP3_CLK_SRC				55
#define GCC_MSS_GPLL0_DIV_CLK_SRC				73

/* GCC block resets */
#define QUSB2PHY_PRIM_BCR			0
#define QUSB2PHY_SEC_BCR			1
#define BLSP1_BCR				2
#define BLSP2_BCR				3
#define BOOT_ROM_BCR				4
#define PRNG_BCR				5
#define UFS_BCR					6
#define USB_30_BCR				7
#define USB3_PHY_BCR				8
#define USB3PHY_PHY_BCR				9
#define PCIE_0_PHY_BCR				10
#define PCIE_PHY_BCR				11
#define PCIE_PHY_COM_BCR			12
#define PCIE_PHY_NOCSR_COM_PHY_BCR		13
