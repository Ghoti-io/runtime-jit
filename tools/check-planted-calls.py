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
                              [--target=arm64|win64] [--self-test] [name-substring ...]
  --prefix  a prefix holding runtime-core's dependencies and runtime-jit's
            (default $GHOTI_PREFIX, then the PREFIX of the make that runs it);
            it is COPIED, so nothing in it is touched
  --core    runtime-core's checkout (default ../runtime-core beside this one)
  --target  `arm64`: the mutations of src/arm64/*.c, each built for AArch64 with the cross
            compiler and run under qemu-user (in the container suite/tools/xarch/jit-arm64.sh uses,
            which has them; the prefix is its AArch64 one, with runtime-core already built
            for it, so no core mutation is made). A mutation caught only by the pin test
            (`testPin`) is PIN-ONLY, which is not a catch: the new tests must see it
            themselves. `win64`: the mutations of the Windows paths of src/x86_64/*.c and
            src/code/memory.c, each cross-built with mingw and run under wine (in the container
            suite/tools/xwin/m1-win.sh starts, whose PATH holds the toolchain; the prefix is its
            win64 one, with runtime-core already built for it, so no core mutation is made; a
            test that crashes under wine is a catch, a test that hangs is a TIMEOUT). Default:
            the x86-64 mutations, run natively.

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

TARGET = opts.get('target', 'x86-64')
if TARGET not in ('x86-64', 'arm64', 'win64'):
    sys.exit('check-planted-calls: --target is x86-64, arm64 or win64')
ARM64 = TARGET == 'arm64'
WIN64 = TARGET == 'win64'
prefix = opts.get('prefix') or os.environ.get('GHOTI_PREFIX') or os.environ.get('PREFIX')
if not prefix or not os.path.isdir(prefix):
    sys.exit('check-planted-calls: give --prefix=DIR (or set PREFIX): a prefix that holds '
             'runtime-core and runtime-jit\'s dependencies')
prefix = os.path.abspath(prefix)
CORE_SRC = os.path.abspath(opts.get('core') or os.path.join(JIT_SRC, '..', 'runtime-core'))
if not (ARM64 or WIN64) and not os.path.isdir(os.path.join(CORE_SRC, 'src')):
    sys.exit('check-planted-calls: runtime-core is not at %s; give --core=DIR' % CORE_SRC)
# arm64 code runs under qemu-user, which is slower: a longer limit and the tests' own watchdog scaled
TIMEOUT = int(opts.get('timeout', 900 if ARM64 else 600 if WIN64 else 180))
SELF_TEST = 'self-test' in opts
QEMU_SYSROOT = opts.get('qemu-sysroot', '/usr/aarch64-linux-gnu')
CROSS_MAKE = 'CC=aarch64-linux-gnu-gcc CXX=aarch64-linux-gnu-g++' if ARM64 else ''

T = tempfile.mkdtemp(prefix='planted-calls-')
P = os.path.join(T, 'prefix')


def sh(cmd, cwd, timeout=None, extra_env=None):
    pkg = os.path.join(P, 'share', 'pkgconfig')
    if ARM64:
        # The target's googletest first, so pkg-config does not find the host's.
        pkg += ':/opt/aarch64/lib/pkgconfig'
    e = dict(os.environ, PKG_CONFIG_PATH=pkg)
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
if not (ARM64 or WIN64):
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
     '  uint32_t area = (uint32_t)((shadow + stack_words * 8 + (hidden ? 16 : 0) + 15) / 16 * 16);',
     '  uint32_t area = (uint32_t)(shadow + stack_words * 8 + (hidden ? 16 : 0));',
     ['testNatives']),
    ("native: the caller does not pop the stack arguments", "jit",
     'src/x86_64/emit.c',
     '    grjit_asm_add_rsp(a, area);',
     '    (void)area;',
     ['testNatives']),
    ("native: stack argument k is stored one word too high", "jit",
     'src/x86_64/emit.c',
     '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(shadow + 8 * (i - first_stack)), GRJIT_RAX);',
     '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(shadow + 8 * (i - first_stack) + 8), GRJIT_RAX);',
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
    ('review: status natives: stack argument one word too high', 'jit', 'src/x86_64/emit.c', '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(shadow + 8 * (i - first_stack)), GRJIT_RAX);\n#endif', '    grjit_asm_store64(a, GRJIT_RSP, (int32_t)(shadow + 8 * (i - first_stack) + (status ? 8 : 0)), GRJIT_RAX);\n#endif', ['testNatives']),
    ('review: status natives: stack check omits the area', 'jit', 'src/x86_64/emit.c', '  uint64_t need = (uint64_t)area + d->stack_bytes;', '  uint64_t need = (uint64_t)(status ? 0 : area) + d->stack_bytes;', ['testNatives']),
    ('review: status natives: stack check omits declared use', 'jit', 'src/x86_64/emit.c', '  uint64_t need = (uint64_t)area + d->stack_bytes;', '  uint64_t need = (uint64_t)area + (status ? 0 : d->stack_bytes);', ['testNatives']),
    ('review: call set keeps the result', 'jit', 'src/backend/liveness.c', '                if (result != GRJIT_NO_VREG && result < f->vreg_count &&\n                    index[result] != UINT32_MAX) {\n                  clear_bit(site_set, index[result]);\n                }', '', ['testNatives']),
    ('review: native with REF arguments > 6 : arg 7 taken from the wrong operand (args[i-1])', 'jit', 'src/x86_64/emit.c', '    load_operand(e, GRJIT_RAX, &op->args[i]);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 19', '    load_operand(e, GRJIT_RAX, &op->args[i > 8 ? i - 1 : i]);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 19', ['testNatives']),
    ('review: status natives: area not rounded', 'jit', 'src/x86_64/emit.c', '  e->c.live_cursor += status ? 3 : 2; /* the call, the exit before it, the status exit */', '  e->c.live_cursor += status ? 3 : 2;\n  if (status) { area = (uint32_t)(stack_words * 8); }', ['testNatives']),
    ('review: native call: no walk start stored on a native whose result is unused', 'jit', 'src/x86_64/emit.c', '#else\n  grjit_emit_store_walk_cell(e, ret);\n#endif', '#else\n  if (op->dst != GRJIT_NO_VREG) grjit_emit_store_walk_cell(e, ret);\n#endif', ['testNatives']),
    ('review: native call: walk start not stored for natives with status', 'jit', 'src/x86_64/emit.c', '#else\n  grjit_emit_store_walk_cell(e, ret);\n#endif', '#else\n  if (!status) grjit_emit_store_walk_cell(e, ret);\n#endif', ['testNatives']),
    ('review: native call: walk start not stored for natives of more than 5 arguments', 'jit', 'src/x86_64/emit.c', '#else\n  grjit_emit_store_walk_cell(e, ret);\n#endif', '#else\n  if (n <= 5) grjit_emit_store_walk_cell(e, ret);\n#endif', ['testNatives']),
    ('review: native call: ctx reloaded wrongly (rdi from rbp-8 +0) for natives with >10 args', 'jit', 'src/x86_64/emit.c', '  grjit_asm_load64(a, C_ARG(e, ctx_arg), GRJIT_RBP, GRJIT_SLOT_CTX);\n  for (size_t i = 0; i < n && i + 1', '  grjit_asm_load64(a, C_ARG(e, ctx_arg), GRJIT_RBP, n > 10 ? GRJIT_SLOT_OUT : GRJIT_SLOT_CTX);\n  for (size_t i = 0; i < n && i + 1', ['testNatives']),
    ('review: verifier: after-state names a vreg checked only for existence (dst exemption widened to any vreg)', 'jit', 'src/ir/function.c', '              s->slots[i].vreg == op->dst) {', '              true) {', ['testNative_ir']),
    ('review: a refused rebuild after a native exit is ignored (shared hook-call code)', 'jit', 'src/x86_64/exit.c', '  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);\n}', '}', ['testNatives']),
    ('review: the status is tested in all 64 bits of rdx, so garbage above a zero status leaves', 'jit', 'src/x86_64/emit.c', '    if (!hidden) {\n      grjit_asm_mov32_rr(a, second, second);\n    }\n    grjit_asm_test_rr(a, second, second);', '    grjit_asm_test_rr(a, second, second);', ['testNatives']),
    ('review: only the low 16 bits of the status are tested', 'jit', 'src/x86_64/emit.c', '    if (!hidden) {\n      grjit_asm_mov32_rr(a, second, second);\n    }\n    grjit_asm_test_rr(a, second, second);', '    grjit_asm_mov_ri(a, GRJIT_RCX, 0xFFFF);\n    grjit_asm_alu_rr(a, GRJIT_ALU_AND, second, GRJIT_RCX);\n    grjit_asm_test_rr(a, second, second);', ['testNatives']),
    ('review: the status exit site names the identity of the state before the call', 'jit', 'src/x86_64/exit.c', '  const GRJIT_FrameState * state = &e->c.f->states[p->op->exit_state];\n  grjit_asm_bind(a, p->entry);\n  GRJIT_Label ret = grjit_asm_label(a);\n  grjit_asm_mov32_rr', '  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];\n  grjit_asm_bind(a, p->entry);\n  GRJIT_Label ret = grjit_asm_label(a);\n  grjit_asm_mov32_rr', ['testNatives']),
    # The call target's alignment (story 7).
    ("a call target that is not on a sixteen-byte boundary is accepted", "jit",
     "src/code/target.c",
     "  if ((target & (GRJIT_ENTRY_TAG_BYTES - 1u)) != 0) {\n    return 0;\n  }\n",
     "",
     ['testCalls']),
    ("a callable poll does not store the walk start before its helper", "jit",
     "src/x86_64/exit.c",
     "    grjit_asm_bind(a, p->entry);\n    grjit_emit_store_walk_cell(e, ret);\n    grjit_asm_mov_rr(a, C_ARG(e, 0), GRJIT_RCX);",
     "    grjit_asm_bind(a, p->entry);\n    grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, GRJIT_SLOT_CTX);\n    grjit_asm_mov_rr(a, C_ARG(e, 0), GRJIT_RCX);",
     ['testCalls']),
    ("the adapter does not clear the walk-start cell on the way out", "jit",
     "src/x86_64/emit.c",
     "  grjit_asm_load64(a, GRJIT_R8, GRJIT_RSP, 16);\n  grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_R9, GRJIT_R9);\n  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset, GRJIT_R9);\n  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset + 8, GRJIT_R9);\n",
     "",
     ['testCalls']),
    ("the prologue's native-stack check refuses a frame that ends exactly at the limit", "jit",
     "src/x86_64/emit.c",
     "  grjit_asm_cmp_rm(a, GRJIT_RAX, ICTX(e), (int32_t)e->c.native_limit_offset);\n  grjit_asm_jcc(a, GRJIT_COND_B, e->overflow);",
     "  grjit_asm_cmp_rm(a, GRJIT_RAX, ICTX(e), (int32_t)e->c.native_limit_offset);\n  grjit_asm_jcc(a, GRJIT_COND_BE, e->overflow);",
     ['testCalls']),
]

