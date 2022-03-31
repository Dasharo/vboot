/*
 * Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <openssl/bn.h>
#include <openssl/pem.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "fmap.h"
#include "futility.h"
#include "gsc_ro.h"
#include "host_key21.h"
#include "host_keyblock.h"
#include "host_signature.h"

/*
 * for testing purposes let's use
 * - tests/devkeys/arv_root.vbprivk as the root private key
 * - tests/devkeys/arv_root.vbpubk as the root public key
 *   used for signing of the platform public key
 * - tests/devkeys/arv_platform.vbprivk signing platform key
 * - tests/devkeys/arv_platform.vbpubk - public key used for signature
 *       verification
 *------------
 * Command to create the signed public key block in ~/tmp/packed:
 *
  ./build/futility/futility vbutil_keyblock --pack ~/tmp/packed \
      --datapubkey  tests/devkeys/arv_platform.vbpubk \
      --signprivate tests/devkeys/arv_root.vbprivk
 *------------
 * Command to fill RO_GSCVD FMAP area in an AP firmware file. The input AP
 *   firmware file is ~/tmp/image-guybrush.serial.bin, the output signed
 *   AP firmware file is ~/tmp/guybrush-signed:
 *
  ./build/futility/futility gscvd --outfile ~/tmp/guybrush-signed \
    -R 818100:10000,f00000:100,f80000:2000,f8c000:1000,0x00804000:0x00000800 \
    -k ~/tmp/packed -p tests/devkeys/arv_platform.vbprivk -b 5a5a4352  \
    -r tests/devkeys/arv_root.vbpubk ~/tmp/image-guybrush.serial.bin
 *------------
 * Command to validate a previously signed AP firmware file. The hash is the
 *  sha256sum of tests/devkeys/kernel_subkey.vbpubk:
 *
  build/futility/futility gscvd ~/tmp/guybrush-signed \
   3d74429f35be8d34bcb425d4397e2218e6961afed456a78ce30047f5b54ed158
 */

/* Command line options processing support. */
enum no_short_opts {
	OPT_OUTFILE = 1000,
};

static const struct option long_opts[] = {
	/* name       hasarg *flag  val */
	{"outfile",       1, NULL, OPT_OUTFILE},
	{"ranges",        1, NULL, 'R'},
	{"board_id",      1, NULL, 'b'},
	{"root_pub_key",  1, NULL, 'r'},
	{"keyblock",      1, NULL, 'k'},
	{"platform_priv", 1, NULL, 'p'},
	{"help",          0, NULL, 'h'},
	{}
};

static const char *short_opts = "R:b:hk:p:r:";

static const char usage[] =
	"\n"
	"This utility creates an RO verification space in the Chrome OS AP\n"
	"firmware image or allows to validate a previously prepared image\n"
	"containing the RO verification space.\n\n"
	"Usage: " MYNAME " gscvd PARAMS <AP FIRMWARE FILE> [<root key hash>]\n"
	"\n\nCreation of RO Verification space:\n\n"
	"Required PARAMS:\n"
	"  -R|--ranges        STRING        Comma separated colon delimited\n"
	"                                     hex tuples <offset>:<size>, the\n"
	"                                     areas of the RO covered by the\n"
	"                                     signature\n"
	"  -b|--board_id  <hex value>      The Board ID of the board for which\n"
	"                                     the image is being signed\n"
	"  -r|--root_pub_key  <file>        The main public key, in .vbpubk\n"
	"                                     format, used to verify platform\n"
	"                                     key\n"
	"  -k|--keyblock      <file>        Signed platform public key in\n"
	"                                     .keyblock format, used for run\n"
	"                                     time RO verifcation\n"
	"  -p|--platform_priv <file>        Private platform key in .vbprivk\n"
	"                                     format, used for signing RO\n"
	"                                     verification data\n"
	"Optional PARAMS:\n"
	"  [--outfile]        OUTFILE       Output firmware image containing\n"
	"                                     RO verification information\n"
	"\n\n"
	"Validation of RO Verification space:\n\n"
	"   The only required parameter is <AP FIRMWARE FILE>, if optional\n"
	"   <root key hash> is given, it is compared to the hash\n"
	"   of the root key found in <AP_FIRMWARE_FILE>.\n"
	"\n\n"
	"  -h|--help                        Print this message\n\n";

