/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the xenon2-recomp authors */
/* The single translation unit of the vendored dr_flac (see README.md). */
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
