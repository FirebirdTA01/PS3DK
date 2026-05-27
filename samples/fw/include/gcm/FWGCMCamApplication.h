/*
 * PS3 Custom Toolchain - samples/fw/include/gcm/FWGCMCamApplication.h
 *
 * Camera-state extension for GCM fw samples.
 */

#ifndef PS3TC_FW_GCM_CAM_APPLICATION_H
#define PS3TC_FW_GCM_CAM_APPLICATION_H

#include <FWMath.h>
#include <gcm/FWGCMApplication.h>

class FWGCMCamApplication : public FWGCMApplication {
public:
	FWGCMCamApplication();
	virtual ~FWGCMCamApplication();

	virtual bool onUpdate();

	const Matrix4 &getViewMatrix() const { return mViewMatrix; }
	const Matrix4 &getProjectionMatrix() const { return mProjectionMatrix; }
	const Point3 &getCameraPosition() const { return mCameraPosition; }
	const Vector3 &getCameraTarget() const { return mCameraTarget; }

	void setViewMatrix(const Matrix4 &matrix) { mViewMatrix = matrix; }
	void setProjectionMatrix(const Matrix4 &matrix) { mProjectionMatrix = matrix; }
	void setCameraPosition(const Point3 &position) { mCameraPosition = position; }
	void setCameraTarget(const Vector3 &target) { mCameraTarget = target; }

protected:
	Matrix4 mViewMatrix;
	Matrix4 mProjectionMatrix;
	Point3 mCameraPosition;
	Vector3 mCameraTarget;
};

#endif /* PS3TC_FW_GCM_CAM_APPLICATION_H */
