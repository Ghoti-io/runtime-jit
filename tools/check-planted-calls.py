#!/usr/bin/env python3
"""Plant, one at a time, the defects that calls between compiled functions
(AD-28) can have, and show that the tests fail on each.

A defect here is a one-line edit to the source of runtime-core or of
runtime-jit, made in a scratch copy of the repository. For each, the copy is
built and the tests that should notice are run:

  CAUGHT       the build succeeded and a test failed, or the program was killed
               by a signal (code freed under a frame faults the process, which
               is what an early free is; a test that hangs is aborted by the
               tests' own watchdog, tests/test_helpers.h, which is a signal too)
  MISSED       the build succeeded and every test passed: the suite is blind to
               the defect, and this script fails
  TIMEOUT      the tests did not finish in time. NOT a catch: a hang says the
               defect was reached, not that a test noticed it, so it fails
  BUILD FAILED the edit does not compile. NOT a catch either: a build that
               breaks proves nothing about the tests

The harness is itself shown to fail: `--self-test` runs it against edits whose
verdict is known (an edit that changes nothing, one that does not compile, one
that hangs) and requires MISSED, BUILD FAILED and TIMEOUT respectively, so a
harness that reports CAUGHT for everything - a test run that never ran, a
build error read as a failure - cannot pass. Before any mutation, the control
runs the unmutated copy: every test must pass there, or the verdicts after it
mean nothing.

Usage: check-planted-calls.py [--prefix=DIR] [--core=DIR] [--timeout=SECONDS]
                              [--self-test] [name-substring ...]
  --prefix  a prefix holding runtime-core's dependencies and runtime-jit's
            (default $GHOTI_PREFIX, then the PREFIX of the make that runs it);
            it is COPIED, so nothing in it is touched
  --core    runtime-core's checkout (default ../runtime-core beside this one)

Nothing in either repository is edited. Exit status 0 only if the control
passed and every mutation was CAUGHT (or, for --self-test, every verdict is the
expected one).
"""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
JIT_SRC = os.path.dirname(HERE)

args = []
opts = {}
for a in sys.argv[1:]:
    if a.startswith('--') and '=' in a:
        k, v = a[2:].split('=', 1)
        opts[k] = v
    elif a.startswith('--'):
        opts[a[2:]] = True
    else:
        args.append(a)

prefix = opts.get('prefix') or os.environ.get('GHOTI_PREFIX') or os.environ.get('PREFIX')
if not prefix or not os.path.isdir(prefix):
    sys.exit('check-planted-calls: give --prefix=DIR (or set PREFIX): a prefix that holds '
             'runtime-core and runtime-jit\'s dependencies')
prefix = os.path.abspath(prefix)
CORE_SRC = os.path.abspath(opts.get('core') or os.path.join(JIT_SRC, '..', 'runtime-core'))
if not os.path.isdir(os.path.join(CORE_SRC, 'src')):
    sys.exit('check-planted-calls: runtime-core is not at %s; give --core=DIR' % CORE_SRC)
TIMEOUT = int(opts.get('timeout', 180))
SELF_TEST = 'self-test' in opts

T = tempfile.mkdtemp(prefix='planted-calls-')
P = os.path.join(T, 'prefix')


def sh(cmd, cwd, timeout=None, extra_env=None):
    e = dict(os.environ, PKG_CONFIG_PATH=os.path.join(P, 'share', 'pkgconfig'))
    e.update(extra_env or {})
    e.pop('LD_LIBRARY_PATH', None)
    try:
        r = subprocess.run(cmd, shell=True, cwd=cwd, env=e, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as ex:
        out = ex.stdout.decode() if isinstance(ex.stdout, bytes) else (ex.stdout or '')
        return 'timeout', out
    return r.returncode, r.stdout


def copy_tree(src, dst):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns('build', '.git'), symlinks=True)


# The copy of the prefix, with its .pc files pointing at the copy.
shutil.copytree(prefix, P, symlinks=True)
pc = os.path.join(P, 'share', 'pkgconfig')
for f in os.listdir(pc):
    path = os.path.join(pc, f)
    text = open(path).read().replace(prefix, P)
    open(path, 'w').write(text)
CORE_PRISTINE = os.path.join(T, 'core-pristine')
JIT_PRISTINE = os.path.join(T, 'jit-pristine')
copy_tree(CORE_SRC, CORE_PRISTINE)
copy_tree(JIT_SRC, JIT_PRISTINE)