# The mutations of the arm64 emitter (--target=arm64): the same entries, in src/arm64.
M_ARM64 = [
    ('a call does not test the status for DEOPTED', 'jit', 'src/arm64/emit.c',
     "  grjit_a64_cbnz(a, GRJIT_A64_X1, e->ret_propagate);\n  if (op->dst != GRJIT_NO_VREG) {\n    store_result(e, op->dst, GRJIT_A64_X0);\n  }\n  /* The call is complete: pop the callee's guest frame. */",
     "  if (op->dst != GRJIT_NO_VREG) {\n    store_result(e, op->dst, GRJIT_A64_X0);\n  }\n  /* The call is complete: pop the callee's guest frame. */",
     ['testCalls']),
    ('a call through a pointer does not test the status for DEOPTED', 'jit', 'src/arm64/emit.c',
     "  grjit_a64_cbnz(a, GRJIT_A64_X1, e->ret_propagate);\n  if (op->dst != GRJIT_NO_VREG) {\n    store_result(e, op->dst, GRJIT_A64_X0);\n  }\n  /* The call is complete: pop the callee's guest frame. */",
     "  if (op->kind == GRJIT_OP_CALL_SLOT) {\n    grjit_a64_cbnz(a, GRJIT_A64_X1, e->ret_propagate);\n  }\n  if (op->dst != GRJIT_NO_VREG) {\n    store_result(e, op->dst, GRJIT_A64_X0);\n  }\n  /* The call is complete: pop the callee's guest frame. */",
     ['testCalls']),
    ("the callee's guest frame is never popped", 'jit', 'src/arm64/emit.c',
     '  grjit_a64_mov_ri(a, GRJIT_A64_X16, (uint64_t)(uintptr_t)f->hooks.pop);\n  grjit_a64_blr(a, GRJIT_A64_X16);',
     '  (void)f->hooks.pop;',
     ['testCalls']),
    ('a call through a slot does not store the walk start before its push', 'jit', 'src/arm64/emit.c',
     '  A * a = &e->as;\n  const size_t regs = GRJIT_SHAPE_REGS(e->c.f, e->c.shape);\n  grjit_a64_emit_store_walk_cell(e, ret);\n',
     '  A * a = &e->as;\n  const size_t regs = GRJIT_SHAPE_REGS(e->c.f, e->c.shape);\n  if (op->kind != GRJIT_OP_CALL_SLOT) {\n    grjit_a64_emit_store_walk_cell(e, ret);\n  } else {\n    load_slot(e, GRJIT_A64_X15, GRJIT_SLOT_CTX);\n  }\n',
     ['testCalls']),
    ('a tail call does not store the walk start before its hook', 'jit', 'src/arm64/emit.c',
     '  A * a = &e->as;\n  const size_t regs = GRJIT_SHAPE_REGS(e->c.f, e->c.shape);\n  grjit_a64_emit_store_walk_cell(e, ret);\n',
     '  A * a = &e->as;\n  const size_t regs = GRJIT_SHAPE_REGS(e->c.f, e->c.shape);\n  if (op->kind != GRJIT_OP_TAIL_CALL_SLOT) {\n    grjit_a64_emit_store_walk_cell(e, ret);\n  } else {\n    load_slot(e, GRJIT_A64_X15, GRJIT_SLOT_CTX);\n  }\n',
     ['testTail']),
    ('a refused entry slot (one) is entered', 'jit', 'src/arm64/emit.c',
     '    grjit_a64_cmp_imm(a, GRJIT_A64_X16, GRCORE_ENTRY_REFUSED);\n    grjit_a64_bcond(a, GRJIT_A64_LS, slow);',
     '    grjit_a64_cmp_imm(a, GRJIT_A64_X16, GRCORE_ENTRY_REFUSED);\n    grjit_a64_bcond(a, GRJIT_A64_LO, slow);',
     ['testCalls']),
    ('a call through a pointer ignores the answer of the target check', 'jit', 'src/arm64/emit.c',
     '    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n    grjit_a64_cbz(a, GRJIT_A64_X0, exit);\n  }\n  memset(&p, 0, sizeof p);',
     '    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n  }\n  memset(&p, 0, sizeof p);',
     ['testCalls']),
    ('a call through a slot does not keep the entry it found', 'jit', 'src/arm64/emit.c',
     '  if (slot) {\n    store_slot(e, GRJIT_A64_X16, entry_slot);\n  }\n  return exit;',
     '  return exit;',
     ['testCalls']),
    ('argument i is staged in slot i + 1 of the arguments area', 'jit', 'src/arm64/emit.c',
     '    load_operand(e, GRJIT_A64_X0, &op->args[i]);\n    store_slot(e, GRJIT_A64_X0, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));',
     '    load_operand(e, GRJIT_A64_X0, &op->args[i]);\n    store_slot(e, GRJIT_A64_X0, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i + 1));',
     ['testCalls']),
    ("a call's stack arguments are stored one word too high", 'jit', 'src/arm64/emit.c',
     '      grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg)));\n    }\n  }\n  for (size_t i = 0; i < n && i < ireg; i++) {\n    load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));\n  }\n  load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);\n  load_slot(e, GRJIT_A64_X16, entry_slot);\n  grjit_a64_blr(a, GRJIT_A64_X16);\n  /* The return address is the site: this frame, with the callee running. */',
     '      grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP, (int32_t)(8 * (i - ireg) + 8));\n    }\n  }\n  for (size_t i = 0; i < n && i < ireg; i++) {\n    load_slot(e, arg_regs_internal[i], GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, i));\n  }\n  load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);\n  load_slot(e, GRJIT_A64_X16, entry_slot);\n  grjit_a64_blr(a, GRJIT_A64_X16);\n  /* The return address is the site: this frame, with the callee running. */',
     ['testCalls']),
    ('a call does not load the context into x9 for the callee', 'jit', 'src/arm64/emit.c',
     '  load_slot(e, GRJIT_A64_X9, GRJIT_SLOT_CTX);\n  load_slot(e, GRJIT_A64_X16, entry_slot);\n  grjit_a64_blr(a, GRJIT_A64_X16);\n  /* The return address is the site: this frame, with the callee running. */',
     '  load_slot(e, GRJIT_A64_X16, entry_slot);\n  grjit_a64_blr(a, GRJIT_A64_X16);\n  /* The return address is the site: this frame, with the callee running. */',
     ['testCalls']),
    ('a tail call does not reload its return address into x30', 'jit', 'src/arm64/emit.c',
     '  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X30, GRJIT_A64_FP, 8);\n  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, 0);',
     '  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, 0);',
     ['testTail']),
    ("a tail call takes the caller's base from x29, not from its saved word (the callee's chain loops)", 'jit', 'src/arm64/emit.c',
     '  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, 0);\n  size_t copy_to = n;',
     '  grjit_a64_mov_rr(a, GRJIT_A64_X15, GRJIT_A64_FP);\n  size_t copy_to = n;',
     ['testTail']),
    ('a tail call sets sp one frame record too high', 'jit', 'src/arm64/emit.c',
     '  lea(e, GRJIT_A64_SP, GRJIT_A64_FP, sp_new);',
     '  lea(e, GRJIT_A64_SP, GRJIT_A64_FP, sp_new + 16);',
     ['testTail']),
    ('a tail call stores its stack arguments one word too low', 'jit', 'src/arm64/emit.c',
     '    store_slot(e, GRJIT_A64_X10, (int32_t)(sp_new + 8 * (int64_t)(i - ireg)));',
     '    store_slot(e, GRJIT_A64_X10, (int32_t)(sp_new - 8 + 8 * (int64_t)(i - ireg)));',
     ['testTail']),
    ('the hooks are handed one argument, whatever the count', 'jit', 'src/arm64/emit.c',
     '  grjit_a64_mov_ri(a, GRJIT_A64_X3, op->arg_count);\n  grjit_a64_mov_ri(a, GRJIT_A64_X16, hook);',
     '  grjit_a64_mov_ri(a, GRJIT_A64_X3, op->arg_count > 1 ? 1 : op->arg_count);\n  grjit_a64_mov_ri(a, GRJIT_A64_X16, hook);',
     ['testTail', 'testCalls']),
    ('the hooks are handed their arguments from the second slot', 'jit', 'src/arm64/emit.c',
     '  lea(e, GRJIT_A64_X2, GRJIT_A64_FP, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, 0));',
     '  lea(e, GRJIT_A64_X2, GRJIT_A64_FP, GRJIT_ARGS_SLOT(regs, e->c.shape.args_area, op->arg_count > 1 ? 1 : 0));',
     ['testTail', 'testCalls']),
    ('a refused rebuild after the deopt hook is ignored', 'jit', 'src/arm64/exit.c',
     '  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n  grjit_a64_cbnz(a, GRJIT_A64_X0, e->ret_failed);\n}\n\nvoid grjit_a64_emit_call_slow_stub',
     '}\n\nvoid grjit_a64_emit_call_slow_stub',
     ['testCalls']),
    ("a poll verdict's cause is lost on its way out", 'jit', 'src/arm64/exit.c',
     '    grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_OUT);\n    grjit_a64_b(a, e->ret_deopted);\n    return;\n  }\n  grjit_a64_bind(a, p->entry);\n  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_CTX);',
     '    grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);\n    grjit_a64_b(a, e->ret_deopted);\n    return;\n  }\n  grjit_a64_bind(a, p->entry);\n  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X0, GRJIT_A64_FP, GRJIT_SLOT_CTX);',
     ['testCalls']),
    ('a callable poll does not store the walk start before the helper', 'jit', 'src/arm64/exit.c',
     '    grjit_a64_bind(a, p->entry);\n    grjit_a64_emit_store_walk_cell(e, ret);\n    grjit_a64_mov_rr(a, GRJIT_A64_X0, GRJIT_A64_X15);\n    grjit_a64_mov_ri(a, GRJIT_A64_X1, state->identity.function);',
     '    grjit_a64_bind(a, p->entry);\n    grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_FP, GRJIT_SLOT_CTX);\n    grjit_a64_mov_rr(a, GRJIT_A64_X0, GRJIT_A64_X15);\n    grjit_a64_mov_ri(a, GRJIT_A64_X1, state->identity.function);',
     ['testCalls']),
    ('the slow path does not look again at a slot the compile hook claims to have filled', 'jit', 'src/arm64/exit.c',
     '  grjit_a64_cmp_imm(a, GRJIT_A64_X16, GRCORE_ENTRY_REFUSED);\n  grjit_a64_bcond(a, GRJIT_A64_LS, p->exit);\n  grjit_a64_b(a, p->back);',
     '  grjit_a64_b(a, p->back);',
     ['testCalls']),
    ('the overflow exit does not recognise the chain-end marker', 'jit', 'src/arm64/exit.c',
     '  grjit_a64_cmp(a, GRJIT_A64_X15, GRJIT_A64_X16);\n  grjit_a64_bcond(a, GRJIT_A64_EQ, none);',
     '  grjit_a64_cmp(a, GRJIT_A64_X15, GRJIT_A64_X16);',
     ['testCalls']),
    ('the adapter does not clear the walk-start cell on the way out', 'jit', 'src/arm64/emit.c',
     '  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset);\n  grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_XZR, GRJIT_A64_X15, (int32_t)e->c.walk_cell_offset + 8);\n  grjit_a64_cbnz(a, GRJIT_A64_X1, deopted);',
     '  grjit_a64_cbnz(a, GRJIT_A64_X1, deopted);',
     ['testNatives', 'testCalls']),
    ('the adapter reads a refused rebuild as a deoptimization', 'jit', 'src/arm64/emit.c',
     '  grjit_a64_cmp_imm(a, GRJIT_A64_X1, GRJIT_STATUS_FAILED);\n  grjit_a64_bcond(a, GRJIT_A64_NE, not_failed);',
     '  grjit_a64_cmp_imm(a, GRJIT_A64_X1, GRJIT_STATUS_FAILED);\n  grjit_a64_b(a, not_failed);',
     ['testCalls']),
    ('the adapter copies the stack parameters from one word too high', 'jit', 'src/arm64/emit.c',
     '      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_X15, (int32_t)(8 * i));',
     '      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_X15, (int32_t)(8 * i + 8));',
     ['testCalls']),
    ('the prologue reads the stack parameters one word low', 'jit', 'src/arm64/emit.c',
     '      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_FP, (int32_t)(16 + 8 * (i - ireg)));',
     '      grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_FP, (int32_t)(8 + 8 * (i - ireg)));',
     ['testCalls']),
    ('the native-stack check admits a frame that ends exactly at the limit', 'jit', 'src/arm64/emit.c',
     '  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);\n  grjit_a64_bcond(a, GRJIT_A64_LO, e->overflow);',
     '  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);\n  grjit_a64_bcond(a, GRJIT_A64_LS, e->overflow);',
     ['testCalls']),
    ('a helper call in a callable function does not store the walk start', 'jit', 'src/arm64/emit.c',
     '  const bool record = e->c.callable && op->attr == GRJIT_CALL_GC_POINT;',
     '  const bool record = false;',
     ['testCalls']),
    ('native: the status exit is built from the state before the call, so the native runs twice', 'jit', 'src/arm64/exit.c',
     '  const GRJIT_FrameState * state = &e->c.f->states[p->op->exit_state];\n  grjit_a64_bind(a, p->entry);\n  GRJIT_Label ret = grjit_a64_label(a);\n  grjit_a64_mov_ri(a, GRJIT_A64_X16, GRJIT_CAUSE_NATIVE);',
     '  const GRJIT_FrameState * state = &e->c.f->states[p->op->state];\n  grjit_a64_bind(a, p->entry);\n  GRJIT_Label ret = grjit_a64_label(a);\n  grjit_a64_mov_ri(a, GRJIT_A64_X16, GRJIT_CAUSE_NATIVE);',
     ['testNatives']),
    ('native: the cause of a status exit lacks the native bit', 'jit', 'src/arm64/exit.c',
     '  grjit_a64_alu(a, GRJIT_A64_ORR, GRJIT_A64_X1, GRJIT_A64_X1, GRJIT_A64_X16);\n',
     '',
     ['testNatives']),
    ('native: the result is stored after the status test, so a status exit finds the old value', 'jit', 'src/arm64/emit.c',
     '  if (op->dst != GRJIT_NO_VREG) {\n    store_result(e, op->dst, GRJIT_A64_X0);\n  }\n  if (status) {\n    GRJIT_Label leave = grjit_a64_label(a);',
     '  if (op->dst != GRJIT_NO_VREG && !status) {\n    store_result(e, op->dst, GRJIT_A64_X0);\n  }\n  if (status) {\n    GRJIT_Label leave = grjit_a64_label(a);',
     ['testNatives']),
    ("native: the stack check leaves out the native's declared use", 'jit', 'src/arm64/emit.c',
     '  uint64_t need = (uint64_t)area + d->stack_bytes;',
     '  uint64_t need = area;',
     ['testNatives']),
    ('native: the stack check rejects a call that ends exactly at the limit', 'jit', 'src/arm64/emit.c',
     '  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);\n  grjit_a64_bcond(a, GRJIT_A64_LO, exit);',
     '  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);\n  grjit_a64_bcond(a, GRJIT_A64_LS, exit);',
     ['testNatives']),
    ("native: argument i is passed in the register of argument i - 1 (the context's)", 'jit', 'src/arm64/emit.c',
     '    load_operand(e, arg_regs_internal[i + 1], &op->args[i]);',
     '    load_operand(e, arg_regs_internal[i], &op->args[i]);',
     ['testNatives']),
    ('native: the context is not passed', 'jit', 'src/arm64/emit.c',
     '  load_slot(e, GRJIT_A64_X0, GRJIT_SLOT_CTX);\n  for (size_t i = 0; i < n && i + 1 < creg; i++) {',
     '  for (size_t i = 0; i < n && i + 1 < creg; i++) {',
     ['testNatives']),
    ('native: only the low 16 bits of the status are tested', 'jit', 'src/arm64/emit.c',
     '    grjit_a64_mov32_rr(a, GRJIT_A64_X1, GRJIT_A64_X1);\n#endif',
     '    grjit_a64_mov_ri(a, GRJIT_A64_X16, 0xFFFF);\n    grjit_a64_alu(a, GRJIT_A64_AND, GRJIT_A64_X1, GRJIT_A64_X1, GRJIT_A64_X16);\n#endif',
     ['testNatives']),
    ('native: stack argument k is stored one word too low', 'jit', 'src/arm64/emit.c',
     '    grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP,\n        (int32_t)(8 * (i - (creg - 1))));',
     '    grjit_a64_store(a, GRJIT_A64_MEM_X64, GRJIT_A64_X10, GRJIT_A64_SP,\n        (int32_t)(8 * (i - (creg - 1)) + (i > creg ? 8 : 0)));',
     ['testNatives']),
    ("native: the references are left out of a native call site's stack map", 'jit', 'src/backend/metadata.c',
     '        GRCORE_CodeLocation l = slot_location(v, info->type);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 2',
     '        if (r->op != NULL && r->op->kind == GRJIT_OP_CALL_NATIVE && r->kind == GRCORE_SITE_GC_POINT_CALL) {\n          continue;\n        }\n        GRCORE_CodeLocation l = slot_location(v, info->type);\n#if defined(GRJIT_TEST_PLANT_BUG) && GRJIT_TEST_PLANT_BUG == 2',
     ['testNatives']),
    ("the function's frame is eight bytes more than a multiple of sixteen, so sp is misaligned at every call", "jit",
     "src/arm64/emit.c",
     "  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);\n  grjit_a64_bcond(a, GRJIT_A64_LO, e->overflow);\n  grjit_a64_sub_sp(a, alloc);",
     "  grjit_a64_cmp(a, GRJIT_A64_X16, GRJIT_A64_X15);\n  grjit_a64_bcond(a, GRJIT_A64_LO, e->overflow);\n  grjit_a64_sub_sp(a, alloc + 8);",
     ['testCalls']),
]