/* Structure helping to keep track of the file mapped into memory. */
struct file_buf {
	uint32_t len;
	uint8_t *data;
	int fd;
	FmapAreaHeader *ro_gscvd;
};

/*
 * Max number of RO ranges to cover. 32 is more than enough, this must be kept
 * in sync with APRO_MAX_NUM_RANGES declaration in
 * common/ap_ro_integrity_check.c in the Cr50 tree.
 */
#define MAX_RANGES 32

/*
 * Container keeping track of the set of ranges to include in hash
 * calculation.
 */
struct gscvd_ro_ranges {
	size_t range_count;
	struct gscvd_ro_range ranges[MAX_RANGES];
};

/**
 * Load the AP firmware file into memory.
 *
 * Map the requested file into memory, find RO_GSCVD area in the file, and
 * cache the information in the passed in file_buf structure.
 *
 * @param file_name  name of the AP firmware file
 * @param file_buf   pointer to the helper structure keeping information about
 *                   the file
 *
 * @return 0 on success 1 on failure.
 */
static int load_ap_firmware(const char *file_name, struct file_buf *file,
			int mode)
{
	if (futil_open_and_map_file(file_name, &file->fd, mode, &file->data,
				    &file->len))
		return 1;

	if (!fmap_find_by_name(file->data, file->len, NULL, "RO_GSCVD",
			       &file->ro_gscvd)) {
		ERROR("Could not find RO_GSCVD in the FMAP\n");
		futil_unmap_and_close_file(file->fd, mode, file->data,
					   file->len);
		file->fd = -1;
		file->data = NULL;
		file->len = 0;
		return 1;
	}

	return 0;
}

/**
 * Check if the passed in offset falls into the passed in FMAP area.
 */
static bool in_range(uint32_t offset, const FmapAreaHeader *ah)
{
	return (offset >= ah->area_offset) &&
	       (offset <= (ah->area_offset + ah->area_size));
}

/**
 * Check if the passed in range fits into the passed in FMAP area.
 */
static bool range_fits(const struct gscvd_ro_range *range,
		       const FmapAreaHeader *ah)
{
	if (in_range(range->offset, ah) &&
	    in_range(range->offset + range->size, ah))
		return true;

	ERROR("Range %#x..+%#x does not fit in %s\n", range->offset,
	      range->size, ah->area_name);

	return false;
}

/**
 * Check if the passed in range overlaps with the area.
 *
 * @param range  pointer to the range to check
 * @param offset  offset of the area to check against
 * @param size  size of the area to check against
 *
 * @return true if range overlaps with the area, false otherwise.
 */
static bool range_overlaps(const struct gscvd_ro_range *range, uint32_t offset,
			   size_t size)
{
	if (((range->offset + range->size) <= offset) ||
	    (offset + size) <= range->offset)
		return false;

	ERROR("Range %x..+%x overlaps with %x..+%zx\n", range->offset,
	      range->size, offset, size);

	return true;
}

/*
 * Check validity of the passed in ranges.
 *
 * All ranges must
 * - fit into the WP_RO FMAP area
 * - not overlap with the RO_GSCVD FMAP area
 * - not overlap with each other
 *
 * @param ranges - pointer to the container of ranges to check
 * @param file - pointer to the file layout descriptor
 *
 * @return zero on success, -1 on failures
 */
