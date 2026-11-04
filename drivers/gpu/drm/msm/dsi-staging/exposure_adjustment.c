/*
 * An exposure adjustment driver based on Qcom DSPP for OLED devices
 *
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (C) Sony Mobile Communications Inc. All rights reserved.
 * Copyright (C) 2014-2018, AngeloGioacchino Del Regno <kholk11@gmail.com>
 * Copyright (C) 2018, Devries <therkduan@gmail.com>
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

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#include "dsi_display.h"
#include "dsi_panel.h"
#include "../sde/sde_crtc.h"
#include "../sde/sde_plane.h"
#include "../sde/sde_color_processing.h"
#include "exposure_adjustment.h"

/*
 * The PCC blob is handed over to the SDE color processing framework and only
 * latched on the next atomic commit, while the DSI backlight command takes
 * effect immediately. Pinning the backlight to ELVSS_OFF_THRESHOLD in the same
 * call would therefore show an undimmed, full brightness frame, so the pin is
 * delayed until the dimming is in effect.
 */
#define EA_CLAMP_DELAY_MS	40

static DEFINE_MUTEX(ea_lock);

static bool pcc_backlight_enable;
static bool pcc_clamped;
static bool pcc_applied;
static struct dsi_panel *ea_panel;
static struct drm_crtc *ea_crtc;
static u32 last_level = ELVSS_OFF_THRESHOLD;

/*
 * Restoring the backlight and dropping the PCC cannot happen atomically: the
 * backlight level is sent over DSI and only reaches the panel on the next
 * frame, while the PCC is latched on the next atomic commit.  Clearing the PCC
 * first would leave a frame at the threshold backlight with no dimming, which
 * is the bright flash seen when the feature was switched off, so it is cleared
 * once the restored backlight level is on the panel.
 */
#define EA_PCC_OFF_DELAY_MS	40

static void ea_clamp_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ea_clamp_work, ea_clamp_work_fn);

static void ea_pcc_off_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ea_pcc_off_work, ea_pcc_off_work_fn);

static int ea_panel_get_pcc(struct drm_crtc **crtc, struct drm_property **prop)
{
	struct dsi_display *display;
	struct msm_drm_private *priv;

	display = get_main_display();
	if (!display || !display->drm_conn || !display->drm_conn->state) {
		pr_err("ERROR: Display is not ready\n");
		return -ENODEV;
	}

	*crtc = display->drm_conn->state->crtc;
	if (!*crtc) {
		pr_err("ERROR: Cannot find display panel with CRTC\n");
		return -ENODEV;
	}

	priv = (*crtc)->dev->dev_private;
	*prop = priv->cp_property[1]; /* SDE_CP_CRTC_DSPP_PCC */
	if (!*prop) {
		pr_err("FAIL! PCC is not supported!!?!?!\n");
		return -EINVAL;
	}

	return 0;
}

static int ea_panel_send_pcc(u32 bl_lvl)
{
	int rc;
	struct drm_crtc *crtc;
	struct drm_msm_pcc pcc_blk = {0};
	struct drm_property *prop;
	struct drm_property_blob *blob;
	u32 ea_coeff;
	uint64_t val;

	rc = ea_panel_get_pcc(&crtc, &prop);
	if (rc)
		return rc;

	rc = sde_cp_crtc_get_property(crtc, prop, &val);
	if (rc) {
		pr_err("Cannot get CRTC property. Things may go wrong.\n");
	}

	pr_debug("%s: Backlight = %d\n", __func__, bl_lvl);

	if (bl_lvl < ELVSS_OFF_THRESHOLD) {
		ea_coeff = bl_lvl * PCC_BACKLIGHT_SCALE + EXPOSURE_ADJUSTMENT_MIN;
	} else {
		ea_coeff = EXPOSURE_ADJUSTMENT_MAX;
	}

	pr_debug("%s: coeff = %d\n", __func__, ea_coeff);

	pcc_blk.r.r = ea_coeff;
	pcc_blk.g.g = ea_coeff;
	pcc_blk.b.b = ea_coeff;

	blob = drm_property_create_blob(crtc->dev, sizeof(pcc_blk), &pcc_blk);
	if (IS_ERR_OR_NULL(blob)) {
		pr_err("Failed to create blob. Bailing out.\n");
		return -EINVAL;
	}
	pr_debug("DSPP Blob ID %d has length %zu\n",
			prop->base.id, blob->length);

	rc = sde_cp_crtc_set_property_ea(crtc, prop, blob->base.id);
	/*
	 * sde_cp_crtc_set_property_ea() takes its own reference on the blob when
	 * it stores it in the property node, so the reference returned by
	 * drm_property_create_blob() has to be dropped here.  Without this
	 * every PCC update leaks a blob and the kernel slowly runs out of
	 * memory while the anti-flicker feature is in use.
	 */
	drm_property_blob_put(blob);
	if (rc) {
		pr_err("DSPP: Cannot set PCC: %d.\n", rc);
		return rc;
	}

	mutex_lock(&ea_lock);
	pcc_applied = true;
	ea_crtc = crtc;
	mutex_unlock(&ea_lock);

	return 0;
}

