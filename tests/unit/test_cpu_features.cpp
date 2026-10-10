/*
 * Tests of the optional CPU features of the x86-64 backend (spec-runtime-float, story 4): the pure
 * decoder over raw CPUID words, the mask that can only remove, the stamp text and the subset rule, then
 * the arm each feature-dependent operation takes (by its emitted bytes and by the set recorded with the
 * code), then the compiled code run under every subset of the host's detected set, and last the checks a
 * qemu CPU model run makes (suite/tools/xarch/jit-x86-cpu.sh).
 *
 * Nothing here "knows" that the SSE2 path ran because a mask was set: the recorded set and the bytes are
 * read off the code. A test that needs a feature the host lacks is skipped by name, and the last suite
 * counts that nothing was skipped that way on a host that has them all.
 */

#include "../feature_fixture.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(__x86_64__) && defined(__linux__)
#define CPU_RUN_HERE 1
#define CPU_ONLY_ON_X86_64_LINUX() ((void)0)
#else
#define CPU_RUN_HERE 0
#define CPU_ONLY_ON_X86_64_LINUX() \
  GTEST_SKIP() << "compiled code with feature arms is run on x86-64 SysV (Linux) only; the pure and " \
                  "emission-only tests of this file run everywhere"
#endif

using namespace featfx;

/* Under a qemu CPU model whose instruction gating is crossed relative to its CPUID bits (suite/tools/xarch/
 * jit-x86-cpu.sh sets GRJIT_TEST_CPU_CROSSED=1 for it), compiled code for the detected set is not asserted: a
 * feature whose bit is set does not have its instruction, so code that uses it is wrong there by qemu's
 * construction. The detection assertions and the forced-arm control still run. */
#define CPU_SKIP_IF_CROSSED() \
  do { \
    const char * crossed_ = std::getenv("GRJIT_TEST_CPU_CROSSED"); \
    if (crossed_ != nullptr && std::strcmp(crossed_, "1") == 0) { \
      GTEST_SKIP() << "model " << (std::getenv("GRJIT_TEST_CPU_MODEL") != nullptr ? std::getenv("GRJIT_TEST_CPU_MODEL") : "?") \
                   << " crosses qemu's instruction gating and its CPUID bits (qemu 10.0.13): compiled code for " \
                      "the detected set is not asserted here (documentation/design.md)"; \
    } \
  } while (0)

namespace {

constexpr uint32_t kSse41 = 1u << 19, kPopcnt = 1u << 23, kVmx = 1u << 5, kLzcnt = 1u << 5, kBmi1 = 1u << 3;

struct Row {
  const char * name;
  uint32_t max_basic, leaf1_ecx, max_ext, ext1_ecx, leaf7_ebx;
  GRJIT_Features want;
};

GRJIT_Features decode(const Row & r) {
  return grjit_features_from_cpuid(r.max_basic, r.leaf1_ecx, r.max_ext, r.ext1_ecx, r.leaf7_ebx);
}

std::string set_name(GRJIT_Features f) {
  std::string s;
  for (GRJIT_Features b : {GRJIT_FEATURE_SSE4_1, GRJIT_FEATURE_POPCNT, GRJIT_FEATURE_LZCNT, GRJIT_FEATURE_BMI1}) {
    if ((f & b) != 0) {
      s += (s.empty() ? "" : "+") + std::string(feature_name(b));
    }
  }
  return s.empty() ? "{}" : "{" + s + "}";
}

std::string stamp_of(GRJIT_Features f) {
  char buf[GRJIT_FEATURES_STAMP_MAX];
  EXPECT_EQ(grjit_features_stamp(f, buf, sizeof buf), GRJIT_OK) << f;
  return buf;
}

} // namespace

/* ---- The values the stamps carry --------------------------------------------------------------- */

TEST(CpuFeatures, TheValuesAreThoseTheStampsCarryAndAreNeverRenumbered) {
  EXPECT_EQ(GRJIT_FEATURE_SSE4_1, 1u);
  EXPECT_EQ(GRJIT_FEATURE_POPCNT, 2u);
  EXPECT_EQ(GRJIT_FEATURE_LZCNT, 4u);
  EXPECT_EQ(GRJIT_FEATURE_BMI1, 8u);
  EXPECT_EQ(GRJIT_FEATURES_NONE, 0u);
  EXPECT_EQ(GRJIT_FEATURES_ALL, 15u);
  EXPECT_EQ(sizeof(GRJIT_Features), sizeof(uint32_t));
  EXPECT_EQ(GRJIT_FEATURE_SSE4_1 | GRJIT_FEATURE_POPCNT | GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_BMI1,
      GRJIT_FEATURES_ALL);
  // The options struct carries the mask last, after every field there was.
  GRJIT_CompileOptions o{};
  EXPECT_EQ(o.disabled_features, GRJIT_FEATURES_NONE) << "zero disables nothing";
  EXPECT_GT(offsetof(GRJIT_CompileOptions, disabled_features), offsetof(GRJIT_CompileOptions, entry_hook));
}

/* ---- The decoder -------------------------------------------------------------------------------- */

TEST(CpuDecode, EachFeatureIsExactlyTheBitNamedAndNoOtherIsInferred) {
  const uint32_t kBasic = 0x16, kExt = 0x80000008;
  const Row rows[] = {
      {"nothing", kBasic, 0, kExt, 0, 0, GRJIT_FEATURES_NONE},
      {"SSE4.1: leaf 1 ECX bit 19", kBasic, kSse41, kExt, 0, 0, GRJIT_FEATURE_SSE4_1},
      {"POPCNT: leaf 1 ECX bit 23", kBasic, kPopcnt, kExt, 0, 0, GRJIT_FEATURE_POPCNT},
      {"LZCNT: leaf 0x80000001 ECX bit 5", kBasic, 0, kExt, kLzcnt, 0, GRJIT_FEATURE_LZCNT},
      {"BMI1: leaf 7 EBX bit 3", kBasic, 0, kExt, 0, kBmi1, GRJIT_FEATURE_BMI1},
      {"all four", kBasic, kSse41 | kPopcnt, kExt, kLzcnt, kBmi1, GRJIT_FEATURES_ALL},
      {"SSE4.2 (leaf 1 bit 20) is not POPCNT and not SSE4.1", kBasic, 1u << 20, kExt, 0, 0, GRJIT_FEATURES_NONE},
      {"SSE4.1 alone is not POPCNT", kBasic, kSse41, kExt, 0, 0, GRJIT_FEATURE_SSE4_1},
      {"POPCNT alone is not SSE4.1", kBasic, kPopcnt, kExt, 0, 0, GRJIT_FEATURE_POPCNT},
      {"LZCNT alone is not POPCNT", kBasic, 0, kExt, kLzcnt, 0, GRJIT_FEATURE_LZCNT},
      {"every other leaf-1 bit set", kBasic, ~(kSse41 | kPopcnt), kExt, 0, 0, GRJIT_FEATURES_NONE},
      {"every other extended bit set", kBasic, 0, kExt, ~kLzcnt, 0, GRJIT_FEATURES_NONE},
      {"every other leaf-7 bit set", kBasic, 0, kExt, 0, ~kBmi1, GRJIT_FEATURES_NONE},
      // The trap: leaf 1 ECX bit 5 is VMX, not LZCNT.
      {"VMX is not LZCNT", kBasic, kVmx, kExt, 0, 0, GRJIT_FEATURES_NONE},
      {"VMX and the real LZCNT are LZCNT once", kBasic, kVmx, kExt, kLzcnt, 0, GRJIT_FEATURE_LZCNT},
      {"leaf 1 bit 5 with every other bit", kBasic, ~0u & ~(kSse41 | kPopcnt), kExt, 0, 0, GRJIT_FEATURES_NONE},
      // Leaf 7 is defined only up to the maximum basic leaf.
      {"leaf 7 not defined below 7 (max 4)", 4, 0, kExt, 0, kBmi1, GRJIT_FEATURES_NONE},
      {"leaf 7 not defined below 7 (max 6)", 6, 0, kExt, 0, ~0u, GRJIT_FEATURES_NONE},
      {"leaf 7 defined at 7", 7, 0, kExt, 0, kBmi1, GRJIT_FEATURE_BMI1},
      {"max basic 0 gives nothing from leaf 7", 0, 0, kExt, 0, kBmi1, GRJIT_FEATURES_NONE},
      // The extended leaf is defined only up to the maximum extended leaf.
      {"extended leaf absent (max 0x80000000)", kBasic, 0, 0x80000000u, kLzcnt, 0, GRJIT_FEATURES_NONE},
      {"extended leaf absent (max 0)", kBasic, 0, 0, ~0u, 0, GRJIT_FEATURES_NONE},
      {"extended leaf absent (max 0x7FFFFFFF)", kBasic, 0, 0x7FFFFFFFu, kLzcnt, 0, GRJIT_FEATURES_NONE},
      {"extended leaf at its minimum", kBasic, 0, 0x80000001u, kLzcnt, 0, GRJIT_FEATURE_LZCNT},
  };
  for (const Row & r : rows) {
    EXPECT_EQ(decode(r), r.want) << r.name << ": got " << set_name(decode(r)) << ", want " << set_name(r.want);
  }
}

