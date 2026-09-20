// SPDX-License-Identifier: GPL-2.0-only
/*
 * Meizu 21 Pro adaptive frame-rate support.
 */

#include <linux/atomic.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kobject.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "../dsi/dsi_clk.h"
#include "../dsi/dsi_display.h"
#include "../dsi/dsi_panel.h"
#include "meizu_display_adfr.h"
#include "meizu_display_adfr_logic.h"
#include "meizu_display_brightness.h"

#define MEIZU_PANEL_NAME "NT37290_BOE_DSC"
#define MEIZU_BACKLIGHT_HBM_THRESHOLD 1470
#define MEIZU_IDLE_DROP_DELAY_NS NSEC_PER_SEC

static_assert(DSI_CMD_SET_MEIZU_AOD_HIGH_NOLP == 19);
static_assert(DSI_CMD_SET_MEIZU_AOD_LOW_NOLP == 20);
static_assert(DSI_CMD_SET_MEIZU_ADFR_ON == 27);
static_assert(DSI_CMD_SET_MEIZU_ADFR_OFF == 28);

struct meizu_display_adfr {
	struct dsi_display *display;
	struct kobject *kobj;
	struct kthread_worker worker;
	struct task_struct *worker_task;
	struct kthread_work drop_work;
	struct hrtimer drop_timer;
	struct meizu_dynamic_te_filter dynamic_filter;
	ktime_t last_te_timestamp;
	atomic_t supported;
	atomic_t min_fps;
	atomic_t last_min_fps;
	atomic_t last_refresh_rate;
	atomic_t backlight_type;
	atomic_t timer_active;
	atomic_t exit_idle_count;
	atomic_t dynamic_te_enabled;
	atomic_t dynamic_te_read_count;
	atomic_t dynamic_te_read_rate;
	atomic_t enter_1hz_count;
	atomic_t aod_light_mode;
	atomic_t aod_presets_light_mode;
	atomic_t aod_seamless_transition;
	atomic_t debug_mode;
	atomic_t debug_saved_min_fps;
	int dynamic_te_gpio;
	int dynamic_te_irq;
	bool dynamic_te_irq_registered;
	bool dynamic_te_irq_enabled;
	bool initialized;
};

static DEFINE_MUTEX(meizu_adfr_lifecycle_lock);
static DEFINE_MUTEX(meizu_star_lock);
static char meizu_star_brightness[128] = "0 0.0";
static struct meizu_display_adfr meizu_adfr = {
	.dynamic_te_gpio = -EINVAL,
	.dynamic_te_irq = -EINVAL,
};

static bool meizu_adfr_panel_supported(const struct dsi_panel *panel)
{
	return panel && panel->name && !strcmp(panel->name, MEIZU_PANEL_NAME);
}

bool meizu_display_adfr_is_supported(void)
{
	return atomic_read(&meizu_adfr.supported) != 0;
}

static int meizu_adfr_patch_min_fps(struct dsi_panel_cmd_set *set, u32 min_fps)
{
	struct dsi_cmd_desc *cmd;
	u8 *payload;

	if (!set || set->count <= 2 || !set->cmds)
		return -EINVAL;

	cmd = &set->cmds[2];
	payload = (u8 *)cmd->msg.tx_buf;
	if (!meizu_bf197_patch_min_fps(payload, cmd->msg.tx_len, min_fps))
		return -EINVAL;

	return 0;
}

