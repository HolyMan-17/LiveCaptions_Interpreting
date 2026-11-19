/* asr-engine-sherpa.c
 * This file implements the sherpa-onnx backend of the engine-agnostic ASR
 * interface declared in asr-engine.h.
 *
 * Copyright 2022 abb128
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>

#include <sherpa-onnx/c-api/c-api.h>

#include "asr-engine.h"
#include "asr-token.h"

struct sherpa_model {
    struct asr_model base;
    const SherpaOnnxOnlineRecognizer *recognizer;

    char *name;
    size_t sample_rate;
};

/* The sherpa-onnx online recognizer is synchronous; a decode thread runs the
   recognizer so feed_pcm16 can be called from the audio thread without
   blocking */
struct sherpa_session {
    struct asr_session base;
    struct sherpa_model *model;

    asr_result_handler handler;
    void *userdata;

    const SherpaOnnxOnlineStream *stream;

    /* Ring buffer of float samples fed by feed_pcm16, drained by the decode
       thread. read/write are absolute sample indices. */
    pthread_mutex_t buf_mutex;
    pthread_cond_t buf_cond;
    float *buf;
    size_t buf_capacity;
    size_t buf_read;
    size_t buf_write;
    bool drop_logged;

    /* Requests from the feeder to the decode thread */
    bool flush_requested;
    bool thread_stop;

    pthread_t thread;
    bool thread_started;

    /* Token scratch passed to the handler; texts live until the next
       callback */
    struct asr_token *scratch;
    char **scratch_text;
    size_t scratch_count;

    /* Last partial text, so we only emit on change */
    char *last_text;
};

static const char *SHERPA_MODEL_DESCRIPTION = "Streaming zipformer";

const struct asr_engine_ops sherpa_onnx_ops;

/* Pick the file in dir starting with prefix and ending with .onnx, skipping
   int8 variants. Returns a malloc'd path or NULL. */
static char *sherpa_find_onnx(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    if(d == NULL) return NULL;

    char *result = NULL;
    char *int8_result = NULL;
    struct dirent *entry;
    while((entry = readdir(d)) != NULL){
        size_t len = strlen(entry->d_name);
        if(strlen(prefix) >= len) continue;
        if(strncmp(entry->d_name, prefix, strlen(prefix)) != 0) continue;

        const char *ext = entry->d_name + len - 5;
        if(strcmp(ext, ".onnx") != 0) continue;

        size_t total = strlen(dir) + 1 + len + 1;
        if(strstr(entry->d_name, ".int8.onnx") != NULL){
            // Nemotron-style models only ship int8 files ("encoder.int8.onnx")
            if(int8_result == NULL){
                int8_result = malloc(total);
                snprintf(int8_result, total, "%s/%s", dir, entry->d_name);
            }
            continue;
        }

        // Prefer int8 (lighter RAM, equivalent accuracy); fall back to fp32
        if(result == NULL){
            result = malloc(total);
            snprintf(result, total, "%s/%s", dir, entry->d_name);
        }
    }

    closedir(d);

    if(int8_result == NULL) return result;
    free(result);
    return int8_result;
}

static bool sherpa_file_exists(const char *dir, const char *name) {
    DIR *d = opendir(dir);
    if(d == NULL) return false;

    bool found = false;
    struct dirent *entry;
    while((entry = readdir(d)) != NULL){
        if(strcmp(entry->d_name, name) == 0){
            found = true;
            break;
        }
    }

    closedir(d);
    return found;
}

static char *sherpa_path(const char *dir, const char *name) {
    size_t total = strlen(dir) + 1 + strlen(name) + 1;
    char *result = malloc(total);
    snprintf(result, total, "%s/%s", dir, name);
    return result;
}

static bool sherpa_init(void) {
    static bool done = false;
    if(done) return true;

    done = true;
    return true;
}

static struct asr_model *sherpa_model_load(const char *model_path) {
    if(!sherpa_file_exists(model_path, "tokens.txt")) {
        printf("sherpa-onnx: %s does not contain tokens.txt\n", model_path);
        return NULL;
    }

    char *encoder = sherpa_find_onnx(model_path, "encoder");
    char *decoder = sherpa_find_onnx(model_path, "decoder");
    char *joiner  = sherpa_find_onnx(model_path, "joiner");
    char *tokens  = sherpa_path(model_path, "tokens.txt");