/*
 * Turn the PCC block off instead of programming a "no dimming" coefficient.
 * EXPOSURE_ADJUSTMENT_MAX is the top of the coefficient range this driver uses
 * for the pin, not unity (the block works in U1.16 and unity is 0x10000), so a
 * disable that leaves that coefficient behind keeps the panel dimmed after the
 * feature was switched off - until something else resets the block, which is
 * visible as a brightness jump.
 */
static void ea_panel_disable_pcc(void)
{
	struct drm_crtc *crtc;
	struct drm_property *prop;
	int rc;

	if (ea_panel_get_pcc(&crtc, &prop))
		return;

	rc = sde_cp_crtc_set_property_ea(crtc, prop, 0);
	if (rc) {
		pr_err("DSPP: Cannot disable PCC: %d.\n", rc);
		return;
	}

	mutex_lock(&ea_lock);
	pcc_applied = false;
	mutex_unlock(&ea_lock);
}

static void ea_clamp_work_fn(struct work_struct *work)
{
	struct dsi_panel *panel;
	u32 level;

	mutex_lock(&ea_lock);

	if (!pcc_backlight_enable || pcc_clamped) {
		mutex_unlock(&ea_lock);
		return;
	}

	panel = ea_panel;
	level = last_level;

	/*
	 * A brand new boot can hand the display over through continuous splash,
	 * in which case dsi_panel_enable() never runs.  Rely on the panel state
	 * itself instead of a flag that only the enable path updates, otherwise
	 * the clamp is never armed and the panel stays at the raw level with
	 * the PCC applied on top of it (twice as dark as asked for).
	 */
	if (!panel || !panel->panel_initialized || panel->in_aod ||
			level == 0 || level >= ELVSS_OFF_THRESHOLD) {
		mutex_unlock(&ea_lock);
		return;
	}

	pcc_clamped = true;
	mutex_unlock(&ea_lock);

	/* The PCC has been latched, the panel can be pinned at the threshold */
	dsi_panel_set_backlight(panel, level);
}

void ea_panel_init(struct dsi_panel *panel)
{
	mutex_lock(&ea_lock);
	ea_panel = panel;
	mutex_unlock(&ea_lock);
}

/*
 * Drop a pending clamp and make the next backlight update go through the
 * deferred path again.  Called on every panel power and AOD transition,
 * because the PCC has to be latched again after the display pipeline was
 * restarted.
 */
void ea_panel_reset(void)
{
	cancel_delayed_work_sync(&ea_clamp_work);

	mutex_lock(&ea_lock);
	pcc_clamped = false;
	mutex_unlock(&ea_lock);
}

bool ea_panel_is_enabled(void)
{
	return pcc_backlight_enable;
}

