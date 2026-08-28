// SPDX-License-Identifier: GPL-2.0-only
#include "meizu_display_adfr_logic.h"

bool meizu_adfr_min_fps_valid(u32 min_fps)
{
	switch (min_fps) {
	case 0:
	case 1:
	case 16:
	case 32:
	case 48:
	case 64:
	case 96:
		return true;
	default:
		return false;
	}
}

u8 meizu_bf197_min_fps_code(u32 min_fps)
{
	switch (min_fps) {
	case 1:
		return 0x77;
	case 16:
		return 0x0b;
	case 32:
		return 0x05;
	case 48:
		return 0x03;
	case 64:
		return 0x02;
	case 96:
		return 0x01;
	default:
		return 0x00;
	}
}

bool meizu_bf197_patch_min_fps(u8 *payload, u32 length, u32 min_fps)
{
	u8 code;

	if (!payload || length != 9 || !meizu_adfr_min_fps_valid(min_fps) ||
	    !min_fps)
		return false;

	code = meizu_bf197_min_fps_code(min_fps);
	payload[2] = code;
	payload[3] = code;
	payload[6] = code;
	payload[7] = code;
	return true;
}

u32 meizu_bf197_filter_dynamic_te(struct meizu_dynamic_te_filter *filter,
				  u32 panel_refresh_rate, u32 min_fps,
				  u32 measured_refresh_rate)
{
	u8 high_count;

	if (!filter)
		return 0;

	if (!min_fps) {
		filter->refresh_rate = panel_refresh_rate;
		return filter->refresh_rate;
	}

	if (min_fps == 1) {
		filter->refresh_rate = 1;
		return filter->refresh_rate;
	}

	if (measured_refresh_rate < 23) {
		filter->refresh_rate_high = 0;
		filter->refresh_rate_high_60hz_count = 0;
		filter->refresh_rate_high_120hz_count = 0;
		filter->refresh_rate_mid++;
		filter->refresh_rate_high_30hz_count = 0;

		if (measured_refresh_rate < 18) {
			filter->refresh_rate_mid_20hz_count = 0;
			return filter->refresh_rate;
		}

		filter->refresh_rate_mid_20hz_count++;
		if (filter->refresh_rate_mid_20hz_count == 2) {
			filter->refresh_rate = 10;
			filter->refresh_rate_mid_20hz_count = 1;
		}
		return filter->refresh_rate;
	}

	filter->refresh_rate_mid = 0;
	filter->refresh_rate_high++;
	high_count = filter->refresh_rate_high;

	if (measured_refresh_rate >= 66) {
		filter->refresh_rate_high_60hz_count = 0;
		filter->refresh_rate_high_30hz_count = 0;
		filter->refresh_rate_mid_20hz_count = 0;
		filter->refresh_rate_high_120hz_count++;
		if (filter->refresh_rate_high_120hz_count == 3) {
			filter->refresh_rate = 120;
			filter->refresh_rate_high_120hz_count = 2;
		}
	} else if (measured_refresh_rate >= 56) {
		filter->refresh_rate_high_120hz_count = 0;
		filter->refresh_rate_high_30hz_count = 0;
		filter->refresh_rate_mid_20hz_count = 0;
		filter->refresh_rate_high_60hz_count++;
		if (filter->refresh_rate_high_60hz_count == 3) {
			filter->refresh_rate = 60;
			filter->refresh_rate_high_60hz_count = 2;
		}
	} else {
		filter->refresh_rate_high_120hz_count = 0;
		filter->refresh_rate_high_60hz_count = 0;
		filter->refresh_rate_mid_20hz_count = 0;
		filter->refresh_rate_high_30hz_count++;
		if (filter->refresh_rate_high_30hz_count == 2) {
			filter->refresh_rate = 30;
			filter->refresh_rate_high_30hz_count = 1;
		}
	}

	if (high_count >= 3 && filter->refresh_rate <= 29) {
		filter->refresh_rate = 30;
		filter->refresh_rate_high = 1;
	}

	return filter->refresh_rate;
}
