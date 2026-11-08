// generate_red_data.js -> red_data.h  (clean named-field C structs)
// Regenerate with:  node generate_red_data.js <fixtures.json> <register-layout.json> <out.h>
// (immutable contract bundle, t_d0968566; see SOURCE-PINS.json)
const fs = require('fs');
const crypto = require('crypto');
const fxPath = process.argv[2];
const rlPath = process.argv[3];
const fx = JSON.parse(fs.readFileSync(fxPath, 'utf8'));
const rl = JSON.parse(fs.readFileSync(rlPath, 'utf8'));
const sha256 = p => crypto.createHash('sha256').update(fs.readFileSync(p)).digest('hex');
const FX_SHA = sha256(fxPath);
const RL_SHA = sha256(rlPath);
function fnv(p) {
  let h = 0xcbf29ce484222325n;
  for (const b of fs.readFileSync(p)) h = BigInt.asUintN(64, (h ^ BigInt(b)) * 0x100000001b3n);
  return '0x' + h.toString(16).padStart(16, '0') + 'ULL';
}
const present = x => x !== undefined && x !== null;
function thread(t) {
  if (!t) return '{ -1, 0 }';
  const kinds = {any: 'OP_ANY', all: 'OP_ALL', id: 'OP_ID'};
  if (!(t.kind in kinds)) throw Error('Unknown thread selector');
  return `{ ${kinds[t.kind]}, ${t.kind === 'id' ? '0x' + t.id + 'ULL' : '0'} }`;
}
const safe = s => s.replace(/[^a-zA-Z0-9_]/g, '_');
const W = [];

W.push('/* red_data.h - generated from fixtures.json + register-layout.json (contract 1). */');
W.push(`/* Provenance: SHA256 of the immutable inputs this header was generated from. */`);
W.push(`/*   fixtures.json       = ${FX_SHA} */`);
W.push(`/*   register-layout.json= ${RL_SHA} */`);
W.push(`/* Regenerate: node generate_red_data.js <fixtures.json> <register-layout.json> red_data.h */`);
W.push('#ifndef RED_DATA_H');
W.push('#define RED_DATA_H');
W.push(`#define RED_FIXTURES_SHA256   "${FX_SHA}"`);
W.push(`#define RED_REGISTRY_SHA256   "${RL_SHA}"`);
W.push('/* Startup accidental-mismatch guards, not cryptographic verification. */');
W.push(`#define RED_FIXTURES_FNV1A ${fnv(fxPath)}`);
W.push(`#define RED_REGISTRY_FNV1A ${fnv(rlPath)}`);
W.push(`#define RED_FIXTURES_LEN ${fs.statSync(fxPath).size}u`);
W.push(`#define RED_REGISTRY_LEN ${fs.statSync(rlPath).size}u`);
W.push('#include <stdbool.h>');
W.push('#include <stddef.h>');
W.push('#include <stdint.h>');
W.push('');
W.push('#define OP_RX 0\n#define OP_PUMP 1\n#define OP_AGAIN 2\n#define OP_EOF 3\n#define OP_TICK 4\n#define OP_STOP 5');
W.push('#define OP_ANY 0\n#define OP_ALL 1\n#define OP_ID 2');
W.push('typedef struct { int kind; uint64_t id; } selector_t;');
W.push('typedef struct { char action; selector_t thread; } action_t;');
W.push('');
W.push('typedef struct {');
W.push('  const char *op;');
W.push('  bool has_reg; uint32_t reg;');
W.push('  const char *data_hex;');
W.push('  bool has_addr; uint64_t addr;');
W.push('  uint32_t length; bool has_length;');
W.push('  int first; bool has_first;');
W.push('  char which;');
W.push('  bool has_insert; int insert; int type;');
W.push('  bool has_kind; uint64_t kind;');
W.push('  int thread_kind; uint64_t thread_id;');
W.push('  char action;');
W.push('  action_t actions[16]; int n_actions;');
W.push('  const char *status;');
W.push('  const char *res_data_hex;');
W.push('  uint32_t result_signal; bool result_has_thread; const char *result_thread;');
W.push('  const char *result_id;');
W.push('  uint64_t res_ids[16]; int n_res_ids;');
W.push('} call_expected_t;');
W.push('');
W.push('typedef struct { const char *id; const char *op; uint32_t capacity; const char *status; const char *input_hex; const char *expect_hex; } codec_case_t;');
W.push('');
W.push('typedef struct { char status; int32_t count; } wr_entry_t;');
W.push('');
W.push('typedef struct {');
W.push('  int op; const char *hex; const char *expect_tx;');
W.push('  const char *expect_state; const char *expect_api; const char *expect_reason;');
W.push('  uint32_t signal; bool has_stop; uint64_t stop_thread;');
W.push('  wr_entry_t wscript[16]; int n_wscript;');
W.push('  call_expected_t calls[8]; int n_calls;');
W.push('} step_t;');
W.push('');
W.push('typedef struct {');
W.push('  const char *id;');
W.push('  uint32_t wire, decoded, response_wire, max_actions, deadline_ms;');
W.push('  const char *enabled_ops[16]; int n_enabled;');
W.push('  step_t steps[64]; int n_steps;');
W.push('} scenario_t;');
W.push('');
W.push('typedef struct { uint32_t count, total_bytes, total_hex; uint32_t widths[96]; } layout_t;');
W.push('');
W.push(`static const layout_t red_layout = { ${rl.count}, ${rl.bytes}, ${rl.hexChars}, { ${rl.registers.map(r => (r.bitsize / 8) + 'u').join(', ')} } };`);
W.push('');
W.push(`static const codec_case_t red_codecs[${fx.codecs.length}] = {`);
for (const c of fx.codecs)
  W.push(`  { ${JSON.stringify(c.id)}, ${JSON.stringify(c.op)}, ${c.capacity}, ${JSON.stringify(c.expect_status)}, ${JSON.stringify(c.input_hex)}, ${JSON.stringify(c.expect_hex)} },`);
