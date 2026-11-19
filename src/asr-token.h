/* asr-token.h
 * This file declares the engine-agnostic token type passed from the ASR
 * engine to the application's result handler.
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

/* Bit values are part of the history file serialization format; do not
   renumber */
enum asr_token_flags {
    /* The token marks the start of a new word. In English, this is
       equivalent to (text[0] == ' ') */
    ASR_TOKEN_FLAG_WORD_BOUNDARY = 0x00000001,

    /* The token marks the end of a sentence, meaning the token is equal
       to ".", "!", or "?" */
    ASR_TOKEN_FLAG_SENTENCE_END = 0x00000002,
};

struct asr_token {
    /* Null-terminated string. The string contains its own formatting,
       for example it may start with a space to denote a new word, or
       not start with a space to denote the next part of a word. */
    const char *text;

    int flags;

    /* Log probability of this being the correct token */
    float logprob;
};
