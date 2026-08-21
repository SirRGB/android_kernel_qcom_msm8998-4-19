/*
 * A flicker free driver based on Qcom MDSS for OLED devices
 *
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (C) Sony Mobile Communications Inc. All rights reserved.
 * Copyright (C) 2014-2018, AngeloGioacchino Del Regno <kholk11@gmail.com>
 * Copyright (C) 2018, Devries <therkduan@gmail.com>
 * Copyright (C) 2019-2020, Tanish <tanish2k09.dev@gmail.com>
 * Copyright (C) 2020, shxyke <shxyke@gmail.com>
 * Copyright (C) 2022, ederekun <sedrickvince@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <asm/uaccess.h>
#include <linux/fb.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "mdss_fb.h"
#include "mdss_mdp.h"

#include "flicker_free.h"

/* Maximum value of RGB possible */
#define FF_MAX_SCALE 32768

/* Minimum value of RGB recommended */
#define FF_MIN_SCALE 2560

/* Number of backlight entries */
#define BACKLIGHT_INDEX 66

/* Minimum backlight value that does not flicker */
static int elvss_off_threshold __read_mostly = 66;

/* Display configuration data */
static const int bkl_to_pcc[BACKLIGHT_INDEX] = {
	42, 56, 67, 75, 84, 91, 98, 104, 109, 114, 119, 124, 128, 133, 136,
	140, 143, 146, 150, 152, 156, 159, 162, 165, 168, 172, 176, 178, 181,
	184, 187, 189, 192, 194, 196, 199, 202, 204, 206, 209, 211, 213, 215,
	217, 220, 222, 224, 226, 228, 230, 233, 236, 237, 239, 241, 241, 243,
	245, 246, 249, 249, 250, 252, 254, 255, 256
};

static const uint32_t pcc_depth[9] = {
	128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768
};

struct flicker_free_ctx {
	struct msm_fb_data_type *mfd;
	struct mdp_pcc_cfg_data pcc_config;
	struct mdp_dither_cfg_data dither_config;
	struct mdp_dither_data_v1_7 dither_payload;
	struct mdp_pcc_data_v1_7 payload;
	uint32_t dither_copyback;
	uint32_t copyback;
	bool pcc_enabled;
	bool enabled;
};

static DEFINE_MUTEX(flicker_free_lock);
static struct flicker_free_ctx *ff_ctx;
static struct proc_dir_entry *ff_proc_root;

static inline int
flicker_free_push(struct flicker_free_ctx *ctx, int bl_lvl, bool enable)
{
	uint32_t backlight, temp, depth;

	/* Override bl_lvl if we're disabling flicker free */
	if (!enable)
		bl_lvl = elvss_off_threshold;

	backlight = clamp_t(int, (((bl_lvl - 1) * (BACKLIGHT_INDEX - 1)) /
					(elvss_off_threshold - 1)) + 1, 1, BACKLIGHT_INDEX);
	temp = clamp_t(int, 0x80 * bkl_to_pcc[backlight - 1], FF_MIN_SCALE,
					FF_MAX_SCALE);

	for (depth = 8; depth >= 1; depth--) {
		if (temp >= pcc_depth[depth])
			break;
	}

	/* Configure dither values */
	ctx->dither_config.flags = MDP_PP_OPS_WRITE;
	if (ctx->enabled)
		ctx->dither_config.flags |= MDP_PP_OPS_ENABLE;
	else
		ctx->dither_config.flags |= MDP_PP_OPS_DISABLE;

	ctx->dither_payload.r_cr_depth = ctx->dither_config.r_cr_depth = depth;
	ctx->dither_payload.g_y_depth = ctx->dither_config.g_y_depth = depth;
	ctx->dither_payload.b_cb_depth = ctx->dither_config.b_cb_depth = depth;

	ctx->dither_payload.len = ctx->dither_payload.temporal_en = 0;

	ctx->dither_config.cfg_payload = &ctx->dither_payload;

	/* Configure pcc values */
	ctx->pcc_config.ops = MDP_PP_OPS_WRITE;
	if (enable)
		ctx->pcc_config.ops |= MDP_PP_OPS_ENABLE;
	else
		ctx->pcc_config.ops |= MDP_PP_OPS_DISABLE;

	ctx->payload.r.r = ctx->pcc_config.r.r = temp;
	ctx->payload.g.g = ctx->pcc_config.g.g = temp;
	ctx->payload.b.b = ctx->pcc_config.b.b = temp;

	ctx->pcc_config.cfg_payload = &ctx->payload;

	/* Push values consecutively */
	return mdss_mdp_dither_config(ctx->mfd, &ctx->dither_config,
			&ctx->dither_copyback, 1) ||
		mdss_mdp_pcc_config(ctx->mfd, &ctx->pcc_config,
			&ctx->copyback);
}

