/* Host test for the float-mode control gate (source/f32_gate.h), the
 * logic run_f32 uses for its global verdict.  Each case drives the gate the
 * way the guest does - control outcomes in manifest order, then corpus
 * rows - and asserts the exit code and the verdict word.  Built and run by
 * f32-gate-test.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "source/f32_gate.h"

/* Fault injection for f32_io.h: each stdio step can be made to fail. */
static int g_io_mode;   /* 0 ok, 1 open fails, 2 short write, 3 close fails */
static int g_dummy_file;
static FILE *inj_fopen(const char *p, const char *m) { (void)p; (void)m; return g_io_mode == 1 ? NULL : (FILE *)&g_dummy_file; }
static size_t inj_fwrite(const void *b, size_t s, size_t n, FILE *f) { (void)b; (void)f; return g_io_mode == 2 ? (n * s) / 2 : n * s; }
static int inj_fclose(FILE *f) { (void)f; return g_io_mode == 3 ? EOF : 0; }
#define F32_FOPEN  inj_fopen
#define F32_FWRITE inj_fwrite
#define F32_FCLOSE inj_fclose
#include "source/f32_io.h"

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
			const char *got = f32_row_refusal(rc[i].ca, rc[i].cb, rc[i].da, rc[i].db, rc[i].vp, rc[i].judge, rc[i].binder, 0, 0, 0);
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
	/* artifact writes: every stdio failure is reported (codex 20:12) */
	{
		static const char *what[4] = { "write ok -> 1", "open fails -> 0", "short write -> 0", "close fails -> 0" };
		char buf[64] = { 0 };
		for (int m = 0; m < 4; m++) {
			g_io_mode = m;
			int got = f32_write_file("x", buf, sizeof(buf));
			int ok = got == (m == 0);
			printf("  %-62s %s\n", what[m], ok ? "ok" : "FAIL");
			g_fail += !ok;
		}
		g_io_mode = 0;
	}
	/* a failed artifact write latches INVALID: on a control even if the
	 * control's own check passes afterwards, and on a corpus row whether it
	 * mismatched (still counted) or not */
	{
		struct { const char *what; int on_control, corpus_mismatch; } ac[] = {
			{ "artifact fails on a control that then passes -> INVALID", 1, 0 },
			{ "artifact fails on a MISMATCHING corpus row -> INVALID, counted", 0, 1 },
			{ "artifact fails on an identical corpus row -> INVALID", 0, 0 },
		};
		for (unsigned i = 0; i < sizeof(ac) / sizeof(ac[0]); i++) {
			f32_gate g;
			f32_gate_init(&g);
			for (int q = 0; q < NALL; q++) {
				if (ac[i].on_control && q == 6)
					f32_gate_artifact_failed(&g, k_all_required[q].name);
				f32_gate_control(&g, k_all_required[q].role, k_all_required[q].name, 1);
			}
			int withheld = 0;
			if (!ac[i].on_control) {
				if (f32_gate_corpus_allowed(&g)) {
					f32_gate_corpus_result(&g, ac[i].corpus_mismatch);
					f32_gate_artifact_failed(&g, "c001");
				} else {
					withheld++;
				}
			}
			int rc = f32_gate_exit(&g);
			int ok = rc == 2 && withheld == 0 && g.corpus_mismatch == ac[i].corpus_mismatch &&
			         strncmp(g.first_failure, "artifact-write:", 15) == 0;
			printf("  %-62s %s (rc=%d first=%s mismatches=%d)\n", ac[i].what, ok ? "ok" : "FAIL", rc,
			       g.first_failure, g.corpus_mismatch);
			g_fail += !ok;
		}
	}
	/* ---- matrix binder (design r3) ---- */
	{
		/* admission: plain float4x4/3x3 only, and only with the matrix set passed */
		struct { const char *what; unsigned binder; int adm, ready, mctl; const char *want; } mr[] = {
			{ "matrix row, admitted type, set passed -> judged", 8, 1, 1, 0, NULL },
			{ "matrix row, admitted type, set NOT passed -> unqualified", 8, 1, 0, 0, "f32-matrix-unqualified" },
			{ "matrix row, non-admitted type (half, 2xN, array) -> refused", 8, 0, 1, 0, "f32-binder-unsupported" },
			{ "matrix + RECT, set passed -> still refused", 9, 1, 1, 0, "f32-binder-unsupported" },
			{ "matrix + CUBE on a MATRIX CONTROL -> still refused", 10, 1, 1, 1, "f32-binder-unsupported" },
			{ "matrix control waives only its own matrix requirement", 8, 0, 0, 1, NULL },
		};
		for (unsigned i = 0; i < sizeof(mr) / sizeof(mr[0]); i++) {
			const char *got = f32_row_refusal(1, 1, 0, 0, 0, 0, mr[i].binder, mr[i].adm, mr[i].ready, mr[i].mctl);
			int ok = (got == NULL && mr[i].want == NULL) || (got && mr[i].want && strcmp(got, mr[i].want) == 0);
			printf("  %-62s %s (%s)\n", mr[i].what, ok ? "ok" : "FAIL", got ? got : "judged");
			g_fail += !ok;
		}
	}
	{
		/* membership: core + binding controls first, then the matrix members in variants */
		enum { ALL, MISSING, DUP_FOR_MISSING, WRONG, FAIL_THEN_PASS, EARLY, UNKNOWN, NVAR };
		static const char *what[NVAR] = {
			"complete matrix set -> ready",
			"one member missing -> not ready",
			"duplicate in place of a missing member -> not ready",
			"member under the wrong role -> INVALID",
			"a member fails, then passes -> INVALID",
			"matrix control before core/binding prerequisites -> INVALID",
			"unknown matrix member name -> INVALID",
		};
		for (int v = ALL; v < NVAR; v++) {
			f32_gate g;
			f32_gate_init(&g);
			f32_gate_request_matrix(&g);
			if (v == EARLY)
				f32_gate_matrix_control(&g, "control-binder-matrix-probe", "mprobe_44_r0", 1);
			for (int q = 0; q < NALL; q++)
				f32_gate_control(&g, k_all_required[q].role, k_all_required[q].name, 1);
			for (int i = 0; i < F32_MATRIX_NRECIPES; i++) {
				const f32_matrix_recipe *m = &f32_matrix_recipes[i];
				if ((v == MISSING || v == DUP_FOR_MISSING) && i == 3) continue;
				if (v == WRONG && i == 0) { f32_gate_matrix_control(&g, "control-binder-matrix-neg", m->name, 1); continue; }
				if (v == FAIL_THEN_PASS && i == 0) f32_gate_matrix_control(&g, m->role, m->name, 0);
				f32_gate_matrix_control(&g, m->role, m->name, 1);
				if (v == DUP_FOR_MISSING && i == 2) f32_gate_matrix_control(&g, m->role, m->name, 1);
			}
			for (int t = 0; t < F32_MATRIX_NTWINS; t++)
				f32_gate_matrix_control(&g, "control-binder-matrix", f32_matrix_twins[t].name, 1);
			if (v == UNKNOWN)
				f32_gate_matrix_control(&g, "control-binder-matrix-probe", "mprobe_22_r0", 1);
			int ready = f32_gate_matrix_ready(&g);
			int ok = v == ALL ? (ready && !g.invalid) :
			         (v == MISSING || v == DUP_FOR_MISSING) ? (!ready && !g.invalid) : (!ready && g.invalid);
			printf("  %-62s %s (ready=%d invalid=%d)\n", what[v], ok ? "ok" : "FAIL", ready, g.invalid);
			g_fail += !ok;
		}
	}
	{
		/* end to end (codex review of 4d7576d4): a manifest with matrix controls
		 * requires the whole set, before the first corpus row and at the end */
		enum { QUAL_FULL, QUAL_INCOMPLETE, MISSING_BEFORE_CORPUS, CORPUS_THEN_SET, LATE_COMPLETION,
		       NO_MATRIX_CONTROLS, NSCEN };
		static const char *what[NSCEN] = {
			"matrix qualification, complete, controls only -> OK",
			"matrix qualification, one member missing, controls only -> INVALID",
			"member missing at the first corpus row -> INVALID, withheld",
			"matrix corpus row before the set, set added after -> INVALID",
			"missing member passes after a corpus row -> stays INVALID",
			"no matrix control: matrix row refused, run stays valid",
		};
		for (int v = QUAL_FULL; v < NSCEN; v++) {
			f32_gate g;
			f32_gate_init(&g);
			if (v != NO_MATRIX_CONTROLS)
				f32_gate_request_matrix(&g);
			for (int q = 0; q < NALL; q++)
				f32_gate_control(&g, k_all_required[q].role, k_all_required[q].name, 1);
			int allowed = -1, withheld = 0;
			const char *refusal = "-";
			if (v == CORPUS_THEN_SET) {
				allowed = f32_gate_corpus_allowed(&g);
				withheld += !allowed;
			}
			for (int m = 0; v != NO_MATRIX_CONTROLS && m < F32_MATRIX_NMEMBERS; m++) {
				if ((v == QUAL_INCOMPLETE || v == MISSING_BEFORE_CORPUS || v == LATE_COMPLETION) && m == 21)
					continue;
				if (m < F32_MATRIX_NRECIPES)
					f32_gate_matrix_control(&g, f32_matrix_recipes[m].role, f32_matrix_recipes[m].name, 1);
				else
					f32_gate_matrix_control(&g, "control-binder-matrix", f32_matrix_twins[m - F32_MATRIX_NRECIPES].name, 1);
			}
			if (v == MISSING_BEFORE_CORPUS || v == LATE_COMPLETION || v == NO_MATRIX_CONTROLS) {
				allowed = f32_gate_corpus_allowed(&g);
				withheld += !allowed;
				if (allowed) {
					const char *rf = f32_row_refusal(1, 1, 0, 0, 0, 0, 8, 1, f32_gate_matrix_ready(&g), 0);
					refusal = rf ? rf : "judged";
				}
			}
			if (v == LATE_COMPLETION) {
				f32_gate_matrix_control(&g, "control-binder-matrix", f32_matrix_twins[1].name, 1);
				withheld += !f32_gate_corpus_allowed(&g);
			}
			int rc = f32_gate_exit(&g);
			int ok;
			switch (v) {
			case QUAL_FULL: ok = rc == 0 && f32_gate_matrix_ready(&g); break;
			case QUAL_INCOMPLETE: ok = rc == 2 && strncmp(g.first_failure, "matrix-member-missing:", 22) == 0; break;
			case MISSING_BEFORE_CORPUS: ok = rc == 2 && withheld == 1; break;
			case CORPUS_THEN_SET: ok = rc == 2 && withheld == 1 && !f32_gate_matrix_ready(&g); break;
			case LATE_COMPLETION: ok = rc == 2 && withheld == 2 && !f32_gate_matrix_ready(&g); break;
			default: ok = rc == 0 && allowed == 1 && strcmp(refusal, "f32-matrix-unqualified") == 0; break;
			}
			printf("  %-62s %s (rc=%d withheld=%d refusal=%s first=%s)\n", what[v], ok ? "ok" : "FAIL", rc, withheld,
			       refusal, g.invalid ? g.first_failure : "-");
			g_fail += !ok;
		}
	}
	{
		/* each member is required on its own: leave any ONE out of a controls-only
		 * qualification and the run is INVALID, naming that member */
		int named = 0;
		for (int miss = 0; miss < F32_MATRIX_NMEMBERS; miss++) {
			f32_gate g;
			f32_gate_init(&g);
			f32_gate_request_matrix(&g);
			for (int q = 0; q < NALL; q++)
				f32_gate_control(&g, k_all_required[q].role, k_all_required[q].name, 1);
			for (int m = 0; m < F32_MATRIX_NMEMBERS; m++)
				if (m != miss)
					f32_gate_matrix_control(&g, m < F32_MATRIX_NRECIPES ? f32_matrix_recipes[m].role : "control-binder-matrix",
					                        m < F32_MATRIX_NRECIPES ? f32_matrix_recipes[m].name
					                                                : f32_matrix_twins[m - F32_MATRIX_NRECIPES].name, 1);
			char want[96];
			snprintf(want, sizeof(want), "matrix-member-missing:%s",
			         miss < F32_MATRIX_NRECIPES ? f32_matrix_recipes[miss].name
			                                    : f32_matrix_twins[miss - F32_MATRIX_NRECIPES].name);
			named += f32_gate_exit(&g) == 2 && strcmp(g.first_failure, want) == 0;
		}
		int ok = named == F32_MATRIX_NMEMBERS;
		printf("  %-62s %s (%d/%d)\n", "any one of the 24 members missing -> INVALID, named", ok ? "ok" : "FAIL",
		       named, F32_MATRIX_NMEMBERS);
		g_fail += !ok;
	}
	{
		/* a matrix control may draw only once every prerequisite passed, before any corpus row */
		enum { ALL_PASSED, TEXTURE_MISSING, UNIFORM_FAILED, AFTER_CORPUS, NOT_REQUESTED, NPRE };
		static const char *what[NPRE] = {
			"prerequisites passed -> matrix control may draw",
			"control-texture never ran -> no matrix draw",
			"control-uniform failed -> no matrix draw",
			"after the first corpus row -> no matrix draw",
			"matrix qualification not requested -> no matrix draw",
		};
		for (int v = ALL_PASSED; v < NPRE; v++) {
			f32_gate g;
			f32_gate_init(&g);
			if (v != NOT_REQUESTED)
				f32_gate_request_matrix(&g);
			for (int q = 0; q < NALL; q++) {
				if (v == TEXTURE_MISSING && strcmp(k_all_required[q].role, "control-texture") == 0) continue;
				f32_gate_control(&g, k_all_required[q].role, k_all_required[q].name,
				                 !(v == UNIFORM_FAILED && strcmp(k_all_required[q].role, "control-uniform") == 0));
			}
			if (v == AFTER_CORPUS) {
				/* a complete set, a corpus row judged (run still valid), then one more matrix row */
				for (int m = 0; m < F32_MATRIX_NMEMBERS; m++)
					f32_gate_matrix_control(&g, m < F32_MATRIX_NRECIPES ? f32_matrix_recipes[m].role : "control-binder-matrix",
					                        m < F32_MATRIX_NRECIPES ? f32_matrix_recipes[m].name
					                                                : f32_matrix_twins[m - F32_MATRIX_NRECIPES].name, 1);
				if (!f32_gate_corpus_allowed(&g) || g.invalid) {
					printf("  %-62s FAIL (setup: corpus row not allowed)\n", what[v]);
					g_fail++;
					continue;
				}
			}
			int may = f32_gate_matrix_prereq_ok(&g);
			int ok = v == ALL_PASSED ? may : !may;
			printf("  %-62s %s (may_draw=%d)\n", what[v], ok ? "ok" : "FAIL", may);
			g_fail += !ok;
		}
	}
	{
		/* twin recipes: shape, both containers, auto uniforms, A reads u_matrix, B bakes */
		const f32_twin_facts good = { F32_M33, 1, 0, 0, 0 };
		f32_twin_facts f;
		struct { const char *what, *name, *set, *a, *b; int edit; const char *want; } tc[] = {
			{ "twin with its recipe -> may draw", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 0, NULL },
			{ "twin A=A (the auto program on both sides) -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_a.fpo", 1, "matrix-twin-path" },
			{ "twin A=A under the pinned paths -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 1, "matrix-twin-same-container" },
			{ "twin B=B under the pinned paths -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 2, "matrix-twin-same-container" },
			{ "twin sides swapped (paths) -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_b.fpo", "controls/binder_matrix_0_a.fpo", 3, "matrix-twin-path" },
			{ "twin sides swapped (contents) -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 3, "matrix-twin-a-reflection" },
			{ "twin with the wrong shape (4x4 for binder_matrix_0) -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 4, "matrix-twin-a-reflection" },
			{ "twin with uniform set 0 -> refused", "binder_matrix_0", "0",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 0, "matrix-twin-uniform-set" },
			{ "twin whose A never reads a row -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 5, "matrix-twin-a-reflection" },
			{ "twin whose A has another uniform -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 6, "matrix-twin-a-reflection" },
			{ "twin whose B declares a uniform -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 7, "matrix-twin-b-not-baked" },
			{ "another twin's containers -> refused", "binder_matrix_0", "auto",
			  "controls/binder_matrix_1_a.fpo", "controls/binder_matrix_1_b.fpo", 0, "matrix-twin-path" },
			{ "unknown twin name -> refused", "binder_matrix_4", "auto",
			  "controls/binder_matrix_0_a.fpo", "controls/binder_matrix_0_b.fpo", 0, "matrix-twin-unknown" },
		};
		for (unsigned i = 0; i < sizeof(tc) / sizeof(tc[0]); i++) {
			f = good;
			switch (tc[i].edit) {
			case 1: f.same_bytes = 1; f.b_uniforms = 1; break;                  /* A on both sides */
			case 2: f.same_bytes = 1; f.a_shape = -1; f.a_rows_referenced = 0; break;   /* B on both sides */
			case 3: f.a_shape = -1; f.a_rows_referenced = 0; f.b_uniforms = 1; break;  /* swapped */
			case 4: f.a_shape = F32_M44; break;
			case 5: f.a_rows_referenced = 0; break;
			case 6: f.a_other_uniforms = 1; break;
			case 7: f.b_uniforms = 1; break;
			}
			const char *got = f32_matrix_twin_refusal(tc[i].name, tc[i].set, tc[i].a, tc[i].b, &f);
			int ok = (got == NULL && tc[i].want == NULL) || (got && tc[i].want && strcmp(got, tc[i].want) == 0);
			printf("  %-62s %s (%s)\n", tc[i].what, ok ? "ok" : "FAIL", got ? got : "may draw");
			g_fail += !ok;
		}
		/* every twin's pinned shape is the one its staged programs declare */
		int shapes_ok = f32_matrix_twins[0].shape == F32_M33 && f32_matrix_twins[1].shape == F32_M44 &&
		                f32_matrix_twins[2].shape == F32_M34 && f32_matrix_twins[3].shape == F32_M43;
		printf("  %-62s %s\n", "twin shapes 3x3, 4x4, 3x4, 4x3", shapes_ok ? "ok" : "FAIL");
		g_fail += !shapes_ok;
	}
	{
		/* recipe fields: the pinned words pass; any edit is refused */
		const f32_matrix_recipe *p = f32_matrix_recipe_find("control-binder-matrix-probe", "mprobe_44_r1");
		const f32_matrix_recipe *u = f32_matrix_recipe_find("control-binder-matrix-neg", "mneg_44_ulp_1_2");
		const f32_matrix_recipe *k = f32_matrix_recipe_find("control-binder-matrix-neg", "mneg_44_skip_r1");
		uint32_t e[4], n[4], z[4] = { 0, 0, 0, 0 };
		struct { const char *what; int got, want; } fc[8];
		int nf = 0;
		fc[nf].what = "probe with its pinned words -> accepted";
		fc[nf].got = f32_matrix_fields_ok(p->role, p->name, 1, p->expect, 0, z); fc[nf++].want = 1;
		memcpy(e, p->expect, sizeof(e)); e[1] ^= 1;
		fc[nf].what = "probe with one word altered -> refused";
		fc[nf].got = f32_matrix_fields_ok(p->role, p->name, 1, e, 0, z); fc[nf++].want = 0;
		memcpy(n, f32_matrix_normal_words(u), sizeof(n));
		fc[nf].what = "ULP negative with pinned faulty + normal words -> accepted";
		fc[nf].got = f32_matrix_fields_ok(u->role, u->name, 1, u->expect, 1, n); fc[nf++].want = 1;
		fc[nf].what = "ULP negative expecting the NORMAL words -> refused";
		fc[nf].got = f32_matrix_fields_ok(u->role, u->name, 1, n, 1, n); fc[nf++].want = 0;
		fc[nf].what = "negative without normal= -> refused";
		fc[nf].got = f32_matrix_fields_ok(u->role, u->name, 1, u->expect, 0, z); fc[nf++].want = 0;
		fc[nf].what = "probe name under the negative role -> refused";
		fc[nf].got = f32_matrix_fields_ok("control-binder-matrix-neg", p->name, 1, p->expect, 1, n); fc[nf++].want = 0;
		fc[nf].what = "matrix fields on a corpus row -> refused";
		fc[nf].got = f32_matrix_fields_ok("corpus", p->name, 1, p->expect, 0, z); fc[nf++].want = 0;
		memcpy(n, f32_matrix_normal_words(k), sizeof(n));
		fc[nf].what = "skip-row negative expecting the normal words -> refused";
		fc[nf].got = f32_matrix_fields_ok(k->role, k->name, 1, n, 1, n); fc[nf++].want = 0;
		for (int i = 0; i < nf; i++) {
			int ok = fc[i].got == fc[i].want;
			printf("  %-62s %s\n", fc[i].what, ok ? "ok" : "FAIL");
			g_fail += !ok;
		}
		/* only the three matrix roles may carry matrix fields at all */
		int roles_ok = f32_is_matrix_role("control-binder-matrix-probe") && f32_is_matrix_role("control-binder-matrix-neg") &&
		               f32_is_matrix_role("control-binder-matrix") && !f32_is_matrix_role("corpus") &&
		               !f32_is_matrix_role("control-texture") && !f32_is_matrix_role("control-f32-k");
		printf("  %-62s %s\n", "matrix roles: the three matrix roles only", roles_ok ? "ok" : "FAIL");
		g_fail += !roles_ok;
	}
	printf("f32-gate-test: %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
