/*
 * PS3 Custom Toolchain - samples/fw/include/cell/FWCellInput.h
 *
 * Cell-backed pad and keyboard devices for the sample fw input facade.
 */

#ifndef PS3TC_FW_CELL_INPUT_H
#define PS3TC_FW_CELL_INPUT_H

#include <FWInput.h>
#include <cell/keyboard.h>
#include <cell/pad.h>

class FWCellPadDevice : public FWInputDevice {
public:
	explicit FWCellPadDevice(int port);
	virtual void update();
	const CellPadData &getPadData() const { return mPadState.raw; }

private:
	FWInputPadState mPadState;
};

class FWCellKeyboardDevice : public FWInputDevice {
public:
	explicit FWCellKeyboardDevice(int port);
	virtual void update();
	const FWInputKeyboardState &getKeyboardState() const { return mKeyboardState; }

private:
	FWInputKeyboardState mKeyboardState;
};

class FWCellInput {
public:
	static void init();
	static void shutdown();
};

#endif /* PS3TC_FW_CELL_INPUT_H */
