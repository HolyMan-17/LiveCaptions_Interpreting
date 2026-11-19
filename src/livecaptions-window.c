/* livecaptions-window.c
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

#include "livecaptions-config.h"
#include "livecaptions-window.h"
#include "livecaptions-application.h"
#include "audiocap.h"
#include "window-helper.h"
#include "history.h"
#include "transcript.h"
#include "asr-engine.h"


G_DEFINE_TYPE(LiveCaptionsWindow, livecaptions_window, GTK_TYPE_APPLICATION_WINDOW)

#define FADE_TAG_BUCKETS 32
#define TEXT_MARGINS_V 24 // 12px top + 12px bottom of the text view
#define FOLLOW_EPSILON 1.0

static void livecaptions_window_class_init (LiveCaptionsWindowClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    gtk_widget_class_set_template_from_resource(widget_class, "/net/sapples/LiveCaptions/livecaptions-window.ui");
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, main);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, side_box);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, side_box_tiny);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, mic_button);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, scroller);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, text);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, too_slow_warning);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, slow_warning);
    gtk_widget_class_bind_template_child(widget_class, LiveCaptionsWindow, slowest_warning);
}

static void change_button_layout(LiveCaptionsWindow *self, gint text_height){
    int button_height = 29;

    if(text_height > (2 * button_height)) {
        gtk_widget_set_visible(GTK_WIDGET(self->side_box), true);
        gtk_widget_set_visible(GTK_WIDGET(self->side_box_tiny), false);
    } else {
        gtk_widget_set_visible(GTK_WIDGET(self->side_box_tiny), true);
        gtk_widget_set_visible(GTK_WIDGET(self->side_box), false);
    }
}

const char LINE_WIDTH_TEXT_TEMPLATE[] = "This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.";

// Measures the default/min window size from the GPL template string and the
// line-count setting, and applies the font-based metrics
static void update_text_metrics(LiveCaptionsWindow *self){
    int preferred_width = g_settings_get_int(self->settings, "line-width");
    size_t text_len = sizeof(LINE_WIDTH_TEXT_TEMPLATE);
    if((size_t)preferred_width < text_len) text_len = (size_t)preferred_width;

    int width, height;
    g_autofree char *font_name = g_settings_get_string(self->settings, "font-name");
    PangoFontDescription *desc = pango_font_description_from_string(font_name);
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self->text), NULL);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_width(layout, -1);
    pango_layout_set_text(layout, LINE_WIDTH_TEXT_TEMPLATE, (int)text_len);
    pango_layout_get_size(layout, &width, &height);
    g_object_unref(layout);
    pango_font_description_free(desc);

    self->line_height = height / PANGO_SCALE;

    int line_count = g_settings_get_int(self->settings, "line-count");
    if(line_count < 2) line_count = 2;

    width = width / PANGO_SCALE + 12 + 24; // left + right text view margins
    int min_height = 2 * self->line_height + TEXT_MARGINS_V;
    int default_height = line_count * self->line_height + TEXT_MARGINS_V;

    change_button_layout(self, min_height);

    gtk_widget_set_size_request(GTK_WIDGET(self->scroller), width, min_height);

    // Works both before mapping (initial size) and while shown
    gtk_window_set_default_size(GTK_WINDOW(self), width, default_height);
}

static void append_escaped_css_string(GString *css, const char *value){
    g_string_append_c(css, '"');
    for(const char *c = value; *c != '\0'; c++){
        if((*c == '"') || (*c == '\\')) g_string_append_c(css, '\\');
        g_string_append_c(css, *c);
    }
    g_string_append_c(css, '"');
}

// Applies the font-name GSetting to the text view via CSS
static void update_font(LiveCaptionsWindow *self) {
    g_autofree char *font_name = g_settings_get_string(self->settings, "font-name");
    PangoFontDescription *desc = pango_font_description_from_string(font_name);

    GString *css = g_string_new(".main-window-parent textview {");
    const char *family = pango_font_description_get_family(desc);
    if(family != NULL){
        g_string_append(css, "font-family: ");
        append_escaped_css_string(css, family);
        g_string_append_c(css, ';');
    }

    // get_size always returns device units scaled by PANGO_SCALE, even when
    // the size is absolute (px); the unit only decides the CSS suffix
    int size = pango_font_description_get_size(desc) / PANGO_SCALE;
    const char *size_unit = pango_font_description_get_size_is_absolute(desc) ? "px" : "pt";
    if(size > 0) g_string_append_printf(css, "font-size: %d%s;", size, size_unit);

    g_string_append_printf(css, "font-weight: %d;", (int)pango_font_description_get_weight(desc));
    switch(pango_font_description_get_style(desc)){
        case PANGO_STYLE_ITALIC:
            g_string_append(css, "font-style: italic;");
            break;
        case PANGO_STYLE_OBLIQUE:
            g_string_append(css, "font-style: oblique;");
            break;
        default:
            break;
    }
    g_string_append(css, "}");

    gtk_css_provider_load_from_string(self->font_css_provider, css->str);
    g_string_free(css, TRUE);

    pango_font_description_free(desc);

    update_text_metrics(self);
}

static void update_window_transparency(LiveCaptionsWindow *self) {
    bool use_transparency = g_settings_get_double(self->settings, "window-transparency") > 0.01;

    // The text view must never paint an opaque background
    const char *textview_css =
        ".main-window-parent textview, .main-window-parent textview text {\n"
        "background-color: transparent;\n"
        "}\n";

    char css_data[512];
    if(use_transparency){
        gtk_widget_add_css_class(GTK_WIDGET(self), "transparent-mode");

        int transparency = (int)((1.0 - g_settings_get_double(self->settings, "window-transparency")) * 255.0);

        snprintf(css_data, sizeof(css_data), ".transparent-mode {\nbackground-color: #000000%02X;\n}\n%s", transparency, textview_css);
    }else{
        gtk_widget_remove_css_class(GTK_WIDGET(self), "transparent-mode");
        snprintf(css_data, sizeof(css_data), "%s", textview_css);
    }
    gtk_css_provider_load_from_string(self->css_provider, css_data);
}

static void update_keep_above(LiveCaptionsWindow *self) {
    gboolean keep_above = g_settings_get_boolean(self->settings, "keep-on-top");
    if(!set_window_keep_above(GTK_WINDOW(self), keep_above)){
        printf("Failed to set keep above\n");
    }
}

static void on_settings_change(G_GNUC_UNUSED GSettings *settings,
                               char      *key,
                               gpointer   user_data){

    LiveCaptionsWindow *self = user_data;
    if(g_str_equal(key, "font-name")) {
        update_font(self);
    }else if(g_str_equal(key, "line-width")) {
        update_text_metrics(self);
    }else if(g_str_equal(key, "line-count")) {
        update_text_metrics(self);
    }else if(g_str_equal(key, "transparent-window") || g_str_equal(key, "window-transparency")) {
        update_window_transparency(self);
    }else if(g_str_equal(key, "keep-on-top")) {
        update_keep_above(self);
    }
}

static GtkTextTag *get_fade_tag(LiveCaptionsWindow *self, int alpha){
    int idx = (alpha * (FADE_TAG_BUCKETS - 1)) / 65535;
    if(idx < 0) idx = 0;
    if(idx >= FADE_TAG_BUCKETS) idx = FADE_TAG_BUCKETS - 1;

    if(self->fade_tags[idx] == NULL){
        GdkRGBA color;
        gtk_widget_get_color(GTK_WIDGET(self->text), &color);
        color.alpha *= (double)idx / (double)(FADE_TAG_BUCKETS - 1);

        char name[32];
        snprintf(name, sizeof(name), "caption-fade-%d", idx);
        self->fade_tags[idx] = gtk_text_buffer_create_tag(self->buffer, name, "foreground-rgba", &color, NULL);
    }

    return self->fade_tags[idx];
}

void livecaptions_window_apply_transcript(LiveCaptionsWindow *self, int op, GPtrArray *segments){
    GtkTextBuffer *buffer = self->buffer;

    if(op == TRANSCRIPT_APPLY_SILENCE){
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_mark(buffer, &end, self->tail_start);

        // If the tail is empty (buffer start, or the preceding character is a
        // newline), skip inserting to avoid a stray blank line
        GtkTextIter prev = end;
        if(gtk_text_iter_is_start(&end) ||
           (gtk_text_iter_backward_char(&prev) && gtk_text_iter_get_char(&prev) == '\n'))
            return;

        // Freeze the tail, append a newline to the frozen content, and leave
        // an empty tail
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_insert(buffer, &end, "\n", 1);
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_move_mark(buffer, self->tail_start, &end);
        return;
    }

    if(op == TRANSCRIPT_APPLY_TAIL || op == TRANSCRIPT_APPLY_FINAL){
        GtkTextIter start, end;
        gtk_text_buffer_get_iter_at_mark(buffer, &start, self->tail_start);
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_delete(buffer, &start, &end);

        if(segments != NULL){
            for(guint i=0; i<segments->len; i++){
                struct transcript_applied_segment *seg = g_ptr_array_index(segments, i);

                GtkTextIter insert_at;
                gtk_text_buffer_get_end_iter(buffer, &insert_at);

                if(seg->alpha > 0)
                    gtk_text_buffer_insert_with_tags(buffer, &insert_at, seg->text, -1, get_fade_tag(self, seg->alpha), NULL);
                else
                    gtk_text_buffer_insert(buffer, &insert_at, seg->text, -1);
            }
        }
    }

    if(op == TRANSCRIPT_APPLY_FINAL){
        // Freeze the tail; it becomes permanent content
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(buffer, &end);
        gtk_text_buffer_move_mark(buffer, self->tail_start, &end);
    }
}

static void on_buffer_changed(GtkTextBuffer *buffer, gpointer user_data){
    LiveCaptionsWindow *self = user_data;
    if(!self->follow) return;

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_move_mark(buffer, self->scroll_end_mark, &end);

    self->scroll_pending = true;
    gtk_text_view_scroll_to_mark(self->text, self->scroll_end_mark, 0.0, TRUE, 0.0, 1.0);
}

static void on_vadjustment_changed(GtkAdjustment *adjustment, gpointer user_data){
    LiveCaptionsWindow *self = user_data;

    double max = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment);
    double value = gtk_adjustment_get_value(adjustment);

    if(max <= 0.0 || value >= (max - FOLLOW_EPSILON)){
        // Content fits in the viewport, or we are genuinely at the bottom
        self->follow = true;
        self->scroll_pending = false;
    }else if(!self->scroll_pending){
        // The user scrolled up; stop following until they return to the bottom
        self->follow = false;
    }
}

static gboolean on_text_scroll(G_GNUC_UNUSED GtkEventControllerScroll *controller,
                               G_GNUC_UNUSED gdouble dx,
                               G_GNUC_UNUSED gdouble dy,
                               gpointer user_data){
    LiveCaptionsWindow *self = user_data;

    // Scrolling while already at the bottom (or when everything fits the
    // viewport) means the user wants to stay there: keep following. The
    // adjustment value cannot change in that state, so value-changed would
    // never re-enable follow by itself.
    double max = gtk_adjustment_get_upper(self->vadjustment)
                 - gtk_adjustment_get_page_size(self->vadjustment);
    double value = gtk_adjustment_get_value(self->vadjustment);
    if(max <= 0.0 || value >= (max - FOLLOW_EPSILON)){
        self->scroll_pending = false;
        return GDK_EVENT_PROPAGATE;
    }

    // Any user scroll while not at the bottom stops auto-following until they
    // return to the bottom; also cancel any queued programmatic scroll so we
    // don't yank the view
    self->follow = false;
    self->scroll_pending = false;

    return GDK_EVENT_PROPAGATE;
}

// The text view is not selectable (its pointer cursor is "default"), so a
// left click on the caption area starts an interactive window move instead
static void on_text_click_pressed(GtkGestureClick *gesture,
                                  G_GNUC_UNUSED int n_press,
                                  double x,
                                  double y,
                                  gpointer user_data){
    LiveCaptionsWindow *self = LIVECAPTIONS_WINDOW(user_data);

    GdkEventSequence *sequence = gtk_gesture_single_get_current_sequence(GTK_GESTURE_SINGLE(gesture));
    GdkEvent *event = gtk_gesture_get_last_event(GTK_GESTURE(gesture), sequence);
    if(event == NULL) return;

    GdkDevice *device = gdk_event_get_device(event);
    GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(GTK_WIDGET(self)));
    if(device == NULL || surface == NULL || !GDK_IS_TOPLEVEL(surface)) return;

    // Take over the sequence so the text view never begins a selection
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    // gdk_toplevel_begin_move expects surface coordinates; translate from the
    // text view's widget coordinates
    graphene_point_t widget_point = GRAPHENE_POINT_INIT(x, y);
    graphene_point_t native_point;
    if(!gtk_widget_compute_point(GTK_WIDGET(self->text),
                                 GTK_WIDGET(gtk_widget_get_native(GTK_WIDGET(self))),
                                 &widget_point, &native_point))
        return;

    // The surface transform translates widget coordinates into surface
    // coordinates (as done by GtkWindowHandle itself); applying it yields
    // toplevel-surface coordinates for begin_move
    double tx, ty;
    gtk_native_get_surface_transform(gtk_widget_get_native(GTK_WIDGET(self)), &tx, &ty);

    gdk_toplevel_begin_move(GDK_TOPLEVEL(surface), device,
                            gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)),
                            native_point.x + tx, native_point.y + ty,
                            gdk_event_get_time(event));
}

// The text view resets its pointer cursor to "text" when it is realized;
// re-apply "default" every time it is mapped
static void on_text_map(GtkWidget *text, G_GNUC_UNUSED gpointer user_data){
    gtk_widget_set_cursor_from_name(text, "default");
}

GtkTextBuffer *livecaptions_window_get_buffer(LiveCaptionsWindow *self) {
    return self->buffer;
}

static void copy_transcript_action(G_GNUC_UNUSED GSimpleAction *action,
                                   G_GNUC_UNUSED GVariant      *parameter,
                                   gpointer       user_data){
    LiveCaptionsWindow *self = user_data;

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(self->buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(self->buffer, &start, &end, FALSE);

    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_set_text(clipboard, text != NULL ? text : "");

    g_free(text);
}

static void clear_transcript_action(G_GNUC_UNUSED GSimpleAction *action,
                                    G_GNUC_UNUSED GVariant      *parameter,
                                    gpointer       user_data){
    LiveCaptionsWindow *self = user_data;

    GtkApplication *gtk_app = gtk_window_get_application(GTK_WINDOW(self));
    if(gtk_app == NULL) return;
    LiveCaptionsApplication *app = LIVECAPTIONS_APPLICATION(gtk_app);

    // Invalidate any pending applies, then reset both sides
    asr_thread_clear_transcript(app->asr);

    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(self->buffer, &start, &end);
    gtk_text_buffer_delete(self->buffer, &start, &end);
}

static gboolean deferred_update_keep_above(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    update_keep_above(self);

    return G_SOURCE_REMOVE;
}

static gboolean show_relevant_slow_warning(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    GtkApplication *curr_app = gtk_window_get_application(GTK_WINDOW(self));
    LiveCaptionsApplication *app = LIVECAPTIONS_APPLICATION(curr_app);

    asr_thread asr = app->asr;

    if(asr_thread_is_errored(asr)){
        if(!self->was_errored){
            self->was_errored = true;

            GtkTextIter end;
            gtk_text_buffer_get_end_iter(self->buffer, &end);
            gtk_text_buffer_insert(self->buffer, &end, "\n[Model Error]\n", -1);
            gtk_text_buffer_get_end_iter(self->buffer, &end);
            gtk_text_buffer_move_mark(self->buffer, self->tail_start, &end);
        }
        return G_SOURCE_CONTINUE;
    }else if(self->was_errored) {
        self->was_errored = false;
    }

    struct asr_session *session = asr_thread_get_session(asr);
    if(session == NULL) return G_SOURCE_CONTINUE;

    float speedup = asr_session_speedup(session);

    if(speedup <= 1.1) {
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
    }else if(speedup <= 1.666) {
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), true);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
    }else if(speedup <= 2.33){
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), true);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
    }else{
        gtk_widget_set_visible(GTK_WIDGET(self->slow_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->slowest_warning), false);
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), true);
    }

    return G_SOURCE_CONTINUE;
}

static void livecaptions_window_destroy_handler(GtkWidget *widget,
                                                G_GNUC_UNUSED gpointer user_data){
    LiveCaptionsWindow *self = LIVECAPTIONS_WINDOW(widget);

    if(self->warning_timeout_id != 0){
        g_source_remove(self->warning_timeout_id);
        self->warning_timeout_id = 0;
    }

    GdkDisplay *display = gdk_display_get_default();
    if(display != NULL){
        gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(self->css_provider));
        gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(self->font_css_provider));
    }
}

static void livecaptions_window_init(LiveCaptionsWindow *self) {
    gtk_widget_init_template(GTK_WIDGET(self));

    self->settings = g_settings_new("net.sapples.LiveCaptions");

    self->buffer = gtk_text_view_get_buffer(self->text);

    self->css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(self->css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    self->font_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(self->font_css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkTextIter end;
    gtk_text_buffer_get_end_iter(self->buffer, &end);
    self->tail_start = gtk_text_buffer_create_mark(self->buffer, "tail-start", &end, TRUE);
    self->scroll_end_mark = gtk_text_buffer_create_mark(self->buffer, "scroll-end", &end, TRUE);

    self->follow = true;
    self->scroll_pending = false;
    for(int i=0; i<FADE_TAG_BUCKETS; i++) self->fade_tags[i] = NULL;

    g_signal_connect(self->buffer, "changed", G_CALLBACK(on_buffer_changed), self);

    self->vadjustment = gtk_scrolled_window_get_vadjustment(self->scroller);
    g_signal_connect(self->vadjustment, "value-changed", G_CALLBACK(on_vadjustment_changed), self);
    // Re-evaluate follow when the content size changes, so a wheel scroll
    // while everything fits the viewport doesn't stick follow off once
    // content later overflows
    g_signal_connect(self->vadjustment, "changed", G_CALLBACK(on_vadjustment_changed), self);

    // Detect user scrolling directly, so we never yank the view back to the
    // bottom while the user is scrolling up
    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll_controller, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_text_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self->text), scroll_controller);

    // A left click/drag on the caption text moves the window (text is not
    // selectable); capture phase so it runs before the text view's gestures
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_text_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->text), GTK_EVENT_CONTROLLER(click));

    g_signal_connect(self->text, "map", G_CALLBACK(on_text_map), self);

    g_autoptr(GSimpleAction) copy_action = g_simple_action_new("copy-transcript", NULL);
    g_signal_connect(copy_action, "activate", G_CALLBACK(copy_transcript_action), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(copy_action));

    g_autoptr(GSimpleAction) clear_action = g_simple_action_new("clear-transcript", NULL);
    g_signal_connect(clear_action, "activate", G_CALLBACK(clear_transcript_action), self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(clear_action));

    g_signal_connect(self->settings, "changed", G_CALLBACK(on_settings_change), self);

    g_signal_connect(self, "destroy", G_CALLBACK(livecaptions_window_destroy_handler), NULL);

    g_settings_bind(self->settings, "microphone", self->mic_button, "active", G_SETTINGS_BIND_DEFAULT);

    update_font(self);
    update_window_transparency(self);

    self->slow_warning_shown = false;
    self->was_errored = false;

    g_idle_add(deferred_update_keep_above, self);
    self->warning_timeout_id = g_timeout_add_seconds(5, show_relevant_slow_warning, self);

    // GTK adds solid-csd class when the compositor does not support window shadows
    // This adds a very ugly thick border, so we remove it
    gtk_widget_remove_css_class(GTK_WIDGET(self), "solid-csd");
}

static gboolean hide_slow_warning_after_some_time(void *userdata) {
    LiveCaptionsWindow *self = userdata;

    time_t current_time = time(NULL);

    if(difftime(current_time, self->slow_time) > 4.0) {
        self->slow_warning_shown = false;
        gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), false);
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

void livecaptions_window_warn_slow(LiveCaptionsWindow *self) {
    self->slow_time = time(NULL);
    if(self->slow_warning_shown) return;

    gtk_widget_set_visible(GTK_WIDGET(self->too_slow_warning), true);
    self->slow_warning_shown = true;

    g_timeout_add_seconds(1, hide_slow_warning_after_some_time, self);
}