# Every place the code tests a hook's answer (a uint32_t: the register above its 32 bits is the callee's to leave as
# it likes) narrows it first. Each edit below takes the narrowing out of one place; the tests that answer through
# the garbage stubs of tests/calls_asm.h (a zero with garbage above, a refusal with garbage above) must fail on it
# by an assertion, the pin and nothing else not being enough.
def _nomask(old, mask):
    assert old.count(mask) == 1, old
    return old.replace(mask, '')

_X86_MASK = '  grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);\n'
_X86_MASK4 = '    grjit_asm_mov32_rr(a, GRJIT_RAX, GRJIT_RAX);\n'
_A64_MASK = '  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n'
_A64_MASK4 = '    grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n'
_A64_MASK6 = '      grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n'

_HOOKS_X86 = [
    ("the deopt hook's answer is tested in 64 bits (a guard or call exit)", 'src/x86_64/exit.c', _X86_MASK,
     "returns FAILED with the answer, and the engine hears of it. */\n" + _X86_MASK + "  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);\n}", ['testCalls']),
    ("the compile hook's answer is tested in 64 bits", 'src/x86_64/exit.c', _X86_MASK,
     "  grjit_asm_call_r(a, GRJIT_RAX);\n" + _X86_MASK + "  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, p->exit);\n  /* The hook says it installed", ['testCalls']),
    ("the deopt hook's answer is tested in 64 bits (the overflow stub)", 'src/x86_64/exit.c', _X86_MASK,
     _X86_MASK + "  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, e->ret_failed);\n  grjit_asm_mov_ri(a, GRJIT_RAX, 0);\n  grjit_asm_mov_ri(a, ISTATUS(e), GRJIT_STATUS_DEOPTED);", ['testCalls']),
    ("a callable poll helper's answer is tested in 64 bits", 'src/x86_64/exit.c', _X86_MASK4,
     _X86_MASK4 + "    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n    grjit_asm_jcc(a, GRJIT_COND_NE, deopt);", ['testCalls']),
    ("a plain function's poll helper's answer is tested in 64 bits", 'src/x86_64/exit.c', _X86_MASK,
     _X86_MASK + "  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);\n  grjit_asm_jmp(a, p->back);", ['testPoll']),
    ("the push hook's answer is tested in 64 bits", 'src/x86_64/emit.c', _X86_MASK,
     _X86_MASK + "  grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n  grjit_asm_jcc(a, GRJIT_COND_NE, exit);\n\n  /* The call. The stack arguments are pushed", ['testCalls']),
    ("a callable function's entry hook's answer is tested in 64 bits", 'src/x86_64/emit.c', _X86_MASK4,
     _X86_MASK4 + "    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n    grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);\n  }\n  grjit_asm_load64(a, scratch, GRJIT_RSP, 8);", ['testCalls']),
    ("a plain function's entry hook's answer is tested in 64 bits", 'src/x86_64/emit.c', _X86_MASK4,
     "    grjit_asm_call_r(a, GRJIT_RAX);\n" + _X86_MASK4 + "    grjit_asm_test_rr(a, GRJIT_RAX, GRJIT_RAX);\n    grjit_asm_jcc(a, GRJIT_COND_NE, e->refuse);\n  }\n  if (f->param_count != 0) {", ['testPoll']),
]
_HOOKS_A64 = [
    ("the deopt hook's answer is tested in 64 bits (a guard or call exit)", 'src/arm64/exit.c', _A64_MASK,
     "  grjit_a64_blr(a, GRJIT_A64_X16);\n" + _A64_MASK + "  grjit_a64_cbnz(a, GRJIT_A64_X0, e->ret_failed);\n}\n\nvoid grjit_a64_emit_call_slow_stub", ['testCalls']),
    ("the compile hook's answer is tested in 64 bits", 'src/arm64/exit.c', _A64_MASK,
     _A64_MASK + "  grjit_a64_cbnz(a, GRJIT_A64_X0, p->exit);\n  /* The hook says it installed", ['testCalls']),
    ("the deopt hook's answer is tested in 64 bits (the overflow stub)", 'src/arm64/exit.c', _A64_MASK,
     _A64_MASK + "  grjit_a64_cbnz(a, GRJIT_A64_X0, e->ret_failed);\n  grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);", ['testCalls']),
    ("a callable poll helper's answer is tested in 64 bits", 'src/arm64/exit.c', _A64_MASK4,
     _A64_MASK4 + "    grjit_a64_cbnz(a, GRJIT_A64_X0, deopt);", ['testCalls']),
    ("a plain function's poll helper's answer is tested in 64 bits", 'src/arm64/exit.c', _A64_MASK,
     _A64_MASK + "  grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);\n  grjit_a64_b(a, p->back);\n}", ['testPoll']),
    ("the push hook's answer is tested in 64 bits", 'src/arm64/emit.c', _A64_MASK,
     _A64_MASK + "  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);\n\n  /* The call. The stack arguments are made", ['testCalls']),
    ("a callable function's entry hook's answer is tested in 64 bits", 'src/arm64/emit.c', _A64_MASK4,
     _A64_MASK4 + "    grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);\n  }\n  grjit_a64_load(a, GRJIT_A64_MEM_X64, GRJIT_A64_X15, GRJIT_A64_SP, 16);", ['testCalls']),
    ("a plain function's entry hook's answer is tested in 64 bits", 'src/arm64/emit.c', _A64_MASK6,
     _A64_MASK6 + "      grjit_a64_cbnz(a, GRJIT_A64_X0, e->refuse);\n    }\n    if (f->param_count != 0) {", ['testPoll']),
]
for _name, _file, _mask, _old, _tests in _HOOKS_X86:
    M.append((_name, 'jit', _file, _old, _nomask(_old, _mask), _tests))
