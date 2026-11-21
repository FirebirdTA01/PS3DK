/* red_data.h - generated from fixtures.json + register-layout.json (contract 1). */
/* Provenance: SHA256 of the immutable inputs this header was generated from. */
/*   fixtures.json       = 5857b302c72452b93d26992a734fb5dbdcd5b97c0d394e63daa63e9d190de686 */
/*   register-layout.json= ad650ca786df3b0250323da88ed514462b6734ec7ddf0af7d08db9a95e870617 */
/* Regenerate: node generate_red_data.js <fixtures.json> <register-layout.json> red_data.h */
#ifndef RED_DATA_H
#define RED_DATA_H
#define RED_FIXTURES_SHA256   "5857b302c72452b93d26992a734fb5dbdcd5b97c0d394e63daa63e9d190de686"
#define RED_REGISTRY_SHA256   "ad650ca786df3b0250323da88ed514462b6734ec7ddf0af7d08db9a95e870617"
/* Startup accidental-mismatch guards, not cryptographic verification. */
#define RED_FIXTURES_FNV1A 0xffbf613bfd56f47eULL
#define RED_REGISTRY_FNV1A 0xd8f12bed70baf9e2ULL
#define RED_FIXTURES_LEN 37439u
#define RED_REGISTRY_LEN 5298u
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OP_RX 0
#define OP_PUMP 1
#define OP_AGAIN 2
#define OP_EOF 3
#define OP_TICK 4
#define OP_STOP 5
#define OP_ANY 0
#define OP_ALL 1
#define OP_ID 2
typedef struct { int kind; uint64_t id; } selector_t;
typedef struct { char action; selector_t thread; } action_t;

typedef struct {
  const char *op;
  bool has_reg; uint32_t reg;
  const char *data_hex;
  bool has_addr; uint64_t addr;
  uint32_t length; bool has_length;
  int first; bool has_first;
  char which;
  bool has_insert; int insert; int type;
  bool has_kind; uint64_t kind;
  int thread_kind; uint64_t thread_id;
  char action;
  action_t actions[16]; int n_actions;
  const char *status;
  const char *res_data_hex;
  uint32_t result_signal; bool result_has_thread; const char *result_thread;
  const char *result_id;
  uint64_t res_ids[16]; int n_res_ids;
} call_expected_t;

typedef struct { const char *id; const char *op; uint32_t capacity; const char *status; const char *input_hex; const char *expect_hex; } codec_case_t;

typedef struct { char status; int32_t count; } wr_entry_t;

typedef struct {
  int op; const char *hex; const char *expect_tx;
  const char *expect_state; const char *expect_api; const char *expect_reason;
  uint32_t signal; bool has_stop; uint64_t stop_thread;
  wr_entry_t wscript[16]; int n_wscript;
  call_expected_t calls[8]; int n_calls;
} step_t;

typedef struct {
  const char *id;
  uint32_t wire, decoded, response_wire, max_actions, deadline_ms;
  const char *enabled_ops[16]; int n_enabled;
  step_t steps[64]; int n_steps;
} scenario_t;

typedef struct { uint32_t count, total_bytes, total_hex; uint32_t widths[96]; } layout_t;

static const layout_t red_layout = { 71, 556, 1112, { 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 8u, 4u, 8u, 8u, 4u, 4u } };

static const codec_case_t red_codecs[7] = {
  { "escape-reserved", "encode", 2048, "ok", "24237D2A", "247D047D037D5D7D0A233632" },
  { "binary-high-byte", "encode", 2048, "ok", "FF", "24FF234646" },
  { "response-expansion-overflow", "encode", 11, "limit", "24237D2A", "" },
  { "decode-escaped", "decode", 2048, "ok", "247D047D037D5D7D0A233632", "24237D2A" },
  { "decode-RLE", "decode", 2048, "ok", "24302A20233741", "30303030" },
  { "decode-RLE-overflow", "decode", 3, "limit", "24302A20233741", "" },
  { "decode-wrong-basis", "decode", 2048, "frame", "247D047D03233437", "" },
};
static const int red_n_codecs = 7;

