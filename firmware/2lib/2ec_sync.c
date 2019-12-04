/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * EC software sync routines for vboot
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2sysincludes.h"
#include "vboot_api.h"
#include "vboot_common.h"
#include "vboot_display.h"
#include "vboot_kernel.h"

#define SYNC_FLAG(select)					\
	((select) == VB_SELECT_FIRMWARE_READONLY ?		\
	 VB2_SD_FLAG_ECSYNC_EC_RO : VB2_SD_FLAG_ECSYNC_EC_RW)

/**
 * If no display is available, set DISPLAY_REQUEST in NV space
 */
static int check_reboot_for_display(struct vb2_context *ctx)
{
	if (!(vb2_get_sd(ctx)->flags & VB2_SD_FLAG_DISPLAY_AVAILABLE)) {
		VB2_DEBUG("Reboot to initialize display\n");
		vb2_nv_set(ctx, VB2_NV_DISPLAY_REQUEST, 1);
		return 1;
	}
	return 0;
}

/**
 * Display the WAIT screen
 */
static void display_wait_screen(struct vb2_context *ctx)
{
	VB2_DEBUG("EC FW update is slow. Show WAIT screen.\n");
	VbDisplayScreen(ctx, VB_SCREEN_WAIT, 0, NULL);
}

/**
 * Set the RECOVERY_REQUEST flag in NV space
 */
static void request_recovery(struct vb2_context *ctx, uint32_t recovery_request)
{
	VB2_DEBUG("request_recovery(%u)\n", recovery_request);

	vb2_nv_set(ctx, VB2_NV_RECOVERY_REQUEST, recovery_request);
}

/**
 * Wrapper around vb2ex_ec_protect() which sets recovery reason on error.
 */
static vb2_error_t protect_ec(struct vb2_context *ctx,
			      enum vb2_firmware_selection select)
{
	vb2_error_t rv = vb2ex_ec_protect(select);

