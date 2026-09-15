#ifndef AUDIO_STREAMER_H
#define AUDIO_STREAMER_H

#include "app_config.h"

void audio_streamer_start(const app_config_t *config);

// Adjusts the running 10-band equalizer live, no restart needed. gains_db[0..9] map to
// the band centers 31/62/125/250/500/1000/2000/4000/8000/16000 Hz. No-op if the pipeline
// hasn't started yet.
void audio_streamer_set_eq(const int gains_db[10]);

#endif // AUDIO_STREAMER_H