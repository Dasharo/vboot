#!/bin/bash -eux
# Copyright 2018 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

me=${0##*/}
TMP="$me.tmp"

# Test --sys_props (primitive test needed for future updating tests).
test_sys_props() {
	! "${FUTILITY}" --debug update --sys_props "$*" |
		sed -n 's/.*property\[\(.*\)].value = \(.*\)/\1,\2,/p' |
		tr '\n' ' '
}

test "$(test_sys_props "1,2,3")" = "0,1, 1,2, 2,3, "
test "$(test_sys_props "1 2 3")" = "0,1, 1,2, 2,3, "
test "$(test_sys_props "1, 2,3 ")" = "0,1, 1,2, 2,3, "
test "$(test_sys_props "   1,, 2")" = "0,1, 2,2, "
test "$(test_sys_props " , 4,")" = "1,4, "

# Test data files
LINK_BIOS="${SCRIPTDIR}/data/bios_link_mp.bin"
PEPPY_BIOS="${SCRIPTDIR}/data/bios_peppy_mp.bin"

# Work in scratch directory
cd "$OUTDIR"
set -o pipefail

# In all the test scenario, we want to test "updating from PEPPY to LINK".
TO_IMAGE=${TMP}.src.link
FROM_IMAGE=${TMP}.src.peppy
cp -f ${LINK_BIOS} ${TO_IMAGE}
cp -f ${PEPPY_BIOS} ${FROM_IMAGE}

unpack_image() {
	local folder="${TMP}.$1"
	local image="$2"
	mkdir -p "${folder}"
	(cd "${folder}" && ${FUTILITY} dump_fmap -x "../${image}")
}

# Unpack images so we can prepare expected results by individual sections.
unpack_image "to" "${TO_IMAGE}"
unpack_image "from" "${FROM_IMAGE}"

# Generate expected results.
cp -f "${TO_IMAGE}" "${TMP}.expected.full"
cp -f "${FROM_IMAGE}" "${TMP}.expected.rw"
"${FUTILITY}" load_fmap "${TMP}.expected.rw" \
	RW_SECTION_A:${TMP}.to/RW_SECTION_A \
	RW_SECTION_B:${TMP}.to/RW_SECTION_B \
	RW_SHARED:${TMP}.to/RW_SHARED \
	RW_LEGACY:${TMP}.to/RW_LEGACY

test_update() {
	local test_name="$1"
	local emu_src="$2"
	local expected="$3"
	local error_msg="${expected#!}"
	local msg

	shift 3
	cp -f "${emu_src}" "${TMP}.emu"
	echo "*** Test Item: ${test_name}"
	"${FUTILITY}" update --emulate "${TMP}.emu" "$@"
	cmp "${TMP}.emu" "${expected}"
}

# Test Full update.
test_update "Full update" \
	"${FROM_IMAGE}" "${TMP}.expected.full" \
	-i "${TO_IMAGE}" --wp=0

# Test RW-only update.
test_update "RW update" \
	"${FROM_IMAGE}" "${TMP}.expected.rw" \
	-i "${TO_IMAGE}" --wp=1