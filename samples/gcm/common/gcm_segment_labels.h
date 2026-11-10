/*
 * samples/gcm/common/gcm_segment_labels.h
 *
 * The RSX label indices the command-segment samples use, in one place.
 * They stay clear of the indices the SDK itself uses: libPSGL (65-68 and
 * 255) and the profiler (240-242; its reports are 32-63 and 512-528).
 */
#ifndef PS3TC_SAMPLES_GCM_SEGMENT_LABELS_H
#define PS3TC_SAMPLES_GCM_SEGMENT_LABELS_H

enum {
    SEG_LABEL_FIRST     = 200,
    SEG_LABEL_RETIRE_0  = 200,  /* segment retirement, slot 0 (back end)  */
    SEG_LABEL_RETIRE_1  = 201,  /* segment retirement, slot 1 (back end)  */
    SEG_LABEL_PROGRESS0 = 202,  /* written inside slot 0's segment        */
    SEG_LABEL_PROGRESS1 = 203,  /* written inside slot 1's segment        */
    SEG_LABEL_RETIRE_L  = 204,  /* the large / probe segment's retirement */
    SEG_LABEL_PROBE     = 205,  /* one-shot probe marker                   */
    SEG_LABEL_NEST_A    = 206,  /* nested probe: outer, before inner CALL */
    SEG_LABEL_NEST_I    = 207,  /* nested probe: inside the inner segment */
    SEG_LABEL_NEST_B    = 208,  /* nested probe: outer, after inner CALL  */
    SEG_LABEL_NEST_R    = 209,  /* nested probe: ring, after the outer    */
    SEG_LABEL_WARM      = 210,  /* ring-only label proving the FIFO runs  */
    SEG_LABEL_LAST      = 215
};

#endif
