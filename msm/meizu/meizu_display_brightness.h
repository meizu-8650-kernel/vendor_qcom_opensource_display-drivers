/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEIZU_DISPLAY_BRIGHTNESS_H_
#define _MEIZU_DISPLAY_BRIGHTNESS_H_

#include <linux/types.h>

struct backlight_device;
struct drm_encoder;
struct dsi_display_mode;
struct dsi_parser_utils;

extern bool synclights_enable;

int display_brightness_parse_vsync_config(struct dsi_display_mode *mode,
					  struct dsi_parser_utils *utils);
bool display_brightness_is_enable_synclights(void);
int display_panel_set_brightness(struct backlight_device *bd,
				 unsigned long brightness);
int display_sync_panel_brightness(struct drm_encoder *encoder);

#endif /* _MEIZU_DISPLAY_BRIGHTNESS_H_ */