TEST(CpuDecode, NoWordsEverProduceABitAboveTheKnownFeaturesAndTheRulesHoldForAnyWords) {
  uint64_t x = 0x9E3779B97F4A7C15ull;
  auto next = [&]() {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return static_cast<uint32_t>(x >> 16);
  };
  const uint32_t maxes[] = {0, 1, 4, 6, 7, 8, 0x16, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0x80000008u};
  for (int i = 0; i < 20000; i++) {
    Row r{"random", maxes[next() % 11], next(), maxes[next() % 11], next(), next(), 0};
    // The rules, spelled again independently of the decoder.
    GRJIT_Features want = 0;
    want |= (r.leaf1_ecx & kSse41) != 0 ? GRJIT_FEATURE_SSE4_1 : 0;
    want |= (r.leaf1_ecx & kPopcnt) != 0 ? GRJIT_FEATURE_POPCNT : 0;
    want |= r.max_ext >= 0x80000001u && (r.ext1_ecx & kLzcnt) != 0 ? GRJIT_FEATURE_LZCNT : 0;
    want |= r.max_basic >= 7 && (r.leaf7_ebx & kBmi1) != 0 ? GRJIT_FEATURE_BMI1 : 0;
    ASSERT_EQ(decode(r), want) << r.max_basic << " " << r.leaf1_ecx << " " << r.max_ext << " " << r.ext1_ecx
                               << " " << r.leaf7_ebx;
    ASSERT_EQ(decode(r) & ~GRJIT_FEATURES_ALL, 0u);
  }
}

/* ---- The mask ----------------------------------------------------------------------------------- */

TEST(CpuMask, AMaskOnlyRemovesAndTheEffectiveSetIsAlwaysASubsetOfTheDetectedOne) {
  const GRJIT_Features masks[] = {0, 1, 2, 3, 4, 5, 8, 9, 12, 14, 15, 0x10, 0x1F, 0x80000000u, 0xFFFFFFF0u,
      0xFFFFFFFFu};
  for (GRJIT_Features detected_set = 0; detected_set <= GRJIT_FEATURES_ALL; detected_set++) {
    for (GRJIT_Features mask : masks) {
      const GRJIT_Features e = grjit_features_apply_mask(detected_set, mask);
      EXPECT_EQ(e, detected_set & ~mask) << set_name(detected_set) << " minus " << mask;
      EXPECT_TRUE(grjit_features_subset(e, detected_set)) << "a mask cannot add a feature";
      EXPECT_EQ(e & ~GRJIT_FEATURES_ALL, 0u) << "a reserved bit is never produced";
    }
  }
  // Examples from the story: the full set minus SSE4.1.
  EXPECT_EQ(grjit_features_apply_mask(GRJIT_FEATURES_ALL, GRJIT_FEATURE_SSE4_1),
      GRJIT_FEATURE_POPCNT | GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_BMI1);
  EXPECT_EQ(grjit_features_apply_mask(GRJIT_FEATURES_ALL, GRJIT_FEATURES_ALL), GRJIT_FEATURES_NONE);
  EXPECT_EQ(grjit_features_apply_mask(GRJIT_FEATURES_ALL, 0), GRJIT_FEATURES_ALL);
}

TEST(CpuMask, OnASyntheticEmptyDetectedSetNoMaskEnablesAnything) {
  for (GRJIT_Features mask : {GRJIT_Features{0}, GRJIT_FEATURES_ALL, GRJIT_Features{0x10},
           GRJIT_Features{0xFFFFFFF0u}, GRJIT_Features{0xFFFFFFFFu}, GRJIT_FEATURE_LZCNT, GRJIT_Features{0x80000000u}}) {
    EXPECT_EQ(grjit_features_apply_mask(GRJIT_FEATURES_NONE, mask), GRJIT_FEATURES_NONE) << mask;
  }
  // A detected set carrying reserved bits (never produced, but a synthetic one may) loses them.
  EXPECT_EQ(grjit_features_apply_mask(0xFFFFFFFFu, 0), GRJIT_FEATURES_ALL);
}

TEST(CpuMask, TheHostsEffectiveSetIsItsDetectedSetLessTheMask) {
  const GRJIT_Features d = grjit_features_detected();
  EXPECT_EQ(d & ~GRJIT_FEATURES_ALL, 0u);
  EXPECT_EQ(grjit_features_effective(0), d);
  EXPECT_EQ(grjit_features_effective(GRJIT_FEATURES_ALL), GRJIT_FEATURES_NONE);
  EXPECT_EQ(grjit_features_effective(0xFFFFFFF0u), d) << "reserved bits in a mask change nothing";
  for (GRJIT_Features mask = 0; mask <= GRJIT_FEATURES_ALL; mask++) {
    const GRJIT_Features e = grjit_features_effective(mask);
    EXPECT_EQ(e, d & ~mask) << mask;
    EXPECT_TRUE(grjit_features_subset(e, d)) << mask;
  }
#if !defined(__x86_64__)
  EXPECT_EQ(d, GRJIT_FEATURES_NONE) << "a target that is not x86-64 has no optional feature";
#endif
}

/* ---- Detection ---------------------------------------------------------------------------------- */

TEST(CpuDetect, TheCachedSetIsTheInstructionsAndEveryThreadSeesTheSameWord) {
  EXPECT_EQ(grjit_features_detected(), grjit_features_read_cpu());
  EXPECT_EQ(grjit_features_detected(), grjit_features_detected());
  std::vector<std::thread> threads;
  std::vector<GRJIT_Features> seen(8, ~GRJIT_Features{0});
  for (size_t i = 0; i < seen.size(); i++) {
    threads.emplace_back([&seen, i] { seen[i] = grjit_features_detected(); });
  }
  for (std::thread & t : threads) {
    t.join();
  }
  for (GRJIT_Features s : seen) {
    EXPECT_EQ(s, grjit_features_detected());
  }
  std::printf("detected features of this host: %s (%s)\n", set_name(grjit_features_detected()).c_str(),
      stamp_of(grjit_features_detected()).c_str());
}

TEST(CpuDetect, TheDetectedSetIsTheCpuinfoFlagsOfTheHostWhenThatFileIsTheHostsOwn) {
#if !(defined(__x86_64__) && defined(__linux__))
  GTEST_SKIP() << "the oracle is /proc/cpuinfo of a native Linux x86-64 host";
#else
  if (std::getenv("GRJIT_TEST_CPU_MODEL") != nullptr) {
    GTEST_SKIP() << "run under a qemu CPU model, whose /proc/cpuinfo is the real host's, not the model's; "
                    "the model run compares against GRJIT_TEST_EXPECT_FEATURES instead";
  }
  std::ifstream in("/proc/cpuinfo");
  ASSERT_TRUE(in.good()) << "/proc/cpuinfo is not readable";
  std::string line, flags;
  while (std::getline(in, line)) {
    if (line.rfind("flags", 0) == 0) {
      flags = " " + line.substr(line.find(':') + 1) + " ";
      break;
    }
  }
  ASSERT_FALSE(flags.empty()) << "no flags line in /proc/cpuinfo";
  auto has = [&](const char * f) { return flags.find(std::string(" ") + f + " ") != std::string::npos; };
  GRJIT_Features want = 0;
  want |= has("sse4_1") ? GRJIT_FEATURE_SSE4_1 : 0;
  want |= has("popcnt") ? GRJIT_FEATURE_POPCNT : 0;
  want |= has("abm") ? GRJIT_FEATURE_LZCNT : 0;
  want |= has("bmi1") ? GRJIT_FEATURE_BMI1 : 0;
  EXPECT_EQ(grjit_features_detected(), want)
      << "detected " << set_name(grjit_features_detected()) << ", /proc/cpuinfo says " << set_name(want);
#endif
}

