/* transcript.h
 * This file contains the declaration for transcript, which renders the
 * in-progress (tail) portion of the current sentence into reusable text
 * segments for display in the transcript GtkTextView.
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
#include <glib.h>

#include "line-gen.h"
#include "asr-token.h"

// What kind of edit the main thread should perform on the transcript buffer
enum transcript_apply_op {
    TRANSCRIPT_APPLY_TAIL = 1,   // rewrite the tail from the given segments
    TRANSCRIPT_APPLY_FINAL = 2,  // rewrite the tail, then freeze it (it becomes permanent)
    TRANSCRIPT_APPLY_SILENCE = 3 // freeze the tail, append a newline, empty tail
};

// A text segment with an optional fade alpha (0 = no fade tag)
struct transcript_applied_segment {
    char *text;
    int alpha;
};

struct transcript {
    struct token_capitalizer tcap;
    // State of the capitalizer at the start of the current sentence
    struct token_capitalizer tcap_saved;

    GPtrArray *segs;
    size_t n_segs;
};

void transcript_init(struct transcript *t);
void transcript_deinit(struct transcript *t);
void transcript_reset(struct transcript *t);
void transcript_set_language(struct transcript *t, const char *language);

// Render the current sentence's tokens into the tail segments
void transcript_update(struct transcript *t, size_t num_tokens, const struct asr_token *tokens);

// The current tail becomes a finalized sentence (capitalizer transition)
void transcript_finish_sentence(struct transcript *t);

// Silence: the current tail becomes a sentence boundary, and the next
// sentence starts capitalized
void transcript_break_sentence(struct transcript *t);

size_t transcript_segment_count(const struct transcript *t);
const char *transcript_segment_text(const struct transcript *t, size_t index);
int transcript_segment_alpha(const struct transcript *t, size_t index);
