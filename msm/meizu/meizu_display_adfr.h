/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEIZU_DISPLAY_ADFR_H_
#define _MEIZU_DISPLAY_ADFR_H_

#include "../dsi/dsi_defs.h"

struct dsi_display;
struct dsi_panel;

#if IS_ENABLED(CONFIG_DRM_MSM_DSI)
int meizu_display_adfr_init(struct dsi_display *display);
bool meizu_display_adfr_is_supported(void);
void meizu_display_adfr_active(void);
void meizu_display_adfr_panel_disable(void);
void meizu_display_adfr_before_backlight(struct dsi_panel *panel);
void meizu_display_adfr_after_backlight(struct dsi_panel *panel);
void meizu_display_adfr_mode_switch(struct dsi_panel *panel);
void meizu_display_adfr_handle_idle(bool enter_idle);
bool meizu_display_adfr_needs_nolp(const struct dsi_panel *panel);
enum dsi_cmd_set_type meizu_display_adfr_nolp_cmd(const struct dsi_panel *panel);
#else
static inline int meizu_display_adfr_init(struct dsi_display *display)
{
	(void)display;
	return 0;
}

static inline bool meizu_display_adfr_is_supported(void)
{
	return false;
}

static inline void meizu_display_adfr_active(void)
{
}

static inline void meizu_display_adfr_panel_disable(void)
{
}

static inline void meizu_display_adfr_before_backlight(struct dsi_panel *panel)
{
	(void)panel;
}

static inline void meizu_display_adfr_after_backlight(struct dsi_panel *panel)
{
	(void)panel;
}

static inline void meizu_display_adfr_mode_switch(struct dsi_panel *panel)
{
	(void)panel;
}

static inline void meizu_display_adfr_handle_idle(bool enter_idle)
{
	(void)enter_idle;
}

static inline bool meizu_display_adfr_needs_nolp(const struct dsi_panel *panel)
{
	(void)panel;
	return true;
}

static inline enum dsi_cmd_set_type
meizu_display_adfr_nolp_cmd(const struct dsi_panel *panel)
{
	(void)panel;
	return DSI_CMD_SET_NOLP;
}
#endif

#endif /* _MEIZU_DISPLAY_ADFR_H_ */