/* ---- The subset rule and the stamp -------------------------------------------------------------- */

TEST(CpuSubset, ASetIsASubsetOfAnotherExactlyWhenItNamesNothingTheOtherLacks) {
  for (GRJIT_Features a = 0; a <= GRJIT_FEATURES_ALL; a++) {
    for (GRJIT_Features b = 0; b <= GRJIT_FEATURES_ALL; b++) {
      EXPECT_EQ(grjit_features_subset(a, b), (a & ~b) == 0) << a << " " << b;
    }
    EXPECT_TRUE(grjit_features_subset(a, a));
    EXPECT_TRUE(grjit_features_subset(0, a));
    EXPECT_TRUE(grjit_features_subset(a, GRJIT_FEATURES_ALL));
  }
  EXPECT_TRUE(grjit_features_subset(GRJIT_FEATURE_LZCNT, GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_BMI1));
  EXPECT_FALSE(grjit_features_subset(GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_BMI1, GRJIT_FEATURE_LZCNT));
  EXPECT_FALSE(grjit_features_subset(GRJIT_FEATURE_LZCNT, GRJIT_FEATURE_POPCNT));
}

TEST(CpuStamp, EachSetHasItsCanonicalTextAndTheTextGivesTheSetBack) {
#if defined(__x86_64__)
  EXPECT_EQ(stamp_of(GRJIT_FEATURES_NONE), "x86_64-v1");
  EXPECT_EQ(stamp_of(GRJIT_FEATURES_ALL), "x86_64-v1+sse4.1+popcnt+lzcnt+bmi1");
  EXPECT_EQ(stamp_of(GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_SSE4_1), "x86_64-v1+sse4.1+lzcnt");
  for (GRJIT_Features f = 0; f <= GRJIT_FEATURES_ALL; f++) {
    std::string want = "x86_64-v1";
    want += (f & GRJIT_FEATURE_SSE4_1) != 0 ? "+sse4.1" : "";
    want += (f & GRJIT_FEATURE_POPCNT) != 0 ? "+popcnt" : "";
    want += (f & GRJIT_FEATURE_LZCNT) != 0 ? "+lzcnt" : "";
    want += (f & GRJIT_FEATURE_BMI1) != 0 ? "+bmi1" : "";
    const std::string got = stamp_of(f);
    EXPECT_EQ(got, want) << f;
    EXPECT_LT(got.size(), size_t{GRJIT_FEATURES_STAMP_MAX});
    GRJIT_Features back = ~GRJIT_Features{0};
    EXPECT_EQ(grjit_features_from_stamp(got.c_str(), &back), GRJIT_OK) << got;
    EXPECT_EQ(back, f) << got;
  }
#elif defined(__aarch64__)
  EXPECT_EQ(stamp_of(GRJIT_FEATURES_NONE), "arm64-v8");
  GRJIT_Features back = ~GRJIT_Features{0};
  EXPECT_EQ(grjit_features_from_stamp("arm64-v8", &back), GRJIT_OK);
  EXPECT_EQ(back, GRJIT_FEATURES_NONE);
  char buf[GRJIT_FEATURES_STAMP_MAX];
  EXPECT_EQ(grjit_features_stamp(GRJIT_FEATURE_LZCNT, buf, sizeof buf), GRJIT_ERR_INVALID)
      << "no feature name is accepted where the detected set is empty";
  for (const char * bad : {"arm64-v8+sse4.1", "arm64-v8+popcnt+lzcnt", "x86_64-v1", "x86_64-v1+sse4.1"}) {
    EXPECT_EQ(grjit_features_from_stamp(bad, &back), GRJIT_ERR_INVALID) << bad;
  }
#else
  GTEST_SKIP() << "the stamp prefix of this target is not one the test knows";
#endif
}

TEST(CpuStamp, AMalformedStampFailsClosed) {
  GRJIT_Features back = 0x77;
#if defined(__x86_64__)
  for (const char * bad : {
           "x86_64-v1+lzcnt+sse4.1",             // out of order
           "x86_64-v1+bmi1+popcnt",              // out of order
           "x86_64-v1+sse4.1+avx2",              // unknown name
           "x86_64-v1+avx2",                     // unknown name
           "x86_64-v1+popcnt+popcnt",            // repeated
           "x86_64-v1+sse4.1+sse4.1",            // repeated
           "arm64-v8",                           // a wrong prefix
           "arm64-v8+sse4.1",                    // a wrong prefix
           "x86_64-v2",                          // a wrong prefix
           "x86_64-v2+sse4.1",
           "x86_64-v1 ",                         // trailing junk
           "x86_64-v1\n",
           "x86_64-v1+",                         // a plus and nothing after it
           "x86_64-v1+sse4.1+",
           "x86_64-v1++sse4.1",
           "x86_64-v1sse4.1",                    // no plus
           "x86_64-v1+sse4.1junk",
           "x86_64-v1+sse",                      // a prefix of a name
           "x86_64-v1+sse4.1+popcnt+lzcnt+bmi1+",
           "x86_64-v1+sse4.1,popcnt",
           "x86_64-v1+SSE4.1",                   // case
           "X86_64-V1",
           "x86_64",
           "",
       }) {
    back = 0x77;
    EXPECT_EQ(grjit_features_from_stamp(bad, &back), GRJIT_ERR_INVALID) << "'" << bad << "'";
    EXPECT_EQ(back, 0x77u) << "nothing is written on a refusal";
  }
#endif
  EXPECT_EQ(grjit_features_from_stamp(nullptr, &back), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_features_from_stamp("x86_64-v1", nullptr), GRJIT_ERR_INVALID);
}

TEST(CpuStamp, AShortBufferIsRefusedWithNothingPartialAndAReservedBitIsInvalid) {
  const GRJIT_Features full = GRJIT_FEATURES_ALL;
  const std::string text = stamp_of(GRJIT_FEATURES_NONE);
  char buf[GRJIT_FEATURES_STAMP_MAX];
  std::memset(buf, 'x', sizeof buf);
  // exactly the text and no room for the NUL: refused, and the buffer says "no stamp"
  EXPECT_EQ(grjit_features_stamp(GRJIT_FEATURES_NONE, buf, text.size()), GRJIT_ERR_LIMIT);
  EXPECT_EQ(buf[0], '\0');
  // one more byte is enough
  EXPECT_EQ(grjit_features_stamp(GRJIT_FEATURES_NONE, buf, text.size() + 1), GRJIT_OK);
  EXPECT_EQ(text, buf);
  // a size of zero writes nothing at all
  std::memset(buf, 'x', sizeof buf);
  EXPECT_EQ(grjit_features_stamp(GRJIT_FEATURES_NONE, buf, 0), GRJIT_ERR_LIMIT);
  EXPECT_EQ(buf[0], 'x');
  EXPECT_EQ(grjit_features_stamp(GRJIT_FEATURES_NONE, nullptr, 0), GRJIT_ERR_LIMIT);
  (void)full;
  EXPECT_EQ(grjit_features_stamp(0x10, buf, sizeof buf), GRJIT_ERR_INVALID);
  EXPECT_EQ(grjit_features_stamp(0x80000000u, buf, sizeof buf), GRJIT_ERR_INVALID);
#if defined(__x86_64__)
  const std::string all = stamp_of(GRJIT_FEATURES_ALL);
  EXPECT_EQ(grjit_features_stamp(GRJIT_FEATURES_ALL, buf, all.size()), GRJIT_ERR_LIMIT);
  EXPECT_EQ(grjit_features_stamp(GRJIT_FEATURES_ALL, buf, all.size() + 1), GRJIT_OK);
  EXPECT_EQ(all, buf);
  EXPECT_LE(all.size() + 1, size_t{GRJIT_FEATURES_STAMP_MAX});
#endif
}

/* ---- The arm an operation takes: its bytes, and the set recorded with them ---------------------- */

namespace {

bool contains(const std::string & hay, const char * needle) {
  return hay.find(needle) != std::string::npos;
}

} // namespace