for _name, _file, _mask, _old, _tests in _HOOKS_A64:
    M_ARM64.append((_name, 'jit', _file, _old, _nomask(_old, _mask), _tests))


# The arm64 emitter's sites, indices and offsets, each shown on the host (the emitter's code is in the library on every
# host, and the tests read what it emits for arm64 through `grjit_emit_for`) and under qemu: a defect in the code only
# arm64 runs is not left to the one environment that runs it.
_ARM64_SHARED = [
    ("the push site and the call site swap their site sets", 'src/arm64/emit.c',
     ["  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),\n      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);\n  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);\n\n  /* The call. The stack",
      "      GRCORE_SITE_GC_POINT_CALL, st->identity, live + 1, op->state, op);\n  grjit_a64_cbnz(a, GRJIT_A64_X1, e->ret_propagate);"],
     ["  grjit_emit_add_site_for(&e->c, (uint32_t)grjit_a64_size(a),\n      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live + 1, op->state, op);\n  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);\n\n  /* The call. The stack",
      "      GRCORE_SITE_GC_POINT_CALL, st->identity, live, op->state, op);\n  grjit_a64_cbnz(a, GRJIT_A64_X1, e->ret_propagate);"]),
    ("a call's exit stub is made from the call's own set, not the exit's", 'src/arm64/emit.c',
     "emit_dispatch(e, op, live, live + 2);", "emit_dispatch(e, op, live, live + 1);"),
    ("a tail call's exit stub is made from the hook's set, not the exit's", 'src/arm64/emit.c',
     "  GRJIT_Label exit = emit_dispatch(e, op, live, live + 1);\n  emit_stage_arguments(e, op);\n\n  GRJIT_Label hook_ret",
     "  GRJIT_Label exit = emit_dispatch(e, op, live, live);\n  emit_stage_arguments(e, op);\n\n  GRJIT_Label hook_ret"),
    ("a tail call's hook is a call site, not a frame-push one", 'src/arm64/emit.c',
     "      GRCORE_SITE_GC_POINT_FRAME_PUSH, st->identity, live, op->state, op);\n  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);\n\n  const unsigned ireg",
     "      GRCORE_SITE_GC_POINT_CALL, st->identity, live, op->state, op);\n  grjit_a64_mov32_rr(a, GRJIT_A64_X0, GRJIT_A64_X0);\n  grjit_a64_cbnz(a, GRJIT_A64_X0, exit);\n\n  const unsigned ireg"),
    ("a native's exit before the call takes the call's site set", 'src/arm64/emit.c',
     "    p.kind = GRJIT_PENDING_NATIVE_EXIT;\n    p.entry = exit;\n    p.op = op;\n    p.live_index = live + 1;",
     "    p.kind = GRJIT_PENDING_NATIVE_EXIT;\n    p.entry = exit;\n    p.op = op;\n    p.live_index = live;"),
    ("a native's status exit takes the exit before the call's site set", 'src/arm64/emit.c',
     "    p.kind = GRJIT_PENDING_NATIVE_STATUS;\n    p.entry = leave;\n    p.op = op;\n    p.live_index = live + 2;",
     "    p.kind = GRJIT_PENDING_NATIVE_STATUS;\n    p.entry = leave;\n    p.op = op;\n    p.live_index = live + 1;"),
    ("a call's or tail call's exit is a call site, not a guard site", 'src/arm64/exit.c',
     "  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GUARD,\n      state->identity, p->live_index, exit_state);",
     "  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GC_POINT_CALL,\n      state->identity, p->live_index, exit_state);"),
    ("a native's exit before the call is a call site, not a guard site", 'src/arm64/exit.c',
     "  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GUARD,\n      state->identity, p->live_index, p->op->state);\n  grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);",
     "  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GC_POINT_CALL,\n      state->identity, p->live_index, p->op->state);\n  grjit_a64_mov_ri(a, GRJIT_A64_X0, 0);"),
    ("a native's status exit is a call site, not a guard site", 'src/arm64/exit.c',
     "  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GUARD,\n      state->identity, p->live_index, p->op->exit_state);",
     "  grjit_emit_add_site(&e->c, (uint32_t)grjit_a64_size(a), GRCORE_SITE_GC_POINT_CALL,\n      state->identity, p->live_index, p->op->exit_state);"),
    ("lea: the shifted high part loses its shift", 'src/arm64/emit.c',
     "(neg ? grjit_a64_sub_imm : grjit_a64_add_imm)(a, rd, from, (uint32_t)(mag >> 12), true);",
     "(neg ? grjit_a64_sub_imm : grjit_a64_add_imm)(a, rd, from, (uint32_t)(mag >> 12), false);"),
    ("lea: the low part is dropped when the high part is present and the offset is not a multiple of 4096", 'src/arm64/emit.c',
     "  if ((mag & 0xFFFu) != 0 || from == base) {",
     "  if ((mag & 0xFFFu) != 0 && from == base) {"),
    ("lea: an offset of nothing emits nothing", 'src/arm64/emit.c',
     "  if ((mag & 0xFFFu) != 0 || from == base) {",
     "  if ((mag & 0xFFFu) != 0) {"),
    ("lea: an offset of 2^24 or more is built with the low 16 bits of its magnitude", 'src/arm64/emit.c',
     "    grjit_a64_mov_ri(a, GRJIT_A64_X17, mag);\n    (neg ?",
     "    grjit_a64_mov_ri(a, GRJIT_A64_X17, mag & 0xFFFFu);\n    (neg ?"),
]
for _name, _file, _old, _new in _ARM64_SHARED:
    # `lea` is the stack check's own arithmetic: the tests that read it (testNatives) must see it, and not a crash in a
    # tail call (which uses it for its own sp) that comes first.
    _t = ['testNatives'] if _name.startswith('lea:') else ['testCalls', 'testTail', 'testNatives']
    M.append((_name, 'jit', _file, _old, _new, _t))
    M_ARM64.append((_name, 'jit', _file, _old, _new, _t))