    if((encoder == NULL) || (decoder == NULL) || (joiner == NULL)) {
        printf("sherpa-onnx: could not find encoder/decoder/joiner onnx files in %s\n", model_path);
        free(encoder);
        free(decoder);
        free(joiner);
        free(tokens);
        return NULL;
    }

    SherpaOnnxOnlineRecognizerConfig config;
    memset(&config, 0, sizeof(config));

    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;

    config.model_config.transducer.encoder = encoder;
    config.model_config.transducer.decoder = decoder;
    config.model_config.transducer.joiner  = joiner;
    config.model_config.tokens             = tokens;
    config.model_config.num_threads        = 1;
    config.model_config.provider           = "cpu";
    /* modeling_unit is only needed for hotwords tokenization; leaving it
       unset avoids the bpe_vocab requirement in sherpa config validation */

    config.decoding_method = "greedy_search";
    config.enable_endpoint = 1;
    config.rule1_min_trailing_silence = 2.0f;
    /* Disable the other endpoint rules */
    config.rule2_min_trailing_silence = 100000.0f;
    config.rule3_min_utterance_length = 100000.0f;

    const SherpaOnnxOnlineRecognizer *recognizer = SherpaOnnxCreateOnlineRecognizer(&config);

    if(recognizer == NULL) {
        printf("sherpa-onnx: failed to create recognizer from %s\n", model_path);
        free(encoder);
        free(decoder);
        free(joiner);
        free(tokens);
        return NULL;
    }

    free(encoder);
    free(decoder);
    free(joiner);
    free(tokens);

    struct sherpa_model *m = calloc(1, sizeof(struct sherpa_model));
    if(m == NULL) {
        SherpaOnnxDestroyOnlineRecognizer(recognizer);
        return NULL;
    }

    /* Name is the model directory basename */
    const char *base = strrchr(model_path, '/');
    base = (base == NULL) ? model_path : (base + 1);
    m->name = strdup(base);

    m->base.ops        = &sherpa_onnx_ops;
    m->recognizer      = recognizer;
    m->sample_rate     = 16000;

    return &m->base;
}

static void sherpa_model_free(struct asr_model *model) {
    struct sherpa_model *m = (struct sherpa_model *)model;

    SherpaOnnxDestroyOnlineRecognizer(m->recognizer);
    free(m->name);
    free(m);
}

static const char *sherpa_model_name(struct asr_model *model) {
    return ((struct sherpa_model *)model)->name;
}

static const char *sherpa_model_description(struct asr_model *model) {
    (void)model;
    return SHERPA_MODEL_DESCRIPTION;
}

static const char *sherpa_model_language(struct asr_model *model) {
    (void)model;
    return "en";
}

static size_t sherpa_model_sample_rate(struct asr_model *model) {
    return ((struct sherpa_model *)model)->sample_rate;
}

/* Split the result text into one token per word, " word" with a leading
   space like april's tokens */
static size_t sherpa_fill_tokens(struct sherpa_session *s, const char *text) {
    size_t count = 0;

    const char *p = text;
    while(*p != '\0'){
        while(*p == ' ') p++;
        if(*p == '\0') break;

        const char *start = p;
        while((*p != '\0') && (*p != ' ')) p++;

        size_t word_len = (size_t)(p - start);

        if(count >= s->scratch_count) {
            size_t new_count = (s->scratch_count == 0) ? 16 : s->scratch_count * 2;
            struct asr_token *scratch = realloc(s->scratch, new_count * sizeof(struct asr_token));
            if(scratch == NULL) break;
            s->scratch = scratch;

            char **text_arr = realloc(s->scratch_text, new_count * sizeof(char *));
            if(text_arr == NULL) break;
            s->scratch_text = text_arr;

            for(size_t i = s->scratch_count; i < new_count; i++)
                s->scratch_text[i] = NULL;

            s->scratch_count = new_count;
        }

        char *buf = s->scratch_text[count];
        if((buf == NULL) || (strlen(buf) < word_len + 1)) {
            free(buf);
            buf = malloc(word_len + 2);
            if(buf == NULL) break;
            s->scratch_text[count] = buf;
        }

        buf[0] = ' ';
        memcpy(buf + 1, start, word_len);
        buf[word_len + 1] = '\0';

        s->scratch[count].text    = buf;
        s->scratch[count].flags   = ASR_TOKEN_FLAG_WORD_BOUNDARY;
        s->scratch[count].logprob = 10.0f;

        count++;
    }

    return count;
}

