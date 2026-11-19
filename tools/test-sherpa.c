/* Standalone smoke test for the sherpa-onnx ASR backend:
 * loads the model, feeds a wav in chunks, prints PARTIAL/FINAL/SILENCE,
 * flushes and exits 0 iff a FINAL result was received.
 *
 * Build (from repo root):
 *   gcc -o builddir/test-sherpa tools/test-sherpa.c src/asr-engine.c src/asr-engine-sherpa.c \
 *     -Isrc -Isubprojects/sherpa-onnx -Isubprojects/sherpa-onnx/__CMake_build/_deps/onnxruntime-src/include \
 *     -O2 -pthread \
 *     builddir/subprojects/sherpa-onnx/libsherpa_onnx_c_api.a \
 *     builddir/subprojects/sherpa-onnx/libsherpa_onnx_core.a \
 *     builddir/subprojects/sherpa-onnx/libkaldi_decoder_core.a \
 *     builddir/subprojects/sherpa-onnx/libkaldifst_core.a \
 *     builddir/subprojects/sherpa-onnx/libfst.a builddir/subprojects/sherpa-onnx/libfstfar.a \
 *     builddir/subprojects/sherpa-onnx/libkaldi_native_fbank_core.a \
 *     builddir/subprojects/sherpa-onnx/libkissfft.a \
 *     builddir/subprojects/sherpa-onnx/libssentencepiece_core.a \
 *     builddir/subprojects/sherpa-onnx/__CMake_build/_deps/onnxruntime-src/lib/libonnxruntime.a \
 *     -lstdc++ -lm -ldl -lpthread
 *
 * Run:
 *   ./builddir/test-sherpa models/sherpa-onnx-streaming-zipformer-en-2023-06-26 /tmp/opencode/test-speech.wav
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "asr-engine.h"

/* Stub: the test only exercises the sherpa backend */
const struct asr_engine_ops april_asr_ops = {0};

static volatile int finals_received = 0;
static volatile int silences_received = 0;
static volatile int partials_received = 0;

static void handler(void *userdata, enum asr_result_type result, size_t count, const struct asr_token *tokens) {
    (void)userdata;

    switch(result) {
        case ASR_RESULT_RECOGNITION_PARTIAL:
            partials_received++;
            printf("PARTIAL:");
            for(size_t i=0; i<count; i++) printf("%s", tokens[i].text);
            printf("\n");
            break;

        case ASR_RESULT_RECOGNITION_FINAL:
            finals_received++;
            printf("FINAL:");
            for(size_t i=0; i<count; i++) printf("%s", tokens[i].text);
            printf("\n");
            break;

        case ASR_RESULT_SILENCE:
            silences_received++;
            printf("SILENCE\n");
            break;

        default:
            printf("OTHER result type %d\n", result);
            break;
    }

    fflush(stdout);
}

/* Reads a wav file (16-bit PCM, mono) and returns malloc'd shorts; sets
 * sample_rate_out if non-NULL. Minimal RIFF parsing. */
