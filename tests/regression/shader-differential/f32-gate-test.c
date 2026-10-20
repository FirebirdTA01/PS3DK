/* Host test for the float-mode control gate (source/f32_gate.h), the
 * logic run_f32 uses for its global verdict.  Each case drives the gate the
 * way the guest does - control outcomes in manifest order, then corpus
 * rows - and asserts the exit code and the verdict word.  Built and run by
 * f32-gate-test.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "source/f32_gate.h"

static int g_fail;

typedef struct { const char *role, *name; int passed; } ctl;

static const ctl k_all_required[] = {
	{ "control-f32-k", "f32-words", 1 }, { "control-f32-k", "f32-ulp", 1 },
	{ "control-f32-k", "f32-sub8", 1 },  { "control-f32-k", "f32-nan", 1 },
	{ "control-f32-xy", "xy", 1 },       { "control-f32-frames", "frames", 1 },
	{ "control-texture", "texture_bind", 1 }, { "control-texture-neg", "texture_neg", 1 },
	{ "control-uniform", "uniform_apply", 1 }, { "control-uniform-neg", "uniform_neg", 1 },
	{ "control-auto", "auto_apply", 1 },
};
#define NALL ((int)(sizeof(k_all_required) / sizeof(k_all_required[0])))

/* Run: required controls (one optionally failed or left out), extra controls,
 * then `ncorpus` corpus rows of which `mismatches` mismatch. */
static int only_core;   /* run the six core controls only (a controls-only qualification) */

static void run_case(const char *what, int skip_req, int fail_req, const ctl *extra, int nextra,
                     int extra_before_required, int ncorpus, int mismatches,
                     int want_rc, int want_withheld)
{
	f32_gate g;
	f32_gate_init(&g);
	if (extra_before_required)
		for (int i = 0; i < nextra; i++)
			f32_gate_control(&g, extra[i].role, extra[i].name, extra[i].passed);
	for (int q = 0; q < (only_core ? 6 : NALL); q++) {
		if (q == skip_req) continue;
		f32_gate_control(&g, k_all_required[q].role, k_all_required[q].name, q == fail_req ? 0 : 1);
	}
	if (!extra_before_required)
		for (int i = 0; i < nextra; i++)
			f32_gate_control(&g, extra[i].role, extra[i].name, extra[i].passed);
	int withheld = 0;
	for (int r = 0; r < ncorpus; r++) {
		if (!f32_gate_corpus_allowed(&g)) { withheld++; continue; }
		f32_gate_corpus_result(&g, r < mismatches);
	}
	int rc = f32_gate_exit(&g);
	const char *v = f32_gate_verdict(rc);
	int ok = rc == want_rc && withheld == want_withheld &&
	         strcmp(v, want_rc == 2 ? "SHADER_DIFF_INVALID" : want_rc == 1 ? "SHADER_DIFF_FAIL" : "SHADER_DIFF_OK") == 0;
	printf("  %-62s %s (rc=%d %s withheld=%d%s%s)\n", what, ok ? "ok" : "FAIL", rc, v, withheld,
	       g.invalid ? " first=" : "", g.invalid ? g.first_failure : "");
	g_fail += !ok;
}

