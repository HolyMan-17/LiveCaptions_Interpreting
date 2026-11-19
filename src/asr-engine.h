/* asr-engine.h
 * This file declares the engine-agnostic interface between the application
 * and the ASR engine backend, along with generic helpers that dispatch
 * through a loaded model/session handle.
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

#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "asr-token.h"

enum asr_result_type {
    ASR_RESULT_UNKNOWN = 0,

    /* The result is only partial; a future call will contain much of the
       same text but updated */
    ASR_RESULT_RECOGNITION_PARTIAL = 1,

    /* The result is final; future calls will start from empty and will not
       contain any of the given text */
    ASR_RESULT_RECOGNITION_FINAL = 2,

    /* There has been some silence; called with count = 0, tokens = NULL */
    ASR_RESULT_SILENCE = 3,

    /* The engine can't keep up; called with count = 0, tokens = NULL */
    ASR_RESULT_ERROR_CANT_KEEP_UP = 4,
};

/* (void *userdata, enum asr_result_type result, size_t count,
    const struct asr_token *tokens);
   May be called from a different thread. The tokens pointer is only valid
   for the duration of the call. */
typedef void(*asr_result_handler)(void*, enum asr_result_type, size_t, const struct asr_token*);

/* Handles returned by a backend. The first member is the ops table used
   for generic dispatch, so backends embed these and cast. */
struct asr_model {
    const struct asr_engine_ops *ops;
};

struct asr_session {
    const struct asr_engine_ops *ops;
};

struct asr_engine_ops {
    /* Must be called once before any other function of the backend.
       Returns false if initialization failed. */
    bool (*init)(void);

    /* Creates a model given a path. Returns NULL if loading failed. */
    struct asr_model *(*model_load)(const char *model_path);
    void (*model_free)(struct asr_model *model);

    /* Get the name/desc/lang of the model. The pointers are valid for the
       lifetime of the model (i.e. until model_free is called on it) */
    const char *(*model_name)(struct asr_model *model);
    const char *(*model_description)(struct asr_model *model);
    const char *(*model_language)(struct asr_model *model);

    /* Get the sample rate of the model in Hz */
    size_t (*model_sample_rate)(struct asr_model *model);

    /* Creates a session with the given model. A model may have many
       sessions associated with it. If async_realtime is set, the handler
       may be called from a background thread. Returns NULL on failure. */
    struct asr_session *(*session_create)(struct asr_model *model,
                                          bool async_realtime,
                                          asr_result_handler handler,
                                          void *userdata);
    void (*session_free)(struct asr_session *session);

    /* Feed PCM16 audio data to the session, must be single-channel and
       sampled to the sample rate given by model_sample_rate.
       short_count is the number of shorts, not bytes! */
    void (*session_feed_pcm16)(struct asr_session *session, short *pcm16, size_t short_count);

    /* Processes any unprocessed samples and produces a final result. */
    void (*session_flush)(struct asr_session *session);

    /* May return a number describing how much audio is being sped up to
       keep up with realtime. If the number is 1.0 or below, audio is not
       being sped up. */
    float (*session_speedup)(struct asr_session *session);
};

/* The april-asr backend */
extern const struct asr_engine_ops april_asr_ops;

/* The sherpa-onnx backend */
extern const struct asr_engine_ops sherpa_onnx_ops;

/* Look up an engine by name ("april", "sherpa-onnx"); unknown names fall
   back to the april backend */
const struct asr_engine_ops *asr_engine_lookup(const char *name);

/* Generic helpers dispatching through the model/session handles */
struct asr_session *asr_session_create(struct asr_model *model,
                                       bool async_realtime,
                                       asr_result_handler handler,
                                       void *userdata);
void asr_session_free(struct asr_session *session);
void asr_session_feed_pcm16(struct asr_session *session, short *pcm16, size_t short_count);
float asr_session_speedup(struct asr_session *session);
size_t asr_model_sample_rate(struct asr_model *model);