	if (rv == VBERROR_EC_REBOOT_TO_RO_REQUIRED) {
		VB2_DEBUG("vb2ex_ec_protect() needs reboot\n");
	} else if (rv != VB2_SUCCESS) {
		VB2_DEBUG("vb2ex_ec_protect() returned %#x\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_PROTECT);
	}
	return rv;
}

/**
 * Print a hash to debug output
 *
 * @param hash		Pointer to the hash
 * @param hash_size	Size of the hash in bytes
 * @param desc		Description of what's being hashed
 */
static void print_hash(const uint8_t *hash, uint32_t hash_size,
		       const char *desc)
{
	int i;

	VB2_DEBUG("%s hash: ", desc);
	for (i = 0; i < hash_size; i++)
		VB2_DEBUG_RAW("%02x", hash[i]);
	VB2_DEBUG_RAW("\n");
}

static const char *image_name_to_string(enum vb2_firmware_selection select)
{
	switch (select) {
	case VB_SELECT_FIRMWARE_READONLY:
		return "RO";
	case VB_SELECT_FIRMWARE_EC_ACTIVE:
		return "RW(active)";
	case VB_SELECT_FIRMWARE_EC_UPDATE:
		return "RW(update)";
	default:
		return "UNKNOWN";
	}
}

/**
 * Check if the hash of the EC code matches the expected hash.
 *
 * @param ctx		Vboot2 context
 * @param select	Which firmware image to check
 * @return VB2_SUCCESS, or non-zero error code.
 */
static vb2_error_t check_ec_hash(struct vb2_context *ctx,
				 enum vb2_firmware_selection select)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	/* Get current EC hash. */
	const uint8_t *ec_hash = NULL;
	int ec_hash_size;
	vb2_error_t rv = vb2ex_ec_hash_image(select, &ec_hash, &ec_hash_size);
	if (rv) {
		VB2_DEBUG("vb2ex_ec_hash_image() returned %#x\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_HASH_FAILED);
		return VB2_ERROR_EC_HASH_IMAGE;
	}
	print_hash(ec_hash, ec_hash_size, image_name_to_string(select));

	/* Get expected EC hash. */
	const uint8_t *hash = NULL;
	int hash_size;
	rv = vb2ex_ec_get_expected_image_hash(select, &hash, &hash_size);
	if (rv) {
		VB2_DEBUG("vb2ex_ec_get_expected_image_hash() returned %#x\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_EXPECTED_HASH);
		return VB2_ERROR_EC_HASH_EXPECTED;
	}
	if (ec_hash_size != hash_size) {
		VB2_DEBUG("EC uses %d-byte hash, but AP-RW contains %d bytes\n",
			  ec_hash_size, hash_size);
		request_recovery(ctx, VB2_RECOVERY_EC_HASH_SIZE);
		return VB2_ERROR_EC_HASH_SIZE;
	}

	if (vb2_safe_memcmp(ec_hash, hash, hash_size)) {
		print_hash(hash, hash_size, "Expected");
		sd->flags |= SYNC_FLAG(select);
	}

	return VB2_SUCCESS;
}

/**
 * Update the specified EC and verify the update succeeded
 *
 * @param ctx		Vboot2 context
 * @param select	Which firmware image to check
 * @return VB2_SUCCESS, or non-zero error code.
 */
static vb2_error_t update_ec(struct vb2_context *ctx,
			     enum vb2_firmware_selection select)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	vb2_error_t rv;

	VB2_DEBUG("Updating %s...\n", image_name_to_string(select));

	rv = vb2ex_ec_update_image(select);
	if (rv != VB2_SUCCESS) {
		VB2_DEBUG("vb2ex_ec_update_image() returned %#x\n", rv);

		/*
		 * The EC may know it needs a reboot.  It may need to
		 * unprotect the region before updating, or may need to
		 * reboot after updating.  Either way, it's not an error
		 * requiring recovery mode.
		 *
		 * If we fail for any other reason, trigger recovery
		 * mode.
		 */
		if (rv != VBERROR_EC_REBOOT_TO_RO_REQUIRED)
			request_recovery(ctx, VB2_RECOVERY_EC_UPDATE);

		return rv;
	}

	/* Verify the EC was updated properly */
	sd->flags &= ~SYNC_FLAG(select);
	if (check_ec_hash(ctx, select) != VB2_SUCCESS)
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	if (sd->flags & SYNC_FLAG(select)) {
		VB2_DEBUG("Failed to update\n");
		request_recovery(ctx, VB2_RECOVERY_EC_UPDATE);
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	return VB2_SUCCESS;
}

/**
 * Set VB2_SD_FLAG_ECSYNC_EC_IN_RW flag for the EC
 *
 * @param ctx		Vboot2 context
 * @return VB2_SUCCESS, or non-zero if error.
 */
static vb2_error_t check_ec_active(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	int in_rw = 0;
	/*
	 * We don't use vb2ex_ec_trusted, which checks EC_IN_RW. It is
	 * controlled by cr50 but on some platforms, cr50 can't know when a EC
	 * resets. So, we trust what EC-RW says. If it lies it's in RO, we'll
	 * flash RW while it's in RW.
	 */
	vb2_error_t rv = vb2ex_ec_running_rw(&in_rw);
	/* If we couldn't determine where the EC was, reboot to recovery. */
	if (rv != VB2_SUCCESS) {
		VB2_DEBUG("vb2ex_ec_running_rw() returned %#x\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_UNKNOWN_IMAGE);
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	if (in_rw)
		sd->flags |= VB2_SD_FLAG_ECSYNC_EC_IN_RW;

	return VB2_SUCCESS;
}

#define RO_RETRIES 2  /* Maximum times to retry flashing RO */

/**
 * Sync, jump, and protect EC device
 *
 * @param ctx		Vboot2 context
 * @return VB2_SUCCESS, or non-zero if error.
 */
static vb2_error_t sync_ec(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	int is_rw_ab = ctx->flags & VB2_CONTEXT_EC_EFS;
	vb2_error_t rv;

	const enum vb2_firmware_selection select_rw = is_rw_ab ?
		VB_SELECT_FIRMWARE_EC_UPDATE :
		VB_SELECT_FIRMWARE_EC_ACTIVE;
	VB2_DEBUG("select_rw=%d\n", select_rw);

	/* Update the RW Image */
	if (sd->flags & SYNC_FLAG(select_rw)) {
		if (VB2_SUCCESS != update_ec(ctx, select_rw))
			return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
		/* Updated successfully. Cold reboot to switch to the new RW.
		 * TODO: Switch slot and proceed if EC is still in RO. */
		if (is_rw_ab) {
			VB2_DEBUG("Rebooting to jump to new EC-RW\n");
			return VBERROR_EC_REBOOT_TO_SWITCH_RW;
		}
	}

	/* Tell EC to jump to its RW image */
	if (!(sd->flags & VB2_SD_FLAG_ECSYNC_EC_IN_RW)) {
		VB2_DEBUG("jumping to EC-RW\n");
		rv = vb2ex_ec_jump_to_rw();
		if (rv != VB2_SUCCESS) {
			VB2_DEBUG("vb2ex_ec_jump_to_rw() returned %x\n", rv);

			/*
			 * If a previous AP boot has called
			 * vb2ex_ec_disable_jump(), we need to reboot the EC to
			 * unlock the ability to jump to the RW firmware.
			 *
			 * All other errors trigger recovery mode.
			 */
			if (rv != VBERROR_EC_REBOOT_TO_RO_REQUIRED)
				request_recovery(ctx, VB2_RECOVERY_EC_JUMP_RW);

			return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
		}
	}

	/* Might need to update EC-RO */
	if (sd->flags & VB2_SD_FLAG_ECSYNC_EC_RO) {
		VB2_DEBUG("RO Software Sync\n");

		/* Reset RO Software Sync NV flag */
		vb2_nv_set(ctx, VB2_NV_TRY_RO_SYNC, 0);

		/*
		 * Get the current recovery request (if any).  This gets
		 * overwritten by a failed try.  If a later try succeeds, we'll
		 * need to restore this request (or the lack of a request), or
		 * else we'll end up in recovery mode even though RO software
		 * sync did eventually succeed.
		 */
		uint32_t recovery_request =
			vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST);

		/* Update the RO Image. */
		int num_tries;
		for (num_tries = 0; num_tries < RO_RETRIES; num_tries++) {
			if (VB2_SUCCESS ==
			    update_ec(ctx, VB_SELECT_FIRMWARE_READONLY))
				break;
		}
		if (num_tries == RO_RETRIES) {
			/* Ran out of tries */
			return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
		} else if (num_tries) {
			/*
			 * Update succeeded after a failure, so we've polluted
			 * the recovery request.  Restore it.
			 */
			request_recovery(ctx, recovery_request);
		}
	}

	/* Protect RO flash */
	rv = protect_ec(ctx, VB_SELECT_FIRMWARE_READONLY);
	if (rv != VB2_SUCCESS)
		return rv;

	/* Protect RW flash */
	rv = protect_ec(ctx, select_rw);
	if (rv != VB2_SUCCESS)
		return rv;

	/* Disable further sysjumps */
	rv = vb2ex_ec_disable_jump();
	if (rv != VB2_SUCCESS) {
		VB2_DEBUG("vb2ex_ec_disable_jump() returned %#x\n", rv);
		request_recovery(ctx, VB2_RECOVERY_EC_SOFTWARE_SYNC);
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	return rv;
}

/**
 * EC sync, phase 1
 *
 * This checks whether the EC is running the correct image to do EC sync, and
 * whether any updates are necessary.
 *
 * @param ctx		Vboot2 context
 * @return VB2_SUCCESS, VBERROR_EC_REBOOT_TO_RO_REQUIRED if the EC must
 * reboot back to its RO code to continue EC sync, or other non-zero error
 * code.
 */
static vb2_error_t ec_sync_phase1(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_gbb_header *gbb = vb2_get_gbb(ctx);

	/* Reasons not to do sync at all */
	if (!(ctx->flags & VB2_CONTEXT_EC_SYNC_SUPPORTED))
		return VB2_SUCCESS;
	if (gbb->flags & VB2_GBB_FLAG_DISABLE_EC_SOFTWARE_SYNC)
		return VB2_SUCCESS;

	/* Set VB2_SD_FLAG_ECSYNC_EC_IN_RW flag */
	if (check_ec_active(ctx))
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;

	/* Check if we need to update RW.  Failures trigger recovery mode. */
	if (check_ec_hash(ctx, VB_SELECT_FIRMWARE_EC_ACTIVE))
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	/* See if we need to update EC-RO. */
	if (vb2_nv_get(ctx, VB2_NV_TRY_RO_SYNC) &&
	    check_ec_hash(ctx, VB_SELECT_FIRMWARE_READONLY)) {
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	/*
	 * If we're in RW, we need to reboot back to RO because RW can't be
	 * updated while we're running it.
	 *
	 * If EC supports RW-A/B slots, we can proceed but we need
	 * to jump to the new RW version later.
	 */
	if ((sd->flags & VB2_SD_FLAG_ECSYNC_EC_RW) &&
	    (sd->flags & VB2_SD_FLAG_ECSYNC_EC_IN_RW) &&
	    !(ctx->flags & VB2_CONTEXT_EC_EFS)) {
		return VBERROR_EC_REBOOT_TO_RO_REQUIRED;
	}

	return VB2_SUCCESS;
}

/**
 * Returns non-zero if the EC will perform a slow update.
 *
 * This is only valid after calling ec_sync_phase1(), before calling
 * sync_ec().
 *
 * @param ctx		Vboot2 context
 * @return non-zero if a slow update will be done; zero if no update or a
 * fast update.
 */
static int ec_will_update_slowly(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);

	return (((sd->flags & VB2_SD_FLAG_ECSYNC_EC_RO) ||
		 (sd->flags & VB2_SD_FLAG_ECSYNC_EC_RW)) &&
		(ctx->flags & VB2_CONTEXT_EC_SYNC_SLOW));
}

/**
 * determine if we can update the EC
 *
 * @param ctx		Vboot2 context
 * @return boolean (true iff we can update the EC)
 */

static int ec_sync_allowed(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	struct vb2_gbb_header *gbb = vb2_get_gbb(ctx);

	/* Reasons not to do sync at all */
	if (!(ctx->flags & VB2_CONTEXT_EC_SYNC_SUPPORTED))
		return 0;
	if (gbb->flags & VB2_GBB_FLAG_DISABLE_EC_SOFTWARE_SYNC)
		return 0;
	if (sd->recovery_reason)
		return 0;
	return 1;
}

/**
 * EC sync, phase 2
 *
 * This updates the EC if necessary, makes sure it has protected its image(s),
 * and makes sure it has jumped to the correct image.
 *
 * If ec_will_update_slowly(), it is suggested that the caller display a
 * warning screen before calling phase 2.
 *
 * @param ctx		Vboot2 context
 * @return VB2_SUCCESS, VBERROR_EC_REBOOT_TO_RO_REQUIRED if the EC must
 * reboot back to its RO code to continue EC sync, or other non-zero error
 * code.
 */
static vb2_error_t ec_sync_phase2(struct vb2_context *ctx)
{
	if (!ec_sync_allowed(ctx))
		return VB2_SUCCESS;

	/* Handle updates and jumps for EC */
	return sync_ec(ctx);
}

vb2_error_t vb2api_ec_sync(struct vb2_context *ctx)
{
	struct vb2_shared_data *sd = vb2_get_sd(ctx);
	vb2_error_t rv;

	/*
	 * If the flags indicate that the EC has already gone through
	 * software sync this boot, then don't do it again.
	 */
	if (sd->flags & VB2_SD_STATUS_EC_SYNC_COMPLETE) {
		VB2_DEBUG("EC software sync already performed this boot, skipping\n");
		return VB2_SUCCESS;
	}

	/*
	 * If the device is in recovery mode, then EC sync should
	 * not be performed.
	 */
	if (ctx->flags & VB2_CONTEXT_RECOVERY_MODE) {
		VB2_DEBUG("In recovery mode, skipping EC sync\n");
		return VB2_SUCCESS;
	}

	/* Phase 1; this determines if we need an update */
	vb2_error_t phase1_rv = ec_sync_phase1(ctx);
	int need_wait_screen = ec_will_update_slowly(ctx);

	if (need_wait_screen && check_reboot_for_display(ctx))
		return VBERROR_REBOOT_REQUIRED;

	if (phase1_rv)
		return phase1_rv;

	if (need_wait_screen)
		display_wait_screen(ctx);

	/* Phase 2; Applies update and/or jumps to the correct EC image */
	rv = ec_sync_phase2(ctx);
	if (rv)
		return rv;

	/* Phase 3; Let the platform know that EC software sync is now done */
	rv = vb2ex_ec_vboot_done(ctx);
	if (rv)
		return rv;

	/* Establish that EC software sync is complete and successful */
	sd->flags |= VB2_SD_STATUS_EC_SYNC_COMPLETE;

	return VB2_SUCCESS;
}