# The mutations of the arm64 emitter that a run finds only as a crash (an address entered that is no entry, a frame
# record left wrong: `judge` accepts a signal as a catch) are shown too to be seen by a test that reads the words of
# the emitted code (testArm64_calls), on the host and under qemu, by an assertion that names what is wrong. Two have no
# such assertion that is cheap, and stay caught by their crash (named in the story's review triage): the entry
# a slot call keeps for its callee's return (a store to a slot the callee never reads back by name) and the order in
# which the arguments are staged in the area (an offset a run reads as the wrong argument, by a fault in the hook).
_BY_WORDS = [
    'a refused entry slot (one) is entered',
    'a call through a pointer ignores the answer of the target check',
    'a call does not load the context into x9 for the callee',
    'a tail call does not reload its return address into x30',
    "a tail call takes the caller's base from x29, not from its saved word (the callee's chain loops)",
    'a tail call sets sp one frame record too high',
    'the slow path does not look again at a slot the compile hook claims to have filled',
]
for _e in list(M_ARM64):
    if _e[0] in _BY_WORDS:
        _w = (_e[0] + ' (seen by the words of the code alone)',) + tuple(_e[1:5]) + (['testArm64_calls'],)
        M.append(_w)
        M_ARM64.append(_w)

# The mutations of the Windows paths of the x86-64 emitter and of the unwind table (--target=win64):
# each built with mingw and run under wine, in the container of suite/tools/xwin. The first three are
# the carried edit of story 7's review (the Win64 register-argument count, which nothing read until
# story 7b made Win64 emit callable functions): 6 is what a copy from SysV gives, 3 and 8 the others
# a slip could give; the tests that read the convention out of the code must fail on each.
_REGS = '#define GRJIT_WIN64_INTERNAL_REG_ARGS 4u'
M_WIN64 = [
    ('the Win64 register-argument count is 6 (the SysV one)', 'jit', 'src/backend/backend_internal.h',
     _REGS, '#define GRJIT_WIN64_INTERNAL_REG_ARGS 6u', ['testWin64_calls']),
    ('the Win64 register-argument count is 3', 'jit', 'src/backend/backend_internal.h',
     _REGS, '#define GRJIT_WIN64_INTERNAL_REG_ARGS 3u', ['testWin64_calls']),
    ('the Win64 register-argument count is 8', 'jit', 'src/backend/backend_internal.h',
     _REGS, '#define GRJIT_WIN64_INTERNAL_REG_ARGS 8u', ['testWin64_calls']),
    ('the Win64 adapter reads its saved out pointer where it was before the callee popped the stack arguments', 'jit',
     'src/x86_64/emit.c',
     '  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, s_out - (int32_t)in_bytes);',
     '  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RSP, s_out);', ['testCalls']),
    ('the Win64 adapter does not clear the walk-start cell on the way out', 'jit', 'src/x86_64/emit.c',
     '  grjit_asm_load64(a, GRJIT_R8, GRJIT_RSP, s_ctx - (int32_t)in_bytes);\n  grjit_asm_alu_rr(a, GRJIT_ALU_XOR, GRJIT_R9, GRJIT_R9);\n  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset, GRJIT_R9);\n  grjit_asm_store64(a, GRJIT_R8, (int32_t)e->c.walk_cell_offset + 8, GRJIT_R9);\n',
     '  grjit_asm_load64(a, GRJIT_R8, GRJIT_RSP, s_ctx - (int32_t)in_bytes);\n', ['testCalls']),
    ('the Win64 adapter gives back the marker in rbp, not the caller\'s', 'jit', 'src/x86_64/emit.c',
     '  grjit_asm_add_rsp(a, frame - in_bytes);\n  grjit_asm_pop(a, GRJIT_RBP);',
     '  grjit_asm_add_rsp(a, frame - in_bytes);\n  grjit_asm_pop(a, GRJIT_RCX);', ['testCalls']),
    ('a Win64 callee returns with a plain ret even when it has stack arguments', 'jit', 'src/x86_64/exit.c',
     '    if (e->c.shape.incoming_bytes == 0) {\n      /* Plain `ret` is the form every unwinder knows as an epilogue\'s end. */',
     '    if (true) {\n      /* Plain `ret` is the form every unwinder knows as an epilogue\'s end. */', ['testCalls']),
    ('a native\'s area has no shadow space', 'jit', 'src/x86_64/emit.c',
     '  const uint32_t shadow = e->abi->shadow_bytes;\n#endif\n  /* The IR argument that is the first C stack word',
     '  const uint32_t shadow = 0;\n#endif\n  /* The IR argument that is the first C stack word', ['testNatives']),
    ('a native\'s status is read from the reserved half of the buffer', 'jit', 'src/x86_64/emit.c',
     '    grjit_asm_load32u(a, GRJIT_RDX, GRJIT_RSP, (int32_t)buffer_at + 8);',
     '    grjit_asm_load32u(a, GRJIT_RDX, GRJIT_RSP, (int32_t)buffer_at + 12);', ['testNatives']),
    ('a native\'s status is read as 64 bits of the buffer', 'jit', 'src/x86_64/emit.c',
     '    grjit_asm_load32u(a, GRJIT_RDX, GRJIT_RSP, (int32_t)buffer_at + 8);',
     '    grjit_asm_load64(a, GRJIT_RDX, GRJIT_RSP, (int32_t)buffer_at + 8);', ['testNatives']),
    ('the hidden pointer of a native with a status points past its buffer', 'jit', 'src/x86_64/emit.c',
     '    grjit_asm_lea(a, C_ARG(e, 0), GRJIT_RSP, (int32_t)buffer_at);',
     '    grjit_asm_lea(a, C_ARG(e, 0), GRJIT_RSP, (int32_t)buffer_at + 16);', ['testNatives']),
    ('a Win64 tail call copies its stack arguments through rdi', 'jit', 'src/x86_64/emit.c',
     '    .tail_copy = GRJIT_RCX,', '    .tail_copy = GRJIT_RDI,', ['testTail']),
    ('the Win64 overflow stub makes no room for the hook\'s shadow space', 'jit', 'src/x86_64/exit.c',
     '    grjit_asm_sub_rsp(a, e->abi->shadow_bytes);\n  }\n  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, 0);',
     '  }\n  grjit_asm_load64(a, GRJIT_RCX, GRJIT_RBP, 0);', ['testCalls']),
    ('a Win64 callable frame of several pages is not probed', 'jit', 'src/x86_64/emit.c',
     '    emit_stack_probe(e, alloc, GRJIT_RAX, GRJIT_R11);', '    (void)emit_stack_probe;', ['testWin64_calls']),
    ('the adapter\'s RUNTIME_FUNCTION ends one byte early', 'jit', 'src/code/memory.c',
     '      put32(rf + 4, prologue->adapter_end);', '      put32(rf + 4, prologue->adapter_end - 1);', ['testWin64_calls']),
    ('the body\'s RUNTIME_FUNCTION begins sixteen bytes after the internal entry', 'jit', 'src/code/memory.c',
     '    put32(body_rf, prologue->body_begin);', '    put32(body_rf, prologue->body_begin + 16);', ['testWin64_calls']),
    ('a body\'s unwind information names no frame register', 'jit', 'src/code/memory.c',
     '  out[3] = (uint8_t)(setfp_end != 0 ? RBP : 0);', '  out[3] = 0;', ['testWin64_calls']),
]
# The register-count edit is also a host mutation: what reads the convention out of the emitted code
# runs on every host, so the native run shows it too.
for _e in M_WIN64[:3]:
    M.append((_e[0] + ' (read from the emitted code on this host)',) + tuple(_e[1:]))

