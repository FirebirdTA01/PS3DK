/* SPU-side job chain controls: urgent commands and the grab limit.
 *
 * CellSpursJobChain fields used (first line):
 *   0x30  u64[4]  urgent commands; 0 = free slot, the job manager runs
 *                 them ahead of the command list
 *   0x72  u16     max jobs a job manager grabs at once (1..16)
 *   0x74  u32     workload id (> 31 until the chain is created)
 *   0x78  u64     SPURS instance EA
 * The instance's enabled-workload word (u32 at +0xb0, workload 0 in the
 * MSB) tells whether the chain's workload exists.
 * Independently written from the published object layouts and semantics.
 */
#include <stdint.h>
#include <spu_mfcio.h>
#include "spurs_line.h"

#define JOB_INVAL 0x80410A02u
#define JOB_BUSY  0x80410A0Au
#define JOB_STAT  0x80410A0Fu
#define JOB_ALIGN 0x80410A10u
#define JOB_NULL  0x80410A11u

#define URGENT_SLOTS 4
#define OPCODE_CALL  4

static uint8_t line[128] __attribute__((aligned(128)));

static int check_chain(uint64_t ea)
{
	if (!ea)
		return (int)JOB_NULL;
	if (ea & 0x7f)
		return (int)JOB_ALIGN;
	return 0;
}

int cellSpursAddUrgentCommand(uint64_t eaJobChain, uint64_t command)
{
	unsigned i;
	int rc = check_chain(eaJobChain);
	if (rc)
		return rc;
	do {
		line_get_at(line, eaJobChain);
		rc = (int)JOB_INVAL;
		if (LINE_U32(line, 0x74) > 31)
			continue;                   /* not created */
		rc = (int)JOB_BUSY;
		for (i = 0; i < URGENT_SLOTS; ++i)
			if (!LINE_U64(line, 0x30 + i * 8)) {
				LINE_U64(line, 0x30 + i * 8) = command;
				rc = 0;
				break;
			}
	} while (!line_put_at(line, eaJobChain));
	return rc;
}

int cellSpursAddUrgentCall(uint64_t eaJobChain, uint64_t commandList)
{
	if (!commandList)
		return (int)JOB_NULL;
	if (commandList & 7)
		return (int)JOB_ALIGN;
	return cellSpursAddUrgentCommand(eaJobChain, ((uint32_t)commandList & ~7u) | OPCODE_CALL);
}

int cellSpursJobSetMaxGrab(uint64_t eaJobChain, unsigned int maxGrab)
{
	static uint8_t instance[128] __attribute__((aligned(128)));
	uint64_t spurs;
	unsigned wid;
	int rc = check_chain(eaJobChain);
	if (rc)
		return rc;
	if (maxGrab - 1 > 15)
		return (int)JOB_INVAL;
	line_get_at(line, eaJobChain);
	spurs = LINE_U64(line, 0x78);
	wid = LINE_U32(line, 0x74);
	if (!spurs || (spurs & 0x7f) || wid > 31)
		return (int)JOB_STAT;
	line_get_at(instance, spurs + 0x80);
	if (!((LINE_U32(instance, 0x30) >> (31 - wid)) & 1))
		return (int)JOB_STAT;                   /* workload not enabled */
	do {
		line_get_at(line, eaJobChain);
		*(volatile uint16_t *)(line + 0x72) = (uint16_t)maxGrab;
	} while (!line_put_at(line, eaJobChain));
	return 0;
}