static int verify_ranges(const struct gscvd_ro_ranges *ranges,
			 const struct file_buf *file)
{
	size_t i;
	FmapAreaHeader *wp_ro;
	int errorcount;

	if (!fmap_find_by_name(file->data, file->len, NULL, "WP_RO", &wp_ro)) {
		ERROR("Could not find WP_RO in the FMAP\n");
		return 1;
	}

	errorcount = 0;
	for (i = 0; i < ranges->range_count; i++) {
		size_t j;

		/* Must fit into WP_RO. */
		if (!range_fits(ranges->ranges + i, wp_ro))
			errorcount++;

		/* Must not overlap with RO_GSCVD. */
		if (range_overlaps(ranges->ranges + i,
				   file->ro_gscvd->area_offset,
				   file->ro_gscvd->area_size))
			errorcount++;

		/* The last range is nothing to compare against. */
		if (i == ranges->range_count - 1)
			break;

		/* Must not overlap with all following ranges. */
		for (j = i + 1; j < ranges->range_count; j++)
			if (range_overlaps(ranges->ranges + i,
					   ranges->ranges[j].offset,
					   ranges->ranges[j].size))
				errorcount++;
	}

	return errorcount ? -1 : 0;
}

/**
 * Parse range specification supplied by the user.
 *
 * The input is a string of the following format:
 * <hex base>:<hex size>[,<hex base>:<hex size>[,...]]
 *
 * @param input  user input, part of the command line
 * @param output  pointer to the ranges container
 *
 * @return zero on success, -1 on failure
 */
static int parse_ranges(const char *input, struct gscvd_ro_ranges *output)
{
	char *cursor;
	char *delim;
	char *str = strdup(input);
	int rv = 0;

	if (!str) {
		ERROR("Failed to allocate memory for ranges string copy!\n");
		return -1;
	}

	output->range_count = 0;
	cursor = str;
	do {
		char *colon;
		char *e;

		if (output->range_count >= ARRAY_SIZE(output->ranges)) {
			ERROR("Too many ranges!\n");
			rv = -1;
			break;
		}

		delim = strchr(cursor, ',');
		if (delim)
			*delim = '\0';
		colon = strchr(cursor, ':');
		if (!colon) {
			rv = -1;
			break;
		}
		*colon = '\0';

		errno = 0;
		output->ranges[output->range_count].offset =
			strtol(cursor, &e, 16);
		if (errno || *e) {
			rv = -1;
			break;
		}

		output->ranges[output->range_count].size =
			strtol(colon + 1, &e, 16);
		if (errno || *e) {
			rv = -1;
			break;
		}

		output->range_count++;
		cursor = delim + 1;
		/* Iterate until there is no more commas. */
	} while (delim);

	free(str);
	if (rv)
		ERROR("Misformatted ranges string\n");

	return rv;
}

/**
 * Calculate hash of the RO ranges.
 *
 * @param ap_firmware_file  pointer to the AP firmware file layout descriptor
 * @param ranges  pointer to the container of ranges to include in hash
 *		  calculation
 * @param hash_alg  algorithm to use for hashing
 * @param digest  memory to copy the calculated hash to
 * @param digest_ size requested size of the digest, padded with zeros if the
 *	          SHA digest size is smaller than digest_size
 *
 * @return zero on success, -1 on failure.
 */
static int calculate_ranges_digest(const struct file_buf *ap_firmware_file,
				   const struct gscvd_ro_ranges *ranges,
				   enum vb2_hash_algorithm hash_alg,
				   void *digest, size_t digest_size)
{
	struct vb2_digest_context dc;
	size_t i;

	/* Calculate the ranges digest. */
	if (vb2_digest_init(&dc, hash_alg) != VB2_SUCCESS) {
		ERROR("Failed to init digest!\n");
		return 1;
	}

	for (i = 0; i < ranges->range_count; i++) {
		if (vb2_digest_extend(&dc,
				      ap_firmware_file->data +
					      ranges->ranges[i].offset,
				      ranges->ranges[i].size) != VB2_SUCCESS) {
			ERROR("Failed to extend digest!\n");
			return -1;
		}
	}

	memset(digest, 0, digest_size);
	if (vb2_digest_finalize(&dc, digest, digest_size) != VB2_SUCCESS) {
		ERROR("Failed to finalize digest!\n");
		return -1;
	}

	return 0;
}