TEST(CpuArms, EachOperationEmitsItsFeatureArmWhenAndOnlyWhenItsFeatureIsInTheSetAndRecordsExactlyThat) {
  size_t checked = 0;
  for (const cpupins::Pin & pin : cpupins::kPins) {
    SCOPED_TRACE(pin.instr);
    ASSERT_NE(std::string(pin.v1), std::string(pin.arm)) << "the two arms are different code";
    floatfx::FloatOp op = floatfx::build_any_op(pin.instr);
    ASSERT_NE(op.function, nullptr);
    Fn f(op.function);
    for (GRJIT_Features set = 0; set <= GRJIT_FEATURES_ALL; set++) {
      for (GRJIT_Features reserved : {GRJIT_Features{0}, GRJIT_Features{0xFFFFFFF0u}}) {
        SCOPED_TRACE("set " + set_name(set));
        const Emit e = emit_with(f.f, set | reserved);
        ASSERT_EQ(e.result, GRJIT_OK);
        const bool arm = (set & pin.need) != 0;
        EXPECT_EQ(e.bytes, arm ? pin.arm : pin.v1)
            << (arm ? "the feature arm" : "the v1 sequence") << " was expected";
        EXPECT_EQ(e.used, arm ? pin.need : GRJIT_FEATURES_NONE) << "the recorded set is the arm taken";
        checked++;
      }
    }
  }
  EXPECT_EQ(checked, size_t{14 * 16 * 2});
}

TEST(CpuArms, TheBytesOfEachArmContainTheInstructionsItIsSaidToUseAndTheV1OnesContainNone) {
  for (const cpupins::Pin & pin : cpupins::kPins) {
    SCOPED_TRACE(pin.instr);
    const std::string name = pin.instr;
    const bool wide = name.rfind("i64", 0) == 0;
    const std::string v1 = pin.v1, arm = pin.arm;
    if (name.find(".clz") != std::string::npos) {
      EXPECT_TRUE(contains(arm, wide ? "f3480fbdc0" : "f30fbdc0")) << "lzcnt rax/eax";
      EXPECT_FALSE(contains(arm, "0f44c1")) << "lzcnt needs no cmov for zero";
      EXPECT_TRUE(contains(v1, wide ? "480fbdc0" : "0fbdc0")) << "bsr";
      EXPECT_TRUE(contains(v1, wide ? "480f44c1" : "0f44c1")) << "the cmov for zero";
      EXPECT_FALSE(contains(v1, "f30fbd") || contains(v1, "f3480fbd")) << "no lzcnt in the v1 sequence";
    } else if (name.find(".ctz") != std::string::npos) {
      EXPECT_TRUE(contains(arm, wide ? "f3480fbcc0" : "f30fbcc0")) << "tzcnt";
      EXPECT_FALSE(contains(arm, "0f44c1"));
      EXPECT_TRUE(contains(v1, wide ? "480fbcc0" : "0fbcc0")) << "bsf";
      EXPECT_TRUE(contains(v1, wide ? "480f44c1" : "0f44c1"));
      EXPECT_FALSE(contains(v1, "f30fbc") || contains(v1, "f3480fbc"));
    } else if (name.find(".popcnt") != std::string::npos) {
      EXPECT_TRUE(contains(arm, wide ? "f3480fb8c0" : "f30fb8c0")) << "popcnt";
      EXPECT_FALSE(contains(v1, "0fb8")) << "the parallel count has no popcnt";
    } else {
      const bool dbl = name.rfind("f64", 0) == 0;
      const char * want = dbl ? "660f3a0bc0" : "660f3a0ac0";
      EXPECT_TRUE(contains(arm, want)) << (dbl ? "roundsd" : "roundss") << " xmm0, xmm0";
      const std::string imm = name.find(".ceil") != std::string::npos    ? "0a"
                              : name.find(".floor") != std::string::npos ? "09"
                              : name.find(".trunc") != std::string::npos ? "0b"
                                                                         : "08";
      EXPECT_TRUE(contains(arm, (std::string(want) + imm).c_str())) << "the immediate is " << imm;
      EXPECT_FALSE(contains(v1, "660f3a")) << "the v1 rounding uses no SSE4.1";
    }
  }
}

TEST(CpuArms, NoOtherOperationChangesArmAndRecordsNothing) {
  std::vector<std::string> names = intfx::all_int_ops("i32");
  for (const std::string & n : intfx::all_int_ops("i64")) {
    names.push_back(n);
  }
  for (const char * ty : {"f32", "f64"}) {
    for (const char * n : {"add", "sub", "mul", "div", "min", "max", "copysign", "eq", "lt", "neg", "abs",
             "sqrt", "convert_i32_s", "convert_i64_u", "promote_f32", "demote_f64", "reinterpret_i32",
             "reinterpret_i64"}) {
      names.push_back(std::string(ty) + "." + n);
    }
  }
  names.push_back("i32.trunc_sat_f64_s");
  names.push_back("i64.trunc_sat_f32_u");
  names.push_back("i32.reinterpret_f32");
  const std::vector<std::string> arms = feature_ops();
  size_t compared = 0;
  for (const std::string & n : names) {
    if (std::find(arms.begin(), arms.end(), n) != arms.end()) {
      continue;
    }
    floatfx::FloatOp op = floatfx::build_any_op(n);
    if (op.function == nullptr) {
      continue;
    }
    Fn f(op.function);
    const Emit none = emit_with(f.f, GRJIT_FEATURES_NONE);
    const Emit all = emit_with(f.f, GRJIT_FEATURES_ALL);
    ASSERT_EQ(none.result, GRJIT_OK) << n;
    ASSERT_EQ(all.result, GRJIT_OK) << n;
    EXPECT_EQ(none.bytes, all.bytes) << n << " has no feature arm";
    EXPECT_EQ(all.used, GRJIT_FEATURES_NONE) << n;
    compared++;
  }
  EXPECT_GT(compared, 60u);
}

TEST(CpuArms, Arm64AndWin64EmissionIgnoreTheSetAndStillRefuseTheNewOperationsByName) {
  // An integer-only function: emitted by all three backends, and the same bytes whatever set is named.
  floatfx::FloatOp add = floatfx::build_any_op("i64.add");
  Fn f(add.function);
  for (GRJIT_Arch arch : {GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
    const Emit none = emit_with(f.f, GRJIT_FEATURES_NONE, arch);
    const Emit all = emit_with(f.f, GRJIT_FEATURES_ALL, arch);
    ASSERT_EQ(none.result, GRJIT_OK) << arch;
    ASSERT_EQ(all.result, GRJIT_OK) << arch;
    EXPECT_EQ(none.bytes, all.bytes) << arch << ": the set changes nothing";
    EXPECT_EQ(all.used, GRJIT_FEATURES_NONE) << arch << ": nothing is recorded";
  }
  // The operations with a feature arm are refused there as they were: no arm, no record.
  for (const std::string & n : feature_ops()) {
    floatfx::FloatOp op = floatfx::build_any_op(n);
    Fn g(op.function);
    for (GRJIT_Arch arch : {GRJIT_ARCH_ARM64, GRJIT_ARCH_X86_64_WIN64}) {
      for (GRJIT_Features set : {GRJIT_FEATURES_NONE, GRJIT_FEATURES_ALL}) {
        // story 5 brings these to Win64 and arm64; until then each refuses the function whole
        const Emit e = emit_with(g.f, set, arch);
        EXPECT_EQ(e.result, GRJIT_ERR_UNSUPPORTED) << n << " on arch " << arch;
        EXPECT_EQ(e.used, ~GRJIT_Features{0}) << "no record was made for a refused function";
      }
    }
  }
}

/* ---- Compiled code: the record, the reuse rule, the answers under every subset ------------------ */

namespace {

GRJIT_Function * clz_and_ceil() {
  B b("clz_and_ceil", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32), y = b.param(GRJIT_TYPE_F64);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_I32), s = b.reg(GRJIT_TYPE_F64);
  b.at(b.block());
  b.un(GRJIT_OP_CLZ, r, V(x));
  b.un(GRJIT_OP_FCEIL, s, V(y));
  b.ret(V(r));
  return b.finish();
}

GRJIT_Function * neither() {
  B b("neither", 0);
  GRJIT_VReg x = b.param(GRJIT_TYPE_I32), y = b.param(GRJIT_TYPE_F64);
  GRJIT_VReg r = b.reg(GRJIT_TYPE_I32), s = b.reg(GRJIT_TYPE_F64);
  b.at(b.block());
  b.bin(GRJIT_OP_ADD, r, V(x), I(1));
  b.bin(GRJIT_OP_FADD, s, V(y), V(y));
  b.ret(V(r));
  return b.finish();
}

struct Code {
  GRJIT_Code * code = nullptr;
  GRJIT_Result result = GRJIT_ERR_INTERNAL;
  Code(const Code &) = delete;
  Code & operator=(const Code &) = delete;
  Code(GRJIT_Function * f, JitWorld & w, GRJIT_Features disabled) {
    GRJIT_CompileOptions o{};
    o.pages = w.pages();
    o.disabled_features = disabled;
    result = grjit_compile(&o, f, &code);
  }
  ~Code() { grjit_code_destroy(code); }
};

} // namespace

