/* line-gen.c
 * This file contains the implementation for token_capitalizer
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

#include <glib.h>
#include <stdbool.h>

#include "line-gen.h"
#include "asr-token.h"

void token_capitalizer_init(struct token_capitalizer *tc) {
    tc->is_english = true;
    tc->previous_was_period = true;
    tc->finished_at_period = false;
    tc->force_next_cap = false;
}

bool token_capitalizer_next(struct token_capitalizer *tc, const char *token, int flags, const char *subsequent_token, int subsequent_flags) {
    if((flags & ASR_TOKEN_FLAG_SENTENCE_END) != 0){
        tc->previous_was_period = true;
        return false;
    }

    if(tc->force_next_cap){
        tc->force_next_cap = false;
        return true;
    }

    if((tc->previous_was_period) && (flags & ASR_TOKEN_FLAG_WORD_BOUNDARY)){
        // If bare space token, capitalize the subsequent token since space can't
        // be capitalized.
        if((token[0] == ' ') && (token[1] == 0)){
            tc->force_next_cap = true;
        }

        tc->previous_was_period = false;
        return true;
    }

    // English-specific behavior: capitalize 'I'
    // TODO: A better way of capitalizing I and names and places
    if(tc->is_english) {
        if((token[0] == ' ') && (token[1] == 'I') && (token[2] == 0)){
            if(subsequent_token != NULL){
                if (((subsequent_flags & (ASR_TOKEN_FLAG_WORD_BOUNDARY | ASR_TOKEN_FLAG_SENTENCE_END)) != 0) || (subsequent_token[0] == '\'')){
                    return true;
                }
            }else{
                return true;
            }
        }
    }

    return false;
}

void token_capitalizer_finish(struct token_capitalizer *tc){
    tc->finished_at_period = tc->previous_was_period;
    tc->previous_was_period = false;
    tc->force_next_cap = false;
}

void token_capitalizer_rewind(struct token_capitalizer *tc){
    tc->previous_was_period = tc->finished_at_period;
    tc->force_next_cap = false;
}
