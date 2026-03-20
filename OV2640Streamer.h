#pragma once
#include "CStreamer.h"
#include <esp_camera.h>

class OV2640Streamer : public CStreamer
{
public:
    OV2640Streamer(int width, int height);
    virtual void streamImage(uint32_t curMsec);
};