int main(void)
{
	static const ctl tex_ok[]   = { { "control-texture", "texture_bind", 1 } };
	static const ctl tex_bad[]  = { { "control-texture", "texture_bind", 0 } };
	static const ctl uni_bad[]  = { { "control-uniform", "uniform_apply", 0 } };
	static const ctl auto_bad[] = { { "control-auto", "auto_apply", 0 } };
	static const ctl neg_miss[] = { { "control-texture-neg", "texture_neg", 0 } };
	static const ctl uneg_miss[] = { { "control-uniform-neg", "uniform_neg", 0 } };
	/* a failure followed by a passing control of the same kind */
	static const ctl tex_bad_then_ok[] = { { "control-texture", "texture_bind", 0 },
	                                       { "control-texture", "texture_wrap", 1 } };
	static const ctl k_bad_then_ok[] = { { "control-f32-k", "f32-words", 0 } };

	run_case("all controls pass, no mismatch -> OK", -1, -1, tex_ok, 1, 0, 3, 0, 0, 0);
	run_case("all controls pass, one corpus mismatch -> FAIL", -1, -1, tex_ok, 1, 0, 3, 1, 1, 0);
	for (int q = 0; q < NALL; q++) {
		char w[110];
		snprintf(w, sizeof(w), "required control %s FAILS -> INVALID, corpus withheld", k_all_required[q].name);
		run_case(w, -1, q, NULL, 0, 0, 3, 0, 2, 3);
		snprintf(w, sizeof(w), "required control %s MISSING -> INVALID, corpus withheld", k_all_required[q].name);
		run_case(w, q, -1, NULL, 0, 0, 3, 0, 2, 3);
	}
	/* controls-only qualification (run1 shape): the six core controls suffice */
	only_core = 1;
	run_case("controls-only, six core controls pass, no binding controls -> OK", -1, -1, NULL, 0, 0, 0, 0, 0, 0);
	run_case("controls-only, a core control missing -> INVALID", 3, -1, NULL, 0, 0, 0, 0, 2, 0);
	run_case("core controls only but a corpus row follows -> INVALID, withheld", -1, -1, NULL, 0, 0, 2, 0, 2, 2);
	only_core = 0;
	run_case("control-texture fails (run2 shape) -> INVALID", -1, -1, tex_bad, 1, 0, 3, 0, 2, 3);
	run_case("control-uniform fails (run2 shape) -> INVALID", -1, -1, uni_bad, 1, 0, 3, 0, 2, 3);
	run_case("control-auto fails -> INVALID", -1, -1, auto_bad, 1, 0, 3, 0, 2, 3);
	run_case("texture negative control misses its difference -> INVALID", -1, -1, neg_miss, 1, 0, 3, 0, 2, 3);
	run_case("uniform negative control misses its difference -> INVALID", -1, -1, uneg_miss, 1, 0, 3, 0, 2, 3);
	run_case("texture fails, a later texture control passes -> still INVALID", -1, -1, tex_bad_then_ok, 2, 0, 3, 0, 2, 3);
	run_case("f32-words fails first, passes later in the run -> still INVALID", -1, -1, k_bad_then_ok, 1, 1, 3, 0, 2, 3);
	run_case("no corpus rows, all controls pass -> OK", -1, -1, NULL, 0, 0, 0, 0, 0, 0);
	run_case("no corpus rows, a required control missing -> INVALID", 2, -1, NULL, 0, 0, 0, 0, 2, 0);
	/* row refusal: every unsupported shape refused, the plain shape allowed */
	{
		struct { const char *what; int ca, cb, da, db, vp, judge; unsigned binder; const char *want; } rc[] = {
			{ "colour 0 only, plain 2D binder -> judged", 1, 1, 0, 0, 0, 0, 0, NULL },
			{ "side A declares COLOR1 -> refused", 3, 1, 0, 0, 0, 0, 0, "f32-outputs-unsupported" },
			{ "side B declares COLOR0..2 (run4 r06) -> refused", 1, 7, 0, 0, 0, 0, 0, "f32-outputs-unsupported" },
			{ "a side declares no output -> refused", 0, 1, 0, 0, 0, 0, 0, "f32-outputs-unsupported" },
			{ "side A replaces depth -> refused", 1, 1, 1, 0, 0, 0, 0, "f32-outputs-unsupported" },
			{ "row names its own vertex program -> refused", 1, 1, 0, 0, 1, 0, 0, "f32-vp-unsupported" },
			{ "row asks judge= extra outputs -> refused", 1, 1, 0, 0, 0, 1, 0, "f32-judge-unsupported" },
			{ "needs a RECT sampler -> refused", 1, 1, 0, 0, 0, 0, 1, "f32-binder-unsupported" },
			{ "needs a CUBE sampler -> refused", 1, 1, 0, 0, 0, 0, 2, "f32-binder-unsupported" },
			{ "needs a 3D sampler -> refused", 1, 1, 0, 0, 0, 0, 4, "f32-binder-unsupported" },
			{ "needs an automatic matrix -> refused", 1, 1, 0, 0, 0, 0, 8, "f32-binder-unsupported" },
		};
		for (unsigned i = 0; i < sizeof(rc) / sizeof(rc[0]); i++) {
			const char *got = f32_row_refusal(rc[i].ca, rc[i].cb, rc[i].da, rc[i].db, rc[i].vp, rc[i].judge, rc[i].binder);
			int ok = (got == NULL && rc[i].want == NULL) || (got && rc[i].want && strcmp(got, rc[i].want) == 0);
			printf("  %-62s %s (%s)\n", rc[i].what, ok ? "ok" : "FAIL", got ? got : "judged");
			g_fail += !ok;
		}
	}
	/* pinned core recipes: the exact words pass, every edited form fails */
	{
		const uint32_t half[4] = { 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f000000u };
		const uint32_t zero[4] = { 0, 0, 0, 0 };
		const uint32_t words[4] = { 0x3f000000u, 0xbfc00000u, 0x40490fdbu, 0x00000000u };
		const uint32_t ulp_b[4] = { 0x3f000001u, 0x3f000000u, 0x3f000000u, 0x3f000000u };
		const uint32_t sub8_b[4] = { 0x3f004000u, 0x3f000000u, 0x3f000000u, 0x3f000000u };
		const uint32_t nan_w[4] = { 0x7fc00000u, 0x3f800000u, 0x7fc00001u, 0xc0000000u };
		const uint32_t nan_gone[4] = { 0x3f800000u, 0x3f800000u, 0x3f800001u, 0xc0000000u };
		const uint32_t words_lane_swap[4] = { 0xbfc00000u, 0x3f000000u, 0x40490fdbu, 0x00000000u };
		struct { const char *what, *name; const uint32_t *ka, *kb; int want; } kc[] = {
			{ "f32-words exact recipe -> accepted", "f32-words", words, words, 1 },
			{ "f32-ulp exact recipe -> accepted", "f32-ulp", half, ulp_b, 1 },
			{ "f32-sub8 exact recipe -> accepted", "f32-sub8", half, sub8_b, 1 },
			{ "f32-nan exact recipe -> accepted", "f32-nan", nan_w, nan_w, 1 },
			{ "f32-words all-zero words (codex probe) -> rejected", "f32-words", zero, zero, 0 },
			{ "f32-ulp made an equality test (kb = ka) -> rejected", "f32-ulp", half, half, 0 },
			{ "f32-ulp all-zero words -> rejected", "f32-ulp", zero, zero, 0 },
			{ "f32-sub8 made an equality test -> rejected", "f32-sub8", half, half, 0 },
			{ "f32-sub8 given the ULP step instead -> rejected", "f32-sub8", half, ulp_b, 0 },
			{ "f32-nan with no NaN in it -> rejected", "f32-nan", nan_gone, nan_gone, 0 },
			{ "f32-nan all-zero words -> rejected", "f32-nan", zero, zero, 0 },
			{ "f32-words with two lanes swapped -> rejected", "f32-words", words_lane_swap, words_lane_swap, 0 },
			{ "f32-words with a and b swapped for the ulp pair -> rejected", "f32-words", half, ulp_b, 0 },
		};
		for (unsigned i = 0; i < sizeof(kc) / sizeof(kc[0]); i++) {
			int got = f32_k_recipe_ok(kc[i].name, kc[i].ka, kc[i].kb);
			int ok = got == kc[i].want;
			printf("  %-62s %s\n", kc[i].what, ok ? "ok" : "FAIL");
			g_fail += !ok;
		}
	}
	printf("f32-gate-test: %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