static int meizu_adfr_send_min_fps_locked(struct dsi_panel *panel, u32 min_fps)
{
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_panel_cmd_set *set;
	u32 previous;
	int rc;

	if (!meizu_adfr_panel_supported(panel) || !panel->cur_mode ||
	    !panel->cur_mode->priv_info)
		return -EINVAL;

	if (!meizu_adfr_min_fps_valid(min_fps))
		return -EINVAL;
	priv_info = panel->cur_mode->priv_info;

	if (!min_fps) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MEIZU_ADFR_OFF);
	} else {
		set = &priv_info->cmd_sets[DSI_CMD_SET_MEIZU_ADFR_ON];
		rc = meizu_adfr_patch_min_fps(set, min_fps);
		if (rc)
			return rc;
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MEIZU_ADFR_ON);
	}

	previous = atomic_xchg(&meizu_adfr.min_fps, min_fps);
	atomic_set(&meizu_adfr.last_min_fps, previous);
	return rc;
}

static int meizu_adfr_update_min_fps(u32 min_fps)
{
	struct dsi_display *display = READ_ONCE(meizu_adfr.display);
	struct dsi_panel *panel;
	bool clocks_enabled = false;
	int rc = 0;

	if (!meizu_display_adfr_is_supported() || !display || !display->panel)
		return -ENODEV;

	if (!meizu_adfr_min_fps_valid(min_fps))
		return -EINVAL;
	if (atomic_read(&meizu_adfr.min_fps) == min_fps)
		return 0;

	panel = display->panel;
	if (panel->power_mode != SDE_MODE_DPMS_ON)
		return -EBUSY;

	mutex_lock(&display->display_lock);
	if (panel->panel_mode == DSI_OP_CMD_MODE && display->dsi_clk_handle) {
		rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
					  DSI_CLK_ON);
		if (rc)
			goto unlock_display;
		clocks_enabled = true;
	}

	mutex_lock(&panel->panel_lock);
	if (dsi_panel_initialized(panel))
		rc = meizu_adfr_send_min_fps_locked(panel, min_fps);
	else
		rc = -EBUSY;
	mutex_unlock(&panel->panel_lock);

	if (clocks_enabled)
		dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
				     DSI_CLK_OFF);

unlock_display:
	mutex_unlock(&display->display_lock);
	return rc;
}

static void meizu_adfr_status_switch(bool enable)
{
	u32 min_fps;

	if (!enable) {
		meizu_adfr_update_min_fps(0);
		return;
	}

	min_fps = atomic_read(&meizu_adfr.last_min_fps);
	if (min_fps == 1)
		min_fps = 32;
	if (meizu_adfr_min_fps_valid(min_fps))
		meizu_adfr_update_min_fps(min_fps);
}

void meizu_display_adfr_active(void)
{
	if (meizu_display_adfr_is_supported())
		meizu_adfr_update_min_fps(48);
}

void meizu_display_adfr_panel_disable(void)
{
	if (!meizu_display_adfr_is_supported())
		return;

	hrtimer_cancel(&meizu_adfr.drop_timer);
	atomic_set(&meizu_adfr.timer_active, 0);
	atomic_set(&meizu_adfr.min_fps, 0);
	atomic_set(&meizu_adfr.aod_light_mode, 2);
}

void meizu_display_adfr_before_backlight(struct dsi_panel *panel)
{
	u32 backlight;
	u32 type;

	if (!meizu_display_adfr_is_supported() ||
	    !meizu_adfr_panel_supported(panel))
		return;

	backlight = panel->bl_config.bl_level;
	type = atomic_read(&meizu_adfr.backlight_type);
	if (!((backlight > MEIZU_BACKLIGHT_HBM_THRESHOLD && type == 0) ||
	      (backlight <= MEIZU_BACKLIGHT_HBM_THRESHOLD && type == 1)))
		return;

	atomic_set(&meizu_adfr.last_min_fps, atomic_read(&meizu_adfr.min_fps));
	meizu_adfr_send_min_fps_locked(panel, 0);
}

void meizu_display_adfr_after_backlight(struct dsi_panel *panel)
{
	u32 backlight;

	if (!meizu_display_adfr_is_supported() ||
	    !meizu_adfr_panel_supported(panel))
		return;

	if (atomic_read(&meizu_adfr.last_min_fps)) {
		meizu_adfr_send_min_fps_locked(panel, 48);
		atomic_set(&meizu_adfr.last_min_fps, 0);
	}

	backlight = panel->bl_config.bl_level;
	atomic_set(&meizu_adfr.backlight_type,
		   backlight > MEIZU_BACKLIGHT_HBM_THRESHOLD);
}

