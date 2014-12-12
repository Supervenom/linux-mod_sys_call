/* Driver for Realtek PCI-Express card reader
 *
 * Copyright(c) 2009 Realtek Semiconductor Corp. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author:
 *   Wei WANG <wei_wang@realsil.com.cn>
 *   No. 450, Shenhu Road, Suzhou Industry Park, Suzhou, China
 */

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/mfd/rtsx_pci.h>
#include <asm/unaligned.h>

#include "rtsx_pcr.h"

static bool msi_en = true;
module_param(msi_en, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(msi_en, "Enable MSI");

static DEFINE_IDR(rtsx_pci_idr);
static DEFINE_SPINLOCK(rtsx_pci_lock);

static struct mfd_cell rtsx_pcr_cells[] = {
	[RTSX_SD_CARD] = {
		.name = DRV_NAME_RTSX_PCI_SDMMC,
	},
	[RTSX_MS_CARD] = {
		.name = DRV_NAME_RTSX_PCI_MS,
	},
};

static DEFINE_PCI_DEVICE_TABLE(rtsx_pci_ids) = {
	{ PCI_DEVICE(0x10EC, 0x5209), PCI_CLASS_OTHERS << 16, 0xFF0000 },
	{ PCI_DEVICE(0x10EC, 0x5229), PCI_CLASS_OTHERS << 16, 0xFF0000 },
	{ PCI_DEVICE(0x10EC, 0x5289), PCI_CLASS_OTHERS << 16, 0xFF0000 },
	{ PCI_DEVICE(0x10EC, 0x5227), PCI_CLASS_OTHERS << 16, 0xFF0000 },
	{ PCI_DEVICE(0x10EC, 0x5249), PCI_CLASS_OTHERS << 16, 0xFF0000 },
	{ PCI_DEVICE(0x10EC, 0x5287), PCI_CLASS_OTHERS << 16, 0xFF0000 },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, rtsx_pci_ids);

void rtsx_pci_start_run(struct rtsx_pcr *pcr)
{
	/* If pci device removed, don't queue idle work any more */
	if (pcr->remove_pci)
		return;

	if (pcr->state != PDEV_STAT_RUN) {
		pcr->state = PDEV_STAT_RUN;
		if (pcr->ops->enable_auto_blink)
			pcr->ops->enable_auto_blink(pcr);

		if (pcr->aspm_en)
			rtsx_pci_write_config_byte(pcr, LCTLR, 0);
	}

	mod_delayed_work(system_wq, &pcr->idle_work, msecs_to_jiffies(200));
}
EXPORT_SYMBOL_GPL(rtsx_pci_start_run);

int rtsx_pci_write_register(struct rtsx_pcr *pcr, u16 addr, u8 mask, u8 data)
{
	int i;
	u32 val = HAIMR_WRITE_START;

	val |= (u32)(addr & 0x3FFF) << 16;
	val |= (u32)mask << 8;
	val |= (u32)data;

	rtsx_pci_writel(pcr, RTSX_HAIMR, val);

	for (i = 0; i < MAX_RW_REG_CNT; i++) {
		val = rtsx_pci_readl(pcr, RTSX_HAIMR);
		if ((val & HAIMR_TRANS_END) == 0) {
			if (data != (u8)val)
				return -EIO;
			return 0;
		}
	}

	return -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(rtsx_pci_write_register);

int rtsx_pci_read_register(struct rtsx_pcr *pcr, u16 addr, u8 *data)
{
	u32 val = HAIMR_READ_START;
	int i;

	val |= (u32)(addr & 0x3FFF) << 16;
	rtsx_pci_writel(pcr, RTSX_HAIMR, val);

	for (i = 0; i < MAX_RW_REG_CNT; i++) {
		val = rtsx_pci_readl(pcr, RTSX_HAIMR);
		if ((val & HAIMR_TRANS_END) == 0)
			break;
	}

	if (i >= MAX_RW_REG_CNT)
		return -ETIMEDOUT;

	if (data)
		*data = (u8)(val & 0xFF);

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_read_register);

int rtsx_pci_write_phy_register(struct rtsx_pcr *pcr, u8 addr, u16 val)
{
	int err, i, finished = 0;
	u8 tmp;

	rtsx_pci_init_cmd(pcr);

	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PHYDATA0, 0xFF, (u8)val);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PHYDATA1, 0xFF, (u8)(val >> 8));
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PHYADDR, 0xFF, addr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PHYRWCTL, 0xFF, 0x81);

	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	for (i = 0; i < 100000; i++) {
		err = rtsx_pci_read_register(pcr, PHYRWCTL, &tmp);
		if (err < 0)
			return err;

		if (!(tmp & 0x80)) {
			finished = 1;
			break;
		}
	}

	if (!finished)
		return -ETIMEDOUT;

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_write_phy_register);

int rtsx_pci_read_phy_register(struct rtsx_pcr *pcr, u8 addr, u16 *val)
{
	int err, i, finished = 0;
	u16 data;
	u8 *ptr, tmp;

	rtsx_pci_init_cmd(pcr);

	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PHYADDR, 0xFF, addr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PHYRWCTL, 0xFF, 0x80);

	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	for (i = 0; i < 100000; i++) {
		err = rtsx_pci_read_register(pcr, PHYRWCTL, &tmp);
		if (err < 0)
			return err;

		if (!(tmp & 0x80)) {
			finished = 1;
			break;
		}
	}

	if (!finished)
		return -ETIMEDOUT;

	rtsx_pci_init_cmd(pcr);

	rtsx_pci_add_cmd(pcr, READ_REG_CMD, PHYDATA0, 0, 0);
	rtsx_pci_add_cmd(pcr, READ_REG_CMD, PHYDATA1, 0, 0);

	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	ptr = rtsx_pci_get_cmd_data(pcr);
	data = ((u16)ptr[1] << 8) | ptr[0];

	if (val)
		*val = data;

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_read_phy_register);

