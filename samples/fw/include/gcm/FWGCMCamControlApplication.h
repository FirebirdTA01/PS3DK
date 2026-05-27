/*
 * PS3 Custom Toolchain - samples/fw/include/gcm/FWGCMCamControlApplication.h
 *
 * Input-ready camera application shell for fw GCM samples.
 */

#ifndef PS3TC_FW_GCM_CAM_CONTROL_APPLICATION_H
#define PS3TC_FW_GCM_CAM_CONTROL_APPLICATION_H

#include <gcm/FWGCMCamApplication.h>

class FWGCMCamControlApplication : public FWGCMCamApplication {
public:
	FWGCMCamControlApplication();
	virtual ~FWGCMCamControlApplication();

	virtual bool onUpdate();

	float getLastMoveX() const { return mLastMoveX; }
	float getLastMoveZ() const { return mLastMoveZ; }
	float getLastYaw() const { return mLastYaw; }
	float getLastPitch() const { return mLastPitch; }

protected:
	float mLastMoveX;
	float mLastMoveZ;
	float mLastYaw;
	float mLastPitch;
};

#endif /* PS3TC_FW_GCM_CAM_CONTROL_APPLICATION_H */