/**
 * Build GSC verification data.
 *
 * Calculate size of the structure including the signature and the root key,
 * allocate memory, fill up the structure, calculate AP RO ranges digest and
 * then the GVD signature.
 *
 * @param ap_firmware_file  pointer to the AP firmware file layout descriptor
 * @param ranges  pointer to the container of ranges to include in verification
 * @param root_pubk  pointer to the root pubk container
 * @param privk   pointer to the private key to use for signing
 * @param board_id  Board ID value to use.
 *
 * @return pointer to the created GVD (to be freed by the caller) on success,
 *         NULL on failure.
 */
static
struct gsc_verification_data *create_gvd(struct file_buf *ap_firmware_file,
					 struct gscvd_ro_ranges *ranges,
					 const struct vb2_packed_key *root_pubk,
					 const struct vb2_private_key *privk,
					 uint32_t board_id)
{
	struct gsc_verification_data *gvd;
	size_t total_size;
	size_t sig_size;
	size_t ranges_size;
	struct vb2_signature *sig;
	const FmapHeader *fmh;

	sig_size = vb2_rsa_sig_size(privk->sig_alg);
	ranges_size = ranges->range_count * sizeof(struct gscvd_ro_range);
	total_size = sizeof(struct gsc_verification_data) +
		root_pubk->key_size + sig_size + ranges_size;

	gvd = calloc(total_size, 1);

	if (!gvd) {
		ERROR("Failed to allocate %zd bytes for gvd\n", total_size);
		return NULL;
	}

	gvd->gv_magic = GSC_VD_MAGIC;
	gvd->size = total_size;
	gvd->gsc_board_id = board_id;
	gvd->rollback_counter = GSC_VD_ROLLBACK_COUNTER;

	/* Guaranteed to succeed. */
	fmh = fmap_find(ap_firmware_file->data, ap_firmware_file->len);

	gvd->fmap_location = (uintptr_t)fmh - (uintptr_t)ap_firmware_file->data;

	gvd->hash_alg = VB2_HASH_SHA256;

	if (calculate_ranges_digest(ap_firmware_file, ranges, gvd->hash_alg,
				    gvd->ranges_digest,
				    sizeof(gvd->ranges_digest))) {
		free(gvd);
		return NULL;
	}

	/* Prepare signature header. */
	vb2_init_signature(&gvd->sig_header,
			   (uint8_t *)(gvd + 1) + ranges_size,
			   sig_size,
			   sizeof(struct gsc_verification_data) + ranges_size);

	/* Copy root key into the structure. */
	vb2_init_packed_key(&gvd->root_key_header,
			    (uint8_t *)(gvd + 1) + ranges_size + sig_size,
			    root_pubk->key_size);
	vb2_copy_packed_key(&gvd->root_key_header, root_pubk);

	/* Copy ranges into the ranges section. */
	gvd->range_count = ranges->range_count;
	memcpy(gvd->ranges, ranges->ranges, ranges_size);

	sig = vb2_calculate_signature((uint8_t *)gvd,
				      sizeof(struct gsc_verification_data) +
				      ranges_size, privk);
	if (!sig) {
		ERROR("Failed to calculate signature\n");
		free(gvd);
		return NULL;
	}

	/* Copy signature body into GVD after some basic checks. */
	if ((sig_size == sig->sig_size) &&
	    (gvd->sig_header.data_size == sig->data_size)) {
		vb2_copy_signature(&gvd->sig_header, sig);
	} else {
		ERROR("Inconsistent signature headers\n");
		free(sig);
		free(gvd);
		return NULL;
	}

	free(sig);

	return gvd;
}

/**
 * Fill RO_GSCVD FMAP area.
 *
 * All trust chain components have been verified, AP RO sections digest
 * calculated, and GVD signature created; put it all together in the dedicated
 * FMAP area.
 *
 * @param ap_firmware_file  pointer to the AP firmware file layout descriptor
 * @param gvd  pointer to the GVD header
 * @param keyblock  pointer to the keyblock container
 *
 * @return zero on success, -1 on failure
 */