static bool meizu_adfr_regular_refresh_rate(u32 refresh_rate)
{
	return refresh_rate == 30 || refresh_rate == 60 || refresh_rate == 120;
}

void meizu_display_adfr_mode_switch(struct dsi_panel *panel)
{
	u32 last_refresh_rate;
	u32 refresh_rate;

	if (!meizu_display_adfr_is_supported() ||
	    !meizu_adfr_panel_supported(panel) || !panel->cur_mode)
		return;

	refresh_rate = panel->cur_mode->timing.refresh_rate;
	last_refresh_rate = atomic_read(&meizu_adfr.last_refresh_rate);
	if (meizu_adfr_regular_refresh_rate(refresh_rate) &&
	    last_refresh_rate == 90) {
		meizu_adfr_send_min_fps_locked(panel, 48);
	} else if (refresh_rate == 90 &&
		   meizu_adfr_regular_refresh_rate(last_refresh_rate)) {
		atomic_set(&meizu_adfr.min_fps, 0);
	}
	atomic_set(&meizu_adfr.last_refresh_rate, refresh_rate);
}

static void meizu_adfr_drop_work(struct kthread_work *work)
{
	if (atomic_read(&meizu_adfr.timer_active) &&
	    atomic_read(&meizu_adfr.min_fps) == 48)
		meizu_adfr_update_min_fps(1);
}

static enum hrtimer_restart meizu_adfr_drop_timer(struct hrtimer *timer)
{
	kthread_queue_work(&meizu_adfr.worker, &meizu_adfr.drop_work);
	return HRTIMER_NORESTART;
}

void meizu_display_adfr_handle_idle(bool enter_idle)
{
	struct dsi_display *display = READ_ONCE(meizu_adfr.display);
	struct dsi_panel *panel;
	int exit_count;

	if (!meizu_display_adfr_is_supported() || !display || !display->panel)
		return;

	panel = display->panel;
	if (panel->panel_mode != DSI_OP_CMD_MODE ||
	    panel->power_mode != SDE_MODE_DPMS_ON ||
	    !dsi_panel_initialized(panel) || !atomic_read(&meizu_adfr.min_fps))
		return;

	if (enter_idle) {
		exit_count = atomic_read(&meizu_adfr.exit_idle_count);
		if (!atomic_read(&meizu_adfr.dynamic_te_enabled) &&
		    atomic_read(&meizu_adfr.min_fps) != 48)
			meizu_adfr_update_min_fps(48);
		if (exit_count >= 2 &&
		    !atomic_xchg(&meizu_adfr.timer_active, 1))
			hrtimer_start(&meizu_adfr.drop_timer,
				      ns_to_ktime(MEIZU_IDLE_DROP_DELAY_NS),
				      HRTIMER_MODE_REL);
		atomic_set(&meizu_adfr.exit_idle_count, 0);
		return;
	}

	atomic_inc(&meizu_adfr.exit_idle_count);
	if (atomic_xchg(&meizu_adfr.timer_active, 0))
		hrtimer_cancel(&meizu_adfr.drop_timer);
	if (atomic_read(&meizu_adfr.min_fps) != 48)
		meizu_adfr_update_min_fps(48);
}

bool meizu_display_adfr_needs_nolp(const struct dsi_panel *panel)
{
	if (!meizu_adfr_panel_supported(panel) ||
	    !meizu_display_adfr_is_supported())
		return true;

	return atomic_read(&meizu_adfr.aod_light_mode) != 2;
}