TEST(CpuRecord, TheRecordedSetIsExactlyTheArmsEmittedUnderEverySubsetOfTheHostsFeatures) {
  CPU_ONLY_ON_X86_64_LINUX();
  JitWorld w;
  Fn both(clz_and_ceil());
  Fn none(neither());
  size_t compiled = 0;
  for (GRJIT_Features mask : every_subset_masks()) {
    const GRJIT_Features effective = detected() & ~mask & GRJIT_FEATURES_ALL;
    SCOPED_TRACE("effective " + set_name(effective));
    Code a(both.f, w, mask);
    ASSERT_EQ(a.result, GRJIT_OK);
    GRJIT_Features want = effective & (GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_SSE4_1);
    EXPECT_EQ(grjit_code_features(a.code), want);
    EXPECT_TRUE(grjit_features_subset(grjit_code_features(a.code), effective))
        << "the recorded set is a subset of the effective one";
    // The same function emitted for exactly that set gives the same record and the same bytes.
    const Emit e = emit_with(both.f, effective);
    EXPECT_EQ(e.used, want);
    Code b2(none.f, w, mask);
    ASSERT_EQ(b2.result, GRJIT_OK);
    EXPECT_EQ(grjit_code_features(b2.code), GRJIT_FEATURES_NONE) << "a function with neither records nothing";
    compiled += 2;
  }
  EXPECT_GE(compiled, 2u);
  // Every v1-compiled function records nothing, whatever it contains.
  Code v1(both.f, w, GRJIT_FEATURES_ALL);
  ASSERT_EQ(v1.result, GRJIT_OK);
  EXPECT_EQ(grjit_code_features(v1.code), GRJIT_FEATURES_NONE);
  EXPECT_EQ(grjit_code_features(nullptr), GRJIT_FEATURES_NONE);
  // The feature arms really are in the code: a host with the feature compiled with the default mask
  // records it, so a test that only saw empty sets would be measuring nothing.
  Code native(both.f, w, GRJIT_FEATURES_NONE);
  ASSERT_EQ(native.result, GRJIT_OK);
  EXPECT_EQ(grjit_code_features(native.code), detected() & (GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_SSE4_1));
}

TEST(CpuRecord, AMaskOfReservedBitsChangesNothingAndAFullMaskIsV1) {
  CPU_ONLY_ON_X86_64_LINUX();
  JitWorld w;
  Fn both(clz_and_ceil());
  Code plain(both.f, w, 0), reserved(both.f, w, 0xFFFFFFF0u), everything(both.f, w, 0xFFFFFFFFu),
      all(both.f, w, GRJIT_FEATURES_ALL);
  ASSERT_EQ(plain.result, GRJIT_OK);
  ASSERT_EQ(reserved.result, GRJIT_OK);
  ASSERT_EQ(everything.result, GRJIT_OK);
  ASSERT_EQ(all.result, GRJIT_OK);
  EXPECT_EQ(grjit_code_features(reserved.code), grjit_code_features(plain.code));
  EXPECT_EQ(grjit_code_features(everything.code), GRJIT_FEATURES_NONE);
  EXPECT_EQ(grjit_code_features(all.code), GRJIT_FEATURES_NONE);
  // and the bytes are the same ones
  ASSERT_EQ(grjit_code_size(reserved.code), grjit_code_size(plain.code));
  EXPECT_EQ(std::memcmp(grjit_code_address(reserved.code), grjit_code_address(plain.code),
                grjit_code_size(plain.code)),
      0);
  ASSERT_EQ(grjit_code_size(everything.code), grjit_code_size(all.code));
  EXPECT_EQ(std::memcmp(grjit_code_address(everything.code), grjit_code_address(all.code),
                grjit_code_size(all.code)),
      0);
}

TEST(CpuRecord, ATargetWithNoFeatureArmAcceptsAnyMaskAndRecordsNothing) {
  // An integer-only function has no operation with a feature arm, so on every backend (x86-64 SysV and Win64,
  // arm64) the mask is accepted, changes nothing, and the record is empty. On arm64 nothing is detected either.
  GRJIT_REQUIRE_BACKEND();
  JitWorld w;
  floatfx::FloatOp op = floatfx::build_any_op("i64.add");
  Fn f(op.function);
  Code plain(f.f, w, 0), all(f.f, w, GRJIT_FEATURES_ALL), junk(f.f, w, 0xFFFFFFFFu);
  ASSERT_EQ(plain.result, GRJIT_OK);
  ASSERT_EQ(all.result, GRJIT_OK);
  ASSERT_EQ(junk.result, GRJIT_OK);
  EXPECT_EQ(grjit_code_features(plain.code), GRJIT_FEATURES_NONE);
  EXPECT_EQ(grjit_code_features(all.code), GRJIT_FEATURES_NONE);
  EXPECT_EQ(grjit_code_features(junk.code), GRJIT_FEATURES_NONE);
  ASSERT_EQ(grjit_code_size(plain.code), grjit_code_size(all.code));
  EXPECT_EQ(std::memcmp(grjit_code_address(plain.code), grjit_code_address(all.code), grjit_code_size(all.code)), 0)
      << "the mask changes no byte of a function with no feature arm";
  EXPECT_TRUE(grjit_code_usable_with(plain.code, GRJIT_FEATURES_NONE)) << "code that needs nothing runs anywhere";
  EXPECT_EQ(grjit_features_effective(GRJIT_FEATURES_ALL), GRJIT_FEATURES_NONE);
#if defined(__aarch64__)
  EXPECT_EQ(grjit_features_detected(), GRJIT_FEATURES_NONE) << "arm64 needs no optional feature";
  EXPECT_EQ(grjit_features_effective(0), GRJIT_FEATURES_NONE);
#endif
}

TEST(CpuReuse, CodeRunsWhereItsRecordedSetIsASubsetOfTheRunningOneAndNowhereElse) {
  CPU_ONLY_ON_X86_64_LINUX();
  GRJIT_REQUIRE_FEATURE(GRJIT_FEATURE_LZCNT);
  JitWorld w;
  floatfx::FloatOp clz = floatfx::build_any_op("i32.clz");
  Fn f(clz.function);
  // U = {LZCNT}: compiled with LZCNT allowed and nothing else it could use.
  Code needs(f.f, w, GRJIT_FEATURES_ALL & ~GRJIT_FEATURE_LZCNT);
  ASSERT_EQ(needs.result, GRJIT_OK);
  ASSERT_EQ(grjit_code_features(needs.code), GRJIT_FEATURE_LZCNT);
  struct Case {
    const char * running;
    GRJIT_Features r;
    bool usable;
  };
  const Case cases[] = {
      {"{LZCNT, BMI1}", GRJIT_FEATURE_LZCNT | GRJIT_FEATURE_BMI1, true},
      {"{LZCNT}", GRJIT_FEATURE_LZCNT, true},
      {"{}", GRJIT_FEATURES_NONE, false},
      {"{POPCNT}", GRJIT_FEATURE_POPCNT, false},
      {"{BMI1}", GRJIT_FEATURE_BMI1, false},
      {"all", GRJIT_FEATURES_ALL, true},
  };
  for (const Case & c : cases) {
    EXPECT_EQ(grjit_code_usable_with(needs.code, c.r), c.usable) << "U = {LZCNT}, R = " << c.running;
  }
  // U = {}: code compiled with no feature arm runs anywhere.
  Code plain(f.f, w, GRJIT_FEATURES_ALL);
  ASSERT_EQ(plain.result, GRJIT_OK);
  ASSERT_EQ(grjit_code_features(plain.code), GRJIT_FEATURES_NONE);
  for (GRJIT_Features r = 0; r <= GRJIT_FEATURES_ALL; r++) {
    EXPECT_TRUE(grjit_code_usable_with(plain.code, r)) << "U = {} is usable in R = " << set_name(r);
  }
  // Equality is not required: code that needs less runs where there is more.
  EXPECT_TRUE(grjit_code_usable_with(plain.code, GRJIT_FEATURES_ALL));
  EXPECT_FALSE(grjit_code_usable_with(nullptr, GRJIT_FEATURES_ALL));
  EXPECT_FALSE(grjit_code_usable_with(nullptr, GRJIT_FEATURES_NONE));
  // The rule is the subset rule and nothing else, for every pair.
  for (GRJIT_Features r = 0; r <= GRJIT_FEATURES_ALL; r++) {
    EXPECT_EQ(grjit_code_usable_with(needs.code, r), grjit_features_subset(GRJIT_FEATURE_LZCNT, r)) << r;
  }
}

