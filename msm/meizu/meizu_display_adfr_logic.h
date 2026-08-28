/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MEIZU_DISPLAY_ADFR_LOGIC_H_
#define _MEIZU_DISPLAY_ADFR_LOGIC_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
typedef __UINT8_TYPE__ u8;
typedef __UINT32_TYPE__ u32;
#endif

struct meizu_dynamic_te_filter {
	u8 refresh_rate_mid;
	u8 refresh_rate_high;
	u8 refresh_rate_high_120hz_count;
	u8 refresh_rate_high_60hz_count;
	u8 refresh_rate_high_30hz_count;
	u8 refresh_rate_mid_20hz_count;
	u32 refresh_rate;
};

bool meizu_adfr_min_fps_valid(u32 min_fps);
u8 meizu_bf197_min_fps_code(u32 min_fps);
bool meizu_bf197_patch_min_fps(u8 *payload, u32 length, u32 min_fps);
u32 meizu_bf197_filter_dynamic_te(struct meizu_dynamic_te_filter *filter,
				  u32 panel_refresh_rate, u32 min_fps,
				  u32 measured_refresh_rate);

#endif /* _MEIZU_DISPLAY_ADFR_LOGIC_H_ */