enum dsi_cmd_set_type meizu_display_adfr_nolp_cmd(const struct dsi_panel *panel)
{
	if (!meizu_adfr_panel_supported(panel) ||
	    !meizu_display_adfr_is_supported() ||
	    !atomic_read(&meizu_adfr.aod_seamless_transition))
		return DSI_CMD_SET_NOLP;

	switch (atomic_read(&meizu_adfr.aod_light_mode)) {
	case 1:
		return DSI_CMD_SET_MEIZU_AOD_HIGH_NOLP;
	case 0:
		return DSI_CMD_SET_MEIZU_AOD_LOW_NOLP;
	default:
		return DSI_CMD_SET_NOLP;
	}
}

static u32 meizu_adfr_dynamic_te_rate(void)
{
	u32 refresh_rate = atomic_read(&meizu_adfr.dynamic_te_read_rate);
	u32 count;

	count = atomic_inc_return(&meizu_adfr.dynamic_te_read_count);
	if (count != 2)
		return refresh_rate;

	atomic_set(&meizu_adfr.dynamic_te_read_count, 0);
	refresh_rate = READ_ONCE(meizu_adfr.dynamic_filter.refresh_rate);
	atomic_set(&meizu_adfr.dynamic_te_read_rate, refresh_rate);

	if (atomic_read(&meizu_adfr.min_fps) == 48 &&
	    atomic_read(&meizu_adfr.timer_active)) {
		count = atomic_inc_return(&meizu_adfr.enter_1hz_count);
		if (count >= 4) {
			atomic_set(&meizu_adfr.enter_1hz_count, 3);
			refresh_rate = 1;
			atomic_set(&meizu_adfr.dynamic_te_read_rate, 1);
		}
	} else if (atomic_read(&meizu_adfr.min_fps) == 1) {
		refresh_rate = 1;
		atomic_set(&meizu_adfr.dynamic_te_read_rate, 1);
	} else {
		atomic_set(&meizu_adfr.enter_1hz_count, 0);
	}

	return refresh_rate;
}

static irqreturn_t meizu_adfr_dynamic_te_irq(int irq, void *data)
{
	struct dsi_display *display = data;
	struct dsi_panel *panel;
	ktime_t now;
	s64 delta_ms;
	u32 measured_refresh_rate = 0;
	u32 panel_refresh_rate;

	if (!display || !display->panel || !display->panel->cur_mode)
		return IRQ_HANDLED;

	panel = display->panel;
	now = ktime_get();
	delta_ms = ktime_ms_delta(now, meizu_adfr.last_te_timestamp);
	meizu_adfr.last_te_timestamp = now;
	if (delta_ms > 0)
		measured_refresh_rate = 1000 / delta_ms;

	panel_refresh_rate = panel->cur_mode->timing.refresh_rate;
	meizu_bf197_filter_dynamic_te(&meizu_adfr.dynamic_filter,
				      panel_refresh_rate,
				      atomic_read(&meizu_adfr.min_fps),
				      measured_refresh_rate);
	return IRQ_HANDLED;
}

static ssize_t adfr_minfps_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "display_panel_adfr_is_support=%d minfps=%d\n",
			  meizu_display_adfr_is_supported(),
			  atomic_read(&meizu_adfr.min_fps));
}

static ssize_t adfr_minfps_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count)
{
	unsigned int first;
	unsigned int second;
	int consumed = 0;
	int parsed;

	parsed = sscanf(buf, "%x%n", &first, &consumed);
	if (parsed < 1)
		return -EINVAL;

	parsed = sscanf(buf + consumed, "%x", &second);
	if (parsed < 1) {
		meizu_adfr_status_switch(first != 0);
		return count;
	}

	if (!first) {
		if (!atomic_xchg(&meizu_adfr.debug_mode, 1))
			atomic_set(&meizu_adfr.debug_saved_min_fps,
				   atomic_read(&meizu_adfr.min_fps));
		meizu_adfr_status_switch(false);
		meizu_adfr_update_min_fps(second);
	} else if (first == 1 && atomic_xchg(&meizu_adfr.debug_mode, 0)) {
		meizu_adfr_status_switch(true);
		second = atomic_read(&meizu_adfr.debug_saved_min_fps);
		meizu_adfr_update_min_fps(second);
	}

	return count;
}

