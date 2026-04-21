/*
 * probe.c — isolated TU that reaches the nidgen NID stub for
 * cellGcmGetConfiguration without going through the cell/gcm.h
 * static-inline widener.
 *
 * Kept separate from main.c specifically so main.c can use the rest
 * of <cell/gcm.h> (cellGcmInit etc.) while this file sees only the
 * raw extern signature.
 */

/* Raw stub signature — the nidgen-generated libgcm_sys symbol has
 * no type constraints at the ABI boundary. */
extern void cellGcmGetConfiguration(void *config);

void gcm_config_abi_probe(void *buf64)
{
    cellGcmGetConfiguration(buf64);
}