namespace {

struct Worked {
  const char * instr;
  uint64_t arg;
  uint64_t want;
};

#define F64(x) 0x##x##ull

std::vector<Worked> worked_integers() {
  return {
      {"i32.clz", 0, 32}, {"i32.clz", 1, 31}, {"i32.clz", 0x10, 27}, {"i32.clz", 0x80000000ull, 0},
      {"i32.clz", 0xFFFFFFFFull, 0}, {"i32.clz", 0x100000000ull, 32}, {"i32.clz", 0xFFFFFFFF00000000ull, 32},
      {"i32.clz", 0xABCD000000000001ull, 31},
      {"i64.clz", 0, 64}, {"i64.clz", 1, 63}, {"i64.clz", 0x10, 59}, {"i64.clz", 1ull << 63, 0},
      {"i64.clz", 0x100000000ull, 31}, {"i64.clz", ~0ull, 0},
      {"i32.ctz", 0, 32}, {"i32.ctz", 1, 0}, {"i32.ctz", 0x10, 4}, {"i32.ctz", 0x80000000ull, 31},
      {"i32.ctz", 0xFFFFFFFF00000000ull, 32}, {"i32.ctz", 0x100000000ull, 32},
      {"i64.ctz", 0, 64}, {"i64.ctz", 0x10, 4}, {"i64.ctz", 1ull << 63, 63}, {"i64.ctz", 0x100000000ull, 32},
      {"i64.ctz", ~0ull, 0},
      {"i32.popcnt", 0, 0}, {"i32.popcnt", 0xFFFFFFFFull, 32}, {"i32.popcnt", 0xFFFFFFFFFFFFFFFFull, 32},
      {"i32.popcnt", 0x55555555ull, 16}, {"i32.popcnt", 0xFFFFFFFF00000000ull, 0},
      {"i64.popcnt", ~0ull, 64}, {"i64.popcnt", 0xFFFFFFFFull, 32}, {"i64.popcnt", 0x5555555555555555ull, 32},
      {"i64.popcnt", 0, 0}, {"i64.popcnt", 1ull << 63, 1},
  };
}

std::vector<Worked> worked_floats() {
  const uint64_t NZ = 0x8000000000000000ull, one = 0x3FF0000000000000ull, mone = 0xBFF0000000000000ull,
                 two = 0x4000000000000000ull, mtwo = 0xC000000000000000ull, three = 0x4008000000000000ull,
                 mthree = 0xC008000000000000ull, four = 0x4010000000000000ull;
  auto d = [](double v) { return floatfx::f64bits(v); };
  auto s = [](float v) { return floatfx::f32bits(v); };
  std::vector<Worked> w = {
      // ceil
      {"f64.ceil", d(0.25), one}, {"f64.ceil", d(-0.25), NZ}, {"f64.ceil", d(0.5), one},
      {"f64.ceil", d(-0.5), NZ}, {"f64.ceil", d(1.5), two}, {"f64.ceil", d(-1.5), mone},
      {"f64.ceil", d(2.5), three}, {"f64.ceil", d(-2.5), mtwo}, {"f64.ceil", 0, 0}, {"f64.ceil", NZ, NZ},
      {"f64.ceil", 0x7FF0000000000000ull, 0x7FF0000000000000ull},
      {"f64.ceil", 0xFFF0000000000000ull, 0xFFF0000000000000ull},
      // floor
      {"f64.floor", d(0.25), 0}, {"f64.floor", d(-0.25), mone}, {"f64.floor", d(0.5), 0},
      {"f64.floor", d(-0.5), mone}, {"f64.floor", d(1.5), one}, {"f64.floor", d(-1.5), mtwo},
      {"f64.floor", d(2.5), two}, {"f64.floor", d(-2.5), mthree}, {"f64.floor", NZ, NZ},
      // trunc
      {"f64.trunc", d(0.5), 0}, {"f64.trunc", d(-0.5), NZ}, {"f64.trunc", d(2.5), two},
      {"f64.trunc", d(-2.5), mtwo}, {"f64.trunc", d(1.5), one}, {"f64.trunc", d(-1.5), mone},
      {"f64.trunc", NZ, NZ},
      // nearest: ties to even
      {"f64.nearest", d(0.5), 0}, {"f64.nearest", d(-0.5), NZ}, {"f64.nearest", d(1.5), two},
      {"f64.nearest", d(2.5), two}, {"f64.nearest", d(3.5), four}, {"f64.nearest", d(-2.5), mtwo},
      {"f64.nearest", d(-1.5), mtwo}, {"f64.nearest", d(0.49999999999999994), 0},
      {"f64.nearest", d(-0.49999999999999994), NZ}, {"f64.nearest", NZ, NZ},
      // values at or above 2^52 are integers already
      {"f64.nearest", d(4503599627370497.0), d(4503599627370497.0)},
      {"f64.ceil", d(4503599627370497.0), d(4503599627370497.0)},
      {"f64.floor", d(-4503599627370497.0), d(-4503599627370497.0)},
      {"f64.trunc", d(9223372036854775808.0), d(9223372036854775808.0)},
      {"f64.ceil", d(4503599627370495.5), d(4503599627370496.0)},
      {"f64.floor", d(4503599627370495.5), d(4503599627370495.0)},
      // a NaN is quiet with its sign and payload kept, in every operation
      {"f64.ceil", 0x7FF0000000000001ull, 0x7FF8000000000001ull},
      {"f64.floor", 0xFFF0000000000001ull, 0xFFF8000000000001ull},
      {"f64.trunc", 0x7FF4000000000000ull, 0x7FFC000000000000ull},
      {"f64.nearest", 0x7FF0000000000001ull, 0x7FF8000000000001ull},
      {"f64.nearest", 0xFFF0000000000001ull, 0xFFF8000000000001ull},
      {"f64.nearest", 0x7FF800000000ABCDull, 0x7FF800000000ABCDull},
      {"f64.ceil", 0xFFF800000000ABCDull, 0xFFF800000000ABCDull},
      // the same at f32, and the upper half of the word is zero
      {"f32.ceil", s(0.25f), s(1.0f)}, {"f32.ceil", s(-0.25f), 0x80000000ull}, {"f32.ceil", s(-0.5f), 0x80000000ull},
      {"f32.ceil", s(2.5f), s(3.0f)}, {"f32.ceil", s(-2.5f), s(-2.0f)},
      {"f32.floor", s(0.25f), 0}, {"f32.floor", s(-0.25f), s(-1.0f)}, {"f32.floor", s(2.5f), s(2.0f)},
      {"f32.floor", s(-2.5f), s(-3.0f)},
      {"f32.trunc", s(-0.5f), 0x80000000ull}, {"f32.trunc", s(2.5f), s(2.0f)}, {"f32.trunc", s(-2.5f), s(-2.0f)},
      {"f32.nearest", s(0.5f), 0}, {"f32.nearest", s(-0.5f), 0x80000000ull}, {"f32.nearest", s(1.5f), s(2.0f)},
      {"f32.nearest", s(2.5f), s(2.0f)}, {"f32.nearest", s(3.5f), s(4.0f)}, {"f32.nearest", s(-2.5f), s(-2.0f)},
      {"f32.nearest", s(0.49999997f), 0}, {"f32.nearest", s(8388609.0f), s(8388609.0f)},
      {"f32.ceil", s(8388607.5f), s(8388608.0f)},
      {"f32.ceil", 0x7F800001ull, 0x7FC00001ull}, {"f32.floor", 0xFF800001ull, 0xFFC00001ull},
      {"f32.nearest", 0x7F800001ull, 0x7FC00001ull}, {"f32.trunc", 0x7FC0ABCDull, 0x7FC0ABCDull},
      {"f32.nearest", 0x7F800000ull, 0x7F800000ull}, {"f32.floor", 0xFF800000ull, 0xFF800000ull},
  };
  return w;
}

#undef F64

void expect_worked(const std::vector<Worked> & table, JitWorld & w, const std::vector<GRJIT_Features> & masks,
    size_t * ran) {
  for (GRJIT_Features mask : masks) {
    std::map<std::string, std::unique_ptr<Under>> built;
    for (const Worked & k : table) {
      auto & slot = built[k.instr];
      if (!slot) {
        slot.reset(new Under(k.instr, w, mask));
        ASSERT_EQ(slot->result, GRJIT_OK) << k.instr;
        ASSERT_EQ(grjit_code_features(slot->code), detected() & ~mask & need_of(k.instr)) << k.instr;
      }
      Outcome o = run(slot->code, w.ctx, {k.arg});
      ASSERT_EQ(o.fault, 0) << k.instr << " faulted under mask " << mask;
      ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
      EXPECT_EQ(o.out[0], k.want) << k.instr << " of 0x" << std::hex << k.arg << " under effective "
                                  << set_name(detected() & ~mask & GRJIT_FEATURES_ALL);
      if (slot->width() == 32) {
        EXPECT_EQ(o.out[0] >> 32, 0u) << k.instr << ": a 32-bit result is zero-extended";
      }
      (*ran)++;
    }
  }
}

} // namespace