# What each library's tests are.
CORE_TESTS = ['testRebuild', 'testRegistry', 'testCompiled']
JIT_TESTS = ['testCalls', 'testCall_ir', 'testTail', 'testTail_ir', 'testNatives', 'testNative_ir']
# arm64: the same, and the structural test that reads the convention out of the code.
JIT_TESTS_ARM64 = JIT_TESTS + ['testArm64_calls']
# Win64: the same, and the tests that read the Win64 convention out of the code and run the unwinder.
JIT_TESTS_WIN64 = JIT_TESTS + ['testWin64_calls']

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
    extra_env = dict({'GRJIT_TEST_WATCHDOG_SECONDS': '240' if ARM64 else '45'}, **(extra_env or {}))
    global P_HOLDS_MUTATED_CORE
    if ARM64:
        return build_and_run_arm64(jit, timeout, extra_env, only)
    if WIN64:
        return build_and_run_win64(jit, timeout, extra_env, only)
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
        verdict = judge(rc, out, t)
        if verdict is not None:
            return verdict
    return 'MISSED', ''


def judge(rc, out, t):
    """One test program's result -> a verdict, or None when it passed."""
    if rc == 'timeout':
        return 'TIMEOUT', t
    failed = sorted(set(re.findall(r'\[  FAILED  \] (\S+)', out)))
    if isinstance(rc, int) and rc < 0 or (rc != 0 and not failed and rc >= 128):
        return 'CAUGHT', '%s: killed by a signal (crash)' % t
    if rc != 0:
        if not failed and WIN64 and re.search(r'Unhandled exception|page fault|wine: Unhandled|err:seh', out):
            return 'CAUGHT', '%s: crashed under wine (%s)' % (t, re.search(r'Unhandled exception[^\n]*|page fault[^\n]*|err:seh[^\n]*', out).group(0)[:60])
        if not failed:
            # Non-zero with no failing test named: the program did not run
            # as a test run, which is not evidence about the defect.
            return 'BUILD FAILED', '%s exited %s without naming a failing test: %s' % (t, rc, out[-200:])
        return 'CAUGHT', '%s: %s' % (t, ', '.join(f.split('.', 1)[-1][:50] for f in failed[:2]))
    return None


