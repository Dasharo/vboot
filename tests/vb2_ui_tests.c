/* Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Tests for developer and recovery mode UIs.
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2ui.h"
#include "2ui_private.h"
#include "test_common.h"
#include "vboot_kernel.h"

/* Fixed value for ignoring some checks. */
#define MOCK_IGNORE 0xffffu

/* Mock data */
struct display_call {
	const struct vb2_screen_info *screen;
	uint32_t locale_id;
	uint32_t selected_item;
	uint32_t disabled_item_mask;
};

static uint8_t workbuf[VB2_KERNEL_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static struct vb2_context *ctx;
static struct vb2_shared_data *sd;
static struct vb2_gbb_header gbb;

static struct vb2_ui_context mock_ui_context;
static struct vb2_screen_state *mock_state;

static struct display_call mock_displayed[64];
static int mock_displayed_count;
static int mock_displayed_i;

static int mock_calls_until_shutdown;

static uint32_t mock_key[64];
static int mock_key_trusted[64];
static int mock_key_count;
static int mock_key_total;

static uint64_t mock_get_timer_last;
static uint64_t mock_time;
static const uint64_t mock_time_start = 31ULL * VB_USEC_PER_SEC;
static int mock_vbexbeep_called;

static enum vb2_dev_default_boot mock_default_boot;
static int mock_dev_boot_allowed;
static int mock_dev_boot_legacy_allowed;
static int mock_dev_boot_usb_allowed;

static int mock_vbexlegacy_called;
static enum VbAltFwIndex_t mock_altfw_num_last;

static vb2_error_t mock_vbtlk_retval[32];
static uint32_t mock_vbtlk_expected_flag[32];
static int mock_vbtlk_count;
static int mock_vbtlk_total;

static void add_mock_key(uint32_t press, int trusted)
{
	if (mock_key_total >= ARRAY_SIZE(mock_key) ||
	    mock_key_total >= ARRAY_SIZE(mock_key_trusted)) {
		TEST_TRUE(0, "  mock_key ran out of entries!");
		return;
	}

	mock_key[mock_key_total] = press;
	mock_key_trusted[mock_key_total] = trusted;
	mock_key_total++;
}

static void add_mock_keypress(uint32_t press)
{
	add_mock_key(press, 0);
}

static void add_mock_vbtlk(vb2_error_t retval, uint32_t get_info_flags)
{
	if (mock_vbtlk_total >= ARRAY_SIZE(mock_vbtlk_retval) ||
	    mock_vbtlk_total >= ARRAY_SIZE(mock_vbtlk_expected_flag)) {
		TEST_TRUE(0, "  mock_vbtlk ran out of entries!");
		return;
	}

	mock_vbtlk_retval[mock_vbtlk_total] = retval;
	mock_vbtlk_expected_flag[mock_vbtlk_total] = get_info_flags;
	mock_vbtlk_total++;
}

static void displayed_eq(const char *text,
			 enum vb2_screen screen,
			 uint32_t locale_id,
			 uint32_t selected_item,
			 uint32_t disabled_item_mask)
{
	char text_buf[256];

	if (mock_displayed_i >= mock_displayed_count) {
		sprintf(text_buf, "  missing screen %s", text);
		TEST_TRUE(0, text_buf);
		return;
	}

	if (screen != MOCK_IGNORE) {
		sprintf(text_buf, "  screen of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].screen->id, screen,
			text_buf);
	}
	if (locale_id != MOCK_IGNORE) {
		sprintf(text_buf, "  locale_id of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].locale_id, locale_id,
			text_buf);
	}
	if (selected_item != MOCK_IGNORE) {
		sprintf(text_buf, "  selected_item of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].selected_item,
			selected_item, text_buf);
	}
	if (disabled_item_mask != MOCK_IGNORE) {
		sprintf(text_buf, "  disabled_item_mask of %s", text);
		TEST_EQ(mock_displayed[mock_displayed_i].disabled_item_mask,
			disabled_item_mask, text_buf);
	}
	mock_displayed_i++;
}

static void displayed_no_extra(void)
{
	if (mock_displayed_i == 0)
		TEST_EQ(mock_displayed_count, 0, "  no screen");
	else
		TEST_EQ(mock_displayed_count, mock_displayed_i,
			"  no extra screens");
}

/* Type of test to reset for */
enum reset_type {
	FOR_DEVELOPER,
	FOR_BROKEN_RECOVERY,
	FOR_MANUAL_RECOVERY,
};

/* Reset mock data (for use before each test) */
static void reset_common_data(enum reset_type t)
{
	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init failed");

	memset(&gbb, 0, sizeof(gbb));

	vb2_nv_init(ctx);

	if (t == FOR_DEVELOPER)
		ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;

	sd = vb2_get_sd(ctx);

	/* For try_recovery_action */
	invalid_disk_last = -1;

	/* Mock ui_context based on real screens */
	mock_ui_context = (struct vb2_ui_context){
		.ctx = ctx,
		.root_screen = vb2_get_screen_info(VB2_SCREEN_BLANK),
		.state = (struct vb2_screen_state){
			.screen = vb2_get_screen_info(VB2_SCREEN_BLANK),
			.selected_item = 0,
			.disabled_item_mask = 0,
		},
		.locale_id = 0,
		.key = 0,

	};
	mock_state = &mock_ui_context.state;

	/* For vb2ex_display_ui */
	memset(mock_displayed, 0, sizeof(mock_displayed));
	mock_displayed_count = 0;
	mock_displayed_i = 0;

	/* For shutdown_required */
	if (t == FOR_DEVELOPER)
		mock_calls_until_shutdown = 2000;  /* Larger than 30s */
	else
		mock_calls_until_shutdown = 10;

	/* For VbExKeyboardRead */
	memset(mock_key, 0, sizeof(mock_key));
	memset(mock_key_trusted, 0, sizeof(mock_key_trusted));
	mock_key_count = 0;
	mock_key_total = 0;
	/* Avoid iteration #0 which has a screen change by global action */
	add_mock_keypress(0);

	/* For vboot_audio.h */
	mock_get_timer_last = 0;
	mock_time = mock_time_start;
	mock_vbexbeep_called = 0;

	/* For dev_boot* in 2misc.h */
	mock_default_boot = VB2_DEV_DEFAULT_BOOT_DISK;
	mock_dev_boot_allowed = 1;
	mock_dev_boot_legacy_allowed = 0;
	mock_dev_boot_usb_allowed = 0;

	/* For VbExLegacy */
	mock_vbexlegacy_called = 0;
	mock_altfw_num_last = -100;

	/* For VbTryLoadKernel */
	memset(mock_vbtlk_retval, 0, sizeof(mock_vbtlk_retval));
	memset(mock_vbtlk_expected_flag, 0, sizeof(mock_vbtlk_expected_flag));
	mock_vbtlk_count = 0;
	mock_vbtlk_total = 0;
}

/* Mock functions */
struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *c)
{
	return &gbb;
}