static ssize_t dynamic_te_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", meizu_adfr_dynamic_te_rate());
}

static ssize_t dynamic_te_store(struct kobject *kobj,
				struct kobj_attribute *attr, const char *buf,
				size_t count)
{
	unsigned int enabled;

	if (kstrtouint(buf, 0, &enabled))
		return -EINVAL;
	if (!meizu_adfr.dynamic_te_irq_registered)
		return -ENODEV;

	enabled = !!enabled;
	if (enabled && !meizu_adfr.dynamic_te_irq_enabled) {
		enable_irq(meizu_adfr.dynamic_te_irq);
		meizu_adfr.dynamic_te_irq_enabled = true;
	} else if (!enabled && meizu_adfr.dynamic_te_irq_enabled) {
		disable_irq(meizu_adfr.dynamic_te_irq);
		meizu_adfr.dynamic_te_irq_enabled = false;
	}
	atomic_set(&meizu_adfr.dynamic_te_enabled, enabled);
	return count;
}

static ssize_t aod_light_mode_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&meizu_adfr.aod_light_mode));
}

static ssize_t aod_light_mode_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	struct dsi_display *display = READ_ONCE(meizu_adfr.display);
	struct dsi_panel *panel;
	unsigned int mode;
	bool clocks_enabled = false;
	int rc;

	if (kstrtouint(buf, 0, &mode) || mode > 1)
		return -EINVAL;
	atomic_set(&meizu_adfr.aod_presets_light_mode, mode);
	if (!display || !display->panel)
		return count;

	panel = display->panel;
	if (panel->power_mode != SDE_MODE_DPMS_LP1 &&
	    panel->power_mode != SDE_MODE_DPMS_LP2)
		return count;
	if (atomic_read(&meizu_adfr.aod_light_mode) == mode)
		return count;

	mutex_lock(&display->display_lock);
	if (panel->panel_mode == DSI_OP_CMD_MODE && display->dsi_clk_handle) {
		rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
					  DSI_CLK_ON);
		if (rc)
			goto unlock_display;
		clocks_enabled = true;
	}
	mutex_lock(&panel->panel_lock);
	dsi_panel_tx_cmd_set(panel, mode ? DSI_CMD_SET_MEIZU_AOD_HIGH :
					   DSI_CMD_SET_MEIZU_AOD_LOW);
	atomic_set(&meizu_adfr.aod_light_mode, mode);
	mutex_unlock(&panel->panel_lock);
	if (clocks_enabled)
		dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS,
				     DSI_CLK_OFF);

unlock_display:
	mutex_unlock(&display->display_lock);
	return count;
}

static ssize_t aod_seamless_transition_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	unsigned int enabled;

	if (kstrtouint(buf, 0, &enabled))
		return -EINVAL;
	atomic_set(&meizu_adfr.aod_seamless_transition, !!enabled);
	return count;
}

static ssize_t star_brightness_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	ssize_t length;

	mutex_lock(&meizu_star_lock);
	length = sysfs_emit(buf, "%s\n", meizu_star_brightness);
	mutex_unlock(&meizu_star_lock);
	return length;
}

static ssize_t star_brightness_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	char backlight[32];
	char pcc[32];

	if (sscanf(buf, "%31s %31s", backlight, pcc) != 2)
		return -EINVAL;

	mutex_lock(&meizu_star_lock);
	scnprintf(meizu_star_brightness, sizeof(meizu_star_brightness), "%s %s",
		  backlight, pcc);
	mutex_unlock(&meizu_star_lock);
	return count;
}