# (name, lib, file, old, new[, tests]): `old` must occur exactly once in the file. With `tests`, only
# those test programs run (and none of core's): the mutations of calls to natives are each shown to be
# caught by `testNatives` or `testNative_ir` alone, so that they cannot lean on an older test.
M = [
    ("frame missed: a deep rebuild skips the fourth frame of the run", "core",
     "src/a/deopt.c",
     "    size_t index = SIZE_MAX;\n    (void)grcore_compiled_guest_index(&cf, &index);\n    if (index >= keep_frames) {\n      continue;\n    }\n    uint64_t * slots = guest_slots(stack, guest);",
     "    size_t index = SIZE_MAX;\n    (void)grcore_compiled_guest_index(&cf, &index);\n    if (index >= keep_frames || cf.run_depth == 3) {\n      continue;\n    }\n    uint64_t * slots = guest_slots(stack, guest);"),
    ("frame missed: the rebuild stops after the run's first frame", "core",
     "src/a/deopt.c",
     "  const size_t n = first.run_length;\n  const size_t record = first.record;",
     "  const size_t n = first.run_length > 1 ? 1 : first.run_length;\n  const size_t record = first.record;"),
    ("early free: a slot cleared under a live JIT record releases its code at once", "core",
     "src/a/registry.c",
     "  if (stack->jit_live == 0) {\n    grcore_code_release(code);\n    const GRCORE_Allocator * a = grcore_context_allocator(stack->context);\n    a->free_fn(a->ctx, node);\n    return;\n  }\n  node->code",
     "  if (true) {\n    grcore_code_release(code);\n    const GRCORE_Allocator * a = grcore_context_allocator(stack->context);\n    a->free_fn(a->ctx, node);\n    return;\n  }\n  node->code"),
    ("early free: unregistering a range under a live JIT record releases it at once", "core",
     "src/a/registry.c",
     "  if (stack->jit_live == 0) {\n    grcore_code_release(r->entries[at].code);",
     "  if (stack->jit_live == 0 || stack != NULL) {\n    grcore_code_release(r->entries[at].code);"),
    ("short reservation: the up-front check that the cells suffice is gone", "core",
     "src/a/deopt.c",
     "  if (need > (reservation == NULL ? 0 : reservation->capacity)) {\n    return GRCORE_ERR_INVALID; /* a short reservation is a defect, caught here */\n  }",
     "  if (false) {\n    return GRCORE_ERR_INVALID;\n  }"),
    ("stack-map reference left out: the root pass skips the last live entry of a compiled frame", "core",
     "src/a/roots.c",
     "  for (size_t i = 0; i < site->live_count; i++) {\n    const GRCORE_CodeLocation * loc = &site->live[i];",
     "  for (size_t i = 0; i + 1 < site->live_count; i++) {\n    const GRCORE_CodeLocation * loc = &site->live[i];"),
    ("pairing lost: a compiled frame's guest frame is reported beside it", "core",
     "src/a/roots.c",
     "      if (have_guest && can_pair && index == remaining - 1 &&\n          h.engine == cf.engine && h.function == cf.identity.function) {",
     "      if (false) {"),
    ("walk start not taken from the cell: a walk finds no run", "core",
     "src/a/compiled.c",
     "  if (stack != NULL && !grcore_activation_absorb_cell((GRCORE_Stack *)stack)) {",
     "  if (false) {"),
    ("chain end not definitive: a return address in no registered code ends a run silently", "core",
     "src/a/compiled.c",
     "    walk->failing = true;\n    walk->reason = \"a compiled frame returns into no registered code, and the \"\n                   \"chain-end marker was not met\";\n    return GRCORE_CWALK_FRAME;",
     "    walk->base = 0;\n    walk->return_address = 0;\n    return GRCORE_CWALK_FRAME;"),

    # The edits of runtime-jit's own code.
    ("a call through a pointer does not test the status for DEOPTED", "jit",
     "src/x86_64/emit.c",
     "  grjit_asm_test_rr(a, ISTATUS(e), ISTATUS(e));\n  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_propagate);\n  if (op->dst != GRJIT_NO_VREG) {",
     "  grjit_asm_test_rr(a, ISTATUS(e), ISTATUS(e));\n  if (op->kind == GRJIT_OP_CALL_SLOT) {\n    grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_propagate);\n  }\n  if (op->dst != GRJIT_NO_VREG) {"),
    ("a call through a pointer leaves the references out of its call site's stack map", "jit",
     "src/backend/metadata.c",
     "        GRCORE_CodeLocation l = slot_location(v, info->type);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 2",
     "        if (r->op != NULL && r->op->kind == GRJIT_OP_CALL_PTR &&\n            r->kind == GRCORE_SITE_GC_POINT_CALL) {\n          continue;\n        }\n        GRCORE_CodeLocation l = slot_location(v, info->type);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 2"),
    ("a call through a pointer does not store the walk start before its push", "jit",
     "src/x86_64/emit.c",
     "  GRJIT_Label push_ret = grjit_asm_label(a);\n  grjit_emit_store_walk_cell(e, push_ret);",
     "  GRJIT_Label push_ret = grjit_asm_label(a);\n  if (op->kind == GRJIT_OP_CALL_SLOT) {\n    grjit_emit_store_walk_cell(e, push_ret);\n  }"),
    ("a call through a pointer enters code that is retired", "jit",
     "src/code/target.c",
     "      range.retired || target < range.start + GRJIT_ENTRY_TAG_BYTES) {",
     "      target < range.start + GRJIT_ENTRY_TAG_BYTES) {"),
    ("a call through a pointer does not check the callee's token", "jit",
     "src/code/target.c",
     "tag[0] == GRJIT_ENTRY_TAG_WORD(arg_count) && tag[1] == callee ? 1u : 0u",
     "(void)callee, tag[0] == GRJIT_ENTRY_TAG_WORD(arg_count) ? 1u : 0u"),
    ("a call through a pointer does not check the callee's arity", "jit",
     "src/code/target.c",
     "tag[0] == GRJIT_ENTRY_TAG_WORD(arg_count) && tag[1] == callee ? 1u : 0u",
     "(tag[0] >> 32) == (GRJIT_ENTRY_TAG_WORD(arg_count) >> 32) && tag[1] == callee ? 1u : 0u"),
    ("a refused rebuild after a call's deopt exit is ignored", "jit",
     "src/x86_64/exit.c",
     "  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);\n}",
     "}"),
    ("a refused rebuild after the exit state's deopt hook is ignored", "jit",
     "src/x86_64/exit.c",
     "  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);\n  grjit_asm_mov_ri(a, GRJIT_RAX, 0);",
     "  grjit_asm_mov_ri(a, GRJIT_RAX, 0);"),
    ("a derived pointer passed as an argument is not recorded in the push site's map", "jit",
     "src/backend/metadata.c",
     "    der_total += push_derived_args(f, &recs[i]);",
     "    der_total += 0 * push_derived_args(f, &recs[i]);"),

    # Tail calls (story 5).
    ("a refused tail hook is ignored: the frame is replaced although the guest frame was not", "jit",
     "src/x86_64/emit.c",
     "  grjit_asm_jcc(a, GRJIT_COND_NE, exit);\n\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 14",
     "  (void)exit;\n\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 14"),
    ("a tail hook's answer is tested in 64 bits, so garbage above a zero is a refusal", "jit",
     "src/x86_64/emit.c",
     "  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, exit);\n\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 14",
     "  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, exit);\n\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 14"),
    ("a tail call does not store the walk start before its hook", "jit",
     "src/x86_64/emit.c",
     "  GRJIT_Label hook_ret = grjit_asm_label(a);\n  grjit_emit_store_walk_cell(e, hook_ret);",
     "  GRJIT_Label hook_ret = grjit_asm_label(a);"),
    ("a tail call leaves the return address where it was, not where the callee's return pops it from", "jit",
     "src/x86_64/emit.c",
     "  grjit_asm_store64(a, GRJIT_RBP, ra_new, scratch);",
     "  grjit_asm_store64(a, GRJIT_RBP, 8, scratch);"),
    ("the verifier does not need a tail hook for a tail call", "jit",
     "src/ir/verify.c",
     "        if (h->tail == NULL || (slot && h->compile == NULL)) {",
     "        if (slot && h->compile == NULL) {"),

    # Found missing by the review of story 5.
    ("a tail call's exit stub is built from the hook's site set, not the exit's", "jit",
     'src/x86_64/emit.c',
     '  GRJIT_Label exit = emit_dispatch(e, op, live, live + 1);\n  emit_stage_arguments(e, op);\n\n  GRJIT_Label hook_ret',
     '  GRJIT_Label exit = emit_dispatch(e, op, live, live);\n  emit_stage_arguments(e, op);\n\n  GRJIT_Label hook_ret'),
    ("a tail call takes the caller's base from rbp, not from its saved word (the callee's chain loops)", "jit",
     'src/x86_64/emit.c',
     '  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, 0);\n  size_t copy_to = n;',
     '  grjit_asm_mov_rr(a, GRJIT_RAX, GRJIT_RBP);\n  size_t copy_to = n;'),
    ("a tail call's code pointer is not a use for the definite-assignment check", "jit",
     'src/ir/function.c',
     '      visit_operand(&op->a, visit, user); /* the code pointer of a pointer call */',
     '      if (op->kind != GRJIT_OP_TAIL_CALL_PTR) visit_operand(&op->a, visit, user); /* the code pointer of a pointer call */'),
    ('a tag alone makes an unregistered address code for a call through a pointer', "jit",
     'src/code/target.c',
     '  if (!grcore_code_lookup((const GRCORE_Context *)context, (uintptr_t)target, &range) ||\n      range.retired || target < range.start + GRJIT_ENTRY_TAG_BYTES) {\n    return 0;\n  }',
     '  if (!grcore_code_lookup((const GRCORE_Context *)context, (uintptr_t)target, &range)) {\n    range.retired = false; range.start = 0;\n  }\n  if (range.retired || target < range.start + GRJIT_ENTRY_TAG_BYTES) {\n    return 0;\n  }'),
    ('the tail hook is handed one argument, whatever the count', "jit",
     'src/x86_64/emit.c',
     '  grjit_asm_mov_ri(a, C_ARG(e, 3), n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.tail);',
     '  grjit_asm_mov_ri(a, C_ARG(e, 3), n > 1 ? 1 : n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.tail);'),
    ('the tail hook is handed its arguments from the second slot', "jit",
     'src/x86_64/emit.c',
     '  grjit_asm_lea(a, C_ARG(e, 2), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, 0));\n  grjit_asm_mov_ri(a, C_ARG(e, 3), n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.tail);',
     '  grjit_asm_lea(a, C_ARG(e, 2), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, n > 1 ? 1 : 0));\n  grjit_asm_mov_ri(a, C_ARG(e, 3), n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.tail);'),
    ('the push hook is handed one argument, whatever the count', "jit",
     'src/x86_64/emit.c',
     '  grjit_asm_mov_ri(a, C_ARG(e, 3), n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.push);',
     '  grjit_asm_mov_ri(a, C_ARG(e, 3), n > 1 ? 1 : n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.push);'),
    ('the push hook is handed its arguments from the second slot', "jit",
     'src/x86_64/emit.c',
     '  grjit_asm_lea(a, C_ARG(e, 2), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, 0));\n  grjit_asm_mov_ri(a, C_ARG(e, 3), n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.push);',
     '  grjit_asm_lea(a, C_ARG(e, 2), GRJIT_RBP, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, n > 1 ? 1 : 0));\n  grjit_asm_mov_ri(a, C_ARG(e, 3), n);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)f->hooks.push);'),
    ("a call's code pointer is not a use for the definite-assignment check", "jit",
     'src/ir/function.c',
     '      visit_operand(&op->a, visit, user); /* the code pointer of a pointer call */',
     '      if (op->kind != GRJIT_OP_CALL_PTR) visit_operand(&op->a, visit, user); /* the code pointer of a pointer call */'),
    ("a tail hook's reference arguments are recorded at the unpadded slot", "jit",
     'src/backend/metadata.c',
     '          l.value = GRJIT_ARGS_SLOT(GRJIT_SHAPE_REGS(f, shape), shape.args_area, k);',
     '          l.value = GRJIT_ARGS_SLOT(f->vreg_count, shape.args_area, k);'),
    ("a tail hook's derived arguments are recorded at the unpadded slot", "jit",
     'src/backend/metadata.c',
     '          d->slot = GRJIT_ARGS_SLOT(GRJIT_SHAPE_REGS(f, shape), shape.args_area, k);',
     '          d->slot = GRJIT_ARGS_SLOT(f->vreg_count, shape.args_area, k);'),

    # Calls to natives (story 6): each is shown to be caught by testNatives or testNative_ir alone.
    ("native: the status is never tested, so a non-zero status continues in compiled code", "jit",
     'src/x86_64/emit.c',
     '    grjit_asm_test_rr(a, second, second);\n    grjit_asm_jcc(a, GRJIT_COND_NE, leave);',
     '    (void)leave;',
     ['testNatives']),
    ("native: the status exit is built from the state before the call, so the native runs twice", "jit",
     'src/x86_64/exit.c',
     '      state->identity, p->live_index, p->op->exit_state);\n  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_OUT);',
     '      state->identity, p->live_index, p->op->state);\n  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_OUT);',
     ['testNatives']),
    ("native: the result is stored after the status test, so a status exit finds the old value", "jit",
     'src/x86_64/emit.c',
     '  if (op->dst != GRJIT_NO_VREG) {\n    store_result(e, op->dst, GRJIT_RAX);\n  }\n  if (status) {\n    GRJIT_Label leave = grjit_asm_label(a);',
     '  if (op->dst != GRJIT_NO_VREG && !status) {\n    store_result(e, op->dst, GRJIT_RAX);\n  }\n  if (status) {\n    GRJIT_Label leave = grjit_asm_label(a);',
     ['testNatives']),
    ("native: the cause of a status exit lacks the native bit", "jit",
     'src/x86_64/exit.c',
     '  grjit_asm_alu_rr(a, GRJIT_ALU_OR, C_ARG(e, 1), GRJIT_RAX);\n',
     '',
     ['testNatives']),
    ("native: the stack-argument area is not rounded to sixteen bytes", "jit",
     'src/x86_64/emit.c',
     '  uint32_t area = (uint32_t)((stack_words * 8 + 15) / 16 * 16);',
     '  uint32_t area = (uint32_t)(stack_words * 8);',
     ['testNatives']),
    ("native: the caller does not pop the stack arguments", "jit",
     'src/x86_64/emit.c',
     '    grjit_asm_add_rsp(a, area);',
     '    (void)area;',
     ['testNatives']),
    ("native: stack argument k is stored one word too high", "jit",
     'src/x86_64/emit.c',
     '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - (creg - 1))), GRJIT_RAX);',
     '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - (creg - 1)) + 8), GRJIT_RAX);',
     ['testNatives']),
    ("native: the references are left out of a native call site's stack map", "jit",
     'src/backend/metadata.c',
     '        GRCORE_CodeLocation l = slot_location(v, info->type);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 2',
     '        if (r->op != NULL && r->op->kind == GRJIT_OP_CALL_NATIVE &&\n            r->kind == GRCORE_SITE_GC_POINT_CALL) {\n          continue;\n        }\n        GRCORE_CodeLocation l = slot_location(v, info->type);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 2',
     ['testNatives']),
    ("native: the derived pointers are left out of a native call site's map", "jit",
     'src/backend/metadata.c',
     '      } else {\n        GRCORE_DerivedPointer * d = &out->derived[der_at + ders++];\n        d->slot = grjit_emit_slot(v);',
     '      } else if (r->op != NULL && r->op->kind == GRJIT_OP_CALL_NATIVE &&\n                 r->kind == GRCORE_SITE_GC_POINT_CALL) {\n        continue;\n      } else {\n        GRCORE_DerivedPointer * d = &out->derived[der_at + ders++];\n        d->slot = grjit_emit_slot(v);',
     ['testNatives']),
    ("native: the stack check leaves out the stack-argument area", "jit",
     'src/x86_64/emit.c',
     '  uint64_t need = (uint64_t)area + d->stack_bytes;',
     '  uint64_t need = d->stack_bytes;',
     ['testNatives']),
    ("native: the stack check leaves out the native's declared use", "jit",
     'src/x86_64/emit.c',
     '  uint64_t need = (uint64_t)area + d->stack_bytes;',
     '  uint64_t need = area;',
     ['testNatives']),
    ("native: the verifier does not refuse a call with fewer arguments than the descriptor", "jit",
     'src/ir/verify.c',
     '      if (op->arg_count != d->param_count) {',
     '      if (op->arg_count > d->param_count) {',
     ['testNative_ir']),
    ("native: the verifier does not check an argument register's type against the descriptor", "jit",
     'src/ir/verify.c',
     '          if (type_of(v, o->vreg) != d->params[i]) {',
     '          if (false) {',
     ['testNative_ir']),
    ("native: the verifier accepts a destination of another type than the native's result", "jit",
     'src/ir/verify.c',
     '        if (type_of(v, op->dst) != d->result) {',
     '        if (false) {',
     ['testNative_ir']),
    ("native: the verifier accepts a status native with no state after the call", "jit",
     'src/ir/verify.c',
     '        if (op->exit_state == GRJIT_NO_STATE) {\n          return refuse(v, GRJIT_ERR_INVALID,\n              "block b%u op %zu: native #%u returns a status',
     '        if (false) {\n          return refuse(v, GRJIT_ERR_INVALID,\n              "block b%u op %zu: native #%u returns a status',
     ['testNative_ir']),
    ("early free (native): a slot cleared under a live JIT record releases its code at once", "core",
     "src/a/registry.c",
     "  if (stack->jit_live == 0) {\n    grcore_code_release(code);\n    const GRCORE_Allocator * a = grcore_context_allocator(stack->context);\n    a->free_fn(a->ctx, node);\n    return;\n  }\n  node->code",
     "  if (true) {\n    grcore_code_release(code);\n    const GRCORE_Allocator * a = grcore_context_allocator(stack->context);\n    a->free_fn(a->ctx, node);\n    return;\n  }\n  node->code",
     ['testNatives']),
    ("early free (native): unregistering a range under a live JIT record releases it at once", "core",
     "src/a/registry.c",
     "  if (stack->jit_live == 0) {\n    grcore_code_release(r->entries[at].code);",
     "  if (stack->jit_live == 0 || stack != NULL) {\n    grcore_code_release(r->entries[at].code);",
     ['testNatives']),

    # Found missing by the review of story 6, each caught by `testNatives` or `testNative_ir` alone (an entry whose
    # `old` and `new` are lists is several edits to one file).
    ('review: exit-before stub of a status native uses the after-call state (and identity)', 'jit', 'src/x86_64/exit.c', '  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];\n  grjit_asm_bind(a, p->entry);\n  GRJIT_Label ret = grjit_asm_label(a);\n  grjit_asm_mov_ri(a, C_ARG(e, 1), 0);\n  call_deopt_hook(e, ret);\n  grjit_asm_bind(a, ret);\n  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,\n      state->identity, p->live_index, p->op->state);', '  const uint32_t xs = p->op->exit_state != GRJIT_NO_STATE ? p->op->exit_state : p->op->state;\n  const GRJIT_FrameState * state = &e->c.f->states[xs];\n  grjit_asm_bind(a, p->entry);\n  GRJIT_Label ret = grjit_asm_label(a);\n  grjit_asm_mov_ri(a, C_ARG(e, 1), 0);\n  call_deopt_hook(e, ret);\n  grjit_asm_bind(a, ret);\n  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,\n      state->identity, p->live_index, xs);', ['testNatives']),
    ('review: pre-call exit and status exit swap their site sets for a status native', 'jit', 'src/x86_64/emit.c', ['    p.live_index = live + 1;', '    p.live_index = live + 2;'], ['    p.live_index = status ? live + 2 : live + 1;', '    p.live_index = live + 1;'], ['testNatives']),
    ("review: status exit uses the call's site set", 'jit', 'src/x86_64/emit.c', '    p.live_index = live + 2;', '    p.live_index = live;', ['testNatives']),
    ('review: visit_uses ignores the state after a native call (definite assignment of its operands)', 'jit', 'src/ir/function.c', '    uint32_t which[2] = {op->state, op->exit_state};', '    uint32_t which[2] = {op->state, op->kind == GRJIT_OP_CALL_NATIVE ? GRJIT_NO_STATE : op->exit_state};', ['testNative_ir']),
    ("review: call's site set keeps the result when only the after state names it", 'jit', 'src/backend/liveness.c', '!(pass_state == 1 && st->slots[q].vreg == result)', 'true', ['testNatives']),
    ("review: status exit's set built from the state before the call", 'jit', 'src/backend/liveness.c', 'const uint32_t which_state = k == 2 ? op->exit_state : op->state;', 'const uint32_t which_state = op->state;', ['testNatives']),
    ('review: status stub returns zero not the cause in out[0]', 'jit', 'src/x86_64/exit.c', '  grjit_asm_load64(a, GRJIT_RAX, GRJIT_RBP, GRJIT_SLOT_OUT);\n  grjit_asm_jmp(a, e->ret_deopted);\n}\n\nvoid grjit_emit_overflow', '  grjit_asm_mov_ri(a, GRJIT_RAX, 0);\n  grjit_asm_jmp(a, e->ret_deopted);\n}\n\nvoid grjit_emit_overflow', ['testNatives']),
    ('review: call site frame state/identity is the after state for a status native', 'jit', 'src/x86_64/emit.c', '      st->identity, live, op->state, op);', '      (status ? f->states[op->exit_state].identity : st->identity), live, status ? op->exit_state : op->state, op);', ['testNatives']),
    ('review: status exit site is a GC_POINT_CALL not a GUARD', 'jit', 'src/x86_64/exit.c', '  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,\n      state->identity, p->live_index, p->op->exit_state);', '  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GC_POINT_CALL,\n      state->identity, p->live_index, p->op->exit_state);', ['testNatives']),
    ("review: walk cell not stored by the exits' hook for natives (cell holds call's start)", 'jit', 'src/x86_64/exit.c', '  grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, C_ARG(e, 1));\n  call_deopt_hook(e, ret);', '  grjit_asm_store64(a, GRJIT_RBP, GRJIT_SLOT_OUT, GRJIT_RSI);\n  grjit_asm_mov_rr(a, C_ARG(e, 0), GRJIT_RCX);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->hooks.deopt);\n  grjit_asm_call_r(a, GRJIT_RAX);\n  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);\n  if (0) call_deopt_hook(e, ret);', ['testNatives']),
    ('review: live cursor advances 3 for a native without status', 'jit', 'src/x86_64/emit.c', 'e->c.live_cursor += status ? 3 : 2;', 'e->c.live_cursor += 3;', ['testNatives']),
    ('review: deopt hook gets no context (rdi zero) on every exit', 'jit', 'src/x86_64/exit.c', '  grjit_asm_mov_rr(a, C_ARG(e, 0), GRJIT_RCX);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->hooks.deopt);', '  grjit_asm_mov_ri(a, C_ARG(e, 0), 0);\n  grjit_asm_mov_ri(a, GRJIT_RAX, (uint64_t)(uintptr_t)e->c.f->hooks.deopt);', ['testNatives']),
    ('review: pre-call exit site is a GC_POINT_CALL', 'jit', 'src/x86_64/exit.c', '  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GUARD,\n      state->identity, p->live_index, p->op->state);\n  grjit_asm_mov_ri(a, GRJIT_RAX, 0);', '  grjit_emit_add_site(&e->c, (uint32_t)grjit_asm_size(a), GRCORE_SITE_GC_POINT_CALL,\n      state->identity, p->live_index, p->op->state);\n  grjit_asm_mov_ri(a, GRJIT_RAX, 0);', ['testNatives']),
    ('review: liveness: pre-call exit of a status native uses the state after', 'jit', 'src/backend/liveness.c', 'const uint32_t which_state = k == 2 ? op->exit_state : op->state;', 'const uint32_t which_state = k >= 1 && op->exit_state != GRJIT_NO_STATE ? op->exit_state : op->state;', ['testNatives']),
    ('review: status natives: stack arguments not popped', 'jit', 'src/x86_64/emit.c', '  if (area != 0) {\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 16', '  if (area != 0 && !status) {\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 16', ['testNatives']),
    ('review: status native with no result: status never tested', 'jit', 'src/x86_64/emit.c', '  if (status) {\n    GRJIT_Label leave = grjit_asm_label(a);', '  if (status && op->dst != GRJIT_NO_VREG) {\n    GRJIT_Label leave = grjit_asm_label(a);', ['testNatives']),
    ('review: status natives: stack argument one word too high', 'jit', 'src/x86_64/emit.c', '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - (creg - 1))), GRJIT_RAX);\n#endif', '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(8 * (i - (creg - 1)) + (status ? 8 : 0)), GRJIT_RAX);\n#endif', ['testNatives']),
    ('review: status natives: stack check omits the area', 'jit', 'src/x86_64/emit.c', '  uint64_t need = (uint64_t)area + d->stack_bytes;', '  uint64_t need = (uint64_t)(status ? 0 : area) + d->stack_bytes;', ['testNatives']),
    ('review: status natives: stack check omits declared use', 'jit', 'src/x86_64/emit.c', '  uint64_t need = (uint64_t)area + d->stack_bytes;', '  uint64_t need = (uint64_t)area + (status ? 0 : d->stack_bytes);', ['testNatives']),
    ('review: call set keeps the result', 'jit', 'src/backend/liveness.c', '                if (result != GRJIT_NO_VREG && result < f->vreg_count &&\n                    index[result] != UINT32_MAX) {\n                  clear_bit(site_set, index[result]);\n                }', '', ['testNatives']),
    ('review: native with REF arguments > 6 : arg 7 taken from the wrong operand (args[i-1])', 'jit', 'src/x86_64/emit.c', '    load_operand(e, GRJIT_RAX, &op->args[i]);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 19', '    load_operand(e, GRJIT_RAX, &op->args[i > 8 ? i - 1 : i]);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 19', ['testNatives']),
    ('review: status natives: area not rounded', 'jit', 'src/x86_64/emit.c', '  e->c.live_cursor += status ? 3 : 2; /* the call, the exit before it, the status exit */', '  e->c.live_cursor += status ? 3 : 2;\n  if (status) { area = (uint32_t)(stack_words * 8); }', ['testNatives']),
    ('review: native call: no walk start stored on a native whose result is unused', 'jit', 'src/x86_64/emit.c', '#else\n  grjit_emit_store_walk_cell(e, ret);\n#endif', '#else\n  if (op->dst != GRJIT_NO_VREG) grjit_emit_store_walk_cell(e, ret);\n#endif', ['testNatives']),
    ('review: native call: walk start not stored for natives with status', 'jit', 'src/x86_64/emit.c', '#else\n  grjit_emit_store_walk_cell(e, ret);\n#endif', '#else\n  if (!status) grjit_emit_store_walk_cell(e, ret);\n#endif', ['testNatives']),
    ('review: native call: walk start not stored for natives of more than 5 arguments', 'jit', 'src/x86_64/emit.c', '#else\n  grjit_emit_store_walk_cell(e, ret);\n#endif', '#else\n  if (n <= 5) grjit_emit_store_walk_cell(e, ret);\n#endif', ['testNatives']),
    ('review: native call: ctx reloaded wrongly (rdi from rbp-8 +0) for natives with >10 args', 'jit', 'src/x86_64/emit.c', '  grjit_asm_load64(a, C_ARG(e, 0), GRJIT_RBP, GRJIT_SLOT_CTX);\n  for (size_t i = 0; i < n && i + 1', '  grjit_asm_load64(a, C_ARG(e, 0), GRJIT_RBP, n > 10 ? GRJIT_SLOT_OUT : GRJIT_SLOT_CTX);\n  for (size_t i = 0; i < n && i + 1', ['testNatives']),
    ('review: verifier: after-state names a vreg checked only for existence (dst exemption widened to any vreg)', 'jit', 'src/ir/function.c', '              s->slots[i].vreg == op->dst) {', '              true) {', ['testNative_ir']),
    ('review: a refused rebuild after a native exit is ignored (shared hook-call code)', 'jit', 'src/x86_64/exit.c', '  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);\n}', '}', ['testNatives']),
    ('review: the status is tested in all 64 bits of rdx, so garbage above a zero status leaves', 'jit', 'src/x86_64/emit.c', '    grjit_asm_mov32_rr(a, second, second);\n    grjit_asm_test_rr(a, second, second);', '    grjit_asm_test_rr(a, second, second);', ['testNatives']),
    ('review: only the low 16 bits of the status are tested', 'jit', 'src/x86_64/emit.c', '    grjit_asm_mov32_rr(a, second, second);\n    grjit_asm_test_rr(a, second, second);', '    grjit_asm_mov_ri(a, GRJIT_RCX, 0xFFFF);\n    grjit_asm_alu_rr(a, GRJIT_ALU_AND, second, GRJIT_RCX);\n    grjit_asm_test_rr(a, second, second);', ['testNatives']),
    ('review: the status exit site names the identity of the state before the call', 'jit', 'src/x86_64/exit.c', '  const GRJIT_FrameState * state = &e->c.f->states[p->op->exit_state];\n  grjit_asm_bind(a, p->entry);\n  GRJIT_Label ret = grjit_asm_label(a);\n  grjit_asm_mov32_rr', '  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];\n  grjit_asm_bind(a, p->entry);\n  GRJIT_Label ret = grjit_asm_label(a);\n  grjit_asm_mov32_rr', ['testNatives']),
]

