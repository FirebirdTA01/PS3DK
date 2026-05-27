/*
 * PS3 Custom Toolchain - samples/fw/include/FWInputAsciiLookup.h
 *
 * ASCII-to-input-channel helpers for the sample fw layer.
 */

#ifndef PS3TC_FW_INPUT_ASCII_LOOKUP_H
#define PS3TC_FW_INPUT_ASCII_LOOKUP_H

class FWInput;

int fwInputAsciiToChannel(int ascii);

#endif /* PS3TC_FW_INPUT_ASCII_LOOKUP_H */