static ssize_t synclights_enable_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  display_brightness_is_enable_synclights());
}

static ssize_t synclights_enable_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int enabled;

	if (kstrtouint(buf, 0, &enabled))
		return -EINVAL;
	WRITE_ONCE(synclights_enable, !!enabled);
	return count;
}

static struct kobj_attribute display_attr_adfr_minfps =
	__ATTR(adfr_minfps, 0644, adfr_minfps_show, adfr_minfps_store);
static struct kobj_attribute display_attr_dynamic_te =
	__ATTR(dynamic_te, 0644, dynamic_te_show, dynamic_te_store);
static struct kobj_attribute display_attr_aod_light_mode =
	__ATTR(aod_light_mode, 0644, aod_light_mode_show, aod_light_mode_store);
static struct kobj_attribute display_attr_aod_seamless_transition =
	__ATTR(aod_seamless_transition_status, 0644, NULL,
	       aod_seamless_transition_store);
static struct kobj_attribute display_attr_star_brightness =
	__ATTR(star_brightness, 0644, star_brightness_show,
	       star_brightness_store);
static struct kobj_attribute display_attr_synclights_enable =
	__ATTR(synclights_enable, 0644, synclights_enable_show,
	       synclights_enable_store);

static struct attribute *meizu_display_attrs[] = {
	&display_attr_adfr_minfps.attr,
	&display_attr_dynamic_te.attr,
	&display_attr_aod_light_mode.attr,
	&display_attr_aod_seamless_transition.attr,
	&display_attr_star_brightness.attr,
	&display_attr_synclights_enable.attr,
	NULL,
};

static const struct attribute_group meizu_display_attr_group = {
	.attrs = meizu_display_attrs,
};

static void meizu_display_adfr_release(void *data)
{
	struct dsi_display *display = data;

	mutex_lock(&meizu_adfr_lifecycle_lock);
	if (!meizu_adfr.initialized || meizu_adfr.display != display) {
		mutex_unlock(&meizu_adfr_lifecycle_lock);
		return;
	}

	if (meizu_adfr.dynamic_te_irq_enabled)
		disable_irq(meizu_adfr.dynamic_te_irq);
	hrtimer_cancel(&meizu_adfr.drop_timer);
	kthread_flush_worker(&meizu_adfr.worker);
	if (meizu_adfr.worker_task)
		kthread_stop(meizu_adfr.worker_task);
	if (meizu_adfr.kobj) {
		sysfs_remove_group(meizu_adfr.kobj, &meizu_display_attr_group);
		kobject_put(meizu_adfr.kobj);
	}

	meizu_adfr.display = NULL;
	meizu_adfr.kobj = NULL;
	meizu_adfr.worker_task = NULL;
	meizu_adfr.dynamic_te_irq_registered = false;
	meizu_adfr.dynamic_te_irq_enabled = false;
	meizu_adfr.initialized = false;
	atomic_set(&meizu_adfr.supported, 0);
	mutex_unlock(&meizu_adfr_lifecycle_lock);
}

