/* line-gen.h
 * This file contains the declaration for token_capitalizer, which tracks
 * whether tokens should be capitalized based on sentence structure.
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

struct token_capitalizer {
    bool is_english;
    bool finished_at_period;
    bool previous_was_period;

    bool force_next_cap;
};

void token_capitalizer_init(struct token_capitalizer *tc);
bool token_capitalizer_next(struct token_capitalizer *tc, const char *token, int flags, const char *subsequent_token, int subsequent_flags);
void token_capitalizer_finish(struct token_capitalizer *tc);
void token_capitalizer_rewind(struct token_capitalizer *tc);
