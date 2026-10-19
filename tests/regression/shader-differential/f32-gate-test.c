/* Host test for the float-mode control gate (source/f32_gate.h), the
 * logic run_f32 uses for its global verdict.  Each case drives the gate the
 * way the guest does - control outcomes in manifest order, then corpus
 * rows - and asserts the exit code and the verdict word.  Built and run by
 * f32-gate-test.sh. */
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
	printf("f32-gate-test: %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
