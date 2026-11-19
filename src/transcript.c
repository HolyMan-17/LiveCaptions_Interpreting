/* transcript.c
 * This file contains the implementation for transcript, which renders the
 * in-progress (tail) portion of the current sentence into reusable text
 * segments, applying capitalization, profanity filtering and confidence fade.
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
#include <string.h>
#include <glib.h>
#include <gio/gio.h>
#include <stdbool.h>

#include "transcript.h"
#include "profanity-filter.h"
#include "common.h"

typedef struct transcript_segment {
    GString *text;
    int alpha;
} transcript_segment;

static GSettings *settings = NULL;

void transcript_init(struct transcript *t) {
    if(settings == NULL) settings = g_settings_new("net.sapples.LiveCaptions");

    t->segs = g_ptr_array_new();
    t->n_segs = 0;

    transcript_reset(t);
}

void transcript_deinit(struct transcript *t) {
    for(guint i=0; i<t->segs->len; i++){
        transcript_segment *seg = g_ptr_array_index(t->segs, i);
        g_string_free(seg->text, TRUE);
        g_free(seg);
    }

    g_ptr_array_unref(t->segs);
    t->segs = NULL;
    t->n_segs = 0;
}

void transcript_reset(struct transcript *t) {
    token_capitalizer_init(&t->tcap);
    t->tcap_saved = t->tcap;
    t->n_segs = 0;
}

void transcript_set_language(struct transcript *t, const char *language) {
    t->tcap.is_english = (language[0] == 'e') && (language[1] == 'n');
    t->tcap_saved.is_english = t->tcap.is_english;
}

static transcript_segment *transcript_segment_at(struct transcript *t, size_t index) {
    if(index < t->segs->len) return g_ptr_array_index(t->segs, index);

    transcript_segment *seg = g_new0(transcript_segment, 1);
    seg->text = g_string_new(NULL);
    seg->alpha = 0;
    g_ptr_array_add(t->segs, seg);
    return seg;
}

#define MAX_TOKEN_SCRATCH 72

void transcript_update(struct transcript *t, size_t num_tokens, const struct asr_token *tokens) {
    // The capitalizer must run from the state at the start of the current
    // sentence, since the whole tail is re-rendered from scratch
    t->tcap = t->tcap_saved;

    static bool should_capitalize[1024];
    if(num_tokens > 1024) num_tokens = 1024;

    for(size_t i=0; i<num_tokens; i++){
        if((i+1) < num_tokens) {
            should_capitalize[i] = token_capitalizer_next(&t->tcap, tokens[i].text, tokens[i].flags, tokens[i+1].text, tokens[i+1].flags);
        }else{
            should_capitalize[i] = token_capitalizer_next(&t->tcap, tokens[i].text, tokens[i].flags, NULL, 0);
        }
    }

    bool use_fade = g_settings_get_boolean(settings, "fade-text");

    bool filter_slurs = g_settings_get_boolean(settings, "filter-slurs");
    bool filter_profanity = g_settings_get_boolean(settings, "filter-profanity");

    FilterMode filter_mode = filter_profanity ? FILTER_PROFANITY : (filter_slurs ? FILTER_SLURS : FILTER_NONE);

    bool use_lowercase = !g_settings_get_boolean(settings, "text-uppercase");
    char token_scratch[MAX_TOKEN_SCRATCH];

    t->n_segs = 0;

    for(size_t j=0; j<num_tokens;){
        size_t skipahead = 1;
        const char *token = tokens[j].text;

        bool should_be_capitalized = should_capitalize[j];

        if(use_lowercase){
            char *out = token_scratch;
            const char *p = tokens[j].text;
            gunichar c;
            while (*p) {
                c = g_utf8_get_char_validated(p, -1);
                if(c == ((gunichar)-2)) {
                    printf("gunichar -2 \n");
                    break;
                }else if(c == ((gunichar)-1)) {
                    printf("gunichar -1 \n");
                    break;
                }

                c = g_unichar_tolower(c);

                if(should_be_capitalized){
                    gunichar c1 = g_unichar_toupper(c);
                    if(c != c1){
                        c = c1;
                        should_be_capitalized = false;
                    }
                }

                out += g_unichar_to_utf8(c, out);
                if((out + 6) >= (token_scratch + MAX_TOKEN_SCRATCH)){
                    printf("Unicode too big for token scratch!\n");
                    break;
                }

                p = g_utf8_next_char(p);
            }

            *out = '\0';

            token = token_scratch;
        }

        // filter current word, if applicable
        if((filter_mode > FILTER_NONE) && (tokens[j].flags & ASR_TOKEN_FLAG_WORD_BOUNDARY)) {
            size_t skip = get_filter_skip(tokens, j, num_tokens, filter_mode);
            if(skip > 0) {
                skipahead = skip;
                token = SWEAR_REPLACEMENT;
            }
        }

        int alpha = 0;
        if(use_fade){
            alpha = (int)((tokens[j].logprob + 2.0) / 8.0 * 65536.0);
            alpha /= 2.0;
            alpha += 32768;
            if(alpha < 10000) alpha = 10000;
            if(alpha > 65535) alpha = 65535;
        }

        // Append the token to the tail segments, starting a new segment when
        // the fade alpha changes so the main thread can apply tags per segment
        transcript_segment *seg = NULL;
        if(t->n_segs > 0){
            seg = transcript_segment_at(t, t->n_segs - 1);
            if(seg->alpha != alpha) seg = NULL;
        }

        if(seg == NULL){
            seg = transcript_segment_at(t, t->n_segs);
            g_string_truncate(seg->text, 0);
            seg->alpha = alpha;
            t->n_segs++;
        }

        g_string_append(seg->text, token);

        j += skipahead;
    }
}

void transcript_finish_sentence(struct transcript *t) {
    token_capitalizer_finish(&t->tcap);
    token_capitalizer_rewind(&t->tcap);
    t->tcap_saved = t->tcap;
}

void transcript_break_sentence(struct transcript *t) {
    // The in-progress tail becomes a sentence boundary; ensure the next
    // sentence starts with a capitalized word, like after a finalized sentence
    t->tcap.previous_was_period = true;
    t->tcap.finished_at_period = true;
    t->tcap.force_next_cap = false;
    t->tcap_saved = t->tcap;
}

size_t transcript_segment_count(const struct transcript *t) {
    return t->n_segs;
}

const char *transcript_segment_text(const struct transcript *t, size_t index) {
    return ((const transcript_segment *)g_ptr_array_index(t->segs, index))->text->str;
}

int transcript_segment_alpha(const struct transcript *t, size_t index) {
    return ((const transcript_segment *)g_ptr_array_index(t->segs, index))->alpha;
}