static int fill_gvd_area(struct file_buf *ap_firmware_file,
			 struct gsc_verification_data *gvd,
			 struct vb2_keyblock *keyblock)
{
	size_t total;
	uint8_t *cursor;

	/* How much room is needed for the whole thing? */
	total = gvd->size + keyblock->keyblock_size;

	if (total > ap_firmware_file->ro_gscvd->area_size) {
		ERROR("GVD section does not fit, %zd > %d\n",
		      total, ap_firmware_file->ro_gscvd->area_size);
		return -1;
	}

	cursor = ap_firmware_file->data +
		 ap_firmware_file->ro_gscvd->area_offset;

	/* Copy GSC verification data */
	memcpy(cursor, gvd, gvd->size);
	cursor += gvd->size;

	/* Keyblock, size includes everything. */
	memcpy(cursor, keyblock, keyblock->keyblock_size);

	return 0;
}

/**
 * Initialize a work buffer structure.
 *
 * Embedded vboot reference code does not use malloc/free, it uses the so
 * called work buffer structure to provide a poor man's memory management
 * tool. This program uses some of the embedded library functions, let's
 * implement work buffer support to keep the embedded code happy.
 *
 * @param wb  pointer to the workubffer structure to initialize
 * @param size  size of the buffer to allocate
 *
 * @return pointer to the allocated buffer on success, NULL on failure.
 */
static void *init_wb(struct vb2_workbuf *wb, size_t size)
{
	void *buf = malloc(size);

	if (!buf)
		ERROR("Failed to allocate workblock of %zd\n", size);
	else
		vb2_workbuf_init(wb, buf, size);

	return buf;
}

/**
 * Validate that platform key keyblock was signed by the root key.
 *
 * This function performs the same step the GSC is supposed to perform:
 * validate the platform key keyblock signature using the root public key.
 *
 * @param root_pubk  pointer to the root public key container
 * @param kblock  pointer to the platform public key keyblock
 *
 * @return 0 on success, -1 on failure
 */
static int validate_pubk_signature(const struct vb2_packed_key *root_pubk,
				   struct vb2_keyblock *kblock)
{
	struct vb2_public_key pubk;
	struct vb2_workbuf wb;
	uint32_t kbsize;
	int rv;
	void *buf;

	if (vb2_unpack_key(&pubk, root_pubk) != VB2_SUCCESS) {
		ERROR("Failed to unpack public key\n");
		return -1;
	}

	/* Let's create an ample sized work buffer. */
	buf = init_wb(&wb, 8192);
	if (!buf)
		return -1;

	rv = -1;
	do {
		void *work;

		kbsize = kblock->keyblock_size;
		work = vb2_workbuf_alloc(&wb, kbsize);
		if (!work) {
			ERROR("Failed to allocate workblock space %d\n",
			      kbsize);
			break;
		}

		memcpy(work, kblock, kbsize);

		if (vb2_verify_keyblock(work, kbsize, &pubk, &wb) !=
		    VB2_SUCCESS) {
			ERROR("Root and keyblock mismatch\n");
			break;
		}

		rv = 0;
	} while (false);

	free(buf);

	return rv;
}

/**
 * Validate that private and public parts of the platform key match.
 *
 * This is a fairly routine validation, the N components of the private and
 * public RSA keys are compared.
 *
 * @param keyblock  pointer to the keyblock containing the public key
 * @param plat_privk  pointer to the matching private key
 *
 * @return 0 on success, nonzero on failure
 */
static int validate_privk(struct vb2_keyblock *kblock,
			  struct vb2_private_key *plat_privk)
{
	const BIGNUM *privn;
	BIGNUM *pubn;
	struct vb2_public_key pubk;
	int rv;

	privn = pubn = NULL;

	RSA_get0_key(plat_privk->rsa_private_key, &privn, NULL, NULL);

	if (vb2_unpack_key(&pubk, &kblock->data_key) != VB2_SUCCESS) {
		ERROR("Failed to unpack public key\n");
		return -1;
	}

