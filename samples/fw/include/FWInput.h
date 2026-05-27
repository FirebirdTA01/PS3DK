/*
 * PS3 Custom Toolchain - samples/fw/include/FWInput.h
 *
 * Sample fw input facade over pad and keyboard devices.
 */

#ifndef PS3TC_FW_INPUT_H
#define PS3TC_FW_INPUT_H

#include <FWInputChannels.h>

class FWInputDevice;
class FWInputFilter;
class FWInputManager;

class FWInput {
public:
	enum DeviceType {
		DeviceType_None = 0,
		DeviceType_Pad,
		DeviceType_Keyboard,
		DeviceType_Mouse
	};

	enum Channel {
		Channel_None = 0,
		Channel_Button_0,
		Channel_Button_1,
		Channel_Button_2,
		Channel_Button_3,
		Channel_Button_4,
		Channel_Button_5,
		Channel_Button_6,
		Channel_Button_7,
		Channel_Button_8,
		Channel_Button_9,
		Channel_Button_10,
		Channel_Button_11,
		Channel_Button_12,
		Channel_Button_13,
		Channel_Button_14,
		Channel_Button_15,
		Channel_XAxis_0,
		Channel_YAxis_0,
		Channel_XAxis_1,
		Channel_YAxis_1,
		Channel_Key_A,
		Channel_Key_B,
		Channel_Key_C,
		Channel_Key_D,
		Channel_Key_E,
		Channel_Key_F,
		Channel_Key_G,
		Channel_Key_H,
		Channel_Key_I,
		Channel_Key_J,
		Channel_Key_K,
		Channel_Key_L,
		Channel_Key_M,
		Channel_Key_N,
		Channel_Key_O,
		Channel_Key_P,
		Channel_Key_Q,
		Channel_Key_R,
		Channel_Key_S,
		Channel_Key_T,
		Channel_Key_U,
		Channel_Key_V,
		Channel_Key_W,
		Channel_Key_X,
		Channel_Key_Y,
		Channel_Key_Z,
		Channel_Key_0,
		Channel_Key_1,
		Channel_Key_2,
		Channel_Key_3,
		Channel_Key_4,
		Channel_Key_5,
		Channel_Key_6,
		Channel_Key_7,
		Channel_Key_8,
		Channel_Key_9,
		Channel_Key_Escape,
		Channel_Key_Space,
		Channel_Key_Return,
		Channel_Key_Left,
		Channel_Key_Right,
		Channel_Key_Up,
		Channel_Key_Down,
		Channel_Count
	};

	static void init(void *pPlatformData = 0);
	static void shutdown();
	static void update();
	static int enumerateDevices(FWInputDevice **ppDevices);
	static FWInputDevice *getDevice(DeviceType type, int instance = 0);
	static Channel asciiToChannel(int ascii);
};

class FWInputFilter {
public:
	FWInputFilter();
	virtual ~FWInputFilter();

	void bind(FWInputDevice *pDevice, FWInput::Channel channel);
	void unbind();
	void update();

	bool getBool() const { return mBool; }
	bool getPressed() const { return mPressed; }
	bool getReleased() const { return mReleased; }
	float getFloat() const { return mFloat; }

private:
	FWInputDevice *mpDevice;
	FWInput::Channel mChannel;
	bool mBool;
	bool mPressed;
	bool mReleased;
	float mFloat;
};

class FWInputDevice {
public:
	FWInputDevice(FWInput::DeviceType type, int instance);
	virtual ~FWInputDevice();

	virtual void update();
	virtual bool getRawBool(FWInput::Channel channel) const;
	virtual float getRawFloat(FWInput::Channel channel) const;

	FWInput::DeviceType getType() const { return mType; }
	int getInstance() const { return mInstance; }
	bool isConnected() const { return mConnected; }

	FWInputFilter *bindFilter(FWInput::Channel channel);

protected:
	static void updateButton(FWInputButtonState &state, bool down);
	static void updateAxis(FWInputAxisState &state, float value);

	FWInput::DeviceType mType;
	int mInstance;
	bool mConnected;
	FWInputButtonState mButtons[FWINPUT_MAX_BUTTONS];
	FWInputAxisState mAxes[4];
	FWInputButtonState mKeys[FWINPUT_MAX_KEYS];
	uint16_t mLastKeyCode;
	char mLastAscii;
};

class FWInputManager {
public:
	static FWInputManager &get();

	void init(void *pPlatformData = 0);
	void shutdown();
	void update();
	int enumerateDevices(FWInputDevice **ppDevices);
	FWInputDevice *getDevice(FWInput::DeviceType type, int instance = 0);
	FWInput::Channel asciiToChannel(int ascii) const;

	void addDevice(FWInputDevice *pDevice);
	void removeDevice(FWInputDevice *pDevice);

private:
	FWInputManager();
	FWInputDevice *mDevices[FWINPUT_MAX_PADS + FWINPUT_MAX_KEYBOARDS];
	int mDeviceCount;
	bool mInitialized;
};

#endif /* PS3TC_FW_INPUT_H */
