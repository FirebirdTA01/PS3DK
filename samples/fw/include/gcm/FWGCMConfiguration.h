/*
 * PS3 Custom Toolchain - samples/fw/include/gcm/FWGCMConfiguration.h
 *
 * GCM command-buffer defaults for fw-style samples.
 */

#ifndef PS3TC_FW_GCM_CONFIGURATION_H
#define PS3TC_FW_GCM_CONFIGURATION_H

#include <stdint.h>
#include <cell/gcm.h>

class FWGCMConfiguration {
public:
	FWGCMConfiguration()
		: mDefaultCBSize(512u << 10),
		  mColorFormat(CELL_GCM_SURFACE_A8R8G8B8),
		  mDefaultStateCmdSize(512u << 10)
	{
	}

	virtual ~FWGCMConfiguration() {}

	uint32_t mDefaultCBSize;
	uint32_t mColorFormat;
	uint32_t mDefaultStateCmdSize;
};

#endif /* PS3TC_FW_GCM_CONFIGURATION_H */
