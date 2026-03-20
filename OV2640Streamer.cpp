#include "OV2640Streamer.h"

OV2640Streamer::OV2640Streamer(int width, int height) : CStreamer(width, height)
{
    // CStreamer initializes width and height
}

void OV2640Streamer::streamImage(uint32_t curMsec)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        return;
    }
    
    // push the packet using CStreamer's native method
    streamFrame(fb->buf, fb->len, curMsec);
    
    esp_camera_fb_return(fb);
}