W.push(`};`);
W.push(`static const int red_n_codecs = ${fx.codecs.length};`);
W.push('');

// call -> C lines
function callC(c) {
  const a = c.args || {}, r = c.result || {};
  const f = [];
  f.push(`    { ${JSON.stringify(c.op)},`);
  f.push(`      ${'reg' in a ? 'true, ' + a.reg : 'false, 0'},`);
  f.push(`      ${a.data_hex ? JSON.stringify(a.data_hex) : '(const char*)0'},`);
  f.push(`      ${a.addr ? 'true, 0x' + a.addr + 'ULL' : 'false, 0'},`);
  f.push(`      ${'length' in a ? a.length + ', true' : '0, false'},`);
  f.push(`      ${'first' in a ? (a.first ? 1 : 0) + ', true' : '0, false'},`);
  f.push(`      ${'which' in a ? "'" + a.which + "'" : "' '"},`);
  const ins = 'insert' in a;
  f.push(`      ${ins ? 'true' : 'false'}, ${ins ? (a.insert ? 1 : 0) : 0}, ${a.type || 0},`);
  f.push(`      ${a.kind !== undefined ? 'true, 0x' + a.kind + 'ULL' : 'false, 0'},`);
  if (a.thread) {
    const tk = a.thread.kind === 'any' ? 'OP_ANY' : a.thread.kind === 'all' ? 'OP_ALL' : 'OP_ID';
    f.push(`      ${tk}, ${a.thread.id ? '0x' + a.thread.id + 'ULL' : '0'},`);
  } else f.push(`      -1, 0,`);
  f.push(`      ${'action' in a ? "'" + a.action + "'" : '0'},`);
  const actions = a.actions || [];
  if (actions.length > 16) throw Error('action capacity');
  f.push(`      { ${actions.length ? actions.map(x => `{ '${x.action}', ${thread(x.thread)} }`).join(', ') : '{0}'} }, ${actions.length},`);
  f.push(`      ${r.status ? JSON.stringify(r.status) : '(const char*)0'},`);
  f.push(`      ${r.data_hex ? JSON.stringify(r.data_hex) : '(const char*)0'},`);
  f.push(`      ${r.signal !== undefined ? r.signal : '0'}, ${present(r.thread) ? 'true' : 'false'}, ${present(r.thread) ? JSON.stringify(r.thread) : '(const char*)0'},`);
  f.push(`      ${r.id !== undefined ? JSON.stringify(r.id) : '(const char*)0'},`);
  if (Array.isArray(r.ids)) {
    if (r.ids.length > 16) throw Error('result id capacity');
    f.push(`      { ${r.ids.length ? r.ids.map(x => '0x' + x + 'ULL').join(', ') : '0'} }, ${r.ids.length} },`);
  } else f.push(`      { 0 }, 0 },`);
  return f.join('\n');
}

