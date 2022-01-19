// Copyright 2022 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// vboot_reference bindings for Rust.

#[allow(
    clippy::all,
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals
)]
pub mod crossystem {
    include!(concat!(env!("OUT_DIR"), "/crossystem.rs"));
}