/*
 * While the anti-flicker dimming is in effect the backlight is pinned at
 * ELVSS_OFF_THRESHOLD and the PCC block carries the actual brightness, so a
 * PCC update coming from anybody else (the composer re-sends its own PCC blob
 * when the display is rotated) would replace the dimming coefficients and
 * leave the panel at the pinned level until the next backlight update.  Tell
 * the color processing layer to drop those updates while this driver owns the
 * block; the driver's own updates go through sde_cp_crtc_set_property_ea().
 */
bool ea_panel_pcc_is_dim_active(struct drm_crtc *crtc)
{
	bool active;

	mutex_lock(&ea_lock);
	active = pcc_backlight_enable && pcc_applied && ea_crtc == crtc &&
			last_level && last_level < ELVSS_OFF_THRESHOLD &&
			ea_panel && ea_panel->panel_initialized &&
			!ea_panel->in_aod;
	mutex_unlock(&ea_lock);

	return active;
}

void ea_panel_mode_ctrl(struct dsi_panel *panel, bool enable)
{
	bool changed;
	u32 level;

	mutex_lock(&ea_lock);
	changed = (pcc_backlight_enable != enable);
	if (changed) {
		pcc_backlight_enable = enable;
		/*
		 * Do not pin the backlight yet: the PCC is only latched on the
		 * next atomic commit, so the panel would show a bright frame
		 * without any dimming.
		 */
		pcc_clamped = false;
	}
	level = last_level;
	mutex_unlock(&ea_lock);

	if (changed) {
		cancel_delayed_work_sync(&ea_clamp_work);
		cancel_delayed_work_sync(&ea_pcc_off_work);
		pr_debug("Recover backlight level = %d\n", last_level);
		dsi_panel_set_backlight(panel, level);
		if (!enable)
			schedule_delayed_work(&ea_pcc_off_work,
					msecs_to_jiffies(EA_PCC_OFF_DELAY_MS));
	} else if (level == 0 && !pcc_backlight_enable) {
		ea_panel_disable_pcc();
	}
}

static void ea_pcc_off_work_fn(struct work_struct *work)
{
	bool disable;

	mutex_lock(&ea_lock);
	disable = !pcc_backlight_enable && pcc_applied;
	mutex_unlock(&ea_lock);

	if (disable)
		ea_panel_disable_pcc();
}

u32 ea_panel_calc_backlight(u32 bl_lvl)
{
	bool clamped;

	mutex_lock(&ea_lock);
	last_level = bl_lvl;

	if (!pcc_backlight_enable || bl_lvl == 0 ||
			bl_lvl >= ELVSS_OFF_THRESHOLD) {
		bool applied;

		/*
		 * The pin is not needed at this level.  When the brightness
		 * went above the threshold while the feature was on, the
		 * dimming coefficient has to go away as well, otherwise the
		 * panel stays dimmed without anything to compensate for it.
		 * Do not do it when the screen is being turned off: the
		 * threshold level would be visible until the backlight is
		 * actually down.
		 */
		applied = pcc_backlight_enable && pcc_applied &&
				bl_lvl >= ELVSS_OFF_THRESHOLD;
		pcc_clamped = false;
		mutex_unlock(&ea_lock);
		cancel_delayed_work(&ea_clamp_work);

		if (applied)
			ea_panel_disable_pcc();

		return bl_lvl;
	}
	mutex_unlock(&ea_lock);

	if (ea_panel_send_pcc(bl_lvl)) {
		pr_err("ERROR: Failed to send PCC\n");
		/*
		 * Never pin the backlight when the dimming could not be
		 * programmed, otherwise the panel stays at full brightness.
		 */
		mutex_lock(&ea_lock);
		pcc_clamped = false;
		mutex_unlock(&ea_lock);
		return bl_lvl;
	}

	mutex_lock(&ea_lock);
	clamped = pcc_clamped;
	mutex_unlock(&ea_lock);

	if (clamped)
		return ELVSS_OFF_THRESHOLD;

	/*
	 * Keep the real backlight level for a couple of frames, until the PCC
	 * blob queued above has been latched by the display controller.
	 */
	schedule_delayed_work(&ea_clamp_work,
			      msecs_to_jiffies(EA_CLAMP_DELAY_MS));

	return bl_lvl;
}