function emitScenario(sc) {
  const name = 'red_sc_' + safe(sc.id);
  const lim = Object.assign({ wire: fx.defaults.wire, decoded: fx.defaults.decoded, response_wire: fx.defaults.response_wire, max_actions: fx.defaults.max_actions, deadline_ms: fx.defaults.deadline_ms }, sc.limits || {});
  const o = [];
  o.push(`static const scenario_t ${name} = {`);
  o.push(`  ${JSON.stringify(sc.id)},`);
  o.push(`  ${lim.wire}, ${lim.decoded}, ${lim.response_wire}, ${lim.max_actions}, ${lim.deadline_ms},`);
  const en = sc.enabled || [];
  if (en.length > 16 || sc.steps.length > 64) throw Error('scenario capacity');
  o.push(`  { ${en.length ? en.map(x => JSON.stringify(x)).join(', ') : '0'} }, ${en.length},`);
  o.push(`  {`);
  for (const st of sc.steps) {
    let op = 'OP_RX';
    if (st.op === 'pump') op = 'OP_PUMP';
    else if (st.op === 'again') op = 'OP_AGAIN';
    else if (st.op === 'eof') op = 'OP_EOF';
    else if (st.op === 'expire') op = 'OP_TICK';
    else if (st.op === 'stop') op = 'OP_STOP';
     const calls = st.expect_calls || [];
    if (calls.length > 8 || (st.write_script || []).length > 16) throw Error('step capacity');
    const ws = st.write_script?.length ? `{ ${st.write_script.map(w => `{ '${w.status[0]}', ${w.count} }`).join(', ')} }` : `{ {0} }`;
    o.push(`    { ${op}, ` +
      `${st.hex !== undefined ? JSON.stringify(st.hex) : '(const char*)0'}, ` +
      `${JSON.stringify(st.expect_tx || '')}, ` +
      `${JSON.stringify(st.expect_state)}, ` +
      `${st.expect_api ? JSON.stringify(st.expect_api) : '(const char*)0'}, ` +
      `${st.expect_reason ? JSON.stringify(st.expect_reason) : '(const char*)0'}, ` +
      `${st.signal !== undefined ? st.signal : '0'}, ` +
      `${present(st.thread) ? 'true' : 'false'}, ` +
      `${present(st.thread) ? ('0x' + st.thread + 'ULL') : '0'}, ` +
      `      ${ws}, ${st.write_script ? st.write_script.length : 0}, ` +
      `      { ${calls.length ? calls.map(callC).join('\n') : '{0}'} }, ${calls.length} },`);
  }
  o.push(`  }, ${sc.steps.length} }`);
  o.push(`;`);
  return o.join('\n') + '\n';
}

for (const sc of fx.scenarios) W.push(emitScenario(sc));
W.push(`static const scenario_t *red_scenarios[${fx.scenarios.length}] = {`);
for (const sc of fx.scenarios) W.push(`  &red_sc_${safe(sc.id)},`);
W.push(`};`);
W.push(`static const int red_n_scenarios = ${fx.scenarios.length};`);
W.push('#endif /* RED_DATA_H */');

fs.writeFileSync(process.argv[4], W.join('\n'));
console.log('wrote', process.argv[4], fx.codecs.length, 'codecs', fx.scenarios.length, 'scenarios');