	pubn = BN_new();
	pubn = BN_lebin2bn((uint8_t *)pubk.n, vb2_rsa_sig_size(pubk.sig_alg),
			   pubn);
	rv = BN_cmp(pubn, privn);
	if (rv)
		ERROR("Public/private key N mismatch!\n");

	BN_free(pubn);
	return rv;
}

/**
 * Copy ranges from AP firmware file into gscvd_ro_ranges container
 *
 * While copying the ranges verify that they do not overlap.
 *
 * @param ap_firmware_file  pointer to the AP firmware file layout descriptor
 * @param gvd  pointer to the GVD header followed by the ranges
 * @param ranges  pointer to the ranges container to copy ranges to
 *
 * @return 0 on successful copy nonzero on errors.
 */
static int copy_ranges(const struct file_buf *ap_firmware_file,
		       const struct gsc_verification_data *gvd,
		       struct gscvd_ro_ranges *ranges)
{
	ranges->range_count = gvd->range_count;
	memcpy(ranges->ranges, gvd->ranges,
	       sizeof(ranges->ranges[0]) * ranges->range_count);

	return verify_ranges(ranges, ap_firmware_file);
}

/**
 * Basic validation of GVD included in a AP firmware file.
 *
 * This is not a cryptographic verification, just a check that the structure
 * makes sense and the expected values are found in certain fields.
 *
 * @param gvd  pointer to the GVD header followed by the ranges
 * @param ap_firmware_file  pointer to the AP firmware file layout descriptor
 *
 * @return zero on success, -1 on failure.
 */
static int validate_gvd(const struct gsc_verification_data *gvd,
			const struct file_buf *ap_firmware_file)
{
	const FmapHeader *fmh;

	if (gvd->gv_magic != GSC_VD_MAGIC) {
		ERROR("Incorrect gscvd magic %x\n", gvd->gv_magic);
		return -1;
	}

	if (!gvd->range_count || (gvd->range_count > MAX_RANGES)) {
		ERROR("Incorrect gscvd range count %d\n", gvd->range_count);
		return -1;
	}

	/* Guaranteed to succeed. */
	fmh = fmap_find(ap_firmware_file->data, ap_firmware_file->len);

	if (gvd->fmap_location !=
	    ((uintptr_t)fmh - (uintptr_t)ap_firmware_file->data)) {
		ERROR("Incorrect gscvd fmap offset %x\n", gvd->fmap_location);
		return -1;
	}

	/* Make sure signature and root key fit. */
	if (vb2_verify_signature_inside(gvd, gvd->size, &gvd->sig_header) !=
	    VB2_SUCCESS) {
		ERROR("Corrupted signature header in GVD\n");
		return -1;
	}

	if (vb2_verify_packed_key_inside(gvd, gvd->size,
					 &gvd->root_key_header) !=
	    VB2_SUCCESS) {
		ERROR("Corrupted root key header in GVD\n");
		return -1;
	}

	return 0;
}

/**
 * Validate GVD signature.
 *
 * Given the entire GVD space (header plus ranges array), the signature and
 * the public key, verify that the signature matches.
 *
 * @param gvd  pointer to gsc_verification_data followed by the ranges array
 * @param gvd_signature  pointer to the vb2 signature container
 * @param packedk  pointer to the keyblock containing the public key
 *
 * @return zero on success, non-zero on failure
 */
static int validate_gvd_signature(struct gsc_verification_data *gvd,
				  const struct vb2_packed_key *packedk)
{
	struct vb2_workbuf wb;
	void *buf;
	int rv;
	struct vb2_public_key pubk;
	size_t signed_size;

	/* Extract public key from the public key keyblock. */
	if (vb2_unpack_key(&pubk, packedk) != VB2_SUCCESS) {
		ERROR("Failed to unpack public key\n");
		return -1;
	}

	/* Let's create an ample sized work buffer. */
	buf = init_wb(&wb, 8192);
	if (!buf)
		return -1;

	signed_size = sizeof(struct gsc_verification_data) +
		gvd->range_count * sizeof(gvd->ranges[0]);

	rv = vb2_verify_data((const uint8_t *)gvd, signed_size,
			     &gvd->sig_header,
			     &pubk, &wb);

	free(buf);
	return rv;
}

