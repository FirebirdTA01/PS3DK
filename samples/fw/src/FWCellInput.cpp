/*
 * PS3 Custom Toolchain - samples/fw/src/FWCellInput.cpp
 *
 * Cell pad and keyboard polling for the sample fw input facade.
 */

#include <FWInputAsciiLookup.h>
#include <cell/FWCellInput.h>

#include <stdio.h>
#include <string.h>

static FWCellPadDevice *sPadDevices[FWINPUT_MAX_PADS];
static FWCellKeyboardDevice *sKeyboardDevice;
static bool sCellInputReady = false;

static float fwNormalizeStick(uint16_t value)
{
	return ((float)value - 128.0f) / 128.0f;
}

static bool fwPadButtonDown(const CellPadData &data, int index, uint16_t mask)
{
	if (data.len <= index)
		return false;
	return (data.button[index] & mask) != 0;
}

static int fwKeyCodeToAscii(uint16_t code)
{
	if (code >= 32 && code < 127)
		return code;
	return 0;
}

static int fwRawKeyToIndex(uint16_t code)
{
	if (code < FWINPUT_MAX_KEYS)
		return (int)code;
	return -1;
}

static FWInput::Channel fwRawKeyToChannel(uint16_t code)
{
	switch (code) {
	case KB_RAWKEY_LEFT_ARROW:
		return FWInput::Channel_Key_Left;
	case KB_RAWKEY_RIGHT_ARROW:
		return FWInput::Channel_Key_Right;
	case KB_RAWKEY_UP_ARROW:
		return FWInput::Channel_Key_Up;
	case KB_RAWKEY_DOWN_ARROW:
		return FWInput::Channel_Key_Down;
	default:
		return FWInput::Channel_None;
	}
}

int fwInputAsciiToChannel(int ascii)
{
	if (ascii >= 'a' && ascii <= 'z')
		return FWInput::Channel_Key_A + (ascii - 'a');
	if (ascii >= 'A' && ascii <= 'Z')
		return FWInput::Channel_Key_A + (ascii - 'A');
	if (ascii >= '0' && ascii <= '9')
		return FWInput::Channel_Key_0 + (ascii - '0');
	switch (ascii) {
	case 27:
		return FWInput::Channel_Key_Escape;
	case ' ':
		return FWInput::Channel_Key_Space;
	case '\n':
	case '\r':
		return FWInput::Channel_Key_Return;
	default:
		return FWInput::Channel_None;
	}
}

void FWInputDevice::updateButton(FWInputButtonState &state, bool down)
{
	bool previous = state.down;
	state.down = down;
	state.pressed = down && !previous;
	state.released = !down && previous;
}

void FWInputDevice::updateAxis(FWInputAxisState &state, float value)
{
	state.previous = state.value;
	state.value = value;
}

FWInputDevice::FWInputDevice(FWInput::DeviceType type, int instance)
	: mType(type)
	, mInstance(instance)
	, mConnected(false)
	, mLastKeyCode(0)
	, mLastAscii(0)
{
	memset(mButtons, 0, sizeof(mButtons));
	memset(mAxes, 0, sizeof(mAxes));
	memset(mKeys, 0, sizeof(mKeys));
	FWInputManager::get().addDevice(this);
}

FWInputDevice::~FWInputDevice()
{
	FWInputManager::get().removeDevice(this);
}

void FWInputDevice::update()
{
}

bool FWInputDevice::getRawBool(FWInput::Channel channel) const
{
	if (channel >= FWInput::Channel_Button_0 && channel <= FWInput::Channel_Button_15)
		return mButtons[channel - FWInput::Channel_Button_0].down;
	if (channel >= FWInput::Channel_Key_A && channel < FWInput::Channel_Count) {
		int index = channel - FWInput::Channel_Key_A;
		if (index >= 0 && index < FWINPUT_MAX_KEYS)
			return mKeys[index].down;
	}
	return false;
}

float FWInputDevice::getRawFloat(FWInput::Channel channel) const
{
	switch (channel) {
	case FWInput::Channel_XAxis_0:
		return mAxes[0].value;
	case FWInput::Channel_YAxis_0:
		return mAxes[1].value;
	case FWInput::Channel_XAxis_1:
		return mAxes[2].value;
	case FWInput::Channel_YAxis_1:
		return mAxes[3].value;
	default:
		return getRawBool(channel) ? 1.0f : 0.0f;
	}
}

FWInputFilter *FWInputDevice::bindFilter(FWInput::Channel channel)
{
	FWInputFilter *filter = new FWInputFilter;
	if (filter)
		filter->bind(this, channel);
	return filter;
}

