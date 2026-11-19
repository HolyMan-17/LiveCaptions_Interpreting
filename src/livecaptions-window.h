/* livecaptions-window.h
 * The main application window, which contains the captions, a microphone
 * button, settings button, and close button.
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

#include <gtk/gtk.h>
#include <glib.h>

struct _LiveCaptionsWindow {
    GtkApplicationWindow  parent_instance;

    GSettings *settings;

    /* Template widgets */
    GtkWidget        *main;
    GtkBox           *side_box;
    GtkBox           *side_box_tiny;
    GtkToggleButton  *mic_button;
    GtkScrolledWindow *scroller;
    GtkTextView      *text;

    GtkTextBuffer *buffer;

    // Marks the start of the in-progress (rewritable) tail of the transcript
    GtkTextMark *tail_start;
    GtkTextMark *scroll_end_mark;

    GtkAdjustment *vadjustment;
    bool follow;         // keep the view scrolled to the newest text
    bool scroll_pending; // a programmatic scroll-to-end is in progress

    // Cached, quantized fade tags (indexed by alpha bucket)
    GtkTextTag *fade_tags[32];

    GtkCssProvider *css_provider;
    GtkCssProvider *font_css_provider;

    int line_height;

    bool slow_warning_shown;
    GtkWidget *too_slow_warning;
    time_t slow_time;
    guint warning_timeout_id;

    GtkWidget *slow_warning;
    GtkWidget *slowest_warning;

    gboolean was_errored;
};

G_BEGIN_DECLS

#define LIVECAPTIONS_TYPE_WINDOW (livecaptions_window_get_type())

G_DECLARE_FINAL_TYPE (LiveCaptionsWindow, livecaptions_window, LIVECAPTIONS, WINDOW, GtkApplicationWindow);

void livecaptions_window_warn_slow(LiveCaptionsWindow *self);

// Applies a transcript edit on the main thread; op is a transcript_apply_op,
// segments is a GPtrArray of struct transcript_applied_segment (may be NULL)
void livecaptions_window_apply_transcript(LiveCaptionsWindow *self, int op, GPtrArray *segments);

GtkTextBuffer *livecaptions_window_get_buffer(LiveCaptionsWindow *self);

G_END_DECLS