/*
 * Validate GVD of the passed in AP firmware file and possibly the root key hash
 *
 * The input parameters are the subset of the command line, the first argv
 * string is the AP firmware file name, the second string, if present, is the
 * hash of the root public key included in the RO_GSCVD area of the AP
 * firmware file.
 *
 * @return zero on success, -1 on failure.
 */
static int validate_gscvd(int argc, char *argv[])
{
	struct file_buf ap_firmware_file;
	int rv;
	struct gscvd_ro_ranges ranges;
	struct gsc_verification_data *gvd;
	const char *file_name;
	uint8_t digest[sizeof(gvd->ranges_digest)];
	struct vb2_hash root_key_digest = { .algo = VB2_HASH_SHA256 };

	/* Guaranteed to be available. */
	file_name = argv[0];

	if (argc > 1)
		parse_digest_or_die(root_key_digest.sha256,
				    sizeof(root_key_digest.sha256),
				    argv[1]);

	do {
		struct vb2_keyblock *kblock;

		rv = -1; /* Speculative, will be cleared on success. */

		if (load_ap_firmware(file_name, &ap_firmware_file, FILE_RO))
			break;

		/* Copy ranges from gscvd to local structure. */
		gvd = (struct gsc_verification_data
			       *)(ap_firmware_file.data +
				  ap_firmware_file.ro_gscvd->area_offset);

		if (validate_gvd(gvd, &ap_firmware_file))
			break;

		if (copy_ranges(&ap_firmware_file, gvd, &ranges))
			break;

		if (calculate_ranges_digest(&ap_firmware_file, &ranges,
					    gvd->hash_alg, digest,
					    sizeof(digest)))
			break;

		if (memcmp(digest, gvd->ranges_digest, sizeof(digest))) {
			ERROR("Ranges digest mismatch\n");
			break;
		}

		/* Find the keyblock. */
		kblock = (struct vb2_keyblock *)((uintptr_t)gvd + gvd->size);

		if ((argc > 1) && (vb2_hash_verify
				   (vb2_packed_key_data(&gvd->root_key_header),
				    gvd->root_key_header.key_size,
				    &root_key_digest) != VB2_SUCCESS)) {
			ERROR("Sha256 mismatch\n");
			break;
		}

		if (validate_pubk_signature(&gvd->root_key_header, kblock)) {
			ERROR("Keyblock not signed by root key\n");
			break;
		}

		if (validate_gvd_signature(gvd, &kblock->data_key)) {
			ERROR("GVD not signed by platform key\n");
			break;
		}

		rv = 0;
	} while (false);

	if (ap_firmware_file.fd != -1)
		futil_unmap_and_close_file(ap_firmware_file.fd, FILE_RO,
					   ap_firmware_file.data,
					   ap_firmware_file.len);

	return rv;
}

/**
 * Calculate and report sha256 hash of the public key body.
 *
 * The hash will be incorporated into GVC firmware to allow it to validate the
 * root key.
 *
 * @param pubk pointer to the public key to process.
 */
static void dump_pubk_hash(const struct vb2_packed_key *pubk)
{
	struct vb2_hash hash;
	size_t i;

	vb2_hash_calculate(vb2_packed_key_data(pubk), pubk->key_size,
			   VB2_HASH_SHA256, &hash);

	printf("Root key body sha256 hash:\n");

	for (i = 0; i < sizeof(hash.sha256); i++)
		printf("%02x", hash.sha256[i]);

	printf("\n");
}

/**
 * The main function of this futilty option.
 *
 * See the usage string for input details.
 *
 * @return zero on success, nonzero on failure.
 */