# What each library's tests are.
CORE_TESTS = ['testRebuild', 'testRegistry', 'testCompiled']
JIT_TESTS = ['testCalls', 'testCall_ir', 'testTail', 'testTail_ir', 'testNatives', 'testNative_ir']

# Edits whose verdict is known, for --self-test: (name, expected, file, old, new).
SELF = [
    ("an edit that changes nothing", "MISSED", "src/code/target.c",
     "/* The tag is in the sixteen bytes before the entry",
     "/* (a comment only) The tag is in the sixteen bytes before the entry"),
    ("an edit that does not compile", "BUILD FAILED", "src/code/target.c",
     "  uint64_t tag[2];\n", "  uint64_t tag[2] = this does not compile;\n"),
    ("an edit that hangs", "TIMEOUT", "src/code/target.c",
     "  GRCORE_CodeRange range;\n", "  GRCORE_CodeRange range;\n  for (volatile int spin = 1; spin;) {\n  }\n"),
]


P_HOLDS_MUTATED_CORE = False


def build_and_run(core, jit, mutated_lib, timeout, extra_env=None, only=None):
    """-> (verdict, detail). core and jit are the trees to build. With `only`, just those jit tests."""
    # A mutated tree may hang a test; the tests' watchdog aborts it after this long
    # (a signal, so a catch) well inside the harness's own timeout.
    extra_env = dict({'GRJIT_TEST_WATCHDOG_SECONDS': '45'}, **(extra_env or {}))
    global P_HOLDS_MUTATED_CORE
    if mutated_lib == 'jit' and P_HOLDS_MUTATED_CORE:
        # The prefix still holds the core of the previous mutation: put the
        # unmutated one back, or this defect is judged against another's.
        rc, out = sh('make -j8 install PREFIX=%s' % P, core)
        if rc != 0:
            return 'BUILD FAILED', out[-300:]
        P_HOLDS_MUTATED_CORE = False
    if mutated_lib == 'core':
        P_HOLDS_MUTATED_CORE = True
        rc, out = sh('make -j8 install PREFIX=%s' % P, core)
        if rc != 0:
            return 'BUILD FAILED', out[-300:]
        for t in ([] if only else CORE_TESTS):
            rc, out = sh('make -j8 build/linux/release/apps/%s PREFIX=%s' % (t, P), core)
            if rc != 0:
                return 'BUILD FAILED', out[-300:]
    for t in (only or JIT_TESTS):
        rc, out = sh('make -j8 build/linux/release/apps/%s PREFIX=%s' % (t, P), jit)
        if rc != 0:
            return 'BUILD FAILED', out[-300:]
    # Every test program of both libraries that bears on the defect runs; the
    # first verdict that is not a pass decides.
    runs = ([(core, t) for t in CORE_TESTS] if mutated_lib == 'core' and not only else []) + \
           [(jit, t) for t in (only or JIT_TESTS)]
    for tree, t in runs:
        rc, out = sh('./build/linux/release/apps/%s --gtest_brief=1' % t, tree, timeout, extra_env)
        if rc == 'timeout':
            return 'TIMEOUT', t
        failed = sorted(set(re.findall(r'\[  FAILED  \] (\S+)', out)))
        if isinstance(rc, int) and rc < 0 or (rc != 0 and not failed and rc >= 128):
            return 'CAUGHT', '%s: killed by a signal (crash)' % t
        if rc != 0:
            if not failed:
                # Non-zero with no failing test named: the program did not run
                # as a test run, which is not evidence about the defect.
                return 'BUILD FAILED', '%s exited %s without naming a failing test: %s' % (t, rc, out[-200:])
            return 'CAUGHT', '%s: %s' % (t, ', '.join(f.split('.', 1)[-1][:50] for f in failed[:2]))
    return 'MISSED', ''