def build_and_run_arm64(jit, timeout, extra_env, only):
    """The arm64 form: the jit tests built with the cross compiler and run under qemu-user. A catch
    by the pin test alone is PIN-ONLY, not a catch."""
    tests = only or JIT_TESTS_ARM64
    for t in tests + ['testPin']:
        rc, out = sh('make -j8 build/linux/release/apps/%s PREFIX=%s %s' % (t, P, CROSS_MAKE), jit)
        if rc != 0:
            return 'BUILD FAILED', out[-300:]
    qemu = 'qemu-aarch64 -L %s -E LD_LIBRARY_PATH=%s:./build/linux/release/apps ' % (
        QEMU_SYSROOT, os.path.join(P, 'lib', 'ghoti.io'))
    for t in tests:
        rc, out = sh(qemu + './build/linux/release/apps/%s --gtest_brief=1' % t, jit, timeout, extra_env)
        verdict = judge(rc, out, t)
        if verdict is not None:
            return verdict
    rc, out = sh(qemu + './build/linux/release/apps/testPin --gtest_brief=1', jit, timeout, extra_env)
    pinned = judge(rc, out, 'testPin')
    if pinned is not None and pinned[0] == 'CAUGHT':
        return 'PIN-ONLY', pinned[1]
    return 'MISSED', ''


