/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-jit.
 *
 * Ghoti.io Runtime-jit is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-jit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file allocator.h
 * @stability free
 *
 * Allocator abstraction for Ghoti.io Runtime-jit.
 *
 * This is cutil's `GCU_Allocator` under a local name. One definition
 * across the suite means an allocator written for any library works with all
 * of them. A `NULL` allocator argument means the default.
 *
 * The library allocates the IR, the assembler's buffers, the metadata and the
 * code handle through this one. The pages that hold machine code are the
 * exception: they come from the page provider the caller names (the
 * context's, so they are counted, AD-13).
 */

#ifndef GHOTI_IO_GRJIT_ALLOCATOR_H
#define GHOTI_IO_GRJIT_ALLOCATOR_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/cutil/allocator.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL. `calloc_fn` must treat overflow of
 * `nitems * size` as failure. A zero-size request returns a usable non-NULL
 * pointer, so NULL always means failure.
 */
typedef GCU_Allocator GRJIT_Allocator;

/**
 * @brief The process-global default allocator.
 *
 * @return cutil's default allocator. Never NULL.
 */
GRJIT_API const GRJIT_Allocator * grjit_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRJIT_ALLOCATOR_H */