void rtsx_pci_stop_cmd(struct rtsx_pcr *pcr)
{
	rtsx_pci_writel(pcr, RTSX_HCBCTLR, STOP_CMD);
	rtsx_pci_writel(pcr, RTSX_HDBCTLR, STOP_DMA);

	rtsx_pci_write_register(pcr, DMACTL, 0x80, 0x80);
	rtsx_pci_write_register(pcr, RBCTL, 0x80, 0x80);
}
EXPORT_SYMBOL_GPL(rtsx_pci_stop_cmd);

void rtsx_pci_add_cmd(struct rtsx_pcr *pcr,
		u8 cmd_type, u16 reg_addr, u8 mask, u8 data)
{
	unsigned long flags;
	u32 val = 0;
	u32 *ptr = (u32 *)(pcr->host_cmds_ptr);

	val |= (u32)(cmd_type & 0x03) << 30;
	val |= (u32)(reg_addr & 0x3FFF) << 16;
	val |= (u32)mask << 8;
	val |= (u32)data;

	spin_lock_irqsave(&pcr->lock, flags);
	ptr += pcr->ci;
	if (pcr->ci < (HOST_CMDS_BUF_LEN / 4)) {
		put_unaligned_le32(val, ptr);
		ptr++;
		pcr->ci++;
	}
	spin_unlock_irqrestore(&pcr->lock, flags);
}
EXPORT_SYMBOL_GPL(rtsx_pci_add_cmd);

void rtsx_pci_send_cmd_no_wait(struct rtsx_pcr *pcr)
{
	u32 val = 1 << 31;

	rtsx_pci_writel(pcr, RTSX_HCBAR, pcr->host_cmds_addr);

	val |= (u32)(pcr->ci * 4) & 0x00FFFFFF;
	/* Hardware Auto Response */
	val |= 0x40000000;
	rtsx_pci_writel(pcr, RTSX_HCBCTLR, val);
}
EXPORT_SYMBOL_GPL(rtsx_pci_send_cmd_no_wait);

int rtsx_pci_send_cmd(struct rtsx_pcr *pcr, int timeout)
{
	struct completion trans_done;
	u32 val = 1 << 31;
	long timeleft;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&pcr->lock, flags);

	/* set up data structures for the wakeup system */
	pcr->done = &trans_done;
	pcr->trans_result = TRANS_NOT_READY;
	init_completion(&trans_done);

	rtsx_pci_writel(pcr, RTSX_HCBAR, pcr->host_cmds_addr);

	val |= (u32)(pcr->ci * 4) & 0x00FFFFFF;
	/* Hardware Auto Response */
	val |= 0x40000000;
	rtsx_pci_writel(pcr, RTSX_HCBCTLR, val);

	spin_unlock_irqrestore(&pcr->lock, flags);

	/* Wait for TRANS_OK_INT */
	timeleft = wait_for_completion_interruptible_timeout(
			&trans_done, msecs_to_jiffies(timeout));
	if (timeleft <= 0) {
		dev_dbg(&(pcr->pci->dev), "Timeout (%s %d)\n",
				__func__, __LINE__);
		err = -ETIMEDOUT;
		goto finish_send_cmd;
	}

	spin_lock_irqsave(&pcr->lock, flags);
	if (pcr->trans_result == TRANS_RESULT_FAIL)
		err = -EINVAL;
	else if (pcr->trans_result == TRANS_RESULT_OK)
		err = 0;
	else if (pcr->trans_result == TRANS_NO_DEVICE)
		err = -ENODEV;
	spin_unlock_irqrestore(&pcr->lock, flags);

finish_send_cmd:
	spin_lock_irqsave(&pcr->lock, flags);
	pcr->done = NULL;
	spin_unlock_irqrestore(&pcr->lock, flags);

	if ((err < 0) && (err != -ENODEV))
		rtsx_pci_stop_cmd(pcr);

	if (pcr->finish_me)
		complete(pcr->finish_me);

	return err;
}
EXPORT_SYMBOL_GPL(rtsx_pci_send_cmd);

static void rtsx_pci_add_sg_tbl(struct rtsx_pcr *pcr,
		dma_addr_t addr, unsigned int len, int end)
{
	u64 *ptr = (u64 *)(pcr->host_sg_tbl_ptr) + pcr->sgi;
	u64 val;
	u8 option = SG_VALID | SG_TRANS_DATA;

	dev_dbg(&(pcr->pci->dev), "DMA addr: 0x%x, Len: 0x%x\n",
			(unsigned int)addr, len);

	if (end)
		option |= SG_END;
	val = ((u64)addr << 32) | ((u64)len << 12) | option;

	put_unaligned_le64(val, ptr);
	pcr->sgi++;
}

