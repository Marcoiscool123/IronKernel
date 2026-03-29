#ifndef AC97_H
#define AC97_H

/* IronKernel — ac97.h
   Intel AC97 / ICH audio controller driver.
   Polled DMA, 16-bit stereo PCM, variable sample rate (VRA). */

#include "../kernel/types.h"

void ac97_init(void);
/* Detect the AC97 PCI device, reset the codec, set volumes.
   Prints "[AC97] ... Hz" on success or "[AC97] not found". */

int  ac97_detected(void);
/* Returns 1 if ac97_init found hardware, 0 otherwise. */

int  ac97_play_pcm(const int16_t *samples, uint32_t num_stereo_frames,
                   uint32_t sample_rate);
/* Play num_stereo_frames frames of 16-bit interleaved stereo.
   Blocks until DMA completes or times out.
   Returns 0 on success, -1 if not initialised. */

int  ac97_play_wav(const uint8_t *wav, uint32_t wav_len);
/* Parse a RIFF/WAV buffer (must be PCM, 16-bit, mono or stereo)
   and play it.  Returns 0 on success, -1 on bad format. */

int  ac97_play_file(const char *name8, const char *ext3);
/* Load a file from FAT32 by 8.3 space-padded name/ext and play it
   as a WAV.  Returns 0 on success, -1 on load or format error. */

void ac97_stop(void);
/* Stop the PCM DMA channel immediately. */

void ac97_boot_chime(void);
/* Synthesise and play the GUI startup chord (≈950 ms).
   No-op if ac97_detected() == 0. */

void ac97_play_boot_wav(void);
/* Try to load BOOT.WAV from FAT32 and play it.
   Falls back to ac97_boot_chime() if the file is absent.
   No-op if ac97_detected() == 0. */

#endif
