// SPDX-License-Identifier: GPL-2.0-only
/*
 * Meizu 21 Pro synchronized backlight support.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/ktime.h>

#include "../dsi/dsi_display.h"
#include "../dsi/dsi_panel.h"
#include "../sde/sde_connector.h"
#include "../sde/sde_encoder.h"
#include "../sde/sde_encoder_phys.h"
#include "../sde/sde_trace.h"
#include "meizu_display_brightness.h"

bool synclights_enable = true;
EXPORT_SYMBOL(synclights_enable);

int display_brightness_parse_vsync_config(struct dsi_display_mode *mode,
					  struct dsi_parser_utils *utils)
{
	struct dsi_display_mode_priv_info *priv_info;
	u32 refresh_rate;

	(void)utils;

	if (!mode || !mode->priv_info)
		return -EINVAL;

	refresh_rate = mode->timing.refresh_rate;
	if (!refresh_rate)
		return -EINVAL;

	priv_info = mode->priv_info;
	priv_info->vsync_period = USEC_PER_SEC / refresh_rate;
	priv_info->vsync_width = priv_info->vsync_period / 2;

	DSI_INFO("vsync width = %u, vsync period = %u\n",
		 priv_info->vsync_width, priv_info->vsync_period);

	return 0;
}

bool display_brightness_is_enable_synclights(void)
{
	return READ_ONCE(synclights_enable);
}

int display_panel_set_brightness(struct backlight_device *bd,
				 unsigned long brightness)
{
	int rc = -ENXIO;

	mutex_lock(&bd->ops_lock);
	if (bd->ops) {
		if (brightness > bd->props.max_brightness) {
			rc = -EINVAL;
		} else {
			bd->props.brightness = brightness;
			rc = backlight_update_status(bd);
		}
	}
	mutex_unlock(&bd->ops_lock);

	return rc;
}

static int display_get_current_vsync_period(struct drm_connector *connector)
{
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_display *display;
	struct sde_connector *c_conn;

	if (!connector)
		return -EINVAL;

	c_conn = to_sde_connector(connector);
	display = c_conn->display;
	if (!display || !display->panel || !display->panel->cur_mode ||
	    !display->panel->cur_mode->priv_info)
		return -EINVAL;

	priv_info = display->panel->cur_mode->priv_info;
	return priv_info->vsync_period;
}

static int display_get_current_vsync_width(struct drm_connector *connector)
{
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_display *display;
	struct sde_connector *c_conn;

	if (!connector)
		return -EINVAL;

	c_conn = to_sde_connector(connector);
	display = c_conn->display;
	if (!display || !display->panel || !display->panel->cur_mode ||
	    !display->panel->cur_mode->priv_info)
		return -EINVAL;

	priv_info = display->panel->cur_mode->priv_info;
	return priv_info->vsync_width;
}

int display_sync_panel_brightness(struct drm_encoder *encoder)
{
	struct sde_encoder_phys_cmd_te_timestamp *te_timestamp;
	struct sde_encoder_phys_cmd *cmd_enc;
	struct sde_encoder_phys *phys_enc;
	struct sde_encoder_virt *sde_enc;
	struct sde_connector *c_conn;
	struct drm_connector *connector;
	ktime_t last_te_timestamp;
	unsigned long brightness;
	s64 elapsed_us;
	s64 delay_us;
	int vsync_period;
	int vsync_width;
	int rc;

	if (!encoder)
		return -EFAULT;

	sde_enc = to_sde_encoder_virt(encoder);
	phys_enc = sde_enc->phys_encs[0];
	if (!phys_enc || !phys_enc->connector || !sde_enc->cur_master ||
	    !sde_enc->cur_master->connector)
		return -EFAULT;

	connector = phys_enc->connector;
	if (connector->connector_type != DRM_MODE_CONNECTOR_DSI)
		return 0;

	c_conn = to_sde_connector(connector);
	if (!connector->state || !c_conn->display || !c_conn->bl_device)
		return -EFAULT;

	vsync_period = display_get_current_vsync_period(sde_enc->cur_master->connector);
	vsync_width = display_get_current_vsync_width(sde_enc->cur_master->connector);
	if (vsync_period <= 0 || vsync_width < 0)
		return -EINVAL;

	cmd_enc = container_of(phys_enc, struct sde_encoder_phys_cmd, base);
	if (list_empty(&cmd_enc->te_timestamp_list))
		return 0;

	te_timestamp = list_last_entry(&cmd_enc->te_timestamp_list,
				       struct sde_encoder_phys_cmd_te_timestamp,
				       list);
	last_te_timestamp = te_timestamp->timestamp;

	if (!READ_ONCE(c_conn->bl_need_sync))
		return 0;
	WRITE_ONCE(c_conn->bl_need_sync, false);

	brightness = sde_connector_get_property(connector->state,
						CONNECTOR_PROP_SYNC_BL);
	elapsed_us = ktime_to_us(ktime_sub(ktime_get(), last_te_timestamp));
	delay_us = vsync_width - (elapsed_us % vsync_period);

	SDE_ATRACE_BEGIN("sync_panel_brightness");
	if (delay_us > 0) {
		SDE_EVT32(vsync_period, ktime_to_us(last_te_timestamp), delay_us);
		usleep_range(delay_us, delay_us + 100);
	}
	rc = display_panel_set_brightness(c_conn->bl_device, brightness);
	SDE_ATRACE_END("sync_panel_brightness");

	return rc;
}