uint32_t mdss_panel_calc_backlight(uint32_t bl_lvl)
{
	struct flicker_free_ctx *ctx;
	bool target_pcc_en;

	mutex_lock(&flicker_free_lock);
	ctx = ff_ctx;
	if (!ctx)
		goto out;
	target_pcc_en = ctx->enabled;

	/* We don't need flicker free above elvss */
	if (bl_lvl >= elvss_off_threshold)
		target_pcc_en = false;

	if (target_pcc_en || ctx->pcc_enabled) {
		/* Only configure target pcc_enabled if we're successful */
		if (!flicker_free_push(ctx, bl_lvl, target_pcc_en))
			ctx->pcc_enabled = target_pcc_en;

		if (ctx->pcc_enabled)
			bl_lvl = elvss_off_threshold;
	}

out:
	mutex_unlock(&flicker_free_lock);
	return bl_lvl;
}

/*
 * Proc directory
 */
static ssize_t ff_write_proc(struct file *file, const char __user *buffer,
								size_t count, loff_t *pos)
{
	struct flicker_free_ctx *ctx;
	struct msm_fb_data_type *mfd;
	int value = 0;
	bool state;

	mutex_lock(&flicker_free_lock);
	ctx = ff_ctx;
	if (!ctx) {
		mutex_unlock(&flicker_free_lock);
		return count;
	}
	mfd = ctx->mfd;

	get_user(value, buffer);
	state = (value != '0');

	if (ctx->enabled != state) {
		/* Reset backlight */
		mutex_lock(&mfd->bl_lock);
		mfd->unset_bl_level = mfd->bl_level_usr;
		mfd->allow_bl_update = false;
		mutex_unlock(&mfd->bl_lock);

		ctx->enabled = state;
		mutex_unlock(&flicker_free_lock);
		mdss_fb_update_backlight(mfd);
		return count;
	}
	mutex_unlock(&flicker_free_lock);

	return count;
}

static int show_ff_state(struct seq_file *seq, void *v)
{
	/* Check ff_mfd, without mfd we cannot use flicker free */
	mutex_lock(&flicker_free_lock);
	seq_printf(seq, "%d\n", ff_ctx && ff_ctx->enabled);
	mutex_unlock(&flicker_free_lock);
	return 0;
}

static int open_ff_proc(struct inode *inode, struct file *file)
{
	return single_open(file, show_ff_state, NULL);
}

static const struct file_operations proc_file_fops_state = {
	.owner = THIS_MODULE,
	.open = open_ff_proc,
	.read = seq_read,
	.write = ff_write_proc,
	.llseek = seq_lseek,
	.release = single_release,
};

void mdss_fb_update_flicker_free_mfd(struct msm_fb_data_type *mfd)
{
	struct flicker_free_ctx *ctx;

	mutex_lock(&flicker_free_lock);
	if (!mfd) {
		kfree(ff_ctx);
		ff_ctx = NULL;
		mutex_unlock(&flicker_free_lock);
		return;
	}
	if (ff_ctx && ff_ctx->mfd == mfd) {
		mutex_unlock(&flicker_free_lock);
		return;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (ctx) {
		ctx->mfd = mfd;
		ctx->pcc_config.version = mdp_pcc_v1_7;
		ctx->pcc_config.block = MDP_LOGICAL_BLOCK_DISP_0;
		ctx->dither_config.version = mdp_dither_v1_7;
		ctx->dither_config.block = MDP_LOGICAL_BLOCK_DISP_0;
		kfree(ff_ctx);
		ff_ctx = ctx;
	}
	mutex_unlock(&flicker_free_lock);
}

void mdss_panel_set_elvss_off_threshold(int val)
{
	elvss_off_threshold = val;
}

static int __init mdss_fb_flicker_free_init(void)
{
	struct proc_dir_entry *node;

	/* File operations init */
	ff_proc_root = proc_mkdir("flicker_free", NULL);
	if (!ff_proc_root)
		return -ENOMEM;

	node = proc_create("flicker_free", 0666, ff_proc_root,
					&proc_file_fops_state);
	if (!node)
		goto remove_proc;

	return 0;

remove_proc:
	remove_proc_subtree("flicker_free", NULL);
	ff_proc_root = NULL;

	pr_err("mdss_fb: Failed to initialize flicker free driver\n");
	return -ENOMEM;
}
late_initcall(mdss_fb_flicker_free_init);

static void __exit mdss_fb_flicker_free_exit(void)
{
	remove_proc_subtree("flicker_free", NULL);
	mutex_lock(&flicker_free_lock);
	kfree(ff_ctx);
	ff_ctx = NULL;
	mutex_unlock(&flicker_free_lock);
}
module_exit(mdss_fb_flicker_free_exit);