int rtsx_pci_transfer_data(struct rtsx_pcr *pcr, struct scatterlist *sglist,
		int num_sg, bool read, int timeout)
{
	struct completion trans_done;
	u8 dir;
	int err = 0, i, count;
	long timeleft;
	unsigned long flags;
	struct scatterlist *sg;
	enum dma_data_direction dma_dir;
	u32 val;
	dma_addr_t addr;
	unsigned int len;

	dev_dbg(&(pcr->pci->dev), "--> %s: num_sg = %d\n", __func__, num_sg);

	/* don't transfer data during abort processing */
	if (pcr->remove_pci)
		return -EINVAL;

	if ((sglist == NULL) || (num_sg <= 0))
		return -EINVAL;

	if (read) {
		dir = DEVICE_TO_HOST;
		dma_dir = DMA_FROM_DEVICE;
	} else {
		dir = HOST_TO_DEVICE;
		dma_dir = DMA_TO_DEVICE;
	}

	count = dma_map_sg(&(pcr->pci->dev), sglist, num_sg, dma_dir);
	if (count < 1) {
		dev_err(&(pcr->pci->dev), "scatterlist map failed\n");
		return -EINVAL;
	}
	dev_dbg(&(pcr->pci->dev), "DMA mapping count: %d\n", count);

	val = ((u32)(dir & 0x01) << 29) | TRIG_DMA | ADMA_MODE;
	pcr->sgi = 0;
	for_each_sg(sglist, sg, count, i) {
		addr = sg_dma_address(sg);
		len = sg_dma_len(sg);
		rtsx_pci_add_sg_tbl(pcr, addr, len, i == count - 1);
	}

	spin_lock_irqsave(&pcr->lock, flags);

	pcr->done = &trans_done;
	pcr->trans_result = TRANS_NOT_READY;
	init_completion(&trans_done);
	rtsx_pci_writel(pcr, RTSX_HDBAR, pcr->host_sg_tbl_addr);
	rtsx_pci_writel(pcr, RTSX_HDBCTLR, val);

	spin_unlock_irqrestore(&pcr->lock, flags);

	timeleft = wait_for_completion_interruptible_timeout(
			&trans_done, msecs_to_jiffies(timeout));
	if (timeleft <= 0) {
		dev_dbg(&(pcr->pci->dev), "Timeout (%s %d)\n",
				__func__, __LINE__);
		err = -ETIMEDOUT;
		goto out;
	}

	spin_lock_irqsave(&pcr->lock, flags);

	if (pcr->trans_result == TRANS_RESULT_FAIL)
		err = -EINVAL;
	else if (pcr->trans_result == TRANS_NO_DEVICE)
		err = -ENODEV;

	spin_unlock_irqrestore(&pcr->lock, flags);

out:
	spin_lock_irqsave(&pcr->lock, flags);
	pcr->done = NULL;
	spin_unlock_irqrestore(&pcr->lock, flags);

	dma_unmap_sg(&(pcr->pci->dev), sglist, num_sg, dma_dir);

	if ((err < 0) && (err != -ENODEV))
		rtsx_pci_stop_cmd(pcr);

	if (pcr->finish_me)
		complete(pcr->finish_me);

	return err;
}
EXPORT_SYMBOL_GPL(rtsx_pci_transfer_data);

int rtsx_pci_read_ppbuf(struct rtsx_pcr *pcr, u8 *buf, int buf_len)
{
	int err;
	int i, j;
	u16 reg;
	u8 *ptr;

	if (buf_len > 512)
		buf_len = 512;

	ptr = buf;
	reg = PPBUF_BASE2;
	for (i = 0; i < buf_len / 256; i++) {
		rtsx_pci_init_cmd(pcr);

		for (j = 0; j < 256; j++)
			rtsx_pci_add_cmd(pcr, READ_REG_CMD, reg++, 0, 0);

		err = rtsx_pci_send_cmd(pcr, 250);
		if (err < 0)
			return err;

		memcpy(ptr, rtsx_pci_get_cmd_data(pcr), 256);
		ptr += 256;
	}

	if (buf_len % 256) {
		rtsx_pci_init_cmd(pcr);

		for (j = 0; j < buf_len % 256; j++)
			rtsx_pci_add_cmd(pcr, READ_REG_CMD, reg++, 0, 0);

		err = rtsx_pci_send_cmd(pcr, 250);
		if (err < 0)
			return err;
	}

	memcpy(ptr, rtsx_pci_get_cmd_data(pcr), buf_len % 256);

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_read_ppbuf);