static void sherpa_emit(struct sherpa_session *s, enum asr_result_type result, const char *text) {
    if(result == ASR_RESULT_SILENCE) {
        s->handler(s->userdata, ASR_RESULT_SILENCE, 0, NULL);
        return;
    }

    if((text == NULL) || (text[0] == '\0')) return;

    size_t count = sherpa_fill_tokens(s, text);
    if(count == 0) return;

    s->handler(s->userdata, result, count, s->scratch);
}

/* Emit FINAL for the current text (if any), then SILENCE, and reset the
   stream so the next utterance starts clean */
static void sherpa_finish_utterance(struct sherpa_session *s, const char *text) {
    if((text != NULL) && (text[0] != '\0'))
        sherpa_emit(s, ASR_RESULT_RECOGNITION_FINAL, text);

    /* Clear the last partial so a stale text can't suppress the next
       utterance's first PARTIAL after the reset */
    free(s->last_text);
    s->last_text = NULL;

    sherpa_emit(s, ASR_RESULT_SILENCE, NULL);

    SherpaOnnxOnlineStreamReset(s->model->recognizer, s->stream);
}

static void *sherpa_decode_thread(void *userdata) {
    struct sherpa_session *s = userdata;
    const SherpaOnnxOnlineRecognizer *recognizer = s->model->recognizer;

    float chunk[1600]; /* 0.1 s of 16 kHz audio */

    while(true) {
        pthread_mutex_lock(&s->buf_mutex);

        while((s->buf_read == s->buf_write) && !s->flush_requested && !s->thread_stop)
            pthread_cond_wait(&s->buf_cond, &s->buf_mutex);

        bool stop = s->thread_stop;
        bool flush_now = s->flush_requested && (s->buf_read == s->buf_write);
        if(flush_now) s->flush_requested = false;

        size_t available = s->buf_write - s->buf_read;
        if(available > sizeof(chunk) / sizeof(chunk[0]))
            available = sizeof(chunk) / sizeof(chunk[0]);

        size_t idx = s->buf_read % s->buf_capacity;
        size_t first = (idx + available <= s->buf_capacity) ? available : (s->buf_capacity - idx);
        memcpy(chunk, s->buf + idx, first * sizeof(float));
        if(available > first)
            memcpy(chunk + first, s->buf, (available - first) * sizeof(float));
        s->buf_read += available;

        pthread_cond_broadcast(&s->buf_cond);
        pthread_mutex_unlock(&s->buf_mutex);

        if(available > 0)
            SherpaOnnxOnlineStreamAcceptWaveform(s->stream, 16000, chunk, (int32_t)available);

        while(SherpaOnnxIsOnlineStreamReady(recognizer, s->stream))
            SherpaOnnxDecodeOnlineStream(recognizer, s->stream);

        const SherpaOnnxOnlineRecognizerResult *result =
            SherpaOnnxGetOnlineStreamResult(recognizer, s->stream);

        const char *text = (result != NULL) ? result->text : NULL;

        bool text_changed = (text != NULL) &&
                            ((s->last_text == NULL) || (strcmp(s->last_text, text) != 0));

        if(!flush_now && !stop && text_changed) {
            free(s->last_text);
            s->last_text = strdup(text);
            sherpa_emit(s, ASR_RESULT_RECOGNITION_PARTIAL, text);
        }

        bool endpoint = (SherpaOnnxOnlineStreamIsEndpoint(recognizer, s->stream) != 0);
        if(endpoint && !stop)
            sherpa_finish_utterance(s, text);

        if(flush_now && !stop) {
            /* The stream may hold samples that were not enough for another
               decode step; push some silence through so everything gets
               decoded, then finalize */
            static float silence[16000 / 4];
            memset(silence, 0, sizeof(silence));
            SherpaOnnxOnlineStreamAcceptWaveform(s->stream, 16000, silence, sizeof(silence) / sizeof(silence[0]));

            while(SherpaOnnxIsOnlineStreamReady(recognizer, s->stream))
                SherpaOnnxDecodeOnlineStream(recognizer, s->stream);

            const SherpaOnnxOnlineRecognizerResult *flush_result =
                SherpaOnnxGetOnlineStreamResult(recognizer, s->stream);

            const char *flush_text = (flush_result != NULL) ? flush_result->text : NULL;
            sherpa_finish_utterance(s, flush_text);

            if(flush_result != NULL)
                SherpaOnnxDestroyOnlineRecognizerResult(flush_result);
        }

        if(result != NULL)
            SherpaOnnxDestroyOnlineRecognizerResult(result);

        if(stop) break;
    }

    return NULL;
}

