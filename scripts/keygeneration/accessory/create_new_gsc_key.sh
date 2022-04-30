#!/bin/bash

# Copyright 2022 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Load common constants and functions.
. "$(dirname "$0")/../common.sh"

usage() {
  cat <<EOF
Usage: ${PROG} [options]

Options:
  -o, --output_dir <dir>:    Where to write the keys (default is cwd)
EOF

  if [[ $# -ne 0 ]]; then
    die "$*"
  else
    exit 0
  fi
}

generate_rsa3070_key() {
  local output_dir="$1"
  local base_name="gsc_3070"
  local len="3070"

  echo "creating ${base_name} key pair..."

  # Make the RSA key pair.
  openssl genrsa -F4 -out "${base_name}.pem" "${len}"
  openssl rsa -in "${base_name}.pem" -outform PEM \
    -pubout -out "${base_name}.pem.pub"
}

main() {
  set -euo pipefail

  local output_dir="${PWD}"

  while [[ $# -gt 0 ]]; do
    case "$1" in
    -h|--help)
      usage
      ;;
    -o|--output_dir)
      output_dir="$2"
      if [[ ! -d "${output_dir}" ]]; then
        die "output dir (${output_dir}) doesn't exist."
      fi
      shift
      ;;
    -*)
      usage "Unknown option: $1"
      ;;
    *)
      usage "Unknown argument $1"
      ;;
    esac
    shift
  done

  generate_rsa3070_key "${output_dir}"
}

main "$@"