def tree_pair(rel_lib=None, rel_file=None, old=None, new=None):
    """Fresh copies of both trees, with the one edit applied to its library."""
    core = os.path.join(T, 'core')
    jit = os.path.join(T, 'jit')
    for d, src in ((core, CORE_PRISTINE), (jit, JIT_PRISTINE)):
        if os.path.exists(d):
            shutil.rmtree(d)
        shutil.copytree(src, d, symlinks=True)
    if old is not None:
        path = os.path.join(core if rel_lib == 'core' else jit, rel_file)
        src = open(path).read()
        for o, n in (zip(old, new) if isinstance(old, list) else [(old, new)]):
            assert src.count(o) == 1, (rel_file, o[:60], src.count(o))
            src = src.replace(o, n)
        open(path, 'w').write(src)
    return core, jit


ok = True
# The control: nothing is edited, and every test must pass.
core, jit = tree_pair()
rc, out = sh('make -j8 install PREFIX=%s' % P, core)
verdict, detail = ('BUILD FAILED', out[-300:]) if rc != 0 else build_and_run(core, jit, 'core', TIMEOUT)
print('control (nothing planted): %s%s' % ('every test passes' if verdict == 'MISSED' else verdict,
      '' if verdict == 'MISSED' else ' ' + detail), flush=True)