vb2_error_t vb2ex_display_ui(enum vb2_screen screen,
			     uint32_t locale_id,
			     uint32_t selected_item,
			     uint32_t disabled_item_mask)
{
	VB2_DEBUG("displayed %d: screen = %#x, locale_id = %u, "
		  "selected_item = %u, disabled_item_mask = %#x\n",
		  mock_displayed_count, screen, locale_id, selected_item,
		  disabled_item_mask);

	if (mock_displayed_count >= ARRAY_SIZE(mock_displayed)) {
		TEST_TRUE(0, "  mock vb2ex_display_ui ran out of entries!");
		return VB2_ERROR_MOCK;
	}

	mock_displayed[mock_displayed_count] = (struct display_call){
		.screen = vb2_get_screen_info(screen),
		.locale_id = locale_id,
		.selected_item = selected_item,
		.disabled_item_mask = disabled_item_mask,
	};
	mock_displayed_count++;

	return VB2_SUCCESS;
}

uint32_t VbExIsShutdownRequested(void)
{
	if (mock_calls_until_shutdown < 0)  /* Never request shutdown */
		return 0;
	if (mock_calls_until_shutdown == 0)
		return 1;
	mock_calls_until_shutdown--;

	return 0;
}

uint32_t VbExKeyboardRead(void)
{
	return VbExKeyboardReadWithFlags(NULL);
}

uint32_t VbExKeyboardReadWithFlags(uint32_t *key_flags)
{
	if (mock_key_count < mock_key_total) {
		if (key_flags != NULL) {
			if (mock_key_trusted[mock_key_count])
				*key_flags = VB_KEY_FLAG_TRUSTED_KEYBOARD;
			else
				*key_flags = 0;
		}
		return mock_key[mock_key_count++];
	}

	return 0;
}

uint64_t VbExGetTimer(void)
{
	mock_get_timer_last = mock_time;
	return mock_time;
}

void VbExSleepMs(uint32_t msec)
{
	mock_time += msec * VB_USEC_PER_MSEC;
}

vb2_error_t VbExBeep(uint32_t msec, uint32_t frequency)
{
	mock_vbexbeep_called++;
	return VB2_SUCCESS;
}

enum vb2_dev_default_boot vb2_get_dev_boot_target(struct vb2_context *c)
{
	return mock_default_boot;
}

int vb2_dev_boot_allowed(struct vb2_context *c)
{
	return mock_dev_boot_allowed;
}