FWInputFilter::FWInputFilter()
	: mpDevice(0)
	, mChannel(FWInput::Channel_None)
	, mBool(false)
	, mPressed(false)
	, mReleased(false)
	, mFloat(0.0f)
{
}

FWInputFilter::~FWInputFilter()
{
}

void FWInputFilter::bind(FWInputDevice *pDevice, FWInput::Channel channel)
{
	mpDevice = pDevice;
	mChannel = channel;
	update();
}

void FWInputFilter::unbind()
{
	mpDevice = 0;
	mChannel = FWInput::Channel_None;
	mBool = false;
	mPressed = false;
	mReleased = false;
	mFloat = 0.0f;
}

void FWInputFilter::update()
{
	bool previous = mBool;
	mBool = mpDevice ? mpDevice->getRawBool(mChannel) : false;
	mPressed = mBool && !previous;
	mReleased = !mBool && previous;
	mFloat = mpDevice ? mpDevice->getRawFloat(mChannel) : 0.0f;
}

FWInputManager &FWInputManager::get()
{
	static FWInputManager sManager;
	return sManager;
}

FWInputManager::FWInputManager()
	: mDeviceCount(0)
	, mInitialized(false)
{
	memset(mDevices, 0, sizeof(mDevices));
}

void FWInputManager::init(void *pPlatformData)
{
	(void)pPlatformData;
	if (mInitialized)
		return;
	FWCellInput::init();
	mInitialized = true;
}

void FWInputManager::shutdown()
{
	if (!mInitialized)
		return;
	FWCellInput::shutdown();
	mInitialized = false;
}

void FWInputManager::update()
{
	if (!mInitialized)
		init();
	for (int i = 0; i < mDeviceCount; ++i) {
		if (mDevices[i])
			mDevices[i]->update();
	}
}

int FWInputManager::enumerateDevices(FWInputDevice **ppDevices)
{
	if (ppDevices) {
		for (int i = 0; i < mDeviceCount; ++i)
			ppDevices[i] = mDevices[i];
	}
	return mDeviceCount;
}

FWInputDevice *FWInputManager::getDevice(FWInput::DeviceType type, int instance)
{
	for (int i = 0; i < mDeviceCount; ++i) {
		if (mDevices[i] && mDevices[i]->getType() == type &&
		    mDevices[i]->getInstance() == instance)
			return mDevices[i];
	}
	return 0;
}

FWInput::Channel FWInputManager::asciiToChannel(int ascii) const
{
	return (FWInput::Channel)fwInputAsciiToChannel(ascii);
}

void FWInputManager::addDevice(FWInputDevice *pDevice)
{
	if (!pDevice || mDeviceCount >= (int)(sizeof(mDevices) / sizeof(mDevices[0])))
		return;
	mDevices[mDeviceCount++] = pDevice;
}

void FWInputManager::removeDevice(FWInputDevice *pDevice)
{
	for (int i = 0; i < mDeviceCount; ++i) {
		if (mDevices[i] == pDevice) {
			for (int j = i; j + 1 < mDeviceCount; ++j)
				mDevices[j] = mDevices[j + 1];
			mDevices[--mDeviceCount] = 0;
			return;
		}
	}
}

void FWInput::init(void *pPlatformData)
{
	FWInputManager::get().init(pPlatformData);
}

void FWInput::shutdown()
{
	FWInputManager::get().shutdown();
}

void FWInput::update()
{
	FWInputManager::get().update();
}

int FWInput::enumerateDevices(FWInputDevice **ppDevices)
{
	return FWInputManager::get().enumerateDevices(ppDevices);
}

FWInputDevice *FWInput::getDevice(DeviceType type, int instance)
{
	return FWInputManager::get().getDevice(type, instance);
}

FWInput::Channel FWInput::asciiToChannel(int ascii)
{
	return FWInputManager::get().asciiToChannel(ascii);
}

FWCellPadDevice::FWCellPadDevice(int port)
	: FWInputDevice(FWInput::DeviceType_Pad, port)
{
	memset(&mPadState, 0, sizeof(mPadState));
}