if verdict != 'MISSED':
    print('check-planted-calls: the control run failed, so no verdict after it means anything')
    shutil.rmtree(T, ignore_errors=True)
    sys.exit(1)

results = []
if SELF_TEST:
    for name, expected, rel, old, new in SELF:
        core, jit = tree_pair('jit', rel, old, new)
        # The tests' own watchdog (tests/test_helpers.h) would abort the hang the self-test
        # plants and turn it into a catch; it is off here so that a hang is seen as one.
        verdict, detail = build_and_run(core, jit, 'jit', 20, {'GRJIT_TEST_WATCHDOG_SECONDS': '0'})
        good = verdict == expected
        ok = ok and good
        print('self-test: %-34s want %-12s got %-12s %s' % (name, expected, verdict, 'ok' if good else 'WRONG'),
              flush=True)
else:
    for idx, (name, lib, rel, old, new, *rest) in enumerate(M):
        if args and not any(a in name or a == str(idx) for a in args):
            continue
        core, jit = tree_pair(lib, rel, old, new)
        verdict, detail = build_and_run(core, jit, lib, TIMEOUT, only=rest[0] if rest else None)
        results.append((idx, name, verdict, detail))
        print('%2d %-6s %s\n      %s %s' % (idx, lib, name, verdict, detail), flush=True)
    bad = [r for r in results if r[2] != 'CAUGHT']
    print('\n%d mutations, %d not caught' % (len(results), len(bad)))
    ok = not bad

shutil.rmtree(T, ignore_errors=True)
sys.exit(0 if ok else 1)
