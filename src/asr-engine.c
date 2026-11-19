/* asr-engine.c
 * This file implements generic helpers that dispatch through a loaded
 * model/session handle to the backend that created it.
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

#include <string.h>

#include "asr-engine.h"

const struct asr_engine_ops *asr_engine_lookup(const char *name) {
    if(name == NULL) return &april_asr_ops;

    if(strcmp(name, "april") == 0) return &april_asr_ops;
    if(strcmp(name, "sherpa-onnx") == 0) return &sherpa_onnx_ops;

    return &april_asr_ops;
}

struct asr_session *asr_session_create(struct asr_model *model,
                                       bool async_realtime,
                                       asr_result_handler handler,
                                       void *userdata)
{
    if(model == NULL) return NULL;
    return model->ops->session_create(model, async_realtime, handler, userdata);
}

void asr_session_free(struct asr_session *session) {
    if(session == NULL) return;
    session->ops->session_free(session);
}

void asr_session_feed_pcm16(struct asr_session *session, short *pcm16, size_t short_count) {
    if(session == NULL) return;
    session->ops->session_feed_pcm16(session, pcm16, short_count);
}

float asr_session_speedup(struct asr_session *session) {
    if(session == NULL) return 0.0f;
    return session->ops->session_speedup(session);
}

size_t asr_model_sample_rate(struct asr_model *model) {
    if(model == NULL) return 0;
    return model->ops->model_sample_rate(model);
}
