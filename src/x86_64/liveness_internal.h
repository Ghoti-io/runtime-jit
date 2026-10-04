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
 * @file liveness_internal.h
 *
 * What is live across each site of a function: the registers the stack map
 * and the derived-pointer check need. Never installed.
 */

#ifndef GHOTI_IO_GRJIT_SRC_X86_64_LIVENESS_INTERNAL_H
#define GHOTI_IO_GRJIT_SRC_X86_64_LIVENESS_INTERNAL_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/ir.h>

#include <stddef.h>

/** The registers live across one site, ascending. */
typedef struct GRJIT_SiteLive {
  GRJIT_BlockId block;       ///< The block the site's operation is in.
  size_t op_index;           ///< Its index in the block.
  const GRJIT_VReg * vregs;  ///< Tracked registers live across the site.
  size_t count;              ///< How many.
} GRJIT_SiteLive;

/** Every site of a function, in block then operation order. */
typedef struct GRJIT_LiveSites {
  const GRJIT_Allocator * allocator;
  GRJIT_SiteLive * sites;
  size_t count;
  GRJIT_VReg * pool;
} GRJIT_LiveSites;

/**
 * Computes the live sets of every `POLL`, `GC_POINT` call and `GUARD` of a
 * function that has passed the structural checks of the verifier.
 *
 * A *tracked* register is one of type REF, or a PTR declared derived. For a
 * poll or a GC-point call the set is what is live after the operation, plus
 * what its frame state names, minus the register the call assigns. For a
 * guard it is what its frame state names (the exit stub leaves the function).
 * A register that has not been assigned is not live: it is not read before it
 * is written, so the backward analysis never reaches it.
 */
GRJIT_Result grjit_liveness_compute(const GRJIT_Function * function,
    const GRJIT_Allocator * allocator, GRJIT_LiveSites * out);

/** Frees what ::grjit_liveness_compute allocated. */
void grjit_liveness_free(GRJIT_LiveSites * sites);

#endif /* GHOTI_IO_GRJIT_SRC_X86_64_LIVENESS_INTERNAL_H */
