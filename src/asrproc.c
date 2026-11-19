/* asrproc.c
 * This file implements asr_thread which takes in audio, passes it to aprilasr,
 * and passes the output to transcript to update the caption text view.
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

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/mman.h>
#include <glib.h>
#include <time.h>

#include <stdbool.h>

#include "asrproc.h"
#include "asr-engine.h"
#include "transcript.h"
#include "livecaptions-window.h"
#include "livecaptions-application.h"
#include "history.h"
#include "common.h"

struct asr_thread_i {
    volatile size_t sound_counter;
    size_t silence_counter;

    struct transcript transcript;

    GMutex text_mutex;

    const struct asr_engine_ops *ops;
    struct asr_model *model;
    struct asr_session *session;

    LiveCaptionsWindow *window;

    volatile bool text_stream_active;
    volatile bool pause;

    bool errored;

    // Source ids of scheduled main-thread applies, guarded by text_mutex
    GPtrArray *pending_apply_sources;

    // Bumped whenever the transcript is cleared, so pending main-thread
    // applies scheduled before the clear are invalidated
    unsigned transcript_generation;
};

// Payload for a main-thread transcript apply
struct apply_payload {
    asr_thread data;
    int op;
    unsigned generation;
    guint source_id;
    GPtrArray *segments; // struct transcript_applied_segment, or NULL
};

static gboolean main_thread_apply(void *userdata);

static void free_applied_segment(gpointer p) {
    struct transcript_applied_segment *seg = p;
    g_free(seg->text);
    g_free(seg);
}

// Caller must hold text_mutex. Copies the current tail segments so the main
// thread can apply them even if the ASR thread renders again meanwhile.
static GPtrArray *copy_tail_segments(struct transcript *t) {
    GPtrArray *out = g_ptr_array_new_with_free_func(free_applied_segment);

    size_t count = transcript_segment_count(t);
    for(size_t i=0; i<count; i++){
        struct transcript_applied_segment *seg = g_new(struct transcript_applied_segment, 1);
        seg->text = g_strdup(transcript_segment_text(t, i));
        seg->alpha = transcript_segment_alpha(t, i);
        g_ptr_array_add(out, seg);
    }

    return out;
}

// Caller must hold text_mutex.
static void schedule_apply(asr_thread data, int op) {
    struct apply_payload *p = g_new(struct apply_payload, 1);
    p->data = data;
    p->op = op;
    p->generation = data->transcript_generation;
    p->segments = NULL;

    if(op == TRANSCRIPT_APPLY_TAIL || op == TRANSCRIPT_APPLY_FINAL)
        p->segments = copy_tail_segments(&data->transcript);

    p->source_id = g_idle_add(main_thread_apply, p);
    g_ptr_array_add(data->pending_apply_sources, GUINT_TO_POINTER(p->source_id));
}

static void free_apply_payload(struct apply_payload *p) {
    if(p->segments != NULL) g_ptr_array_unref(p->segments);
    g_free(p);
}

static gboolean main_thread_apply(void *userdata){
    struct apply_payload *p = userdata;
    asr_thread data = p->data;

    g_mutex_lock(&data->text_mutex);
    g_ptr_array_remove(data->pending_apply_sources, GUINT_TO_POINTER(p->source_id));
    bool stale = (p->generation != data->transcript_generation) || (data->window == NULL) || (data->pause);
    LiveCaptionsWindow *window = data->window;
    g_mutex_unlock(&data->text_mutex);

    if(!stale){
        livecaptions_window_apply_transcript(window, p->op, p->segments);

        if(data->text_stream_active) {
            LiveCaptionsApplication *application = LIVECAPTIONS_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
            if(application != NULL) {
                GtkTextBuffer *buffer = livecaptions_window_get_buffer(window);

                // Send the plaintext of the most recent text, capped to keep
                // the signal small
                #define TEXT_STREAM_MAX_CHARS 2000
                int end_offset = gtk_text_buffer_get_char_count(buffer);
                int start_offset = (end_offset > TEXT_STREAM_MAX_CHARS) ? (end_offset - TEXT_STREAM_MAX_CHARS) : 0;

                GtkTextIter start_iter, end_iter;
                gtk_text_buffer_get_iter_at_offset(buffer, &start_iter, start_offset);
                gtk_text_buffer_get_end_iter(buffer, &end_iter);

                gchar *text = gtk_text_buffer_get_text(buffer, &start_iter, &end_iter, FALSE);
                livecaptions_application_stream_text(application, text);
                g_free(text);
            }
        }
    }

    free_apply_payload(p);
    return G_SOURCE_REMOVE;
}

static gboolean warn_slow_on_main_thread(void *userdata){
    livecaptions_window_warn_slow(LIVECAPTIONS_WINDOW(userdata));
    return G_SOURCE_REMOVE;
}

static void asrproc_result_handler(void* userdata, enum asr_result_type result, size_t count, const struct asr_token *tokens) {
    asr_thread data = userdata;
    if((data->window == NULL) || (data->pause)) return;

    switch(result) {
        case ASR_RESULT_RECOGNITION_PARTIAL:
        case ASR_RESULT_RECOGNITION_FINAL:
        {
            g_mutex_lock(&data->text_mutex);

            // The model may have been swapped while we waited for the lock;
            // drop results for a paused/freeing session
            if(data->pause){
                g_mutex_unlock(&data->text_mutex);
                break;
            }

            transcript_update(&data->transcript, count, tokens);
            if(result == ASR_RESULT_RECOGNITION_FINAL) {
                transcript_finish_sentence(&data->transcript);
                commit_tokens_to_current_history(tokens, count);
            }

            schedule_apply(data, (result == ASR_RESULT_RECOGNITION_FINAL) ? TRANSCRIPT_APPLY_FINAL
                                                                            : TRANSCRIPT_APPLY_TAIL);

            g_mutex_unlock(&data->text_mutex);
            break;
        }

        case ASR_RESULT_ERROR_CANT_KEEP_UP: {
            g_mutex_lock(&data->text_mutex);
            LiveCaptionsWindow *window = data->window;
            g_mutex_unlock(&data->text_mutex);

            // Must run on the main thread, we're on the April proc thread
            if(window != NULL)
                g_main_context_invoke(NULL, warn_slow_on_main_thread, window);
            break;
        }

        case ASR_RESULT_SILENCE: {
            g_mutex_lock(&data->text_mutex);

            // The model may have been swapped while we waited for the lock;
            // drop results for a paused/freeing session
            if(data->pause){
                g_mutex_unlock(&data->text_mutex);
                break;
            }

            transcript_break_sentence(&data->transcript);
            save_silence_to_history();

            schedule_apply(data, TRANSCRIPT_APPLY_SILENCE);

            g_mutex_unlock(&data->text_mutex);
            break;
        }

        default:
            break;
    }
}

void asr_thread_enqueue_audio(asr_thread thread, short *data, size_t num_shorts) {
    if((thread->window == NULL) || thread->pause) return;
    if((thread->session == NULL) || (thread->model == NULL)) return;


    bool found_nonzero = false;
    for(size_t i=0; i<num_shorts; i++){
        if((data[i] > 16) || (data[i] < -16)){
            found_nonzero = true;
            break;
        }
    }

    thread->silence_counter = found_nonzero ? 0 : (thread->silence_counter + num_shorts);

    if(thread->silence_counter >= 24000){
        thread->silence_counter = 24000;
        return thread->ops->session_flush(thread->session);
    }

    thread->sound_counter += num_shorts;
    thread->ops->session_feed_pcm16(thread->session, data, num_shorts); // TODO?
}

gpointer asr_thread_get_model(asr_thread thread) {
    return thread->model;
}

gpointer asr_thread_get_session(asr_thread thread) {
    return thread->session;
}

void asr_thread_pause(asr_thread thread, bool pause) {
    thread->pause = pause;
}

void asr_thread_set_text_stream_active(asr_thread thread, bool active) {
    thread->text_stream_active = active;
}

int asr_thread_samplerate(asr_thread thread) {
    return (int)thread->ops->model_sample_rate(thread->model);
}

asr_thread create_asr_thread(void) {
    GSettings *settings = g_settings_new("net.sapples.LiveCaptions");

    char *engine_name = g_settings_get_string(settings, "active-engine");
    const struct asr_engine_ops *ops = asr_engine_lookup(engine_name);

    const char *model_key = (ops == &sherpa_onnx_ops) ? "sherpa-active-model" : "active-model";
    char *model_path = g_settings_get_string(settings, model_key);

    bool is_april = (ops == &april_asr_ops);
    if(is_april && (model_path == NULL || model_path[0] == '\0')) {
        g_free(model_path);
        model_path = g_strdup(GET_MODEL_PATH());
    }

    asr_thread data = create_asr_thread_for_engine(ops, model_path);

    if(data == NULL) {
        // The configured engine/model failed to load; fall back to the
        // built-in april model so the app never ends up without ASR
        if(!is_april)
            g_settings_set_string(settings, "active-engine", "april");
        g_settings_set_string(settings, "active-model", GET_MODEL_PATH());

        data = create_asr_thread_for_engine(&april_asr_ops, GET_MODEL_PATH());
    }

    g_free(model_path);
    g_free(engine_name);
    g_object_unref(G_OBJECT(settings));

    return data;
}

asr_thread create_asr_thread_for_engine(const struct asr_engine_ops *ops, const char *model_path){
    asr_thread data = calloc(1, sizeof(struct asr_thread_i));
    if(data == NULL) return NULL;

    data->ops = ops;

    if(!ops->init()){
        printf("Initializing ASR engine failed!\n");
        free(data);
        return NULL;
    }

    transcript_init(&data->transcript);
    data->pending_apply_sources = g_ptr_array_new();

    g_mutex_init(&data->text_mutex);

    if(!asr_thread_update_model(data, model_path)){
        if(ops != &april_asr_ops || !asr_thread_update_model(data, GET_MODEL_PATH())) {
            free_asr_thread(data);
            return NULL;
        }

        GSettings *settings = g_settings_new("net.sapples.LiveCaptions");
        g_settings_set_string(settings, "active-model", GET_MODEL_PATH());
        g_object_unref(G_OBJECT(settings));
    }

    data->text_stream_active = false;

    return data;
}

bool asr_thread_update_model_for_engine(asr_thread data, const struct asr_engine_ops *ops, const char *model_path) {
    ops->init();

    // Freeing model frees token list, which may be being accessed during
    // transcript rendering
    g_mutex_lock(&data->text_mutex);

    data->pause = true;

    const struct asr_engine_ops *old_ops = data->ops;
    struct asr_model *old_model = data->model;
    struct asr_session *old_session = data->session;

    data->model = NULL;
    data->session = NULL;

    g_mutex_unlock(&data->text_mutex);

    // Load the new model before freeing the old one, so a failed load can
    // fall back to the previously working model (possibly of the other
    // engine) and the app never ends up without ASR. Sessions created while
    // paused drop their results, so it is safe to create the new session
    // before swapping.
    struct asr_model *new_model = ops->model_load(model_path);
    struct asr_session *new_session = NULL;
    bool fallback = false;

    if(new_model == NULL) {
        printf("Loading model %s failed!\n", model_path);
        fallback = true;
    } else {
        new_session = ops->session_create(new_model, true, asrproc_result_handler, data);
        if(new_session == NULL) {
            printf("Creating session %s failed!\n", model_path);
            ops->model_free(new_model);
            new_model = NULL;
            fallback = true;
        }
    }

    if(fallback) {
        // Try to restore the previously working model (possibly of the
        // other engine) so the app never ends up without ASR
        struct asr_session *restored_session = NULL;
        if(old_model != NULL)
            restored_session = old_ops->session_create(old_model, true, asrproc_result_handler, data);

        if(restored_session != NULL) {
            g_mutex_lock(&data->text_mutex);
            data->ops = old_ops;
            data->model = old_model;
            data->session = restored_session;
            data->errored = false;
            data->pause = false;

            schedule_apply(data, TRANSCRIPT_APPLY_FINAL);
            g_mutex_unlock(&data->text_mutex);

            // The old session is replaced by the restored one; free it
            // outside the lock for the same reason as above
            if(old_session != NULL)
                old_ops->session_free(old_session);

            return false;
        }

        // Nothing worked; free what was there before (outside the lock)
        if(old_session != NULL)
            old_ops->session_free(old_session);

        if(old_model != NULL)
            old_ops->model_free(old_model);

        data->errored = true;
        return false;
    }

    // Free the old session/model outside the lock: with ASYNC_RT the April
    // proc thread may be inside asrproc_result_handler waiting on text_mutex,
    // so freeing while holding the lock would deadlock the session teardown
    if(old_session != NULL)
        old_ops->session_free(old_session);

    if(old_model != NULL)
        old_ops->model_free(old_model);

    {
        printf("\n-- Model metadata --\n");
        printf("Name: %s\n", ops->model_name(new_model));

        // The description string is owned by the backend; copy before
        // truncating for display
        char description_buf[128];
        snprintf(description_buf, sizeof(description_buf), "%s",
                 ops->model_description(new_model));
        for(size_t i=0; i < sizeof(description_buf) - 1; i++){
            if((description_buf[i] == ' ') && (description_buf[i+1] == 'D') && (description_buf[i+2] == 'i')){
                description_buf[i] = 0;
                break;
            }
        }
        printf("Description: %s\n", description_buf);
        printf("Language: %s\n", ops->model_language(new_model));
        printf("-- --\n\n");
    }

    transcript_set_language(&data->transcript, ops->model_language(new_model));

    g_mutex_lock(&data->text_mutex);

    data->ops = ops;
    data->model = new_model;
    data->session = new_session;

    data->errored = false;
    data->pause = false;

    transcript_finish_sentence(&data->transcript);

    // Freeze the tail so the new model's output starts on a clean tail
    schedule_apply(data, TRANSCRIPT_APPLY_FINAL);

    g_mutex_unlock(&data->text_mutex);

    return true;
}

bool asr_thread_update_model(asr_thread data, const char *model_path) {
    return asr_thread_update_model_for_engine(data, data->ops, model_path);
}

bool asr_thread_is_errored(asr_thread thread) {
    return thread->errored;
}

void asr_thread_set_main_window(asr_thread thread, LiveCaptionsWindow *window) {
    thread->window = window;
}

void asr_thread_flush(asr_thread thread) {
    if(thread->session == NULL) return;
    thread->ops->session_flush(thread->session);
}

void asr_thread_clear_transcript(asr_thread thread) {
    g_mutex_lock(&thread->text_mutex);

    thread->transcript_generation++;
    transcript_reset(&thread->transcript);

    g_mutex_unlock(&thread->text_mutex);
}

void free_asr_thread(asr_thread thread) {
    g_mutex_lock(&thread->text_mutex);

    // Cancel scheduled main-thread applies so they never touch the freed
    // thread data
    for(guint i=0; i<thread->pending_apply_sources->len; i++)
        g_source_remove(GPOINTER_TO_UINT(thread->pending_apply_sources->pdata[i]));

    g_ptr_array_set_size(thread->pending_apply_sources, 0);

    thread->pause = true;

    struct asr_model *old_model = thread->model;
    struct asr_session *old_session = thread->session;

    thread->model = NULL;
    thread->session = NULL;

    g_mutex_unlock(&thread->text_mutex);

    // Free outside the lock: with ASYNC_RT the April proc thread may be
    // inside asrproc_result_handler waiting on text_mutex, so freeing while
    // holding the lock would deadlock the session teardown
    if(old_session != NULL)
        thread->ops->session_free(old_session);

    if(old_model != NULL)
        thread->ops->model_free(old_model);

    g_ptr_array_unref(thread->pending_apply_sources);

    transcript_deinit(&thread->transcript);

    g_mutex_clear(&thread->text_mutex);

    free(thread);
}
