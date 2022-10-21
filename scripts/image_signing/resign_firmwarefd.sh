#!/bin/sh

# Copyright 2014 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Abort on error
set -e

# This script is obsolete. The required functionality is now provided by the
# compiled futility executable, so all this does is invoke that. This wrapper
# should go away Real Soon Now.


# Which futility to run?
[ -z "$FUTILITY" ] && FUTILITY=futility

# required
SRC_FD=$1
DST_FD=$2
FIRMWARE_DATAKEY=$3
FIRMWARE_KEYBLOCK=$4
KERNEL_SUBKEY=$5
# optional
VERSION=$6
PREAMBLE_FLAG=$7
LOEM_OUTPUT_DIR=$8
LOEMID=$9

# pass optional args
[ -n "$VERSION" ] && VERSION="--version $VERSION"
[ -n "$PREAMBLE_FLAG" ] && PREAMBLE_FLAG="--flags $PREAMBLE_FLAG"
[ -n "$LOEM_OUTPUT_DIR" ] && LOEM_OUTPUT_DIR="--loemdir $LOEM_OUTPUT_DIR"
[ -n "$LOEMID" ] && LOEMID="--loemid $LOEMID"

exec ${FUTILITY} sign \
  --signprivate $FIRMWARE_DATAKEY \
  --keyblock $FIRMWARE_KEYBLOCK \
  --kernelkey $KERNEL_SUBKEY \
  $VERSION \
  $PREAMBLE_FLAG \
  $LOEM_OUTPUT_DIR \
  $LOEMID \
  $SRC_FD \
  $DST_FD

echo UNABLE TO EXEC FUTILITY 1>&2
exit 1