static const scenario_t red_sc_supported_features = {
  "supported-features",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2471537570706F727465643A6D756C746970726F636573732B3B5153746172744E6F41636B4D6F64652B234443", "2B245061636B657453697A653D383030234338", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_supported_bare = {
  "supported-bare",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2471537570706F72746564233337", "2B245061636B657453697A653D383030234338", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_supported_configured = {
  "supported-configured",
  256, 512, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2471537570706F72746564233337", "2B245061636B657453697A653D313030234331", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_unsupported_qSupportedExtra = {
  "unsupported-qSupportedExtra",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2471537570706F727465644578747261233342", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_unsupported_QStartNoAckMode = {
  "unsupported-QStartNoAckMode",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "245153746172744E6F41636B4D6F6465234230", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_unsupported_QNonStop = {
  "unsupported-QNonStop",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "24514E6F6E53746F703A31233844", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_unsupported_qXfer = {
  "unsupported-qXfer",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2471586665723A6C69627261726965733A726561643A3A302C3230233035", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_unsupported_Ht8 = {
  "unsupported-Ht8",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "24487438234634", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_unsupported_Z1_1000_4 = {
  "unsupported-Z1,1000,4",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "245A312C313030302C34234438", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_missing_stop = {
  "missing-stop",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "243F233346", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_actual_stop = {
  "actual-stop",
  2048, 2048, 2048, 16, 1000,
  { "stop_reason" }, 1,
  {
    { OP_RX, "243F233346", "2B245430357468726561643A383B234445", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "stop_reason",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      5, true, "8",
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_stop_backend_error = {
  "stop-backend-error",
  2048, 2048, 2048, 16, 1000,
  { "stop_reason" }, 1,
  {
    { OP_RX, "243F233346", "2B24453035234141", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "stop_reason",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "error",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_g_full = {
  "g-full",
  2048, 2048, 2048, 16, 1000,
  { "read_regs" }, 1,
  {
    { OP_RX, "2467233637", "2B243030303030303030303030303030303130303030303030303030303030303032303030303030303030303030303030333030303030303030303030303030303430303030303030303030303030303035303030303030303030303030303030363030303030303030303030303030303730303030303030303030303030303038303030303030303030303030303030393030303030303030303030303030304130303030303030303030303030303042303030303030303030303030303030433030303030303030303030303030304430303030303030303030303030303045303030303030303030303030303030463030303030303030303030303030313030303030303030303030303030303131303030303030303030303030303031323030303030303030303030303030313330303030303030303030303030303134303030303030303030303030303031353030303030303030303030303030313630303030303030303030303030303137303030303030303030303030303031383030303030303030303030303030313930303030303030303030303030303141303030303030303030303030303031423030303030303030303030303030314330303030303030303030303030303144303030303030303030303030303031453030303030303030303030303030314630303030303030303030303030303230303030303030303030303030303032313030303030303030303030303030323230303030303030303030303030303233303030303030303030303030303032343030303030303030303030303030323530303030303030303030303030303236303030303030303030303030303032373030303030303030303030303030323830303030303030303030303030303239303030303030303030303030303032413030303030303030303030303030324230303030303030303030303030303243303030303030303030303030303032443030303030303030303030303030324530303030303030303030303030303246303030303030303030303030303033303030303030303030303030303030333130303030303030303030303030303332303030303030303030303030303033333030303030303030303030303030333430303030303030303030303030303335303030303030303030303030303033363030303030303030303030303030333730303030303030303030303030303338303030303030303030303030303033393030303030303030303030303030334130303030303030303030303030303342303030303030303030303030303033433030303030303030303030303030334430303030303030303030303030303345303030303030303030303030303033463030303030303030303030303030343030303030303030303030303030303431303030303030303030303030303034323030303030303433303030303030303030303030303034343030303030303030303030303030343530303030303034363030303030303437234134", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_regs",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "000000000000000100000000000000020000000000000003000000000000000400000000000000050000000000000006000000000000000700000000000000080000000000000009000000000000000A000000000000000B000000000000000C000000000000000D000000000000000E000000000000000F0000000000000010000000000000001100000000000000120000000000000013000000000000001400000000000000150000000000000016000000000000001700000000000000180000000000000019000000000000001A000000000000001B000000000000001C000000000000001D000000000000001E000000000000001F0000000000000020000000000000002100000000000000220000000000000023000000000000002400000000000000250000000000000026000000000000002700000000000000280000000000000029000000000000002A000000000000002B000000000000002C000000000000002D000000000000002E000000000000002F0000000000000030000000000000003100000000000000320000000000000033000000000000003400000000000000350000000000000036000000000000003700000000000000380000000000000039000000000000003A000000000000003B000000000000003C000000000000003D000000000000003E000000000000003F00000000000000400000000000000041000000000000004200000043000000000000004400000000000000450000004600000047",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_G_full = {
  "G-full",
  2048, 2048, 2048, 16, 1000,
  { "write_regs" }, 1,
  {
    { OP_RX, "24473030303030303030303030303030303130303030303030303030303030303032303030303030303030303030303030333030303030303030303030303030303430303030303030303030303030303035303030303030303030303030303030363030303030303030303030303030303730303030303030303030303030303038303030303030303030303030303030393030303030303030303030303030304130303030303030303030303030303042303030303030303030303030303030433030303030303030303030303030304430303030303030303030303030303045303030303030303030303030303030463030303030303030303030303030313030303030303030303030303030303131303030303030303030303030303031323030303030303030303030303030313330303030303030303030303030303134303030303030303030303030303031353030303030303030303030303030313630303030303030303030303030303137303030303030303030303030303031383030303030303030303030303030313930303030303030303030303030303141303030303030303030303030303031423030303030303030303030303030314330303030303030303030303030303144303030303030303030303030303031453030303030303030303030303030314630303030303030303030303030303230303030303030303030303030303032313030303030303030303030303030323230303030303030303030303030303233303030303030303030303030303032343030303030303030303030303030323530303030303030303030303030303236303030303030303030303030303032373030303030303030303030303030323830303030303030303030303030303239303030303030303030303030303032413030303030303030303030303030324230303030303030303030303030303243303030303030303030303030303032443030303030303030303030303030324530303030303030303030303030303246303030303030303030303030303033303030303030303030303030303030333130303030303030303030303030303332303030303030303030303030303033333030303030303030303030303030333430303030303030303030303030303335303030303030303030303030303033363030303030303030303030303030333730303030303030303030303030303338303030303030303030303030303033393030303030303030303030303030334130303030303030303030303030303342303030303030303030303030303033433030303030303030303030303030334430303030303030303030303030303345303030303030303030303030303033463030303030303030303030303030343030303030303030303030303030303431303030303030303030303030303034323030303030303433303030303030303030303030303034343030303030303030303030303030343530303030303034363030303030303437234542", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "write_regs",
      false, 0,
      "000000000000000100000000000000020000000000000003000000000000000400000000000000050000000000000006000000000000000700000000000000080000000000000009000000000000000A000000000000000B000000000000000C000000000000000D000000000000000E000000000000000F0000000000000010000000000000001100000000000000120000000000000013000000000000001400000000000000150000000000000016000000000000001700000000000000180000000000000019000000000000001A000000000000001B000000000000001C000000000000001D000000000000001E000000000000001F0000000000000020000000000000002100000000000000220000000000000023000000000000002400000000000000250000000000000026000000000000002700000000000000280000000000000029000000000000002A000000000000002B000000000000002C000000000000002D000000000000002E000000000000002F0000000000000030000000000000003100000000000000320000000000000033000000000000003400000000000000350000000000000036000000000000003700000000000000380000000000000039000000000000003A000000000000003B000000000000003C000000000000003D000000000000003E000000000000003F00000000000000400000000000000041000000000000004200000043000000000000004400000000000000450000004600000047",
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_p_general = {
  "p-general",
  2048, 2048, 2048, 16, 1000,
  { "read_reg" }, 1,
  {
    { OP_RX, "247031234131", "2B2430303030303030303030303030303031233031", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_reg",
      true, 1,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "0000000000000001",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_p_special = {
  "p-special",
  2048, 2048, 2048, 16, 1000,
  { "read_reg" }, 1,
  {
    { OP_RX, "24703432234436", "2B243030303030303433233837", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_reg",
      true, 66,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "00000043",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_P_general = {
  "P-general",
  2048, 2048, 2048, 16, 1000,
  { "write_reg" }, 1,
  {
    { OP_RX, "2450303D30303030303030303030303030303031234245", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "write_reg",
      true, 0,
      "0000000000000001",
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_P_special = {
  "P-special",
  2048, 2048, 2048, 16, 1000,
  { "write_reg" }, 1,
  {
    { OP_RX, "245034323D3030303030303433233741", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "write_reg",
      true, 66,
      "00000043",
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_memory_four = {
  "memory-four",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D3130302C34233545", "2B244445414442454546233230", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_mem",
      false, 0,
      (const char*)0,
      true, 0x100ULL,
      4, true,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "DEADBEEF",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_memory_one = {
  "memory-one",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D31303066662C31233237", "2B244445233839", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_mem",
      false, 0,
      (const char*)0,
      true, 0x100FFULL,
      1, true,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "DE",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_memory_zero = {
  "memory-zero",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D3130302C30233541", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_mem",
      false, 0,
      (const char*)0,
      true, 0x100ULL,
      0, true,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_memory_write = {
  "memory-write",
  2048, 2048, 2048, 16, 1000,
  { "write_mem" }, 1,
  {
    { OP_RX, "244D3130302C343A6465616462656566233938", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "write_mem",
      false, 0,
      "DEADBEEF",
      true, 0x100ULL,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_backend_short_read = {
  "backend-short-read",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D3130302C34233545", "2B24453035234141", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_mem",
      false, 0,
      (const char*)0,
      true, 0x100ULL,
      4, true,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "DEAD",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_backend_wrong_reg_width = {
  "backend-wrong-reg-width",
  2048, 2048, 2048, 16, 1000,
  { "read_reg" }, 1,
  {
    { OP_RX, "24703432234436", "2B24453035234141", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_reg",
      true, 66,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "0000000000000043",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_backend_oversize_thread_list = {
  "backend-oversize-thread-list",
  2048, 2048, 16, 16, 1000,
  { "threads" }, 1,
  {
    { OP_RX, "247166546872656164496E666F234242", "2B24453035234141", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "threads",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      1, true,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0x123456789ABCDEFULL, 0x23456789ABCDEF0ULL }, 2 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_thread_enumeration = {
  "thread-enumeration",
  2048, 2048, 2048, 16, 1000,
  { "threads" }, 1,
  {
    { OP_RX, "247166546872656164496E666F234242", "2B246D312C38233032", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "threads",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      1, true,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0x1ULL, 0x8ULL }, 2 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "247173546872656164496E666F234338", "2B246C233643", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "threads",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, true,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 4 }
;

static const scenario_t red_sc_current_thread = {
  "current-thread",
  2048, 2048, 2048, 16, 1000,
  { "current_thread" }, 1,
  {
    { OP_RX, "247143234234", "2B24514338234343", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "current_thread",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      "8",
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_select_g0 = {
  "select-g0",
  2048, 2048, 2048, 16, 1000,
  { "select_thread" }, 1,
  {
    { OP_RX, "24486730234446", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "select_thread",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      'g',
      false, 0, 0,
      false, 0,
      OP_ANY, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_select_c_1 = {
  "select-c-1",
  2048, 2048, 2048, 16, 1000,
  { "select_thread" }, 1,
  {
    { OP_RX, "2448632D31233039", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "select_thread",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      'c',
      false, 0, 0,
      false, 0,
      OP_ALL, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_select_c8 = {
  "select-c8",
  2048, 2048, 2048, 16, 1000,
  { "select_thread" }, 1,
  {
    { OP_RX, "24486338234533", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "select_thread",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      'c',
      false, 0, 0,
      false, 0,
      OP_ID, 0x8ULL,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_break_Z = {
  "break-Z",
  2048, 2048, 2048, 16, 1000,
  { "breakpoint" }, 1,
  {
    { OP_RX, "245A302C313030302C34234437", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "breakpoint",
      false, 0,
      (const char*)0,
      true, 0x1000ULL,
      0, false,
      0, false,
      ' ',
      true, 1, 0,
      true, 0x4ULL,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_break_z = {
  "break-z",
  2048, 2048, 2048, 16, 1000,
  { "breakpoint" }, 1,
  {
    { OP_RX, "247A302C313030302C34234637", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "breakpoint",
      false, 0,
      (const char*)0,
      true, 0x1000ULL,
      0, false,
      0, false,
      ' ',
      true, 0, 0,
      true, 0x4ULL,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_vcont_capabilities = {
  "vcont-capabilities",
  2048, 2048, 2048, 16, 1000,
  { "vcont" }, 1,
  {
    { OP_RX, "2476436F6E743F233439", "2B2476436F6E743B633B73233536", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_vcont_missing_capabilities = {
  "vcont-missing-capabilities",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2476436F6E743F233439", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_continue = {
  "continue",
  2048, 2048, 2048, 16, 1000,
  { "resume" }, 1,
  {
    { OP_RX, "2463233633", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "resume",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      'c',
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2463233633", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_STOP, (const char*)0, "24533035234238", "WAIT_ACK", (const char*)0, (const char*)0, 5, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2D", "24533035234238", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 5 }
;

static const scenario_t red_sc_step_address_zero = {
  "step-address-zero",
  2048, 2048, 2048, 16, 1000,
  { "resume" }, 1,
  {
    { OP_RX, "247330234133", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "resume",
      false, 0,
      (const char*)0,
      true, 0x0ULL,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      's',
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "247330234133", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_STOP, (const char*)0, "24533035234238", "WAIT_ACK", (const char*)0, (const char*)0, 5, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2D", "24533035234238", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 5 }
;

static const scenario_t red_sc_vcont_action_list = {
  "vcont-action-list",
  2048, 2048, 2048, 16, 1000,
  { "vcont" }, 1,
  {
    { OP_RX, "2476436F6E743B733A383B63234338", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "vcont",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { { 's', { OP_ID, 0x8ULL } }, { 'c', { OP_ALL, 0 } } }, 2,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2476436F6E743B733A383B63234338", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_STOP, (const char*)0, "24533035234238", "WAIT_ACK", (const char*)0, (const char*)0, 5, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2D", "24533035234238", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 5 }
;

static const scenario_t red_sc_unsupported_resume = {
  "unsupported-resume",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2463233633", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_resume_error = {
  "resume-error",
  2048, 2048, 2048, 16, 1000,
  { "resume" }, 1,
  {
    { OP_RX, "2463233633", "2B24453035234141", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "resume",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      'c',
      { {0} }, 0,
      "error",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_vcont_unknown_action = {
  "vcont-unknown-action",
  2048, 2048, 2048, 16, 1000,
  { "vcont" }, 1,
  {
    { OP_RX, "2476436F6E743B633A383B74234339", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_vcont_signal_action_not_in_scope = {
  "vcont-signal-action-not-in-scope",
  2048, 2048, 2048, 16, 1000,
  { "vcont" }, 1,
  {
    { OP_RX, "2476436F6E743B433035234544", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_unknown_break_extension = {
  "unknown-break-extension",
  2048, 2048, 2048, 16, 1000,
  { "breakpoint" }, 1,
  {
    { OP_RX, "245A302C313030302C343B58312C3030233237", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_0 = {
  "invalid-0",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D2C31234341", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_1 = {
  "invalid-1",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D313030234645", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_2 = {
  "invalid-2",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D472C31233131", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_3 = {
  "invalid-3",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D31303030303030303030303030303030302C31234642", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_4 = {
  "invalid-4",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D464646464646464646464646464646462C31233241", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_5 = {
  "invalid-5",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D302C3130303030303030303030303030303030234641", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_6 = {
  "invalid-6",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D302C343030233544", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_7 = {
  "invalid-7",
  2048, 2048, 2048, 16, 1000,
  { "write_mem" }, 1,
  {
    { OP_RX, "244D3130302C323A4445234646", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_8 = {
  "invalid-8",
  2048, 2048, 2048, 16, 1000,
  { "write_mem" }, 1,
  {
    { OP_RX, "244D3130302C313A4747233033", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_9 = {
  "invalid-9",
  2048, 2048, 2048, 16, 1000,
  { "read_reg" }, 1,
  {
    { OP_RX, "24703437234442", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_10 = {
  "invalid-10",
  2048, 2048, 2048, 16, 1000,
  { "write_reg" }, 1,
  {
    { OP_RX, "2450302C30303030303030303030303030303031234144", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_11 = {
  "invalid-11",
  2048, 2048, 2048, 16, 1000,
  { "write_reg" }, 1,
  {
    { OP_RX, "2450303D3030303030303031233345", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_12 = {
  "invalid-12",
  2048, 2048, 2048, 16, 1000,
  { "write_regs" }, 1,
  {
    { OP_RX, "24473030234137", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_13 = {
  "invalid-13",
  2048, 2048, 2048, 16, 1000,
  { "resume" }, 1,
  {
    { OP_RX, "24633B313030233246", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_14 = {
  "invalid-14",
  2048, 2048, 2048, 16, 1000,
  { "select_thread" }, 1,
  {
    { OP_RX, "2448632D32233041", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_15 = {
  "invalid-15",
  2048, 2048, 2048, 16, 1000,
  { "vcont" }, 1,
  {
    { OP_RX, "2476436F6E743B233435", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_16 = {
  "invalid-16",
  2048, 2048, 2048, 16, 1000,
  { "vcont" }, 1,
  {
    { OP_RX, "2476436F6E743B733A383B633A233032", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_17 = {
  "invalid-17",
  2048, 2048, 2048, 16, 1000,
  { "breakpoint" }, 1,
  {
    { OP_RX, "245A302C313030302C342C38233342", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_too_many_actions = {
  "too-many-actions",
  2048, 2048, 2048, 16, 1000,
  { "vcont" }, 1,
  {
    { OP_RX, "2476436F6E743B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A383B633A38233141", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_read_at_response_boundary = {
  "read-at-response-boundary",
  2048, 2048, 16, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D302C36234646", "2B24303030313032303330343035233446", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_mem",
      false, 0,
      (const char*)0,
      true, 0x0ULL,
      6, true,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "000102030405",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_read_over_response_boundary = {
  "read-over-response-boundary",
  2048, 2048, 16, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D302C37233030", "2B24453031234136", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_write_retry_no_side_effect = {
  "write-retry-no-side-effect",
  2048, 2048, 2048, 16, 1000,
  { "write_mem" }, 1,
  {
    { OP_RX, "244D3130302C313A4142234638", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "write_mem",
      false, 0,
      "AB",
      true, 0x100ULL,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2D", "244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "244D3130302C313A4142234638", "2B244F4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 4 }
;

static const scenario_t red_sc_detach_ack = {
  "detach-ack",
  2048, 2048, 2048, 16, 1000,
  { "detach" }, 1,
  {
    { OP_RX, "2444233434", "2B244F4B233941", "DETACH_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "detach",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2D", "244F4B233941", "DETACH_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2444233434", "2B244F4B233941", "DETACH_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "CLOSED", (const char*)0, "detached", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 4 }
;

static const scenario_t red_sc_detach_absent = {
  "detach-absent",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2444233434", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_detach_fails = {
  "detach-fails",
  2048, 2048, 2048, 16, 1000,
  { "detach" }, 1,
  {
    { OP_RX, "2444233434", "2B24453035234141", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "detach",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "error",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_detach_timeout = {
  "detach-timeout",
  2048, 2048, 2048, 16, 1000,
  { "detach" }, 1,
  {
    { OP_RX, "2444233434", "2B244F4B233941", "DETACH_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "detach",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_TICK, (const char*)0, "", "CLOSED", (const char*)0, "timeout", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_kill_ok = {
  "kill-ok",
  2048, 2048, 2048, 16, 1000,
  { "kill" }, 1,
  {
    { OP_RX, "246B233642", "2B", "CLOSED", (const char*)0, "kill_ok", 0, false, 0,       { {0} }, 0,       {     { "kill",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
  }, 1 }
;

static const scenario_t red_sc_kill_error = {
  "kill-error",
  2048, 2048, 2048, 16, 1000,
  { "kill" }, 1,
  {
    { OP_RX, "246B233642", "2B", "CLOSED", (const char*)0, "kill_error", 0, false, 0,       { {0} }, 0,       {     { "kill",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "error",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
  }, 1 }
;

static const scenario_t red_sc_kill_absent = {
  "kill-absent",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "246B233642", "2B", "CLOSED", (const char*)0, "kill_absent", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t red_sc_byte_at_a_time = {
  "byte-at-a-time",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "24", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "6D", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "31", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "30", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "30", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2C", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "31", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "23", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "35", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "42", "2B244142233833", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_mem",
      false, 0,
      (const char*)0,
      true, 0x100ULL,
      1, true,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "AB",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 11 }
;

static const scenario_t red_sc_bad_checksum_resync = {
  "bad-checksum-resync",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467233030247146616B65234538", "2D2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_bad_checksum_digits = {
  "bad-checksum-digits",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467237A7A", "2D", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t red_sc_raw_byte_escape_request = {
  "raw-byte-escape-request",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "24717D047D037D5D7D0A234433", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_RLE_request = {
  "RLE-request",
  2048, 2048, 2048, 16, 1000,
  { "read_mem" }, 1,
  {
    { OP_RX, "246D302A202C31233434", "2B244141233832", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "read_mem",
      false, 0,
      (const char*)0,
      true, 0x0ULL,
      1, true,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      "AA",
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_RLE_no_predecessor = {
  "RLE-no-predecessor",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "242A20233441", "2D", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t red_sc_RLE_invalid_count = {
  "RLE-invalid-count",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "24302A08233632", "2D", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t red_sc_wire_overflow = {
  "wire-overflow",
  8, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "24713132333435363738233135", "2D", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t red_sc_decoded_overflow = {
  "decoded-overflow",
  2048, 8, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "24302A7E234438", "2D", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t red_sc_incomplete_frame_eof = {
  "incomplete-frame-eof",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_EOF, (const char*)0, "", "CLOSED", (const char*)0, "eof", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_partial_frame_again = {
  "partial-frame-again",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_AGAIN, (const char*)0, "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_AGAIN, (const char*)0, "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "233637", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 5 }
;

static const scenario_t red_sc_frame_deadline = {
  "frame-deadline",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_AGAIN, (const char*)0, "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_TICK, (const char*)0, "", "CLOSED", (const char*)0, "timeout", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 3 }
;

static const scenario_t red_sc_running_eof = {
  "running-eof",
  2048, 2048, 2048, 16, 1000,
  { "resume" }, 1,
  {
    { OP_RX, "2463233633", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "resume",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      'c',
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_EOF, (const char*)0, "", "CLOSED", (const char*)0, "eof", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_running_deadline = {
  "running-deadline",
  2048, 2048, 2048, 16, 1000,
  { "resume" }, 1,
  {
    { OP_RX, "2463233633", "2B", "RUNNING", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       {     { "resume",
      false, 0,
      (const char*)0,
      false, 0,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      'c',
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_TICK, (const char*)0, "", "CLOSED", (const char*)0, "timeout", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_different_command_before_ack = {
  "different-command-before-ack",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467233637", "2B24233030", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "247031234131", "", "CLOSED", (const char*)0, "protocol", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_stop_without_resume = {
  "stop-without-resume",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_STOP, (const char*)0, "", "READY", "bad_state", (const char*)0, 5, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t red_sc_partial_write_again_progress = {
  "partial-write-again-progress",
  2048, 2048, 2048, 16, 1000,
  { "write_mem" }, 1,
  {
    { OP_RX, "244D3130302C313A4142234638", "", "TX", (const char*)0, (const char*)0, 0, false, 0,       { { 'a', 0 } }, 1,       { {0} }, 0 },
    { OP_PUMP, (const char*)0, "", "TX", (const char*)0, (const char*)0, 0, false, 0,       { { 'a', 0 } }, 1,       { {0} }, 0 },
    { OP_PUMP, (const char*)0, "2B244F", "TX", (const char*)0, (const char*)0, 0, false, 0,       { { 'o', 1 }, { 'o', 2 }, { 'a', 0 } }, 3,       {     { "write_mem",
      false, 0,
      "AB",
      true, 0x100ULL,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_PUMP, (const char*)0, "", "TX", (const char*)0, (const char*)0, 0, false, 0,       { { 'a', 0 } }, 1,       { {0} }, 0 },
    { OP_PUMP, (const char*)0, "4B233941", "WAIT_ACK", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
    { OP_RX, "2B", "", "READY", (const char*)0, (const char*)0, 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 6 }
;

static const scenario_t red_sc_partial_output_error = {
  "partial-output-error",
  2048, 2048, 2048, 16, 1000,
  { "write_mem" }, 1,
  {
    { OP_RX, "244D3130302C313A4142234638", "2B24", "TX", (const char*)0, (const char*)0, 0, false, 0,       { { 'o', 1 }, { 'o', 1 }, { 'a', 0 } }, 3,       {     { "write_mem",
      false, 0,
      "AB",
      true, 0x100ULL,
      0, false,
      0, false,
      ' ',
      false, 0, 0,
      false, 0,
      -1, 0,
      0,
      { {0} }, 0,
      "ok",
      (const char*)0,
      0, false, (const char*)0,
      (const char*)0,
      { 0 }, 0 }, }, 1 },
    { OP_PUMP, (const char*)0, "", "CLOSED", (const char*)0, "io", 0, false, 0,       { { 'e', 0 } }, 1,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_partial_output_deadline = {
  "partial-output-deadline",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467233637", "2B24", "TX", (const char*)0, (const char*)0, 0, false, 0,       { { 'o', 1 }, { 'o', 1 }, { 'a', 0 } }, 3,       { {0} }, 0 },
    { OP_TICK, (const char*)0, "", "CLOSED", (const char*)0, "timeout", 0, false, 0,       { {0} }, 0,       { {0} }, 0 },
  }, 2 }
;

static const scenario_t red_sc_invalid_success_zero_progress = {
  "invalid-success-zero-progress",
  2048, 2048, 2048, 16, 1000,
  { 0 }, 0,
  {
    { OP_RX, "2467233637", "", "CLOSED", (const char*)0, "io_contract", 0, false, 0,       { { 'o', 0 } }, 1,       { {0} }, 0 },
  }, 1 }
;

static const scenario_t *red_scenarios[91] = {
  &red_sc_supported_features,
  &red_sc_supported_bare,
  &red_sc_supported_configured,
  &red_sc_unsupported_qSupportedExtra,
  &red_sc_unsupported_QStartNoAckMode,
  &red_sc_unsupported_QNonStop,
  &red_sc_unsupported_qXfer,
  &red_sc_unsupported_Ht8,
  &red_sc_unsupported_Z1_1000_4,
  &red_sc_missing_stop,
  &red_sc_actual_stop,
  &red_sc_stop_backend_error,
  &red_sc_g_full,
  &red_sc_G_full,
  &red_sc_p_general,
  &red_sc_p_special,
  &red_sc_P_general,
  &red_sc_P_special,
  &red_sc_memory_four,
  &red_sc_memory_one,
  &red_sc_memory_zero,
  &red_sc_memory_write,
  &red_sc_backend_short_read,
  &red_sc_backend_wrong_reg_width,
  &red_sc_backend_oversize_thread_list,
  &red_sc_thread_enumeration,
  &red_sc_current_thread,
  &red_sc_select_g0,
  &red_sc_select_c_1,
  &red_sc_select_c8,
  &red_sc_break_Z,
  &red_sc_break_z,
  &red_sc_vcont_capabilities,
  &red_sc_vcont_missing_capabilities,
  &red_sc_continue,
  &red_sc_step_address_zero,
  &red_sc_vcont_action_list,
  &red_sc_unsupported_resume,
  &red_sc_resume_error,
  &red_sc_vcont_unknown_action,
  &red_sc_vcont_signal_action_not_in_scope,
  &red_sc_unknown_break_extension,
  &red_sc_invalid_0,
  &red_sc_invalid_1,
  &red_sc_invalid_2,
  &red_sc_invalid_3,
  &red_sc_invalid_4,
  &red_sc_invalid_5,
  &red_sc_invalid_6,
  &red_sc_invalid_7,
  &red_sc_invalid_8,
  &red_sc_invalid_9,
  &red_sc_invalid_10,
  &red_sc_invalid_11,
  &red_sc_invalid_12,
  &red_sc_invalid_13,
  &red_sc_invalid_14,
  &red_sc_invalid_15,
  &red_sc_invalid_16,
  &red_sc_invalid_17,
  &red_sc_too_many_actions,
  &red_sc_read_at_response_boundary,
  &red_sc_read_over_response_boundary,
  &red_sc_write_retry_no_side_effect,
  &red_sc_detach_ack,
  &red_sc_detach_absent,
  &red_sc_detach_fails,
  &red_sc_detach_timeout,
  &red_sc_kill_ok,
  &red_sc_kill_error,
  &red_sc_kill_absent,
  &red_sc_byte_at_a_time,
  &red_sc_bad_checksum_resync,
  &red_sc_bad_checksum_digits,
  &red_sc_raw_byte_escape_request,
  &red_sc_RLE_request,
  &red_sc_RLE_no_predecessor,
  &red_sc_RLE_invalid_count,
  &red_sc_wire_overflow,
  &red_sc_decoded_overflow,
  &red_sc_incomplete_frame_eof,
  &red_sc_partial_frame_again,
  &red_sc_frame_deadline,
  &red_sc_running_eof,
  &red_sc_running_deadline,
  &red_sc_different_command_before_ack,
  &red_sc_stop_without_resume,
  &red_sc_partial_write_again_progress,
  &red_sc_partial_output_error,
  &red_sc_partial_output_deadline,
  &red_sc_invalid_success_zero_progress,
};
static const int red_n_scenarios = 91;
#endif /* RED_DATA_H */