int rtsx_pci_write_ppbuf(struct rtsx_pcr *pcr, u8 *buf, int buf_len)
{
	int err;
	int i, j;
	u16 reg;
	u8 *ptr;

	if (buf_len > 512)
		buf_len = 512;

	ptr = buf;
	reg = PPBUF_BASE2;
	for (i = 0; i < buf_len / 256; i++) {
		rtsx_pci_init_cmd(pcr);

		for (j = 0; j < 256; j++) {
			rtsx_pci_add_cmd(pcr, WRITE_REG_CMD,
					reg++, 0xFF, *ptr);
			ptr++;
		}

		err = rtsx_pci_send_cmd(pcr, 250);
		if (err < 0)
			return err;
	}

	if (buf_len % 256) {
		rtsx_pci_init_cmd(pcr);

		for (j = 0; j < buf_len % 256; j++) {
			rtsx_pci_add_cmd(pcr, WRITE_REG_CMD,
					reg++, 0xFF, *ptr);
			ptr++;
		}

		err = rtsx_pci_send_cmd(pcr, 250);
		if (err < 0)
			return err;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_write_ppbuf);

static int rtsx_pci_set_pull_ctl(struct rtsx_pcr *pcr, const u32 *tbl)
{
	int err;

	rtsx_pci_init_cmd(pcr);

	while (*tbl & 0xFFFF0000) {
		rtsx_pci_add_cmd(pcr, WRITE_REG_CMD,
				(u16)(*tbl >> 16), 0xFF, (u8)(*tbl));
		tbl++;
	}

	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	return 0;
}

int rtsx_pci_card_pull_ctl_enable(struct rtsx_pcr *pcr, int card)
{
	const u32 *tbl;

	if (card == RTSX_SD_CARD)
		tbl = pcr->sd_pull_ctl_enable_tbl;
	else if (card == RTSX_MS_CARD)
		tbl = pcr->ms_pull_ctl_enable_tbl;
	else
		return -EINVAL;

	return rtsx_pci_set_pull_ctl(pcr, tbl);
}
EXPORT_SYMBOL_GPL(rtsx_pci_card_pull_ctl_enable);

int rtsx_pci_card_pull_ctl_disable(struct rtsx_pcr *pcr, int card)
{
	const u32 *tbl;

	if (card == RTSX_SD_CARD)
		tbl = pcr->sd_pull_ctl_disable_tbl;
	else if (card == RTSX_MS_CARD)
		tbl = pcr->ms_pull_ctl_disable_tbl;
	else
		return -EINVAL;


	return rtsx_pci_set_pull_ctl(pcr, tbl);
}
EXPORT_SYMBOL_GPL(rtsx_pci_card_pull_ctl_disable);

static void rtsx_pci_enable_bus_int(struct rtsx_pcr *pcr)
{
	pcr->bier = TRANS_OK_INT_EN | TRANS_FAIL_INT_EN | SD_INT_EN;

	if (pcr->num_slots > 1)
		pcr->bier |= MS_INT_EN;

	/* Enable Bus Interrupt */
	rtsx_pci_writel(pcr, RTSX_BIER, pcr->bier);

	dev_dbg(&(pcr->pci->dev), "RTSX_BIER: 0x%08x\n", pcr->bier);
}

static inline u8 double_ssc_depth(u8 depth)
{
	return ((depth > 1) ? (depth - 1) : depth);
}

static u8 revise_ssc_depth(u8 ssc_depth, u8 div)
{
	if (div > CLK_DIV_1) {
		if (ssc_depth > (div - 1))
			ssc_depth -= (div - 1);
		else
			ssc_depth = SSC_DEPTH_4M;
	}

	return ssc_depth;
}

int rtsx_pci_switch_clock(struct rtsx_pcr *pcr, unsigned int card_clock,
		u8 ssc_depth, bool initial_mode, bool double_clk, bool vpclk)
{
	int err, clk;
	u8 n, clk_divider, mcu_cnt, div;
	u8 depth[] = {
		[RTSX_SSC_DEPTH_4M] = SSC_DEPTH_4M,
		[RTSX_SSC_DEPTH_2M] = SSC_DEPTH_2M,
		[RTSX_SSC_DEPTH_1M] = SSC_DEPTH_1M,
		[RTSX_SSC_DEPTH_500K] = SSC_DEPTH_500K,
		[RTSX_SSC_DEPTH_250K] = SSC_DEPTH_250K,
	};

	if (initial_mode) {
		/* We use 250k(around) here, in initial stage */
		clk_divider = SD_CLK_DIVIDE_128;
		card_clock = 30000000;
	} else {
		clk_divider = SD_CLK_DIVIDE_0;
	}
	err = rtsx_pci_write_register(pcr, SD_CFG1,
			SD_CLK_DIVIDE_MASK, clk_divider);
	if (err < 0)
		return err;

	card_clock /= 1000000;
	dev_dbg(&(pcr->pci->dev), "Switch card clock to %dMHz\n", card_clock);

	clk = card_clock;
	if (!initial_mode && double_clk)
		clk = card_clock * 2;
	dev_dbg(&(pcr->pci->dev),
			"Internal SSC clock: %dMHz (cur_clock = %d)\n",
			clk, pcr->cur_clock);

	if (clk == pcr->cur_clock)
		return 0;

	if (pcr->ops->conv_clk_and_div_n)
		n = (u8)pcr->ops->conv_clk_and_div_n(clk, CLK_TO_DIV_N);
	else
		n = (u8)(clk - 2);
	if ((clk <= 2) || (n > MAX_DIV_N_PCR))
		return -EINVAL;

	mcu_cnt = (u8)(125/clk + 3);
	if (mcu_cnt > 15)
		mcu_cnt = 15;

	/* Make sure that the SSC clock div_n is not less than MIN_DIV_N_PCR */
	div = CLK_DIV_1;
	while ((n < MIN_DIV_N_PCR) && (div < CLK_DIV_8)) {
		if (pcr->ops->conv_clk_and_div_n) {
			int dbl_clk = pcr->ops->conv_clk_and_div_n(n,
					DIV_N_TO_CLK) * 2;
			n = (u8)pcr->ops->conv_clk_and_div_n(dbl_clk,
					CLK_TO_DIV_N);
		} else {
			n = (n + 2) * 2 - 2;
		}
		div++;
	}
	dev_dbg(&(pcr->pci->dev), "n = %d, div = %d\n", n, div);

	ssc_depth = depth[ssc_depth];
	if (double_clk)
		ssc_depth = double_ssc_depth(ssc_depth);

	ssc_depth = revise_ssc_depth(ssc_depth, div);
	dev_dbg(&(pcr->pci->dev), "ssc_depth = %d\n", ssc_depth);

	rtsx_pci_init_cmd(pcr);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CLK_CTL,
			CLK_LOW_FREQ, CLK_LOW_FREQ);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CLK_DIV,
			0xFF, (div << 4) | mcu_cnt);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, 0);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SSC_CTL2,
			SSC_DEPTH_MASK, ssc_depth);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SSC_DIV_N_0, 0xFF, n);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, SSC_RSTB);
	if (vpclk) {
		rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD_VPCLK0_CTL,
				PHASE_NOT_RESET, 0);
		rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SD_VPCLK0_CTL,
				PHASE_NOT_RESET, PHASE_NOT_RESET);
	}

	err = rtsx_pci_send_cmd(pcr, 2000);
	if (err < 0)
		return err;

	/* Wait SSC clock stable */
	udelay(10);
	err = rtsx_pci_write_register(pcr, CLK_CTL, CLK_LOW_FREQ, 0);
	if (err < 0)
		return err;

	pcr->cur_clock = clk;
	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_switch_clock);