TEST(CpuRun, TheWorkedAnswersOfTheIntegerOperationsAreTheSameUnderEverySubsetOfTheHostsFeatures) {
  CPU_ONLY_ON_X86_64_LINUX();
  CPU_SKIP_IF_CROSSED();
  JitWorld w;
  size_t ran = 0;
  expect_worked(worked_integers(), w, every_subset_masks(), &ran);
  EXPECT_GE(ran, worked_integers().size());
}

TEST(CpuRun, TheWorkedAnswersOfTheRoundingsAreTheSameUnderEverySubsetOfTheHostsFeatures) {
  CPU_ONLY_ON_X86_64_LINUX();
  CPU_SKIP_IF_CROSSED();
  JitWorld w;
  size_t ran = 0;
  expect_worked(worked_floats(), w, every_subset_masks(), &ran);
  EXPECT_GE(ran, worked_floats().size());
}

TEST(CpuRun, TheV1ArmGivesTheWorkedAnswersOnAHostThatHasTheFeaturesBecauseItsMaskedOff) {
  CPU_ONLY_ON_X86_64_LINUX();
  CPU_SKIP_IF_CROSSED();
  JitWorld w;
  // The fallback sequences are run on every x86-64 host through a mask of ALL; the record shows it.
  size_t ran = 0;
  expect_worked(worked_integers(), w, {GRJIT_FEATURES_ALL}, &ran);
  expect_worked(worked_floats(), w, {GRJIT_FEATURES_ALL}, &ran);
  EXPECT_EQ(ran, worked_integers().size() + worked_floats().size());
  Under u("i32.clz", w, GRJIT_FEATURES_ALL);
  ASSERT_EQ(u.result, GRJIT_OK);
  EXPECT_EQ(grjit_code_features(u.code), GRJIT_FEATURES_NONE);
}

TEST(CpuRun, EveryFeatureDependentOperationGivesTheEvaluatorsAnswersUnderEverySubsetAndEveryMaskAgrees) {
  CPU_ONLY_ON_X86_64_LINUX();
  CPU_SKIP_IF_CROSSED();
  JitWorld w;
  const std::vector<GRJIT_Features> masks = every_subset_masks();
  ASSERT_FALSE(masks.empty());
  size_t compared = 0, nans = 0;
  for (const std::string & instr : feature_ops()) {
    SCOPED_TRACE(instr);
    const bool is_float = instr[0] == 'f';
    const bool narrow = instr.find("32.") != std::string::npos;
    const std::vector<uint64_t> values = is_float ? float_boundaries(narrow) : integer_boundaries(narrow);
    // The first mask's results are what every other mask is compared with, and the evaluator is
    // compared with all of them.
    std::vector<uint64_t> first;
    for (size_t m = 0; m < masks.size(); m++) {
      Under u(instr, w, masks[m]);
      ASSERT_EQ(u.result, GRJIT_OK);
      EXPECT_EQ(grjit_code_features(u.code), detected() & ~masks[m] & need_of(instr));
      std::vector<uint64_t> got;
      for (uint64_t x : values) {
        Outcome o = run(u.code, w.ctx, {x});
        ASSERT_EQ(o.fault, 0) << hex_of(reinterpret_cast<const uint8_t *>(&x), 8);
        ASSERT_EQ(o.exit, uint32_t{GRJIT_EXIT_RETURNED});
        ireval::Result e = ireval::eval(u.function, w.ctx, nullptr, {x});
        ASSERT_EQ(e.exit, uint32_t{GRJIT_EXIT_RETURNED});
        const bool same = u.float_result() ? ireval::same_float(u.width(), o.out[0], e.out[0])
                                           : o.out[0] == e.out[0];
        EXPECT_TRUE(same) << std::hex << "0x" << x << " got 0x" << o.out[0] << " the evaluator 0x" << e.out[0]
                          << " under effective " << set_name(detected() & ~masks[m] & GRJIT_FEATURES_ALL);
        if (u.width() == 32) {
          EXPECT_EQ(o.out[0] >> 32, 0u) << "an F32 or I32 result has its upper half zero";
        }
        if (u.float_result() && ireval::is_nan_bits(u.width(), o.out[0])) {
          nans++;
        }
        got.push_back(o.out[0]);
        compared++;
      }
      if (m == 0) {
        first = got;
      } else {
        for (size_t i = 0; i < got.size(); i++) {
          const bool same = u.float_result() ? ireval::same_float(u.width(), got[i], first[i]) : got[i] == first[i];
          EXPECT_TRUE(same) << "mask " << masks[m] << " differs from mask " << masks[0] << " on 0x" << std::hex
                            << values[i];
        }
      }
    }
  }
  std::printf("feature-dependent operations: %zu runs compared with the evaluator across %zu masks, %zu NaN results\n",
      compared, masks.size(), nans);
  EXPECT_GT(compared, 1000u * masks.size());
  EXPECT_GT(nans, 5u * masks.size());
}

/* ---- A qemu CPU model (suite/tools/xarch/jit-x86-cpu.sh) ---------------------------------------- */

namespace {

// Maps emitted bytes executable and calls them, as the entry of a function with no poll and no hook.
struct Mapped {
  void * mapping = nullptr;
  size_t size = 0;
  void * unwind = nullptr;
  const GRCORE_PageProvider * pages;
  GRJIT_EntryFn entry = nullptr;
  Mapped(const Mapped &) = delete;
  Mapped & operator=(const Mapped &) = delete;
  Mapped(JitWorld & w, GRJIT_Function * f, GRJIT_Features features) : pages(w.pages()) {
    GRJIT_Emitted e;
    EXPECT_EQ(grjit_emit_for_features(GRJIT_ARCH_X86_64, f, grjit_allocator_default(), nullptr, nullptr, 0x40,
                  features, &e),
        GRJIT_OK);
    EXPECT_EQ(grjit_memory_create(pages, GRJIT_ARCH_X86_64, e.bytes, e.size, &e.prologue, &mapping, &size, &unwind),
        GRJIT_OK);
    grjit_emitted_free(&e);
    std::memcpy(&entry, &mapping, sizeof entry);
  }
  ~Mapped() {
    if (mapping != nullptr) {
      grjit_memory_destroy(pages, mapping, size, unwind);
    }
  }
  uint64_t call(void * ctx, uint64_t arg) const {
    uint64_t out[8] = {0xDEADBEEFu};
    uint64_t args[1] = {arg};
    EXPECT_EQ(entry(ctx, args, out), uint32_t{GRJIT_EXIT_RETURNED});
    return out[0];
  }
};

const char * env_or(const char * name, const char * dflt) {
  const char * v = std::getenv(name);
  return v != nullptr ? v : dflt;
}

} // namespace

