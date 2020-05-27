/* Copyright 2020 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * User interfaces for developer and recovery mode menus.
 */

#ifndef VBOOT_REFERENCE_2UI_H_
#define VBOOT_REFERENCE_2UI_H_

#include <2api.h>
#include <2sysincludes.h>

/*****************************************************************************/
/* Data structures */

struct vb2_ui_context;  /* Forward declaration */

struct vb2_screen_info {
	/* Screen id */
	enum vb2_screen id;
	/* Screen name for printing to console only */
	const char *name;
	/* Init function runs once when changing to the screen. */
	vb2_error_t (*init)(struct vb2_ui_context *ui);
	/* Action function runs repeatedly while on the screen. */
	vb2_error_t (*action)(struct vb2_ui_context *ui);
	/* Number of menu items */
	uint16_t num_items;
	/* List of menu items */
	const struct vb2_menu_item *items;
};

struct vb2_menu_item {
	/* Text description */
	const char *text;
	/* Target screen */
	enum vb2_screen target;
	/* Action function takes precedence over target screen if non-NULL. */
	vb2_error_t (*action)(struct vb2_ui_context *ui);
};

struct vb2_screen_state {
	const struct vb2_screen_info *screen;
	uint32_t selected_item;
	uint32_t disabled_item_mask;
};

enum vb2_power_button {
	VB2_POWER_BUTTON_HELD_SINCE_BOOT = 0,
	VB2_POWER_BUTTON_RELEASED,
	VB2_POWER_BUTTON_PRESSED,  /* Must have been previously released */
};

struct vb2_ui_context {
	struct vb2_context *ctx;
	const struct vb2_screen_info *root_screen;
	struct vb2_screen_state state;
	uint32_t locale_id;
	uint32_t key;
	int key_trusted;

	/* For check_shutdown_request. */
	enum vb2_power_button power_button;

	/* For developer mode. */
	int disable_timer;
	uint64_t start_time;
	int beep_count;

	/* For manual recovery. */
	vb2_error_t recovery_rv;

	/* For to_dev transition flow. */
	int physical_presence_button_pressed;
};

vb2_error_t vb2_ui_developer_mode_boot_internal_action(
	struct vb2_ui_context *ui);
vb2_error_t vb2_ui_developer_mode_boot_external_action(
	struct vb2_ui_context *ui);

/**
 * Get info struct of a screen.
 *
 * @param screen	Screen from enum vb2_screen
 *
 * @return screen info struct on success, NULL on error.
 */
const struct vb2_screen_info *vb2_get_screen_info(enum vb2_screen id);

/*****************************************************************************/
/* Menu navigation functions */

/**
 * Move selection to the previous menu item.
 *
 * Update selected_item, taking into account disabled indices (from
 * disabled_item_mask).  The selection does not wrap, meaning that we block
 * on 0 when we hit the start of the menu.
 *
 * @param ui		UI context pointer
 * @return VB2_REQUEST_UI_CONTINUE, or error code on error.
 */
vb2_error_t vb2_ui_menu_prev(struct vb2_ui_context *ui);

/**
 * Move selection to the next menu item.
 *
 * Update selected_item, taking into account disabled indices (from
 * disabled_item_mask).  The selection does not wrap, meaning that we block
 * on the max index when we hit the end of the menu.
 *
 * @param ui		UI context pointer
 * @return VB2_REQUEST_UI_CONTINUE, or error code on error.
 */
vb2_error_t vb2_ui_menu_next(struct vb2_ui_context *ui);

/**
 * Select the current menu item.
 *
 * If the current menu item has an action associated with it, run the action.
 * Otherwise, navigate to the target screen.  If neither of these are set, then
 * selecting the menu item is a no-op.
 *
 * @param ui		UI context pointer
 * @return VB2_REQUEST_UI_CONTINUE, or error code on error.
 */
vb2_error_t vb2_ui_menu_select(struct vb2_ui_context *ui);

/*****************************************************************************/
/* Screen navigation functions */

/**
 * Return back to the root screen.
 *
 * Return to the root screen originally provided to the ui_loop() function.
 *
 * @param ui		UI context pointer
 * @return VB2_REQUEST_UI_CONTINUE, or error code on error.
 */
vb2_error_t vb2_ui_change_root(struct vb2_ui_context *ui);

/**
 * Change to the given screen.
 *
 * If the screen is not found, the request is ignored.
 *
 * @param ui		UI context pointer
 * @return VB2_REQUEST_UI_CONTINUE, or error code on error.
 */
vb2_error_t vb2_ui_change_screen(struct vb2_ui_context *ui, enum vb2_screen id);

/*****************************************************************************/
/* UI loops */

/**
 * UI for a developer-mode boot.
 *
 * Enter the developer menu, which provides options to switch out of developer
 * mode, boot from external media, use legacy bootloader, or boot Chrome OS from
 * disk.
 *
 * If a timeout occurs, take the default boot action.
 *
 * @param ctx		Vboot context
 * @returns VB2_SUCCESS, or non-zero error code.
 */
vb2_error_t vb2_developer_menu(struct vb2_context *ctx);

/**
 * UI for a non-manual recovery ("BROKEN").
 *
 * Enter the recovery menu, which shows that an unrecoverable error was
 * encountered last boot. Wait for the user to physically reset or shut down.
 *
 * @param ctx		Vboot context
 * @returns VB2_SUCCESS, or non-zero error code.
 */
vb2_error_t vb2_broken_recovery_menu(struct vb2_context *ctx);

/**
 * UI for a manual recovery-mode boot.
 *
 * Enter the recovery menu, which prompts the user to insert recovery media,
 * navigate the step-by-step recovery, or enter developer mode if allowed.
 *
 * @param ctx		Vboot context
 * @returns VB2_SUCCESS, or non-zero error code.
 */
vb2_error_t vb2_manual_recovery_menu(struct vb2_context *ctx);

#endif  /* VBOOT_REFERENCE_2UI_H_ */