static int do_gscvd(int argc, char *argv[])
{
	int i;
	int longindex;
	char *infile = NULL;
	char *outfile = NULL;
	char *work_file = NULL;
	struct gscvd_ro_ranges ranges;
	int errorcount = 0;
	struct vb2_packed_key *root_pubk = NULL;
	struct vb2_keyblock *kblock = NULL;
	struct vb2_private_key *plat_privk = NULL;
	struct gsc_verification_data *gvd = NULL;
	struct file_buf ap_firmware_file = { .fd = -1 };
	uint32_t board_id = UINT32_MAX;
	int rv = 0;

	ranges.range_count = 0;

	while ((i = getopt_long(argc, argv, short_opts, long_opts,
				&longindex)) != -1) {
		switch (i) {
		case OPT_OUTFILE:
			outfile = optarg;
			break;
		case 'R':
			if (parse_ranges(optarg, &ranges)) {
				ERROR("Could not parse ranges\n");
				/* Error message has been already printed. */
				errorcount++;
			}
			break;
		case 'b': {
			char *e;
			long long bid;

			bid = strtoull(optarg, &e, 16);
			if (*e || (bid >= UINT32_MAX)) {
				ERROR("Board ID value '%s' is invalid\n",
				      optarg);
				errorcount++;
			} else {
				board_id = (uint32_t)bid;
			}
			break;
		}
		case 'r':
			root_pubk = vb2_read_packed_key(optarg);
			if (!root_pubk) {
				ERROR("Could not read %s\n", optarg);
				errorcount++;
			}
			break;
		case 'k':
			kblock = vb2_read_keyblock(optarg);
			if (!kblock) {
				ERROR("Could not read %s\n", optarg);
				errorcount++;
			}
			break;
		case 'p':
			plat_privk = vb2_read_private_key(optarg);
			if (!plat_privk) {
				ERROR("Could not read %s\n", optarg);
				errorcount++;
			}
			break;
		case 'h':
			printf("%s", usage);
			return 0;
		case '?':
			if (optopt)
				ERROR("Unrecognized option: -%c\n", optopt);
			else
				ERROR("Unrecognized option: %s\n",
				      argv[optind - 1]);
			errorcount++;
			break;
		case ':':
			ERROR("Missing argument to -%c\n", optopt);
			errorcount++;
			break;
		case 0: /* handled option */
			break;
		default:
			FATAL("Unrecognized getopt output: %d\n", i);
		}
	}

	if ((optind == 1) && (argc > 1))
		/* This must be a validation request. */
		return validate_gscvd(argc - 1, argv + 1);

	if (optind != (argc - 1)) {
		ERROR("Misformatted command line\n%s\n", usage);
		return 1;
	}

	if (errorcount || !ranges.range_count || !root_pubk || !kblock ||
	    !plat_privk || (board_id == UINT32_MAX)) {
		/* Error message(s) should have been printed by now. */
		ERROR("%s\n", usage);
		return 1;
	}

	infile = argv[optind];

	if (outfile) {
		futil_copy_file_or_die(infile, outfile);
		work_file = outfile;
	} else {
		work_file = infile;
	}

	do {
		rv = 1; /* Speculative, will be cleared on success. */

		if (validate_pubk_signature(root_pubk, kblock))
			break;

		if (validate_privk(kblock, plat_privk))
			break;

		if (load_ap_firmware(work_file, &ap_firmware_file, FILE_RW))
			break;

		if (verify_ranges(&ranges, &ap_firmware_file))
			break;

		gvd = create_gvd(&ap_firmware_file, &ranges,
				 root_pubk, plat_privk, board_id);
		if (!gvd)
			break;

		if (fill_gvd_area(&ap_firmware_file, gvd, kblock))
			break;

		dump_pubk_hash(root_pubk);

		rv = 0;
	} while (false);

	free(gvd);
	free(root_pubk);
	free(kblock);
	vb2_private_key_free(plat_privk);

	if (ap_firmware_file.fd != -1)
		futil_unmap_and_close_file(ap_firmware_file.fd, FILE_RW,
					   ap_firmware_file.data,
					   ap_firmware_file.len);

	return rv;
}

DECLARE_FUTIL_COMMAND(gscvd, do_gscvd, VBOOT_VERSION_2_1,
		      "Create RO verification structure");