int rtsx_pci_card_power_on(struct rtsx_pcr *pcr, int card)
{
	if (pcr->ops->card_power_on)
		return pcr->ops->card_power_on(pcr, card);

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_card_power_on);

int rtsx_pci_card_power_off(struct rtsx_pcr *pcr, int card)
{
	if (pcr->ops->card_power_off)
		return pcr->ops->card_power_off(pcr, card);

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_card_power_off);

int rtsx_pci_card_exclusive_check(struct rtsx_pcr *pcr, int card)
{
	unsigned int cd_mask[] = {
		[RTSX_SD_CARD] = SD_EXIST,
		[RTSX_MS_CARD] = MS_EXIST
	};

	if (!(pcr->flags & PCR_MS_PMOS)) {
		/* When using single PMOS, accessing card is not permitted
		 * if the existing card is not the designated one.
		 */
		if (pcr->card_exist & (~cd_mask[card]))
			return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_card_exclusive_check);

int rtsx_pci_switch_output_voltage(struct rtsx_pcr *pcr, u8 voltage)
{
	if (pcr->ops->switch_output_voltage)
		return pcr->ops->switch_output_voltage(pcr, voltage);

	return 0;
}
EXPORT_SYMBOL_GPL(rtsx_pci_switch_output_voltage);

unsigned int rtsx_pci_card_exist(struct rtsx_pcr *pcr)
{
	unsigned int val;

	val = rtsx_pci_readl(pcr, RTSX_BIPR);
	if (pcr->ops->cd_deglitch)
		val = pcr->ops->cd_deglitch(pcr);

	return val;
}
EXPORT_SYMBOL_GPL(rtsx_pci_card_exist);

void rtsx_pci_complete_unfinished_transfer(struct rtsx_pcr *pcr)
{
	struct completion finish;

	pcr->finish_me = &finish;
	init_completion(&finish);

	if (pcr->done)
		complete(pcr->done);

	if (!pcr->remove_pci)
		rtsx_pci_stop_cmd(pcr);

	wait_for_completion_interruptible_timeout(&finish,
			msecs_to_jiffies(2));
	pcr->finish_me = NULL;
}
EXPORT_SYMBOL_GPL(rtsx_pci_complete_unfinished_transfer);

static void rtsx_pci_card_detect(struct work_struct *work)
{
	struct delayed_work *dwork;
	struct rtsx_pcr *pcr;
	unsigned long flags;
	unsigned int card_detect = 0, card_inserted, card_removed;
	u32 irq_status;

	dwork = to_delayed_work(work);
	pcr = container_of(dwork, struct rtsx_pcr, carddet_work);

	dev_dbg(&(pcr->pci->dev), "--> %s\n", __func__);

	mutex_lock(&pcr->pcr_mutex);
	spin_lock_irqsave(&pcr->lock, flags);

	irq_status = rtsx_pci_readl(pcr, RTSX_BIPR);
	dev_dbg(&(pcr->pci->dev), "irq_status: 0x%08x\n", irq_status);

	irq_status &= CARD_EXIST;
	card_inserted = pcr->card_inserted & irq_status;
	card_removed = pcr->card_removed;
	pcr->card_inserted = 0;
	pcr->card_removed = 0;

	spin_unlock_irqrestore(&pcr->lock, flags);

	if (card_inserted || card_removed) {
		dev_dbg(&(pcr->pci->dev),
				"card_inserted: 0x%x, card_removed: 0x%x\n",
				card_inserted, card_removed);

		if (pcr->ops->cd_deglitch)
			card_inserted = pcr->ops->cd_deglitch(pcr);

		card_detect = card_inserted | card_removed;

		pcr->card_exist |= card_inserted;
		pcr->card_exist &= ~card_removed;
	}

	mutex_unlock(&pcr->pcr_mutex);

	if ((card_detect & SD_EXIST) && pcr->slots[RTSX_SD_CARD].card_event)
		pcr->slots[RTSX_SD_CARD].card_event(
				pcr->slots[RTSX_SD_CARD].p_dev);
	if ((card_detect & MS_EXIST) && pcr->slots[RTSX_MS_CARD].card_event)
		pcr->slots[RTSX_MS_CARD].card_event(
				pcr->slots[RTSX_MS_CARD].p_dev);
}

static irqreturn_t rtsx_pci_isr(int irq, void *dev_id)
{
	struct rtsx_pcr *pcr = dev_id;
	u32 int_reg;

	if (!pcr)
		return IRQ_NONE;

	spin_lock(&pcr->lock);

	int_reg = rtsx_pci_readl(pcr, RTSX_BIPR);
	/* Clear interrupt flag */
	rtsx_pci_writel(pcr, RTSX_BIPR, int_reg);
	if ((int_reg & pcr->bier) == 0) {
		spin_unlock(&pcr->lock);
		return IRQ_NONE;
	}
	if (int_reg == 0xFFFFFFFF) {
		spin_unlock(&pcr->lock);
		return IRQ_HANDLED;
	}

	int_reg &= (pcr->bier | 0x7FFFFF);

	if (int_reg & SD_INT) {
		if (int_reg & SD_EXIST) {
			pcr->card_inserted |= SD_EXIST;
		} else {
			pcr->card_removed |= SD_EXIST;
			pcr->card_inserted &= ~SD_EXIST;
		}
	}

	if (int_reg & MS_INT) {
		if (int_reg & MS_EXIST) {
			pcr->card_inserted |= MS_EXIST;
		} else {
			pcr->card_removed |= MS_EXIST;
			pcr->card_inserted &= ~MS_EXIST;
		}
	}

	if (int_reg & (NEED_COMPLETE_INT | DELINK_INT)) {
		if (int_reg & (TRANS_FAIL_INT | DELINK_INT)) {
			pcr->trans_result = TRANS_RESULT_FAIL;
			if (pcr->done)
				complete(pcr->done);
		} else if (int_reg & TRANS_OK_INT) {
			pcr->trans_result = TRANS_RESULT_OK;
			if (pcr->done)
				complete(pcr->done);
		}
	}

	if (pcr->card_inserted || pcr->card_removed)
		schedule_delayed_work(&pcr->carddet_work,
				msecs_to_jiffies(200));

	spin_unlock(&pcr->lock);
	return IRQ_HANDLED;
}

static int rtsx_pci_acquire_irq(struct rtsx_pcr *pcr)
{
	dev_info(&(pcr->pci->dev), "%s: pcr->msi_en = %d, pci->irq = %d\n",
			__func__, pcr->msi_en, pcr->pci->irq);

	if (request_irq(pcr->pci->irq, rtsx_pci_isr,
			pcr->msi_en ? 0 : IRQF_SHARED,
			DRV_NAME_RTSX_PCI, pcr)) {
		dev_err(&(pcr->pci->dev),
			"rtsx_sdmmc: unable to grab IRQ %d, disabling device\n",
			pcr->pci->irq);
		return -1;
	}

	pcr->irq = pcr->pci->irq;
	pci_intx(pcr->pci, !pcr->msi_en);

	return 0;
}

static void rtsx_pci_idle_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct rtsx_pcr *pcr = container_of(dwork, struct rtsx_pcr, idle_work);

	dev_dbg(&(pcr->pci->dev), "--> %s\n", __func__);

	mutex_lock(&pcr->pcr_mutex);

	pcr->state = PDEV_STAT_IDLE;

	if (pcr->ops->disable_auto_blink)
		pcr->ops->disable_auto_blink(pcr);
	if (pcr->ops->turn_off_led)
		pcr->ops->turn_off_led(pcr);

	if (pcr->aspm_en)
		rtsx_pci_write_config_byte(pcr, LCTLR, pcr->aspm_en);

	mutex_unlock(&pcr->pcr_mutex);
}