void FWCellPadDevice::update()
{
	memset(&mPadState.raw, 0, sizeof(mPadState.raw));
	int rc = cellPadGetData((uint32_t)mInstance, &mPadState.raw);
	mConnected = (rc == 0 && mPadState.raw.len > CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y);
	mPadState.connected = mConnected;
	updateButton(mButtons[0], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS));
	updateButton(mButtons[1], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE));
	updateButton(mButtons[2], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE));
	updateButton(mButtons[3], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE));
	updateButton(mButtons[4], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1));
	updateButton(mButtons[5], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1));
	updateButton(mButtons[6], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2));
	updateButton(mButtons[7], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2));
	updateButton(mButtons[8], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT));
	updateButton(mButtons[9], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START));
	updateButton(mButtons[10], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP));
	updateButton(mButtons[11], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN));
	updateButton(mButtons[12], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT));
	updateButton(mButtons[13], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT));
	updateButton(mButtons[14], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3));
	updateButton(mButtons[15], fwPadButtonDown(mPadState.raw, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3));

	float lx = mConnected ? fwNormalizeStick(mPadState.raw.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]) : 0.0f;
	float ly = mConnected ? fwNormalizeStick(mPadState.raw.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]) : 0.0f;
	float rx = mConnected ? fwNormalizeStick(mPadState.raw.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X]) : 0.0f;
	float ry = mConnected ? fwNormalizeStick(mPadState.raw.button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y]) : 0.0f;
	updateAxis(mAxes[0], lx);
	updateAxis(mAxes[1], ly);
	updateAxis(mAxes[2], rx);
	updateAxis(mAxes[3], ry);
	mPadState.leftX = mAxes[0];
	mPadState.leftY = mAxes[1];
	mPadState.rightX = mAxes[2];
	mPadState.rightY = mAxes[3];
}

FWCellKeyboardDevice::FWCellKeyboardDevice(int port)
	: FWInputDevice(FWInput::DeviceType_Keyboard, port)
{
	memset(&mKeyboardState, 0, sizeof(mKeyboardState));
}

void FWCellKeyboardDevice::update()
{
	for (int i = 0; i < FWINPUT_MAX_KEYS; ++i)
		updateButton(mKeys[i], false);
	CellKbData data;
	memset(&data, 0, sizeof(data));
	int rc = cellKbRead((uint32_t)mInstance, &data);
	mConnected = (rc == 0);
	mKeyboardState.connected = mConnected;
	mKeyboardState.lastCode = 0;
	mKeyboardState.lastAscii = 0;
	if (!mConnected)
		return;

	for (int i = 0; i < data.len && i < CELL_KB_MAX_KEYCODES; ++i) {
		uint16_t code = data.keycode[i];
		int index = fwRawKeyToIndex(code);
		if (index >= 0)
			updateButton(mKeys[index], true);
		FWInput::Channel rawChannel = fwRawKeyToChannel(code);
		if (rawChannel != FWInput::Channel_None) {
			int rawIndex = rawChannel - FWInput::Channel_Key_A;
			if (rawIndex >= 0 && rawIndex < FWINPUT_MAX_KEYS)
				updateButton(mKeys[rawIndex], true);
		}
		int ascii = fwKeyCodeToAscii(code);
		if (ascii) {
			mLastAscii = (char)ascii;
			mKeyboardState.lastAscii = (char)ascii;
			int channel = fwInputAsciiToChannel(ascii);
			int keyIndex = channel - FWInput::Channel_Key_A;
			if (keyIndex >= 0 && keyIndex < FWINPUT_MAX_KEYS)
				updateButton(mKeys[keyIndex], true);
		}
		mLastKeyCode = code;
		mKeyboardState.lastCode = code;
	}
	memcpy(mKeyboardState.keys, mKeys, sizeof(mKeys));
}

void FWCellInput::init()
{
	if (sCellInputReady)
		return;
	int padRc = cellPadInit(FWINPUT_MAX_PADS);
	int kbRc = cellKbInit(FWINPUT_MAX_KEYBOARDS);
	printf("  FWCellInput: cellPadInit -> 0x%08x\n", (unsigned)padRc);
	printf("  FWCellInput: cellKbInit -> 0x%08x\n", (unsigned)kbRc);
	for (int i = 0; i < FWINPUT_MAX_PADS; ++i)
		sPadDevices[i] = new FWCellPadDevice(i);
	sKeyboardDevice = new FWCellKeyboardDevice(0);
	sCellInputReady = true;
}

void FWCellInput::shutdown()
{
	if (!sCellInputReady)
		return;
	for (int i = 0; i < FWINPUT_MAX_PADS; ++i) {
		delete sPadDevices[i];
		sPadDevices[i] = 0;
	}
	delete sKeyboardDevice;
	sKeyboardDevice = 0;
	cellKbEnd();
	cellPadEnd();
	sCellInputReady = false;
}
