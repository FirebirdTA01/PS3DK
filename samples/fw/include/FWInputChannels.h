/*
 * PS3 Custom Toolchain - samples/fw/include/FWInputChannels.h
 *
 * Shared input channel state for the sample fw layer.
 */

#ifndef PS3TC_FW_INPUT_CHANNELS_H
#define PS3TC_FW_INPUT_CHANNELS_H

#include <cell/pad.h>
#include <stdint.h>

#define FWINPUT_MAX_PADS      7
#define FWINPUT_MAX_KEYBOARDS 1
#define FWINPUT_MAX_BUTTONS   16
#define FWINPUT_MAX_KEYS      128

struct FWInputButtonState {
	bool down;
	bool pressed;
	bool released;
};

struct FWInputAxisState {
	float value;
	float previous;
};

struct FWInputPadState {
	bool connected;
	FWInputButtonState buttons[FWINPUT_MAX_BUTTONS];
	FWInputAxisState leftX;
	FWInputAxisState leftY;
	FWInputAxisState rightX;
	FWInputAxisState rightY;
	CellPadData raw;
};

struct FWInputKeyboardState {
	bool connected;
	FWInputButtonState keys[FWINPUT_MAX_KEYS];
	uint16_t lastCode;
	char lastAscii;
};

#endif /* PS3TC_FW_INPUT_CHANNELS_H */
