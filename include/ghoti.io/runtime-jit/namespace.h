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
 * @file namespace.h
 * @stability free
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GRJIT_NAMESPACE_H
#define GHOTI_IO_GRJIT_NAMESPACE_H

#include <ghoti.io/runtime-jit/libver.h>

/// @cond HIDDEN_SYMBOLS

/* Public types, and the private ones an internal header may name through a
 * struct tag. */
#define GRJIT_Allocator GHOTIIO_RUNTIME_JIT(GRJIT_Allocator)
#define GRJIT_AluOp GHOTIIO_RUNTIME_JIT(GRJIT_AluOp)
#define GRJIT_Asm GHOTIIO_RUNTIME_JIT(GRJIT_Asm)
#define GRJIT_AsmFixup GHOTIIO_RUNTIME_JIT(GRJIT_AsmFixup)
#define GRJIT_AsmStatus GHOTIIO_RUNTIME_JIT(GRJIT_AsmStatus)
#define GRJIT_BlockId GHOTIIO_RUNTIME_JIT(GRJIT_BlockId)
#define GRJIT_BlockInfo GHOTIIO_RUNTIME_JIT(GRJIT_BlockInfo)
#define GRJIT_Builder GHOTIIO_RUNTIME_JIT(GRJIT_Builder)
#define GRJIT_CallAttr GHOTIIO_RUNTIME_JIT(GRJIT_CallAttr)
#define GRJIT_Cmp GHOTIIO_RUNTIME_JIT(GRJIT_Cmp)
#define GRJIT_Code GHOTIIO_RUNTIME_JIT(GRJIT_Code)
#define GRJIT_CompileOptions GHOTIIO_RUNTIME_JIT(GRJIT_CompileOptions)
#define GRJIT_Cond GHOTIIO_RUNTIME_JIT(GRJIT_Cond)
#define GRJIT_Emit GHOTIIO_RUNTIME_JIT(GRJIT_Emit)
#define GRJIT_EntryFn GHOTIIO_RUNTIME_JIT(GRJIT_EntryFn)
#define GRJIT_EntryHook GHOTIIO_RUNTIME_JIT(GRJIT_EntryHook)
#define GRJIT_Exit GHOTIIO_RUNTIME_JIT(GRJIT_Exit)
#define GRJIT_FrameSlot GHOTIIO_RUNTIME_JIT(GRJIT_FrameSlot)
#define GRJIT_FrameSlotKind GHOTIIO_RUNTIME_JIT(GRJIT_FrameSlotKind)
#define GRJIT_FrameState GHOTIIO_RUNTIME_JIT(GRJIT_FrameState)
#define GRJIT_Function GHOTIIO_RUNTIME_JIT(GRJIT_Function)
#define GRJIT_Label GHOTIIO_RUNTIME_JIT(GRJIT_Label)
#define GRJIT_Limits GHOTIIO_RUNTIME_JIT(GRJIT_Limits)
#define GRJIT_LiveSites GHOTIIO_RUNTIME_JIT(GRJIT_LiveSites)
#define GRJIT_MetaStorage GHOTIIO_RUNTIME_JIT(GRJIT_MetaStorage)
#define GRJIT_Op GHOTIIO_RUNTIME_JIT(GRJIT_Op)
#define GRJIT_OpKind GHOTIIO_RUNTIME_JIT(GRJIT_OpKind)
#define GRJIT_Operand GHOTIIO_RUNTIME_JIT(GRJIT_Operand)
#define GRJIT_OperandKind GHOTIIO_RUNTIME_JIT(GRJIT_OperandKind)
#define GRJIT_Pending GHOTIIO_RUNTIME_JIT(GRJIT_Pending)
#define GRJIT_PendingKind GHOTIIO_RUNTIME_JIT(GRJIT_PendingKind)
#define GRJIT_PollHelper GHOTIIO_RUNTIME_JIT(GRJIT_PollHelper)
#define GRJIT_Reg GHOTIIO_RUNTIME_JIT(GRJIT_Reg)
#define GRJIT_Result GHOTIIO_RUNTIME_JIT(GRJIT_Result)
#define GRJIT_SiteLive GHOTIIO_RUNTIME_JIT(GRJIT_SiteLive)
#define GRJIT_SiteRec GHOTIIO_RUNTIME_JIT(GRJIT_SiteRec)
#define GRJIT_Type GHOTIIO_RUNTIME_JIT(GRJIT_Type)
#define GRJIT_UseVisitor GHOTIIO_RUNTIME_JIT(GRJIT_UseVisitor)
#define GRJIT_VReg GHOTIIO_RUNTIME_JIT(GRJIT_VReg)
#define GRJIT_VRegInfo GHOTIIO_RUNTIME_JIT(GRJIT_VRegInfo)

