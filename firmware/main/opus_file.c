#include "opus_file.h"
#include "opus.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "vmsg";

#define MAX_PACKET 400

struct vmsg_writer {
    FILE *f;
    OpusEncoder *enc;
};

struct vmsg_reader {
    FILE *f;
    OpusDecoder *dec;
};

vmsg_writer_t *vmsg_writer_open(const char *path)
{
    vmsg_writer_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    int err = 0;
    w->enc = opus_encoder_create(VMSG_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK) { free(w); return NULL; }
    opus_encoder_ctl(w->enc, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(w->enc, OPUS_SET_COMPLEXITY(3));
    opus_encoder_ctl(w->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    w->f = fopen(path, "wb");
    if (!w->f) { opus_encoder_destroy(w->enc); free(w); return NULL; }

    uint16_t hdr[4] = { 1, VMSG_SAMPLE_RATE / 100, VMSG_FRAME_MS, 0 };
    fwrite("VMSG", 1, 4, w->f);
    fwrite(hdr, 2, 4, w->f);
    return w;
}

bool vmsg_writer_frame(vmsg_writer_t *w, const int16_t *pcm)
{
    unsigned char pkt[MAX_PACKET];
    int n = opus_encode(w->enc, pcm, VMSG_FRAME_SAMPLES, pkt, sizeof(pkt));
    if (n < 0) { ESP_LOGE(TAG, "encode err %d", n); return false; }
    uint16_t len = (uint16_t)n;
    fwrite(&len, 2, 1, w->f);
    fwrite(pkt, 1, n, w->f);
    return true;
}

void vmsg_writer_close(vmsg_writer_t *w, uint16_t duration_s)
{
    if (!w) return;
    fseek(w->f, 4 + 2 * 3, SEEK_SET);       /* duration field in header */
    fwrite(&duration_s, 2, 1, w->f);
    fclose(w->f);
    opus_encoder_destroy(w->enc);
    free(w);
}

vmsg_reader_t *vmsg_reader_open(const char *path, uint16_t *duration_s)
{
    vmsg_reader_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;

    r->f = fopen(path, "rb");
    if (!r->f) { free(r); return NULL; }

    char magic[4];
    uint16_t hdr[4];
    if (fread(magic, 1, 4, r->f) != 4 || memcmp(magic, "VMSG", 4) ||
        fread(hdr, 2, 4, r->f) != 4) {
        fclose(r->f); free(r);
        return NULL;
    }
    if (duration_s) *duration_s = hdr[3];

    int err = 0;
    r->dec = opus_decoder_create(VMSG_SAMPLE_RATE, 1, &err);
    if (err != OPUS_OK) { fclose(r->f); free(r); return NULL; }
    return r;
}

bool vmsg_reader_frame(vmsg_reader_t *r, int16_t *pcm)
{
    uint16_t len = 0;
    unsigned char pkt[MAX_PACKET];
    if (fread(&len, 2, 1, r->f) != 1 || len == 0 || len > MAX_PACKET) return false;
    if (fread(pkt, 1, len, r->f) != len) return false;
    int n = opus_decode(r->dec, pkt, len, pcm, VMSG_FRAME_SAMPLES, 0);
    return n == VMSG_FRAME_SAMPLES;
}

void vmsg_reader_close(vmsg_reader_t *r)
{
    if (!r) return;
    fclose(r->f);
    opus_decoder_destroy(r->dec);
    free(r);
}