static int rtsx_pci_init_hw(struct rtsx_pcr *pcr)
{
	int err;

	rtsx_pci_writel(pcr, RTSX_HCBAR, pcr->host_cmds_addr);

	rtsx_pci_enable_bus_int(pcr);

	/* Power on SSC */
	err = rtsx_pci_write_register(pcr, FPDCTL, SSC_POWER_DOWN, 0);
	if (err < 0)
		return err;

	/* Wait SSC power stable */
	udelay(200);

	if (pcr->ops->optimize_phy) {
		err = pcr->ops->optimize_phy(pcr);
		if (err < 0)
			return err;
	}

	rtsx_pci_init_cmd(pcr);

	/* Set mcu_cnt to 7 to ensure data can be sampled properly */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CLK_DIV, 0x07, 0x07);

	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, HOST_SLEEP_STATE, 0x03, 0x00);
	/* Disable card clock */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_CLK_EN, 0x1E, 0);
	/* Reset ASPM state to default value */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, ASPM_FORCE_CTL, 0x3F, 0);
	/* Reset delink mode */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CHANGE_LINK_STATE, 0x0A, 0);
	/* Card driving select */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CARD_DRIVE_SEL,
			0xFF, pcr->card_drive_sel);
	/* Enable SSC Clock */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SSC_CTL1,
			0xFF, SSC_8X_EN | SSC_SEL_4M);
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, SSC_CTL2, 0xFF, 0x12);
	/* Disable cd_pwr_save */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, CHANGE_LINK_STATE, 0x16, 0x10);
	/* Clear Link Ready Interrupt */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, IRQSTAT0,
			LINK_RDY_INT, LINK_RDY_INT);
	/* Enlarge the estimation window of PERST# glitch
	 * to reduce the chance of invalid card interrupt
	 */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PERST_GLITCH_WIDTH, 0xFF, 0x80);
	/* Update RC oscillator to 400k
	 * bit[0] F_HIGH: for RC oscillator, Rst_value is 1'b1
	 *                1: 2M  0: 400k
	 */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, RCCTL, 0x01, 0x00);
	/* Set interrupt write clear
	 * bit 1: U_elbi_if_rd_clr_en
	 *	1: Enable ELBI interrupt[31:22] & [7:0] flag read clear
	 *	0: ELBI interrupt flag[31:22] & [7:0] only can be write clear
	 */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, NFTS_TX_CTRL, 0x02, 0);
	/* Force CLKREQ# PIN to drive 0 to request clock */
	rtsx_pci_add_cmd(pcr, WRITE_REG_CMD, PETXCFG, 0x08, 0x08);

	err = rtsx_pci_send_cmd(pcr, 100);
	if (err < 0)
		return err;

	rtsx_pci_write_config_byte(pcr, LCTLR, 0);

	/* Enable clk_request_n to enable clock power management */
	rtsx_pci_write_config_byte(pcr, 0x81, 1);
	/* Enter L1 when host tx idle */
	rtsx_pci_write_config_byte(pcr, 0x70F, 0x5B);

	if (pcr->ops->extra_init_hw) {
		err = pcr->ops->extra_init_hw(pcr);
		if (err < 0)
			return err;
	}

	/* No CD interrupt if probing driver with card inserted.
	 * So we need to initialize pcr->card_exist here.
	 */
	if (pcr->ops->cd_deglitch)
		pcr->card_exist = pcr->ops->cd_deglitch(pcr);
	else
		pcr->card_exist = rtsx_pci_readl(pcr, RTSX_BIPR) & CARD_EXIST;

	return 0;
}

