#include <cell/mixer.h>

#include <stddef.h>

static uintptr_t next_handle = 0x1000u;
static uint64_t current_tag;

static CellAANHandle mixer_handle(void)
{
    return (CellAANHandle)(uintptr_t)0x4d495855u;
}

static CellAANHandle alloc_handle(void)
{
    next_handle += 0x10u;
    return (CellAANHandle)next_handle;
}

int cellSurMixerCreate(const CellSurMixerConfig *config)
{
    (void)config;
    return CELL_OK;
}

int cellSurMixerFinalize(void)
{
    return CELL_OK;
}

int cellSurMixerStart(void)
{
    return CELL_OK;
}

int cellSurMixerGetAANHandle(CellAANHandle *handle)
{
    if (handle) {
        *handle = mixer_handle();
    }
    return CELL_OK;
}

int cellSurMixerChStripGetAANPortNo(unsigned int *port_no, uint32_t type, unsigned int index)
{
    if (port_no) {
        *port_no = (type << 16) | index;
    }
    return CELL_OK;
}

int cellSurMixerChStripSetParameter(uint32_t type, uint32_t index, CellSurMixerChStripParam *param)
{
    (void)type;
    (void)index;
    (void)param;
    return CELL_OK;
}

int cellSurMixerSetNotifyCallback(CellSurMixerNotifyCallbackFunction callback, void *arg)
{
    (void)callback;
    (void)arg;
    return CELL_OK;
}

int cellSurMixerRemoveNotifyCallback(CellSurMixerNotifyCallbackFunction callback)
{
    (void)callback;
    return CELL_OK;
}

int cellSurMixerSetParameter(uint32_t param, float value)
{
    (void)param;
    (void)value;
    return CELL_OK;
}

int cellSurMixerSurBusAddData(uint32_t bus, uint32_t offset, float *data, uint32_t samples)
{
    (void)bus;
    (void)offset;
    (void)data;
    (void)samples;
    return CELL_OK;
}

int cellSurMixerGetCurrentBlockTag(uint64_t *tag)
{
    if (tag) {
        *tag = ++current_tag;
    }
    return CELL_OK;
}

int cellSurMixerGetTimestamp(uint64_t tag, usecond_t *timestamp)
{
    if (timestamp) {
        *timestamp = tag;
    }
    return CELL_OK;
}

int cellAANConnect(CellAANHandle source, uint32_t source_port, CellAANHandle dest, uint32_t dest_port)
{
    (void)source;
    (void)source_port;
    (void)dest;
    (void)dest_port;
    return CELL_OK;
}

int cellAANDisconnect(CellAANHandle source, uint32_t source_port, CellAANHandle dest, uint32_t dest_port)
{
    (void)source;
    (void)source_port;
    (void)dest;
    (void)dest_port;
    return CELL_OK;
}

int cellAANAddData(CellAANHandle handle, uint32_t port_no, uint32_t offset, float *data, uint32_t samples)
{
    (void)handle;
    (void)port_no;
    (void)offset;
    (void)data;
    (void)samples;
    return CELL_OK;
}

int cellSSPlayerCreate(CellAANHandle *handle, CellSSPlayerConfig *config)
{
    (void)config;
    if (handle) {
        *handle = alloc_handle();
    }
    return CELL_OK;
}

int cellSSPlayerRemove(CellAANHandle handle)
{
    (void)handle;
    return CELL_OK;
}

int cellSSPlayerSetWave(CellAANHandle handle, CellSSPlayerWaveParam *wave, CellSSPlayerCommonParam *reserved)
{
    (void)handle;
    (void)wave;
    (void)reserved;
    return CELL_OK;
}

int cellSSPlayerPlay(CellAANHandle handle, CellSSPlayerRuntimeInfo *runtime)
{
    (void)handle;
    (void)runtime;
    return CELL_OK;
}

int cellSSPlayerSetParam(CellAANHandle handle, CellSSPlayerRuntimeInfo *runtime)
{
    (void)handle;
    (void)runtime;
    return CELL_OK;
}

void cellSurMixerBeep(void *arg)
{
    (void)arg;
}

int cellSurMixerPause(uint32_t sw)
{
    (void)sw;
    return CELL_OK;
}

int cellSSPlayerStop(CellAANHandle handle, uint32_t mode)
{
    (void)handle;
    (void)mode;
    return CELL_OK;
}

int32_t cellSSPlayerGetState(CellAANHandle handle)
{
    (void)handle;
    return CELL_SSPLAYER_STATE_OFF;
}

float cellSurMixerUtilGetLevelFromDB(float dB)
{
    (void)dB;
    return 1.0f;
}

float cellSurMixerUtilGetLevelFromDBIndex(int index)
{
    (void)index;
    return 1.0f;
}

float cellSurMixerUtilNoteToRatio(unsigned char refNote, unsigned char note)
{
    (void)refNote;
    (void)note;
    return 1.0f;
}
