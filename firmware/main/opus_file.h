/* opus_file: minimal container for voice messages (.vmsg)
 *   header:  "VMSG" | u16 version | u16 sample_rate/100 | u16 frame_ms | u16 duration_s
 *   frames:  u16 packet_len | opus packet bytes   (repeated)
 * 16 kHz mono, 20 ms frames, ~16 kbps.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define VMSG_SAMPLE_RATE   16000
#define VMSG_FRAME_MS      20
#define VMSG_FRAME_SAMPLES (VMSG_SAMPLE_RATE * VMSG_FRAME_MS / 1000)  /* 320 */

typedef struct vmsg_writer vmsg_writer_t;
typedef struct vmsg_reader vmsg_reader_t;

vmsg_writer_t *vmsg_writer_open(const char *path);
/* Encode+append exactly VMSG_FRAME_SAMPLES of s16 mono PCM. */
bool vmsg_writer_frame(vmsg_writer_t *w, const int16_t *pcm);
/* Finalize header with real duration; frees writer. */
void vmsg_writer_close(vmsg_writer_t *w, uint16_t duration_s);

vmsg_reader_t *vmsg_reader_open(const char *path, uint16_t *duration_s);
/* Decode next frame into VMSG_FRAME_SAMPLES of s16 PCM; false at EOF. */
bool vmsg_reader_frame(vmsg_reader_t *r, int16_t *pcm);
void vmsg_reader_close(vmsg_reader_t *r);