static int rtsx_pci_init_chip(struct rtsx_pcr *pcr)
{
	int err;

	spin_lock_init(&pcr->lock);
	mutex_init(&pcr->pcr_mutex);

	switch (PCI_PID(pcr)) {
	default:
	case 0x5209:
		rts5209_init_params(pcr);
		break;

	case 0x5229:
		rts5229_init_params(pcr);
		break;

	case 0x5289:
		rtl8411_init_params(pcr);
		break;

	case 0x5227:
		rts5227_init_params(pcr);
		break;

	case 0x5249:
		rts5249_init_params(pcr);
		break;

	case 0x5287:
		rtl8411b_init_params(pcr);
		break;
	}

	dev_dbg(&(pcr->pci->dev), "PID: 0x%04x, IC version: 0x%02x\n",
			PCI_PID(pcr), pcr->ic_version);

	pcr->slots = kcalloc(pcr->num_slots, sizeof(struct rtsx_slot),
			GFP_KERNEL);
	if (!pcr->slots)
		return -ENOMEM;

	if (pcr->ops->fetch_vendor_settings)
		pcr->ops->fetch_vendor_settings(pcr);

	dev_dbg(&(pcr->pci->dev), "pcr->aspm_en = 0x%x\n", pcr->aspm_en);
	dev_dbg(&(pcr->pci->dev), "pcr->sd30_drive_sel_1v8 = 0x%x\n",
			pcr->sd30_drive_sel_1v8);
	dev_dbg(&(pcr->pci->dev), "pcr->sd30_drive_sel_3v3 = 0x%x\n",
			pcr->sd30_drive_sel_3v3);
	dev_dbg(&(pcr->pci->dev), "pcr->card_drive_sel = 0x%x\n",
			pcr->card_drive_sel);
	dev_dbg(&(pcr->pci->dev), "pcr->flags = 0x%x\n", pcr->flags);

	pcr->state = PDEV_STAT_IDLE;
	err = rtsx_pci_init_hw(pcr);
	if (err < 0) {
		kfree(pcr->slots);
		return err;
	}

	return 0;
}

static int rtsx_pci_probe(struct pci_dev *pcidev,
			  const struct pci_device_id *id)
{
	struct rtsx_pcr *pcr;
	struct pcr_handle *handle;
	u32 base, len;
	int ret, i;

	dev_dbg(&(pcidev->dev),
		": Realtek PCI-E Card Reader found at %s [%04x:%04x] (rev %x)\n",
		pci_name(pcidev), (int)pcidev->vendor, (int)pcidev->device,
		(int)pcidev->revision);

	ret = pci_set_dma_mask(pcidev, DMA_BIT_MASK(32));
	if (ret < 0)
		return ret;

	ret = pci_enable_device(pcidev);
	if (ret)
		return ret;

	ret = pci_request_regions(pcidev, DRV_NAME_RTSX_PCI);
	if (ret)
		goto disable;

	pcr = kzalloc(sizeof(*pcr), GFP_KERNEL);
	if (!pcr) {
		ret = -ENOMEM;
		goto release_pci;
	}

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle) {
		ret = -ENOMEM;
		goto free_pcr;
	}
	handle->pcr = pcr;

	idr_preload(GFP_KERNEL);
	spin_lock(&rtsx_pci_lock);
	ret = idr_alloc(&rtsx_pci_idr, pcr, 0, 0, GFP_NOWAIT);
	if (ret >= 0)
		pcr->id = ret;
	spin_unlock(&rtsx_pci_lock);
	idr_preload_end();
	if (ret < 0)
		goto free_handle;

	pcr->pci = pcidev;
	dev_set_drvdata(&pcidev->dev, handle);

	len = pci_resource_len(pcidev, 0);
	base = pci_resource_start(pcidev, 0);
	pcr->remap_addr = ioremap_nocache(base, len);
	if (!pcr->remap_addr) {
		ret = -ENOMEM;
		goto free_host;
	}

	pcr->rtsx_resv_buf = dma_alloc_coherent(&(pcidev->dev),
			RTSX_RESV_BUF_LEN, &(pcr->rtsx_resv_buf_addr),
			GFP_KERNEL);
	if (pcr->rtsx_resv_buf == NULL) {
		ret = -ENXIO;
		goto unmap;
	}
	pcr->host_cmds_ptr = pcr->rtsx_resv_buf;
	pcr->host_cmds_addr = pcr->rtsx_resv_buf_addr;
	pcr->host_sg_tbl_ptr = pcr->rtsx_resv_buf + HOST_CMDS_BUF_LEN;
	pcr->host_sg_tbl_addr = pcr->rtsx_resv_buf_addr + HOST_CMDS_BUF_LEN;

	pcr->card_inserted = 0;
	pcr->card_removed = 0;
	INIT_DELAYED_WORK(&pcr->carddet_work, rtsx_pci_card_detect);
	INIT_DELAYED_WORK(&pcr->idle_work, rtsx_pci_idle_work);

	pcr->msi_en = msi_en;
	if (pcr->msi_en) {
		ret = pci_enable_msi(pcidev);
		if (ret < 0)
			pcr->msi_en = false;
	}

	ret = rtsx_pci_acquire_irq(pcr);
	if (ret < 0)
		goto disable_msi;

	pci_set_master(pcidev);
	synchronize_irq(pcr->irq);

	ret = rtsx_pci_init_chip(pcr);
	if (ret < 0)
		goto disable_irq;

	for (i = 0; i < ARRAY_SIZE(rtsx_pcr_cells); i++) {
		rtsx_pcr_cells[i].platform_data = handle;
		rtsx_pcr_cells[i].pdata_size = sizeof(*handle);
	}
	ret = mfd_add_devices(&pcidev->dev, pcr->id, rtsx_pcr_cells,
			ARRAY_SIZE(rtsx_pcr_cells), NULL, 0, NULL);
	if (ret < 0)
		goto disable_irq;

	schedule_delayed_work(&pcr->idle_work, msecs_to_jiffies(200));

	return 0;