TEST(CpuModel, TheDetectedSetIsTheOneTheRunExpectsAndTheNativeRunExpectsTheHostsOwn) {
  CPU_ONLY_ON_X86_64_LINUX();
  const char * model = std::getenv("GRJIT_TEST_CPU_MODEL");
  const char * expect = std::getenv("GRJIT_TEST_EXPECT_FEATURES");
  if (model == nullptr) {
    // Not under a model: the host's own set is expected, and the cpuinfo oracle above reads it.
    ASSERT_EQ(expect, nullptr) << "GRJIT_TEST_EXPECT_FEATURES is for a qemu model run (GRJIT_TEST_CPU_MODEL)";
    EXPECT_EQ(grjit_features_detected(), grjit_features_read_cpu());
    return;
  }
  ASSERT_NE(expect, nullptr) << "a model run names the features the model has in GRJIT_TEST_EXPECT_FEATURES";
  GRJIT_Features want = 0;
  ASSERT_EQ(grjit_features_from_stamp(expect, &want), GRJIT_OK) << "'" << expect << "' is not a feature stamp";
  std::printf("model %s: detected %s, expected %s\n", model, set_name(detected()).c_str(), set_name(want).c_str());
  EXPECT_EQ(detected(), want) << "model " << model << ": detected " << set_name(detected()) << ", expected "
                              << set_name(want);
  EXPECT_EQ(grjit_features_effective(0), want);
  EXPECT_EQ(grjit_features_effective(GRJIT_FEATURES_ALL), GRJIT_FEATURES_NONE);
  // A mask cannot name a feature the model lacks.
  for (GRJIT_Features mask : {GRJIT_Features{0}, GRJIT_FEATURES_ALL, GRJIT_Features{0xFFFFFFF0u}}) {
    EXPECT_TRUE(grjit_features_subset(grjit_features_effective(mask), want)) << mask;
  }
}

TEST(CpuModel, CodeCompiledForTheDetectedSetIsRightWhereTheModelsInstructionsBehaveAsItsBitsSay) {
  CPU_ONLY_ON_X86_64_LINUX();
  if (std::string(env_or("GRJIT_TEST_CPU_CROSSED", "0")) == "1") {
    GTEST_SKIP() << "model " << env_or("GRJIT_TEST_CPU_MODEL", "?")
                 << " crosses qemu's instruction gating and its CPUID bits (qemu 10.0.13): compiled code is not "
                    "asserted here, the detection and the forced-arm control are (documentation/design.md)";
  }
  JitWorld w;
  size_t ran = 0;
  expect_worked(worked_integers(), w, {GRJIT_FEATURES_NONE, GRJIT_FEATURES_ALL}, &ran);
  expect_worked(worked_floats(), w, {GRJIT_FEATURES_NONE, GRJIT_FEATURES_ALL}, &ran);
  // And the generated boundary values: the model's compile (default mask) against the evaluator.
  size_t compared = 0;
  for (const std::string & instr : feature_ops()) {
    Under u(instr, w, GRJIT_FEATURES_NONE);
    ASSERT_EQ(u.result, GRJIT_OK);
    EXPECT_EQ(grjit_code_features(u.code), detected() & need_of(instr)) << instr;
    const bool is_float = instr[0] == 'f';
    const bool narrow = instr.find("32.") != std::string::npos;
    for (uint64_t x : is_float ? float_boundaries(narrow) : integer_boundaries(narrow)) {
      Outcome o = run(u.code, w.ctx, {x});
      ASSERT_EQ(o.fault, 0) << instr;
      ireval::Result e = ireval::eval(u.function, w.ctx, nullptr, {x});
      EXPECT_TRUE(u.float_result() ? ireval::same_float(u.width(), o.out[0], e.out[0]) : o.out[0] == e.out[0])
          << instr << " of 0x" << std::hex << x;
      compared++;
    }
  }
  EXPECT_GT(compared, 1500u);
  std::printf("model run: %zu worked answers and %zu boundary runs right under detected set %s\n", ran, compared,
      set_name(detected()).c_str());
}

TEST(CpuModel, AnArmForcedOntoACpuRunsAsTheInstructionTheCpuHasAndALackingOneRunsAsTheBitScan) {
  CPU_ONLY_ON_X86_64_LINUX();
  // lzcnt and tzcnt do not fault where they are missing: they run as bsr and bsf. The arm is forced
  // through grjit_emit_for_features, which names the set, so the bytes are the feature's whatever the
  // host has. With GRJIT_TEST_LZCNT_AS / GRJIT_TEST_TZCNT_AS ("lzcnt"|"bsr", "tzcnt"|"bsf") a model run
  // says what the model's instruction was measured to do; with neither, the host's detected bit does.
  const std::string lz = env_or("GRJIT_TEST_LZCNT_AS", (detected() & GRJIT_FEATURE_LZCNT) != 0 ? "lzcnt" : "bsr");
  const std::string tz = env_or("GRJIT_TEST_TZCNT_AS", (detected() & GRJIT_FEATURE_BMI1) != 0 ? "tzcnt" : "bsf");
  JitWorld w;
  {
    floatfx::FloatOp op = floatfx::build_any_op("i64.clz");
    Fn f(op.function);
    Mapped forced(w, f.f, GRJIT_FEATURE_LZCNT);
    const uint64_t got = forced.call(w.ctx, 0x10);
    std::printf("forced lzcnt arm: clz(0x10) = %llu (the instruction ran as %s)\n",
        static_cast<unsigned long long>(got), lz.c_str());
    EXPECT_EQ(got, lz == "lzcnt" ? 59u : 4u) << "clz(0x10): 59 if lzcnt ran, 4 if the CPU ran it as bsr";
    Mapped v1(w, f.f, GRJIT_FEATURES_NONE);
    EXPECT_EQ(v1.call(w.ctx, 0x10), 59u) << "the v1 sequence is right on every CPU";
  }
  {
    floatfx::FloatOp op = floatfx::build_any_op("i32.ctz");
    Fn f(op.function);
    Mapped forced(w, f.f, GRJIT_FEATURE_BMI1);
    const uint64_t got = forced.call(w.ctx, 0);
    std::printf("forced tzcnt arm: ctz(0) = %llu (the instruction ran as %s)\n",
        static_cast<unsigned long long>(got), tz.c_str());
    if (tz == "tzcnt") {
      EXPECT_EQ(got, 32u) << "tzcnt of zero is the width";
    } else {
      EXPECT_NE(got, 32u) << "bsf of zero leaves its destination alone: not the width";
    }
    Mapped v1(w, f.f, GRJIT_FEATURES_NONE);
    EXPECT_EQ(v1.call(w.ctx, 0), 32u) << "the v1 sequence is right on every CPU";
  }
}

TEST(CpuModel, AForcedArmOfAFeatureTheHostHasRunsItsInstruction) {
  CPU_ONLY_ON_X86_64_LINUX();
  GRJIT_REQUIRE_FEATURE(GRJIT_FEATURE_SSE4_1);
  GRJIT_REQUIRE_FEATURE(GRJIT_FEATURE_POPCNT);
  JitWorld w;
  floatfx::FloatOp pop = floatfx::build_any_op("i64.popcnt");
  Fn pf(pop.function);
  Mapped p(w, pf.f, GRJIT_FEATURE_POPCNT);
  EXPECT_EQ(p.call(w.ctx, 0xF0F0F0F0F0F0F0F0ull), 32u);
  floatfx::FloatOp rnd = floatfx::build_any_op("f64.floor");
  Fn rf(rnd.function);
  Mapped r(w, rf.f, GRJIT_FEATURE_SSE4_1);
  EXPECT_EQ(r.call(w.ctx, floatfx::f64bits(-2.5)), floatfx::f64bits(-3.0));
}

/* ---- Nothing was skipped for a missing feature on a host that has them all ---------------------- */

TEST(CpuSkips, OnAHostWithEveryFeatureNothingWasSkippedForAMissingOne) {
  CPU_ONLY_ON_X86_64_LINUX();
  if (detected() != GRJIT_FEATURES_ALL) {
    GTEST_SKIP() << "this host's set is " << set_name(detected()) << ", not every feature, so a skip for a missing "
                 << "one is expected here (" << g_skipped_for_missing_feature.load() << " so far)";
  }
  EXPECT_EQ(g_skipped_for_missing_feature.load(), 0)
      << "a feature suite that skips on a host that has the features measures nothing";
}

GRJIT_TEST_MAIN()