/* Public functions, and the private ones the static archive carries. */
#define grjit_allocator_default GHOTIIO_RUNTIME_JIT(grjit_allocator_default)
#define grjit_allocator_or_default GHOTIIO_RUNTIME_JIT(grjit_allocator_or_default)
#define grjit_asm_add_rsp GHOTIIO_RUNTIME_JIT(grjit_asm_add_rsp)
#define grjit_asm_alu_rr GHOTIIO_RUNTIME_JIT(grjit_asm_alu_rr)
#define grjit_asm_bind GHOTIIO_RUNTIME_JIT(grjit_asm_bind)
#define grjit_asm_bytes GHOTIIO_RUNTIME_JIT(grjit_asm_bytes)
#define grjit_asm_call_r GHOTIIO_RUNTIME_JIT(grjit_asm_call_r)
#define grjit_asm_finish GHOTIIO_RUNTIME_JIT(grjit_asm_finish)
#define grjit_asm_free GHOTIIO_RUNTIME_JIT(grjit_asm_free)
#define grjit_asm_imul_rr GHOTIIO_RUNTIME_JIT(grjit_asm_imul_rr)
#define grjit_asm_init GHOTIIO_RUNTIME_JIT(grjit_asm_init)
#define grjit_asm_jcc GHOTIIO_RUNTIME_JIT(grjit_asm_jcc)
#define grjit_asm_jmp GHOTIIO_RUNTIME_JIT(grjit_asm_jmp)
#define grjit_asm_label GHOTIIO_RUNTIME_JIT(grjit_asm_label)
#define grjit_asm_label_offset GHOTIIO_RUNTIME_JIT(grjit_asm_label_offset)
#define grjit_asm_leave GHOTIIO_RUNTIME_JIT(grjit_asm_leave)
#define grjit_asm_load16s GHOTIIO_RUNTIME_JIT(grjit_asm_load16s)
#define grjit_asm_load16u GHOTIIO_RUNTIME_JIT(grjit_asm_load16u)
#define grjit_asm_load32s GHOTIIO_RUNTIME_JIT(grjit_asm_load32s)
#define grjit_asm_load32u GHOTIIO_RUNTIME_JIT(grjit_asm_load32u)
#define grjit_asm_load64 GHOTIIO_RUNTIME_JIT(grjit_asm_load64)
#define grjit_asm_load8s GHOTIIO_RUNTIME_JIT(grjit_asm_load8s)
#define grjit_asm_load8u GHOTIIO_RUNTIME_JIT(grjit_asm_load8u)
#define grjit_asm_mov32_rr GHOTIIO_RUNTIME_JIT(grjit_asm_mov32_rr)
#define grjit_asm_mov_ri GHOTIIO_RUNTIME_JIT(grjit_asm_mov_ri)
#define grjit_asm_mov_ri64 GHOTIIO_RUNTIME_JIT(grjit_asm_mov_ri64)
#define grjit_asm_mov_rr GHOTIIO_RUNTIME_JIT(grjit_asm_mov_rr)
#define grjit_asm_movzx_r8 GHOTIIO_RUNTIME_JIT(grjit_asm_movzx_r8)
#define grjit_asm_neg GHOTIIO_RUNTIME_JIT(grjit_asm_neg)
#define grjit_asm_not GHOTIIO_RUNTIME_JIT(grjit_asm_not)
#define grjit_asm_pop GHOTIIO_RUNTIME_JIT(grjit_asm_pop)
#define grjit_asm_push GHOTIIO_RUNTIME_JIT(grjit_asm_push)
#define grjit_asm_raw GHOTIIO_RUNTIME_JIT(grjit_asm_raw)
#define grjit_asm_ret GHOTIIO_RUNTIME_JIT(grjit_asm_ret)
#define grjit_asm_sar_cl GHOTIIO_RUNTIME_JIT(grjit_asm_sar_cl)
#define grjit_asm_setcc GHOTIIO_RUNTIME_JIT(grjit_asm_setcc)
#define grjit_asm_shl_cl GHOTIIO_RUNTIME_JIT(grjit_asm_shl_cl)
#define grjit_asm_shr_cl GHOTIIO_RUNTIME_JIT(grjit_asm_shr_cl)
#define grjit_asm_size GHOTIIO_RUNTIME_JIT(grjit_asm_size)
#define grjit_asm_status GHOTIIO_RUNTIME_JIT(grjit_asm_status)
#define grjit_asm_store16 GHOTIIO_RUNTIME_JIT(grjit_asm_store16)
#define grjit_asm_store32 GHOTIIO_RUNTIME_JIT(grjit_asm_store32)
#define grjit_asm_store64 GHOTIIO_RUNTIME_JIT(grjit_asm_store64)
#define grjit_asm_store8 GHOTIIO_RUNTIME_JIT(grjit_asm_store8)
#define grjit_asm_sub_rsp GHOTIIO_RUNTIME_JIT(grjit_asm_sub_rsp)
#define grjit_asm_test_rr GHOTIIO_RUNTIME_JIT(grjit_asm_test_rr)
#define grjit_asm_ud2 GHOTIIO_RUNTIME_JIT(grjit_asm_ud2)
#define grjit_backend_available GHOTIIO_RUNTIME_JIT(grjit_backend_available)
#define grjit_builder_binary GHOTIIO_RUNTIME_JIT(grjit_builder_binary)
#define grjit_builder_bitcast GHOTIIO_RUNTIME_JIT(grjit_builder_bitcast)
#define grjit_builder_block GHOTIIO_RUNTIME_JIT(grjit_builder_block)
#define grjit_builder_br GHOTIIO_RUNTIME_JIT(grjit_builder_br)
#define grjit_builder_br_if GHOTIIO_RUNTIME_JIT(grjit_builder_br_if)
#define grjit_builder_call GHOTIIO_RUNTIME_JIT(grjit_builder_call)
#define grjit_builder_cmp GHOTIIO_RUNTIME_JIT(grjit_builder_cmp)
#define grjit_builder_const GHOTIIO_RUNTIME_JIT(grjit_builder_const)
#define grjit_builder_create GHOTIIO_RUNTIME_JIT(grjit_builder_create)
#define grjit_builder_derived GHOTIIO_RUNTIME_JIT(grjit_builder_derived)
#define grjit_builder_destroy GHOTIIO_RUNTIME_JIT(grjit_builder_destroy)
#define grjit_builder_finish GHOTIIO_RUNTIME_JIT(grjit_builder_finish)
#define grjit_builder_guard GHOTIIO_RUNTIME_JIT(grjit_builder_guard)
#define grjit_builder_load GHOTIIO_RUNTIME_JIT(grjit_builder_load)
#define grjit_builder_move GHOTIIO_RUNTIME_JIT(grjit_builder_move)
#define grjit_builder_param GHOTIIO_RUNTIME_JIT(grjit_builder_param)
#define grjit_builder_poll GHOTIIO_RUNTIME_JIT(grjit_builder_poll)
#define grjit_builder_ret GHOTIIO_RUNTIME_JIT(grjit_builder_ret)
#define grjit_builder_set_block GHOTIIO_RUNTIME_JIT(grjit_builder_set_block)
#define grjit_builder_set_poll_helper GHOTIIO_RUNTIME_JIT(grjit_builder_set_poll_helper)
#define grjit_builder_store GHOTIIO_RUNTIME_JIT(grjit_builder_store)
#define grjit_builder_unary GHOTIIO_RUNTIME_JIT(grjit_builder_unary)
#define grjit_builder_vreg GHOTIIO_RUNTIME_JIT(grjit_builder_vreg)
#define grjit_code_address GHOTIIO_RUNTIME_JIT(grjit_code_address)
#define grjit_code_call GHOTIIO_RUNTIME_JIT(grjit_code_call)
#define grjit_code_destroy GHOTIIO_RUNTIME_JIT(grjit_code_destroy)
#define grjit_code_entry GHOTIIO_RUNTIME_JIT(grjit_code_entry)
#define grjit_code_mapped_size GHOTIIO_RUNTIME_JIT(grjit_code_mapped_size)
#define grjit_code_meta GHOTIIO_RUNTIME_JIT(grjit_code_meta)
#define grjit_code_out_words GHOTIIO_RUNTIME_JIT(grjit_code_out_words)
#define grjit_code_param_count GHOTIIO_RUNTIME_JIT(grjit_code_param_count)
#define grjit_code_size GHOTIIO_RUNTIME_JIT(grjit_code_size)
#define grjit_compile GHOTIIO_RUNTIME_JIT(grjit_compile)
#define grjit_emit_add_pending GHOTIIO_RUNTIME_JIT(grjit_emit_add_pending)
#define grjit_emit_add_site GHOTIIO_RUNTIME_JIT(grjit_emit_add_site)
#define grjit_emit_free GHOTIIO_RUNTIME_JIT(grjit_emit_free)
#define grjit_emit_function GHOTIIO_RUNTIME_JIT(grjit_emit_function)
#define grjit_emit_guard_stub GHOTIIO_RUNTIME_JIT(grjit_emit_guard_stub)
#define grjit_emit_poll_stub GHOTIIO_RUNTIME_JIT(grjit_emit_poll_stub)
#define grjit_emit_refuse_stub GHOTIIO_RUNTIME_JIT(grjit_emit_refuse_stub)
#define grjit_emit_slot GHOTIIO_RUNTIME_JIT(grjit_emit_slot)
#define grjit_frame_slot_constant GHOTIIO_RUNTIME_JIT(grjit_frame_slot_constant)
#define grjit_frame_slot_dead GHOTIIO_RUNTIME_JIT(grjit_frame_slot_dead)
#define grjit_frame_slot_vreg GHOTIIO_RUNTIME_JIT(grjit_frame_slot_vreg)
#define grjit_function_block_count GHOTIIO_RUNTIME_JIT(grjit_function_block_count)
#define grjit_function_block_ops GHOTIIO_RUNTIME_JIT(grjit_function_block_ops)
#define grjit_function_destroy GHOTIIO_RUNTIME_JIT(grjit_function_destroy)
#define grjit_function_frame_state GHOTIIO_RUNTIME_JIT(grjit_function_frame_state)
#define grjit_function_frame_state_count GHOTIIO_RUNTIME_JIT(grjit_function_frame_state_count)
#define grjit_function_interp_slot_count GHOTIIO_RUNTIME_JIT(grjit_function_interp_slot_count)
#define grjit_function_name GHOTIIO_RUNTIME_JIT(grjit_function_name)
#define grjit_function_op_count GHOTIIO_RUNTIME_JIT(grjit_function_op_count)
#define grjit_function_param_count GHOTIIO_RUNTIME_JIT(grjit_function_param_count)
#define grjit_function_poll_helper GHOTIIO_RUNTIME_JIT(grjit_function_poll_helper)
#define grjit_function_print GHOTIIO_RUNTIME_JIT(grjit_function_print)
#define grjit_function_verify GHOTIIO_RUNTIME_JIT(grjit_function_verify)
#define grjit_function_vreg_count GHOTIIO_RUNTIME_JIT(grjit_function_vreg_count)
#define grjit_function_vreg_derived GHOTIIO_RUNTIME_JIT(grjit_function_vreg_derived)
#define grjit_function_vreg_type GHOTIIO_RUNTIME_JIT(grjit_function_vreg_type)
#define grjit_limits_default GHOTIIO_RUNTIME_JIT(grjit_limits_default)
#define grjit_limits_resolve GHOTIIO_RUNTIME_JIT(grjit_limits_resolve)
#define grjit_liveness_compute GHOTIIO_RUNTIME_JIT(grjit_liveness_compute)
#define grjit_liveness_free GHOTIIO_RUNTIME_JIT(grjit_liveness_free)
#define grjit_memory_create GHOTIIO_RUNTIME_JIT(grjit_memory_create)
#define grjit_memory_destroy GHOTIIO_RUNTIME_JIT(grjit_memory_destroy)
#define grjit_metadata_build GHOTIIO_RUNTIME_JIT(grjit_metadata_build)
#define grjit_metadata_free GHOTIIO_RUNTIME_JIT(grjit_metadata_free)
#define grjit_op_def GHOTIIO_RUNTIME_JIT(grjit_op_def)
#define grjit_op_has_state GHOTIIO_RUNTIME_JIT(grjit_op_has_state)
#define grjit_op_is_terminator GHOTIIO_RUNTIME_JIT(grjit_op_is_terminator)
#define grjit_op_visit_uses GHOTIIO_RUNTIME_JIT(grjit_op_visit_uses)
#define grjit_operand_imm GHOTIIO_RUNTIME_JIT(grjit_operand_imm)
#define grjit_operand_none GHOTIIO_RUNTIME_JIT(grjit_operand_none)
#define grjit_operand_vreg GHOTIIO_RUNTIME_JIT(grjit_operand_vreg)
#define grjit_result_string GHOTIIO_RUNTIME_JIT(grjit_result_string)
#define grjit_unwind_register GHOTIIO_RUNTIME_JIT(grjit_unwind_register)
#define grjit_version_number GHOTIIO_RUNTIME_JIT(grjit_version_number)
#define grjit_version_string GHOTIIO_RUNTIME_JIT(grjit_version_string)

/// @endcond

#endif /* GHOTI_IO_GRJIT_NAMESPACE_H */