def build_and_run_win64(jit, timeout, extra_env, only):
    """The Windows form: the jit tests cross-built with mingw and run under wine. A catch by the pin
    test alone is PIN-ONLY, not a catch. A test that dies under wine without naming a failure (an
    unhandled exception, a page fault) is a crash, which is a catch here as a signal is natively."""
    tests = only or JIT_TESTS_WIN64
    for t in tests + ['testPin']:
        rc, out = sh('make -j4 build/win64/release/apps/%s.exe PREFIX=%s' % (t, P), jit)
        if rc != 0:
            return 'BUILD FAILED', out[-300:]
    for t in tests:
        rc, out = sh('cd build/win64/release/apps && ./%s.exe --gtest_brief=1' % t, jit, timeout, extra_env)
        verdict = judge(rc, out.replace('\r', ''), t)
        if verdict is not None:
            return verdict
    rc, out = sh('cd build/win64/release/apps && ./testPin.exe --gtest_brief=1', jit, timeout, extra_env)
    pinned = judge(rc, out.replace('\r', ''), 'testPin')
    if pinned is not None and pinned[0] == 'CAUGHT':
        return 'PIN-ONLY', pinned[1]
    return 'MISSED', ''


def tree_pair(rel_lib=None, rel_file=None, old=None, new=None):
    """Fresh copies of both trees, with the one edit applied to its library."""
    core = os.path.join(T, 'core')
    jit = os.path.join(T, 'jit')
    for d, src in ((core, CORE_PRISTINE), (jit, JIT_PRISTINE)):
        if (ARM64 or WIN64) and src == CORE_PRISTINE:
            continue
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
if ARM64 or WIN64:
    rc, out = 0, ''
else:
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
        verdict, detail = build_and_run(core, jit, 'jit', 120 if ARM64 else 60 if WIN64 else 20, {'GRJIT_TEST_WATCHDOG_SECONDS': '0'})
        good = verdict == expected
        ok = ok and good
        print('self-test: %-34s want %-12s got %-12s %s' % (name, expected, verdict, 'ok' if good else 'WRONG'),
              flush=True)
else:
    for idx, (name, lib, rel, old, new, *rest) in enumerate(M_ARM64 if ARM64 else M_WIN64 if WIN64 else M):
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
