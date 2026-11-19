/* asr-engine-april.c
 * This file implements the april-asr backend of the engine-agnostic ASR
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

#include <april_api.h>

#include "asr-engine.h"

struct april_model {
    struct asr_model base;
    AprilASRModel model;
};

struct april_session {
    struct asr_session base;
    AprilASRSession session;

    asr_result_handler handler;
    void *userdata;

    /* Token scratch buffer; tokens passed to the handler live until the
       next callback */
    struct asr_token *scratch;
    size_t scratch_count;
};

static bool april_init(void) {
    static bool done = false;
    if(done) return true;

    aam_api_init(APRIL_VERSION);
    done = true;
    return true;
}

static struct asr_model *april_model_load(const char *model_path) {
    AprilASRModel model = aam_create_model(model_path);
    if(model == NULL) return NULL;

    struct april_model *m = calloc(1, sizeof(struct april_model));
    if(m == NULL) {
        aam_free(model);
        return NULL;
    }

    m->base.ops = &april_asr_ops;
    m->model = model;

    return &m->base;
}

static void april_model_free(struct asr_model *model) {
    struct april_model *m = (struct april_model *)model;

    aam_free(m->model);
    free(m);
}

static const char *april_model_name(struct asr_model *model) {
    return aam_get_name(((struct april_model *)model)->model);
}

static const char *april_model_description(struct asr_model *model) {
    return aam_get_description(((struct april_model *)model)->model);
}

static const char *april_model_language(struct asr_model *model) {
    return aam_get_language(((struct april_model *)model)->model);
}

static size_t april_model_sample_rate(struct asr_model *model) {
    return aam_get_sample_rate(((struct april_model *)model)->model);
}

static void april_handler(void *userdata, AprilResultType result, size_t count, const AprilToken *tokens) {
    struct april_session *s = userdata;

    enum asr_result_type mapped;
    switch(result) {
        case APRIL_RESULT_RECOGNITION_PARTIAL:
            mapped = ASR_RESULT_RECOGNITION_PARTIAL;
            break;
        case APRIL_RESULT_RECOGNITION_FINAL:
            mapped = ASR_RESULT_RECOGNITION_FINAL;
            break;
        case APRIL_RESULT_SILENCE:
            mapped = ASR_RESULT_SILENCE;
            break;
        case APRIL_RESULT_ERROR_CANT_KEEP_UP:
            mapped = ASR_RESULT_ERROR_CANT_KEEP_UP;
            break;
        default:
            return;
    }

    if((tokens == NULL) || (count == 0)) {
        s->handler(s->userdata, mapped, 0, NULL);
        return;
    }

    if(count > s->scratch_count) {
        struct asr_token *scratch = realloc(s->scratch, count * sizeof(struct asr_token));
        if(scratch == NULL) return;
        s->scratch = scratch;
        s->scratch_count = count;
    }

    for(size_t i=0; i<count; i++){
        s->scratch[i].text    = tokens[i].token;
        s->scratch[i].flags   = (int)tokens[i].flags;
        s->scratch[i].logprob = tokens[i].logprob;
    }

    s->handler(s->userdata, mapped, count, s->scratch);
}

static struct asr_session *april_session_create(struct asr_model *model,
                                                bool async_realtime,
                                                asr_result_handler handler,
                                                void *userdata)
{
    struct april_model *m = (struct april_model *)model;

    struct april_session *s = calloc(1, sizeof(struct april_session));
    if(s == NULL) return NULL;

    AprilConfig config = {
        .handler = april_handler,
        .userdata = s,
        .flags = async_realtime ? APRIL_CONFIG_FLAG_ASYNC_RT_BIT : APRIL_CONFIG_FLAG_ZERO_BIT
    };

    AprilASRSession session = aas_create_session(m->model, config);
    if(session == NULL) {
        free(s);
        return NULL;
    }

    s->base.ops = &april_asr_ops;
    s->session = session;
    s->handler = handler;
    s->userdata = userdata;

    return &s->base;
}

static void april_session_free(struct asr_session *session) {
    struct april_session *s = (struct april_session *)session;

    aas_free(s->session);
    free(s->scratch);
    free(s);
}

static void april_session_feed_pcm16(struct asr_session *session, short *pcm16, size_t short_count) {
    aas_feed_pcm16(((struct april_session *)session)->session, pcm16, short_count);
}

static void april_session_flush(struct asr_session *session) {
    aas_flush(((struct april_session *)session)->session);
}

static float april_session_speedup(struct asr_session *session) {
    return aas_realtime_get_speedup(((struct april_session *)session)->session);
}

const struct asr_engine_ops april_asr_ops = {
    .init = april_init,

    .model_load = april_model_load,
    .model_free = april_model_free,

    .model_name = april_model_name,
    .model_description = april_model_description,
    .model_language = april_model_language,
    .model_sample_rate = april_model_sample_rate,

    .session_create = april_session_create,
    .session_free = april_session_free,
    .session_feed_pcm16 = april_session_feed_pcm16,
    .session_flush = april_session_flush,
    .session_speedup = april_session_speedup,
};