static struct asr_session *sherpa_session_create(struct asr_model *model,
                                                 bool async_realtime,
                                                 asr_result_handler handler,
                                                 void *userdata)
{
    struct sherpa_model *m = (struct sherpa_model *)model;

    struct sherpa_session *s = calloc(1, sizeof(struct sherpa_session));
    if(s == NULL) return NULL;

    s->base.ops = &sherpa_onnx_ops;
    s->model = m;
    s->handler = handler;
    s->userdata = userdata;

    s->stream = SherpaOnnxCreateOnlineStream(m->recognizer);
    if(s->stream == NULL) {
        free(s);
        return NULL;
    }

    s->buf_capacity = m->sample_rate * 10; /* 10 s of audio */
    s->buf = malloc(s->buf_capacity * sizeof(float));
    if(s->buf == NULL) {
        SherpaOnnxDestroyOnlineStream(s->stream);
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->buf_mutex, NULL);
    pthread_cond_init(&s->buf_cond, NULL);

    (void)async_realtime;

    int err = pthread_create(&s->thread, NULL, sherpa_decode_thread, s);
    if(err != 0) {
        pthread_mutex_destroy(&s->buf_mutex);
        pthread_cond_destroy(&s->buf_cond);
        free(s->buf);
        SherpaOnnxDestroyOnlineStream(s->stream);
        free(s);
        return NULL;
    }

    s->thread_started = true;

    return &s->base;
}

static void sherpa_session_free(struct asr_session *session) {
    struct sherpa_session *s = (struct sherpa_session *)session;

    /* Stop the decode thread before touching anything it uses */
    if(s->thread_started) {
        pthread_mutex_lock(&s->buf_mutex);
        s->thread_stop = true;
        pthread_cond_broadcast(&s->buf_cond);
        pthread_mutex_unlock(&s->buf_mutex);

        pthread_join(s->thread, NULL);
    }

    SherpaOnnxDestroyOnlineStream(s->stream);

    for(size_t i=0; i<s->scratch_count; i++)
        free(s->scratch_text[i]);
    free(s->scratch_text);
    free(s->scratch);
    free(s->last_text);

    free(s->buf);

    pthread_mutex_destroy(&s->buf_mutex);
    pthread_cond_destroy(&s->buf_cond);

    free(s);
}

static void sherpa_session_feed_pcm16(struct asr_session *session, short *pcm16, size_t short_count) {
    struct sherpa_session *s = (struct sherpa_session *)session;

    pthread_mutex_lock(&s->buf_mutex);

    /* Drop audio that arrives while the buffer is full; the decode thread
       normally keeps up well (streaming zipformer RTF is far below 1) */
    size_t used = s->buf_write - s->buf_read;
    size_t space = s->buf_capacity - used;

    size_t feed = short_count;
    if(feed > space) {
        feed = space;
        if(!s->drop_logged) {
            printf("sherpa-onnx: ring buffer full, dropping audio\n");
            s->drop_logged = true;
        }
    }

    for(size_t i=0; i<feed; i++) {
        s->buf[s->buf_write % s->buf_capacity] = pcm16[i] / 32768.0f;
        s->buf_write++;
    }

    pthread_cond_signal(&s->buf_cond);
    pthread_mutex_unlock(&s->buf_mutex);
}

static void sherpa_session_flush(struct asr_session *session) {
    struct sherpa_session *s = (struct sherpa_session *)session;

    pthread_mutex_lock(&s->buf_mutex);
    s->flush_requested = true;
    pthread_cond_broadcast(&s->buf_cond);
    pthread_mutex_unlock(&s->buf_mutex);
}

static float sherpa_session_speedup(struct asr_session *session) {
    (void)session;
    /* No speedup mechanism; the recognizer is fast enough */
    return 0.0f;
}

const struct asr_engine_ops sherpa_onnx_ops = {
    .init = sherpa_init,

    .model_load = sherpa_model_load,
    .model_free = sherpa_model_free,

    .model_name = sherpa_model_name,
    .model_description = sherpa_model_description,
    .model_language = sherpa_model_language,
    .model_sample_rate = sherpa_model_sample_rate,

    .session_create = sherpa_session_create,
    .session_free = sherpa_session_free,
    .session_feed_pcm16 = sherpa_session_feed_pcm16,
    .session_flush = sherpa_session_flush,
    .session_speedup = sherpa_session_speedup,
};