static short *read_wav(const char *path, size_t *count_out, uint32_t *sample_rate_out) {
    FILE *f = fopen(path, "rb");
    if(f == NULL) { perror("fopen"); return NULL; }

    unsigned char hdr[44];
    if(fread(hdr, 1, 44, f) != 44) { fclose(f); return NULL; }

    if(memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr+8, "WAVE", 4) != 0) {
        printf("not a wav\n");
        fclose(f);
        return NULL;
    }

    /* Search for fmt and data chunks properly */
    fseek(f, 12, SEEK_SET);
    uint32_t sample_rate = 0;
    uint16_t channels = 0, bits = 0;
    long data_pos = -1;
    uint32_t data_len = 0;

    while(true) {
        unsigned char chunk_hdr[8];
        if(fread(chunk_hdr, 1, 8, f) != 8) break;

        uint32_t chunk_size = chunk_hdr[4] | (chunk_hdr[5]<<8) | (chunk_hdr[6]<<16) | ((uint32_t)chunk_hdr[7]<<24);

        if(memcmp(chunk_hdr, "fmt ", 4) == 0) {
            unsigned char fmt[16];
            if(chunk_size < 16 || fread(fmt, 1, 16, f) != 16) break;
            channels = fmt[2] | (fmt[3]<<8);
            sample_rate = fmt[4] | (fmt[5]<<8) | (fmt[6]<<16) | ((uint32_t)fmt[7]<<24);
            bits = fmt[14] | (fmt[15]<<8);
            if(chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
        } else if(memcmp(chunk_hdr, "data", 4) == 0) {
            data_pos = ftell(f);
            data_len = chunk_size;
            break;
        } else {
            fseek(f, (chunk_size + 1) & ~1UL, SEEK_CUR);
        }
    }

    if(data_pos < 0 || sample_rate == 0 || bits != 16) {
        printf("unsupported wav (bits=%u)\n", bits);
        fclose(f);
        return NULL;
    }

    fseek(f, data_pos, SEEK_SET);
    size_t frames = data_len / (2 * channels);
    short *pcm = malloc(frames * sizeof(short));
    if(pcm == NULL) { fclose(f); return NULL; }

    if(channels == 1) {
        if(fread(pcm, 2, frames, f) != frames) { free(pcm); fclose(f); return NULL; }
    } else {
        short *tmp = malloc(data_len);
        if(tmp == NULL || fread(tmp, 1, data_len, f) != data_len) { free(tmp); free(pcm); fclose(f); return NULL; }
        for(size_t i=0; i<frames; i++) pcm[i] = (short)((tmp[2*i] + tmp[2*i+1]) / 2);
        free(tmp);
    }

    fclose(f);
    *count_out = frames;
    *sample_rate_out = sample_rate;
    return pcm;
}

int main(int argc, char **argv) {
    const char *model_path = (argc > 1) ? argv[1] : "models/sherpa-onnx-streaming-zipformer-en-2023-06-26";
    const char *wav_path = (argc > 2) ? argv[2] : "/tmp/opencode/test-speech.wav";

    const struct asr_engine_ops *ops = asr_engine_lookup("sherpa-onnx");
    if(ops == NULL || ops == &april_asr_ops) {
        printf("FAIL: asr_engine_lookup(\"sherpa-onnx\") wrong\n");
        return 2;
    }

    if(!ops->init()) {
        printf("FAIL: init\n");
        return 2;
    }

    struct asr_model *model = ops->model_load(model_path);
    if(model == NULL) {
        printf("FAIL: model_load\n");
        return 2;
    }

    printf("model: name=%s lang=%s desc=%s rate=%zu\n",
           ops->model_name(model), ops->model_language(model),
           ops->model_description(model), ops->model_sample_rate(model));

    struct asr_session *session = ops->session_create(model, true, handler, NULL);
    if(session == NULL) {
        printf("FAIL: session_create\n");
        return 2;
    }

    size_t count = 0;
    uint32_t sample_rate = 0;
    short *pcm = read_wav(wav_path, &count, &sample_rate);
    if(pcm == NULL) {
        printf("FAIL: read wav %s\n", wav_path);
        return 2;
    }

    printf("wav: %zu shorts at %u Hz\n", count, sample_rate);
    if(sample_rate != ops->model_sample_rate(model)) {
        printf("FAIL: wav sample rate %u != model rate %zu\n", sample_rate, ops->model_sample_rate(model));
        return 2;
    }

    /* Feed in 100 ms chunks, roughly realtime */
    size_t chunk = sample_rate / 10;
    for(size_t off = 0; off < count; off += chunk) {
        size_t n = (count - off < chunk) ? (count - off) : chunk;
        ops->session_feed_pcm16(session, pcm + off, n);
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    ops->session_flush(session);

    /* Give the decode thread time to process the flush */
    for(int i = 0; i < 50 && finals_received == 0; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    ops->session_free(session);
    ops->model_free(model);
    free(pcm);

    printf("summary: partials=%d finals=%d silences=%d\n",
           partials_received, finals_received, silences_received);

    if(finals_received == 0) {
        printf("FAIL: no FINAL received\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
