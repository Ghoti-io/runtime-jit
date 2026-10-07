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
 * @file
 *
 * The table of registered natives (natives.h): append-only, descriptors copied
 * and individually allocated so that a pointer to one stays valid while the
 * table lives and its ids never change.
 */

#include <ghoti.io/runtime-jit/macros.h>

#include "ir_internal.h"

#include <string.h>

struct GRJIT_NativeTable {
  const GRJIT_Allocator * allocator;
  GRJIT_Limits limits;
  GRJIT_NativeDesc ** entries;
  size_t count;
  size_t capacity;
};

GRJIT_Result grjit_native_table_create(const GRJIT_Limits * limits,
    const GRJIT_Allocator * allocator, GRJIT_NativeTable ** out_table) {
  if (out_table == NULL) {
    return GRJIT_ERR_INVALID;
  }
  const GRJIT_Allocator * a = grjit_allocator_or_default(allocator);
  GRJIT_NativeTable * t = a->calloc_fn(a->ctx, 1, sizeof *t);
  if (t == NULL) {
    return GRJIT_ERR_OOM;
  }
  t->allocator = a;
  grjit_limits_resolve(limits, &t->limits);
  *out_table = t;
  return GRJIT_OK;
}

static bool value_type_ok(GRJIT_Type t) {
  return t == GRJIT_TYPE_I64 || t == GRJIT_TYPE_REF || t == GRJIT_TYPE_PTR;
}

GRJIT_Result grjit_native_table_add(
    GRJIT_NativeTable * table, const GRJIT_NativeDesc * desc, uint32_t * out_id) {
  if (table == NULL || desc == NULL || desc->address == 0 ||
      (desc->param_count != 0 && desc->params == NULL) ||
      (desc->flags & ~(GRJIT_NATIVE_STATUS | GRJIT_NATIVE_REENTERS)) != 0 ||
      (!value_type_ok(desc->result) && desc->result != GRJIT_NATIVE_NO_RESULT)) {
    return GRJIT_ERR_INVALID;
  }
  /* The count is checked before the types are read, so a count that is wrong
   * cannot be what makes the loop read past the array. */
  if (desc->param_count > table->limits.max_native_arguments ||
      desc->stack_bytes > table->limits.max_native_stack_bytes) {
    return GRJIT_ERR_LIMIT;
  }
  for (size_t i = 0; i < desc->param_count; i++) {
    if (!value_type_ok(desc->params[i])) {
      return GRJIT_ERR_INVALID;
    }
  }
  if (table->count >= UINT32_MAX - 1) {
    return GRJIT_ERR_LIMIT;
  }
  const GRJIT_Allocator * a = table->allocator;
  if (table->count == table->capacity) {
    size_t cap = table->capacity == 0 ? 8 : table->capacity * 2;
    if (cap > SIZE_MAX / sizeof *table->entries) {
      return GRJIT_ERR_LIMIT;
    }
    GRJIT_NativeDesc ** grown =
        a->realloc_fn(a->ctx, table->entries, cap * sizeof *grown);
    if (grown == NULL) {
      return GRJIT_ERR_OOM;
    }
    table->entries = grown;
    table->capacity = cap;
  }
  /* One block: the descriptor, then its parameter types. */
  size_t bytes = sizeof(GRJIT_NativeDesc) + desc->param_count * sizeof(GRJIT_Type);
  GRJIT_NativeDesc * copy = a->malloc_fn(a->ctx, bytes);
  if (copy == NULL) {
    return GRJIT_ERR_OOM;
  }
  *copy = *desc;
  GRJIT_Type * types = (GRJIT_Type *)(void *)((char *)copy + sizeof *copy);
  if (desc->param_count != 0) {
    memcpy(types, desc->params, desc->param_count * sizeof *types);
  }
  copy->params = desc->param_count != 0 ? types : NULL;
  if (out_id != NULL) {
    *out_id = (uint32_t)table->count;
  }
  table->entries[table->count++] = copy;
  return GRJIT_OK;
}

size_t grjit_native_table_count(const GRJIT_NativeTable * table) {
  return table == NULL ? 0 : table->count;
}

const GRJIT_NativeDesc * grjit_native_table_get(
    const GRJIT_NativeTable * table, uint32_t id) {
  return table == NULL || id >= table->count ? NULL : table->entries[id];
}

void grjit_native_table_free(GRJIT_NativeTable * table) {
  if (table == NULL) {
    return;
  }
  const GRJIT_Allocator * a = table->allocator;
  for (size_t i = 0; i < table->count; i++) {
    a->free_fn(a->ctx, table->entries[i]);
  }
  a->free_fn(a->ctx, table->entries);
  a->free_fn(a->ctx, table);
}
