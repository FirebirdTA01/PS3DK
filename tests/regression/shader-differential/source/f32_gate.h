/* Float-mode (`@rt f32`) control gate and run verdict: pure logic, no RSX,
 * shared by the guest (main.c run_f32) and the host test f32-gate-test.c.
 *
 * Rules (codex review 2026-10-19 17:43): every control row is REQUIRED to
 * pass; ANY failed control - load, binding, readback, a negative control
 * that failed to detect its difference - latches the run INVALID for
 * good, and no later passing control can clear it.  A required control that
 * never ran before the first corpus row also invalidates the run.  Once
 * invalid, every corpus row is withheld.  Verdict: INVALID (exit 2) if the
 * gate is not clean, else FAIL (exit 1) on any corpus mismatch, else OK. */
#ifndef F32_GATE_H
#define F32_GATE_H

#include <stdint.h>
#include <string.h>

/* Keys 0..5 are the core float controls (by name); 6..10 are the binding
 * controls (by role), required too whenever a corpus row is judged, so a
 * qualification manifest cannot drop a positive or negative binding
 * control and still judge rows (codex 2026-10-19 17:51). */
#define F32_GATE_NREQ 11
static const char *const f32_gate_required[F32_GATE_NREQ] = {
	"f32-words", "f32-ulp", "f32-sub8", "f32-nan", "xy", "frames",
	"control-texture", "control-texture-neg", "control-uniform", "control-uniform-neg", "control-auto"
};

typedef struct {
	int invalid;                  /* latched: a control failed or was missing */
	int passed[F32_GATE_NREQ];    /* required controls seen passing */
	int checked;                  /* the required set was checked (first corpus row) */
	int corpus_mismatch;          /* corpus rows that judged mismatch */
	char first_failure[96];       /* the first control that invalidated the run */
} f32_gate;

static void f32_gate_init(f32_gate *g)
{
	memset(g, 0, sizeof(*g));
}

/* The required-control key a control row satisfies, or -1. */
static int f32_gate_key(const char *role, const char *name)
{
	if (strcmp(role, "control-f32-k") == 0) {
		for (int q = 0; q < 4; q++)
			if (strcmp(name, f32_gate_required[q]) == 0)
				return q;
		return -1;
	}
	if (strcmp(role, "control-f32-xy") == 0) return 4;
	if (strcmp(role, "control-f32-frames") == 0) return 5;
	for (int q = 6; q < F32_GATE_NREQ; q++)
		if (strcmp(role, f32_gate_required[q]) == 0)
			return q;
	return -1;
}

static void f32_gate_fail(f32_gate *g, const char *what)
{
	if (!g->invalid) {
		size_t n = strlen(what);
		if (n >= sizeof(g->first_failure)) n = sizeof(g->first_failure) - 1;
		memcpy(g->first_failure, what, n);
		g->first_failure[n] = 0;
	}
	g->invalid = 1;
}

/* Record a control row's outcome.  A pass never clears an earlier failure. */
static void f32_gate_control(f32_gate *g, const char *role, const char *name, int passed)
{
	if (!passed) {
		f32_gate_fail(g, name);
		return;
	}
	int k = f32_gate_key(role, name);
	if (k >= 0)
		g->passed[k] = 1;
}

#define F32_GATE_NCORE 6

static void f32_gate_require(f32_gate *g, int n)
{
	for (int q = 0; q < n; q++)
		if (!g->passed[q])
			f32_gate_fail(g, f32_gate_required[q]);
}

/* Called before each corpus row: 1 if it may be judged.  The first call
 * checks that EVERY required control, core and binding, ran and passed. */
static int f32_gate_corpus_allowed(f32_gate *g)
{
	if (!g->checked) {
		g->checked = 1;
		f32_gate_require(g, F32_GATE_NREQ);
	}
	return !g->invalid;
}

static void f32_gate_corpus_result(f32_gate *g, int mismatch)
{
	g->corpus_mismatch += mismatch ? 1 : 0;
}

/* End of run.  A controls-only manifest (no corpus row: a qualification of
 * the float path itself) needs the six core controls; the binding controls
 * are required once a corpus row is judged (checked above). */
static int f32_gate_exit(f32_gate *g)
{
	if (!g->checked)
		f32_gate_require(g, F32_GATE_NCORE);
	return g->invalid ? 2 : (g->corpus_mismatch ? 1 : 0);
}

static const char *f32_gate_verdict(int exit_code)
{
	return exit_code == 2 ? "SHADER_DIFF_INVALID" : exit_code == 1 ? "SHADER_DIFF_FAIL" : "SHADER_DIFF_OK";
}

/* ---- row refusal (codex review of f37d5f41) ----
 * Float mode judges colour 0 only, through the plain 2D binder and scalar
 * auto uniforms.  Applied to EVERY row - controls included - before any
 * special case: a row is refused (never drawn, never silently narrowed)
 * when either container declares an output other than COLOR0 or replaces
 * depth, when the row names its own vertex program, when it asks for
 * extra judged outputs (judge=), or when a container needs a binder kind
 * float mode has no proving set for (binder mask bits: RECT=1 CUBE=2
 * 3D=4 matrix=8).  Returns the refusal status, or NULL. */
static const char *f32_row_refusal(int color_mask_a, int color_mask_b, int depth_a, int depth_b,
                                   int has_vp, int judge_extra, unsigned binder_mask)
{
	if (judge_extra) return "f32-judge-unsupported";
	if (has_vp) return "f32-vp-unsupported";
	if (color_mask_a != 1 || color_mask_b != 1 || depth_a || depth_b) return "f32-outputs-unsupported";
	if (binder_mask) return "f32-binder-unsupported";
	return 0;
}

/* ---- pinned core control recipes ----
 * The four named core k controls carry FIXED words; the manifest's ka/kb
 * must equal them exactly, so an edited manifest (identical zero words,
 * no NaN, no ULP step) cannot make them pass without measuring anything.
 * A k control under any other name is not a core control. */
typedef struct { const char *name; uint32_t ka[4], kb[4]; } f32_k_recipe;
static const f32_k_recipe f32_k_recipes[4] = {
	{ "f32-words", { 0x3f000000u, 0xbfc00000u, 0x40490fdbu, 0x00000000u },
	               { 0x3f000000u, 0xbfc00000u, 0x40490fdbu, 0x00000000u } },
	{ "f32-ulp",   { 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f000000u },
	               { 0x3f000001u, 0x3f000000u, 0x3f000000u, 0x3f000000u } },
	{ "f32-sub8",  { 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f000000u },
	               { 0x3f004000u, 0x3f000000u, 0x3f000000u, 0x3f000000u } },
	{ "f32-nan",   { 0x7fc00000u, 0x3f800000u, 0x7fc00001u, 0xc0000000u },
	               { 0x7fc00000u, 0x3f800000u, 0x7fc00001u, 0xc0000000u } },
};

/* 1 if the row may run as a core control: a named core control must carry
 * its recipe's words exactly; any other k-control name passes through. */
static int f32_k_recipe_ok(const char *name, const uint32_t ka[4], const uint32_t kb[4])
{
	for (int i = 0; i < 4; i++)
		if (strcmp(name, f32_k_recipes[i].name) == 0)
			return memcmp(ka, f32_k_recipes[i].ka, sizeof(f32_k_recipes[i].ka)) == 0 &&
			       memcmp(kb, f32_k_recipes[i].kb, sizeof(f32_k_recipes[i].kb)) == 0;
	return 1;
}

#endif
