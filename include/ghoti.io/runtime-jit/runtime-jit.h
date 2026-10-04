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
 * @file runtime-jit.h
 * @stability free
 *
 * Umbrella header for Ghoti.io Runtime-jit.
 *
 * The library is the baseline JIT of the language runtime stack: a low-level
 * IR with a builder, a verifier and a printer; an x86-64 backend; and the
 * stack maps and deopt records the backend emits in runtime-core's format
 * (`a/codemeta.h`). It depends on `cutil` and `runtime-core` only (AD-2) and
 * accepts no collector type: the engine passes any barrier or allocation code
 * in, in a later story.
 *
 * Every header is labelled `free` (AD-14): the IR, the backend and the
 * metadata consumers are JIT internals, and a consumer requires the exact
 * version it was built against.
 */

#ifndef GHOTI_IO_GRJIT_RUNTIME_JIT_H
#define GHOTI_IO_GRJIT_RUNTIME_JIT_H

#include <ghoti.io/runtime-jit/macros.h>

#include <ghoti.io/runtime-jit/allocator.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/builder.h>
#include <ghoti.io/runtime-jit/code.h>
#include <ghoti.io/runtime-jit/core.h>
#include <ghoti.io/runtime-jit/ir.h>
#include <ghoti.io/runtime-jit/libver.h>
#include <ghoti.io/runtime-jit/limits.h>
#include <ghoti.io/runtime-jit/print.h>
#include <ghoti.io/runtime-jit/verify.h>

#endif /* GHOTI_IO_GRJIT_RUNTIME_JIT_H */
