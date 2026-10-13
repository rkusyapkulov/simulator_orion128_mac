// simulator_orion128.h
#ifndef SIMULATOR_ORION128_H
#define SIMULATOR_ORION128_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SCREEN_WIDTH  384
#define SCREEN_HEIGHT 256

void orion128_init(const char *monitor_path, const char *romdisk_path);
void orion128_run_frame(void);
void orion128_reset(void);
const uint32_t* orion128_get_pixel_buffer(void);
void orion128_press_key(int row, int bit, bool isPressed);
void orion128_get_audio_samples(int16_t *buffer, int sample_count);
bool orion128_load_file(const char *filepath, char *out_msg, size_t msg_max_len);
    
#endif // SIMULATOR_ORION128_H