int vb2_dev_boot_legacy_allowed(struct vb2_context *c)
{
	return mock_dev_boot_legacy_allowed;
}

int vb2_dev_boot_usb_allowed(struct vb2_context *c)
{
	return mock_dev_boot_usb_allowed;
}

vb2_error_t VbExLegacy(enum VbAltFwIndex_t altfw_num)
{
	mock_vbexlegacy_called++;
	mock_altfw_num_last = altfw_num;

	return VB2_SUCCESS;
}

vb2_error_t VbTryLoadKernel(struct vb2_context *c, uint32_t get_info_flags)
{
	if (mock_vbtlk_total == 0) {
		TEST_TRUE(0, "  VbTryLoadKernel is not allowed!");
		return VB2_ERROR_MOCK;
	}

	/* Return last entry if called too many times */
	if (mock_vbtlk_count >= mock_vbtlk_total)
		mock_vbtlk_count = mock_vbtlk_total - 1;

	TEST_EQ(mock_vbtlk_expected_flag[mock_vbtlk_count], get_info_flags,
		"  unexpected get_info_flags");

	return mock_vbtlk_retval[mock_vbtlk_count++];
}

/* Tests */
static void developer_tests(void)
{
	VB2_DEBUG("Testing developer mode...\n");

	/* Proceed to internal disk after timeout */
	reset_common_data(FOR_DEVELOPER);
	add_mock_vbtlk(VB2_SUCCESS, VB_DISK_FLAG_FIXED);
	TEST_EQ(vb2_developer_menu(ctx), VB2_SUCCESS,
		"proceed to internal disk after timeout");
	displayed_eq("dev mode", VB2_SCREEN_DEVELOPER_MODE, MOCK_IGNORE,
		     MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();
	TEST_TRUE(mock_get_timer_last - mock_time_start >=
		  30 * VB_USEC_PER_SEC, "  finished delay");
	TEST_EQ(mock_vbexbeep_called, 2, "  beeped twice");
	TEST_EQ(mock_vbtlk_count, mock_vbtlk_total, "  used up mock_vbtlk");

	/* Proceed to USB after timeout */
	reset_common_data(FOR_DEVELOPER);
	add_mock_vbtlk(VB2_SUCCESS, VB_DISK_FLAG_REMOVABLE);
	mock_default_boot = VB2_DEV_DEFAULT_BOOT_USB;
	mock_dev_boot_usb_allowed = 1;
	TEST_EQ(vb2_developer_menu(ctx), VB2_SUCCESS,
		"proceed to USB after timeout");
	displayed_eq("dev mode", VB2_SCREEN_DEVELOPER_MODE, MOCK_IGNORE,
		     MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();
	TEST_TRUE(mock_get_timer_last - mock_time_start >=
		  30 * VB_USEC_PER_SEC, "  finished delay");
	TEST_EQ(mock_vbexbeep_called, 2, "  beeped twice");
	TEST_EQ(mock_vbtlk_count, mock_vbtlk_total, "  used up mock_vbtlk");

	/* Default boot USB not allowed, don't boot */
	reset_common_data(FOR_DEVELOPER);
	mock_default_boot = VB2_DEV_DEFAULT_BOOT_USB;
	TEST_EQ(vb2_developer_menu(ctx), VB2_REQUEST_SHUTDOWN,
		"default USB not allowed, don't boot");
	displayed_eq("dev mode", VB2_SCREEN_DEVELOPER_MODE, MOCK_IGNORE,
		     MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();
	TEST_TRUE(mock_get_timer_last - mock_time_start >=
		  30 * VB_USEC_PER_SEC, "  finished delay");
	TEST_EQ(mock_vbexbeep_called, 2, "  beeped twice");
	TEST_EQ(mock_vbtlk_count, mock_vbtlk_total, "  used up mock_vbtlk");

	VB2_DEBUG("...done.\n");
}

static void broken_recovery_tests(void)
{
	VB2_DEBUG("Testing broken recovery mode...\n");

	/* BROKEN screen shutdown request */
	if (!DETACHABLE) {
		reset_common_data(FOR_BROKEN_RECOVERY);
		add_mock_keypress(VB_BUTTON_POWER_SHORT_PRESS);
		mock_calls_until_shutdown = -1;
		TEST_EQ(vb2_broken_recovery_menu(ctx),
			VB2_REQUEST_SHUTDOWN,
			"power button short pressed = shutdown");
		displayed_eq("broken screen", VB2_SCREEN_RECOVERY_BROKEN,
			     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
		displayed_no_extra();
	}

	/* Shortcuts that are always ignored in BROKEN */
	reset_common_data(FOR_BROKEN_RECOVERY);
	add_mock_key(VB_KEY_CTRL('D'), 1);
	add_mock_key(VB_KEY_CTRL('U'), 1);
	add_mock_key(VB_KEY_CTRL('L'), 1);
	add_mock_key(VB_BUTTON_VOL_UP_DOWN_COMBO_PRESS, 1);
	add_mock_key(VB_BUTTON_VOL_UP_LONG_PRESS, 1);
	add_mock_key(VB_BUTTON_VOL_DOWN_LONG_PRESS, 1);
	TEST_EQ(vb2_broken_recovery_menu(ctx), VB2_REQUEST_SHUTDOWN,
		"Shortcuts ignored in BROKEN");
	TEST_EQ(mock_calls_until_shutdown, 0, "  loop forever");
	displayed_eq("broken screen", VB2_SCREEN_RECOVERY_BROKEN,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	VB2_DEBUG("...done.\n");
}

static void manual_recovery_tests(void)
{
	VB2_DEBUG("Testing manual recovery mode...\n");

	/* Timeout, shutdown */
	reset_common_data(FOR_MANUAL_RECOVERY);
	add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND, VB_DISK_FLAG_REMOVABLE);
	TEST_EQ(vb2_manual_recovery_menu(ctx), VB2_REQUEST_SHUTDOWN,
		"timeout, shutdown");
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	/* Power button short pressed = shutdown request */
	if (!DETACHABLE) {
		reset_common_data(FOR_MANUAL_RECOVERY);
		add_mock_keypress(VB_BUTTON_POWER_SHORT_PRESS);
		add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND,
			       VB_DISK_FLAG_REMOVABLE);
		TEST_EQ(vb2_manual_recovery_menu(ctx),
			VB2_REQUEST_SHUTDOWN,
			"power button short pressed = shutdown");
		displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
			     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
		displayed_no_extra();
	}

	/* Item 1 = phone recovery */
	reset_common_data(FOR_MANUAL_RECOVERY);
	add_mock_keypress(VB_KEY_ENTER);
	add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND, VB_DISK_FLAG_REMOVABLE);
	TEST_EQ(vb2_manual_recovery_menu(ctx), VB2_REQUEST_SHUTDOWN,
		"phone recovery");
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, 0, MOCK_IGNORE);
	displayed_eq("phone recovery", VB2_SCREEN_RECOVERY_PHONE_STEP1,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	/* Item 2 = external disk recovery */
	reset_common_data(FOR_MANUAL_RECOVERY);
	add_mock_keypress(VB_KEY_DOWN);
	add_mock_keypress(VB_KEY_ENTER);
	add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND, VB_DISK_FLAG_REMOVABLE);
	TEST_EQ(vb2_manual_recovery_menu(ctx), VB2_REQUEST_SHUTDOWN,
		"external disk recovery");
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, 0, MOCK_IGNORE);
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, 1, MOCK_IGNORE);
	displayed_eq("disk recovery", VB2_SCREEN_RECOVERY_DISK_STEP1,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	/* Boots if we have a valid image on first try */
	reset_common_data(FOR_MANUAL_RECOVERY);
	add_mock_vbtlk(VB2_SUCCESS, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_ERROR_MOCK, VB_DISK_FLAG_REMOVABLE);
	TEST_EQ(vb2_manual_recovery_menu(ctx), VB2_SUCCESS,
		"boots if valid on first try");
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	/* Boots eventually if we get a valid image later */
	reset_common_data(FOR_MANUAL_RECOVERY);
	add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_SUCCESS, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_ERROR_MOCK, VB_DISK_FLAG_REMOVABLE);
	TEST_EQ(vb2_manual_recovery_menu(ctx), VB2_SUCCESS,
		"boots after valid image appears");
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	/* Invalid image, then remove, then valid image */
	reset_common_data(FOR_MANUAL_RECOVERY);
	add_mock_vbtlk(VB2_ERROR_MOCK, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_ERROR_LK_NO_DISK_FOUND, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_SUCCESS, VB_DISK_FLAG_REMOVABLE);
	add_mock_vbtlk(VB2_ERROR_MOCK, VB_DISK_FLAG_REMOVABLE);
	TEST_EQ(vb2_manual_recovery_menu(ctx), VB2_SUCCESS,
		"boots after valid image appears");
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_INVALID,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_eq("recovery select", VB2_SCREEN_RECOVERY_SELECT,
		     MOCK_IGNORE, MOCK_IGNORE, MOCK_IGNORE);
	displayed_no_extra();

	VB2_DEBUG("...done.\n");
}

int main(void)
{
	developer_tests();
	broken_recovery_tests();
	manual_recovery_tests();

	return gTestSuccess ? 0 : 255;
}