disable_irq:
	free_irq(pcr->irq, (void *)pcr);
disable_msi:
	if (pcr->msi_en)
		pci_disable_msi(pcr->pci);
	dma_free_coherent(&(pcr->pci->dev), RTSX_RESV_BUF_LEN,
			pcr->rtsx_resv_buf, pcr->rtsx_resv_buf_addr);
unmap:
	iounmap(pcr->remap_addr);
free_host:
	dev_set_drvdata(&pcidev->dev, NULL);
free_handle:
	kfree(handle);
free_pcr:
	kfree(pcr);
release_pci:
	pci_release_regions(pcidev);
disable:
	pci_disable_device(pcidev);

	return ret;
}

static void rtsx_pci_remove(struct pci_dev *pcidev)
{
	struct pcr_handle *handle = pci_get_drvdata(pcidev);
	struct rtsx_pcr *pcr = handle->pcr;

	pcr->remove_pci = true;

	/* Disable interrupts at the pcr level */
	spin_lock_irq(&pcr->lock);
	rtsx_pci_writel(pcr, RTSX_BIER, 0);
	pcr->bier = 0;
	spin_unlock_irq(&pcr->lock);

	cancel_delayed_work_sync(&pcr->carddet_work);
	cancel_delayed_work_sync(&pcr->idle_work);

	mfd_remove_devices(&pcidev->dev);

	dma_free_coherent(&(pcr->pci->dev), RTSX_RESV_BUF_LEN,
			pcr->rtsx_resv_buf, pcr->rtsx_resv_buf_addr);
	free_irq(pcr->irq, (void *)pcr);
	if (pcr->msi_en)
		pci_disable_msi(pcr->pci);
	iounmap(pcr->remap_addr);

	dev_set_drvdata(&pcidev->dev, NULL);
	pci_release_regions(pcidev);
	pci_disable_device(pcidev);

	spin_lock(&rtsx_pci_lock);
	idr_remove(&rtsx_pci_idr, pcr->id);
	spin_unlock(&rtsx_pci_lock);

	kfree(pcr->slots);
	kfree(pcr);
	kfree(handle);

	dev_dbg(&(pcidev->dev),
		": Realtek PCI-E Card Reader at %s [%04x:%04x] has been removed\n",
		pci_name(pcidev), (int)pcidev->vendor, (int)pcidev->device);
}

#ifdef CONFIG_PM

static int rtsx_pci_suspend(struct pci_dev *pcidev, pm_message_t state)
{
	struct pcr_handle *handle;
	struct rtsx_pcr *pcr;
	int ret = 0;

	dev_dbg(&(pcidev->dev), "--> %s\n", __func__);

	handle = pci_get_drvdata(pcidev);
	pcr = handle->pcr;

	cancel_delayed_work(&pcr->carddet_work);
	cancel_delayed_work(&pcr->idle_work);

	mutex_lock(&pcr->pcr_mutex);

	if (pcr->ops->turn_off_led)
		pcr->ops->turn_off_led(pcr);

	rtsx_pci_writel(pcr, RTSX_BIER, 0);
	pcr->bier = 0;

	rtsx_pci_write_register(pcr, PETXCFG, 0x08, 0x08);
	rtsx_pci_write_register(pcr, HOST_SLEEP_STATE, 0x03, 0x02);

	pci_save_state(pcidev);
	pci_enable_wake(pcidev, pci_choose_state(pcidev, state), 0);
	pci_disable_device(pcidev);
	pci_set_power_state(pcidev, pci_choose_state(pcidev, state));

	mutex_unlock(&pcr->pcr_mutex);
	return ret;
}

static int rtsx_pci_resume(struct pci_dev *pcidev)
{
	struct pcr_handle *handle;
	struct rtsx_pcr *pcr;
	int ret = 0;

	dev_dbg(&(pcidev->dev), "--> %s\n", __func__);

	handle = pci_get_drvdata(pcidev);
	pcr = handle->pcr;

	mutex_lock(&pcr->pcr_mutex);

	pci_set_power_state(pcidev, PCI_D0);
	pci_restore_state(pcidev);
	ret = pci_enable_device(pcidev);
	if (ret)
		goto out;
	pci_set_master(pcidev);

	ret = rtsx_pci_write_register(pcr, HOST_SLEEP_STATE, 0x03, 0x00);
	if (ret)
		goto out;

	ret = rtsx_pci_init_hw(pcr);
	if (ret)
		goto out;

	schedule_delayed_work(&pcr->idle_work, msecs_to_jiffies(200));

out:
	mutex_unlock(&pcr->pcr_mutex);
	return ret;
}

#else /* CONFIG_PM */

#define rtsx_pci_suspend NULL
#define rtsx_pci_resume NULL

#endif /* CONFIG_PM */

static struct pci_driver rtsx_pci_driver = {
	.name = DRV_NAME_RTSX_PCI,
	.id_table = rtsx_pci_ids,
	.probe = rtsx_pci_probe,
	.remove = rtsx_pci_remove,
	.suspend = rtsx_pci_suspend,
	.resume = rtsx_pci_resume,
};
module_pci_driver(rtsx_pci_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wei WANG <wei_wang@realsil.com.cn>");
MODULE_DESCRIPTION("Realtek PCI-E Card Reader Driver");