int meizu_display_adfr_init(struct dsi_display *display)
{
	struct device *dev;
	struct dsi_panel *panel;
	u32 config = 0;
	int irq;
	int rc;

	if (!display || !display->panel)
		return -EINVAL;
	dev = &display->pdev->dev;
	panel = display->panel;
	if (!meizu_adfr_panel_supported(panel))
		return 0;

	mutex_lock(&meizu_adfr_lifecycle_lock);
	if (meizu_adfr.initialized) {
		mutex_unlock(&meizu_adfr_lifecycle_lock);
		return 0;
	}

	of_property_read_u32(panel->panel_of_node, "qcom,display-adfr-config",
			     &config);
	if (!config) {
		mutex_unlock(&meizu_adfr_lifecycle_lock);
		return 0;
	}

	meizu_adfr.display = display;
	memset(&meizu_adfr.dynamic_filter, 0,
	       sizeof(meizu_adfr.dynamic_filter));
	meizu_adfr.dynamic_te_irq_registered = false;
	meizu_adfr.dynamic_te_irq_enabled = false;
	atomic_set(&meizu_adfr.supported, 1);
	atomic_set(&meizu_adfr.last_refresh_rate, 120);
	atomic_set(&meizu_adfr.backlight_type, 3);
	atomic_set(&meizu_adfr.dynamic_te_read_rate, 120);
	atomic_set(&meizu_adfr.aod_light_mode, 2);
	atomic_set(&meizu_adfr.aod_presets_light_mode, 2);

	kthread_init_worker(&meizu_adfr.worker);
	kthread_init_work(&meizu_adfr.drop_work, meizu_adfr_drop_work);
	meizu_adfr.worker_task = kthread_run(kthread_worker_fn,
					     &meizu_adfr.worker, "meizu_adfr");
	if (IS_ERR(meizu_adfr.worker_task)) {
		rc = PTR_ERR(meizu_adfr.worker_task);
		meizu_adfr.worker_task = NULL;
		goto fail;
	}

	hrtimer_init(&meizu_adfr.drop_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	meizu_adfr.drop_timer.function = meizu_adfr_drop_timer;

	meizu_adfr.dynamic_te_gpio =
		of_get_named_gpio(panel->panel_of_node,
				  "qcom,dynamic-te-gpio", 0);
	if (gpio_is_valid(meizu_adfr.dynamic_te_gpio) &&
	    !display->trusted_vm_env) {
		meizu_adfr.dynamic_te_irq =
			gpio_to_irq(meizu_adfr.dynamic_te_gpio);
		if (meizu_adfr.dynamic_te_irq > 0) {
			irq = meizu_adfr.dynamic_te_irq;
			irq_set_status_flags(meizu_adfr.dynamic_te_irq,
					     IRQ_DISABLE_UNLAZY);
			rc = devm_request_threaded_irq(dev, irq, NULL,
						       meizu_adfr_dynamic_te_irq,
					       IRQF_TRIGGER_RISING |
						       IRQF_ONESHOT,
					       "DYNAMIC_TE_GPIO", display);
			if (!rc) {
				disable_irq(meizu_adfr.dynamic_te_irq);
				meizu_adfr.dynamic_te_irq_registered = true;
			}
		}
	}

	meizu_adfr.kobj =
		kobject_create_and_add("display_drivers", kernel_kobj);
	if (!meizu_adfr.kobj) {
		rc = -ENOMEM;
		goto fail_worker;
	}
	rc = sysfs_create_group(meizu_adfr.kobj, &meizu_display_attr_group);
	if (rc)
		goto fail_kobject;

	meizu_adfr.initialized = true;
	mutex_unlock(&meizu_adfr_lifecycle_lock);
	rc = devm_add_action(&display->pdev->dev, meizu_display_adfr_release,
			     display);
	if (rc)
		meizu_display_adfr_release(display);
	return rc;

fail_kobject:
	kobject_put(meizu_adfr.kobj);
	meizu_adfr.kobj = NULL;
fail_worker:
	if (meizu_adfr.dynamic_te_irq_registered) {
		devm_free_irq(&display->pdev->dev, meizu_adfr.dynamic_te_irq,
			      display);
		irq_clear_status_flags(meizu_adfr.dynamic_te_irq,
				       IRQ_DISABLE_UNLAZY);
		meizu_adfr.dynamic_te_irq_registered = false;
	}
	hrtimer_cancel(&meizu_adfr.drop_timer);
	kthread_stop(meizu_adfr.worker_task);
	meizu_adfr.worker_task = NULL;
fail:
	meizu_adfr.display = NULL;
	atomic_set(&meizu_adfr.supported, 0);
	mutex_unlock(&meizu_adfr_lifecycle_lock);
	return rc;
}
