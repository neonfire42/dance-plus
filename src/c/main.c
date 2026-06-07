#include <pebble.h>

// ── Timing ────────────────────────────────────────────────────────────────
// 120 BPM: beat every 500ms. At 50ms/frame = 10 frames/beat.
// Notes spawn LOOKAHEAD beats early so they travel exactly to TARGET_Y on beat.
// Distance = NOTE_SPEED × BEAT_FRAMES × LOOKAHEAD = 5 × 10 × 4 = 200px.
// SPAWN_Y = TARGET_Y + 200 = 62 + 200 = 262 (off-screen bottom).
#define FRAME_MS      50
#define BEAT_FRAMES   10
#define LOOKAHEAD      4
#define NOTE_SPEED     5

// ── Layout ────────────────────────────────────────────────────────────────
#define HUD_H         26
#define TARGET_Y      62
#define SPAWN_Y      262
#define DIVIDER_1     70
#define DIVIDER_2    130

// ── Hit windows ───────────────────────────────────────────────────────────
#define HIT_PERFECT    8
#define HIT_GOOD      16

// ── Game ──────────────────────────────────────────────────────────────────
#define MAX_NOTES     16
#define NUM_BEATS     64

// Lane centers (x): left=40, center=100, right=160
static const int LANE_CX[3] = {40, 100, 160};

// Lane colors: cyan, yellow, orange
static const GColor LANE_CLR[3] = {
    {.argb = 0b11001111},  // GColorCyan
    {.argb = 0b11111100},  // GColorYellow
    {.argb = 0b11111000},  // GColorOrange
};

// ── Arrow shapes (centered at origin) ─────────────────────────────────────
// Left  (←): shaft right, tip left
static GPoint s_lpts[7] = {
    {-14, 0}, {0,-10}, {0,-6}, {12,-6}, {12,6}, {0,6}, {0,10}
};
// Up (↑): shaft down, tip up
static GPoint s_upts[7] = {
    {0,-14}, {10,0}, {6,0}, {6,12}, {-6,12}, {-6,0}, {-10,0}
};
// Right (→): shaft left, tip right
static GPoint s_rpts[7] = {
    {14,0}, {0,10}, {0,6}, {-12,6}, {-12,-6}, {0,-6}, {0,-10}
};
static GPathInfo s_arrow_info[3] = {
    {7, s_lpts}, {7, s_upts}, {7, s_rpts}
};
static GPath *s_arrows[3];

// ── Song pattern [64 beats × 3 lanes: L,C,R] ─────────────────────────────
// 0 = rest, 1 = note spawns in that lane — always exactly one note per beat
static const uint8_t SONG[NUM_BEATS][3] = {
    // Intro: every other beat (rests between notes, easy)
    {1,0,0},{0,0,0},{0,1,0},{0,0,0},{0,0,1},{0,0,0},{0,1,0},{0,0,0},
    {1,0,0},{0,0,0},{0,0,1},{0,0,0},{0,1,0},{0,0,0},{1,0,0},{0,0,0},
    // Build: every beat, single notes L/R/C pattern
    {1,0,0},{0,0,1},{0,1,0},{0,0,1},{1,0,0},{0,0,1},{0,1,0},{1,0,0},
    {0,0,1},{0,1,0},{1,0,0},{0,0,1},{0,1,0},{1,0,0},{0,1,0},{0,0,1},
    // Drop: varied single-note pattern
    {1,0,0},{0,0,1},{1,0,0},{0,1,0},{0,0,1},{0,1,0},{0,0,1},{1,0,0},
    {0,1,0},{0,0,1},{0,1,0},{1,0,0},{0,0,1},{0,1,0},{1,0,0},{0,0,1},
    // Finale: quick alternating, no rests
    {1,0,0},{0,0,1},{0,1,0},{1,0,0},{0,0,1},{0,1,0},{0,0,1},{1,0,0},
    {0,1,0},{1,0,0},{0,0,1},{0,1,0},{1,0,0},{0,1,0},{0,0,1},{1,0,0},
};

// ── Music (64 quarter notes × 500ms = 32s, C major pentatonic, square wave) ─
// SN = note, SR = rest; fields: midi_note, waveform, duration_ms, velocity, _reserved
#define SN(p) {(p), SpeakerWaveformSquare, 500, 0, 0}
#define SR    {0,   SpeakerWaveformSquare, 500, 0, 0}

static const SpeakerNote MUSIC[] = {
    // Phase 1 (0-15): main theme, rests on beats 3 & 11
    SN(72),SN(76),SN(79),SR,      SN(81),SN(79),SN(76),SN(72),  // C5 E5 G5 _ | A5 G5 E5 C5
    SN(74),SN(76),SN(79),SR,      SN(76),SN(74),SN(72),SN(69),  // D5 E5 G5 _ | E5 D5 C5 A4
    // Phase 2 (16-31): higher variation
    SN(76),SN(79),SN(81),SR,      SN(79),SN(81),SN(79),SN(76),  // E5 G5 A5 _ | G5 A5 G5 E5
    SN(74),SN(76),SN(79),SN(81),  SN(79),SN(76),SN(74),SN(72),  // D5 E5 G5 A5 | G5 E5 D5 C5
    // Phase 3 (32-47): ascending build
    SN(72),SN(74),SN(76),SN(79),  SN(81),SN(79),SN(76),SN(74),  // C5 D5 E5 G5 | A5 G5 E5 D5
    SN(76),SN(79),SN(81),SR,      SN(79),SN(76),SN(72),SR,       // E5 G5 A5 _ | G5 E5 C5 _
    // Phase 4 (48-63): finale
    SN(72),SN(76),SN(79),SN(76),  SN(74),SN(76),SN(79),SN(81),  // C5 E5 G5 E5 | D5 E5 G5 A5
    SN(79),SN(81),SN(79),SN(76),  SN(72),SR,     SN(72),SR,      // G5 A5 G5 E5 | C5 _ C5 _
};
#undef SN
#undef SR

// ── Note ──────────────────────────────────────────────────────────────────
typedef struct {
    int  y;
    int  lane;
    bool active;
    bool scored;
} Note;

// ── State ─────────────────────────────────────────────────────────────────
typedef enum { ST_TITLE, ST_PLAY, ST_RESULT } GameState;

static Window    *s_win;
static Layer     *s_canvas;
static AppTimer  *s_timer;
static GameState  s_state = ST_TITLE;

static Note  s_notes[MAX_NOTES];
static int   s_frame       = 0;
static int   s_spawn_idx   = 0;
static int   s_score       = 0;
static int   s_combo       = 0;
static int   s_max_combo   = 0;
static int   s_perfects    = 0;
static int   s_goods       = 0;
static int   s_misses      = 0;

static char  s_feedback[12] = "";
static int   s_fb_frames    = 0;
static GColor s_fb_color;
static int   s_lane_lit[3]  = {0, 0, 0};

static int   s_anim = 0;  // title animation counter

// ── Helpers ───────────────────────────────────────────────────────────────

static void set_feedback(const char *txt, GColor col) {
    snprintf(s_feedback, sizeof(s_feedback), "%s", txt);
    s_fb_frames = 20;
    s_fb_color  = col;
}

static void spawn_note(int lane) {
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!s_notes[i].active) {
            s_notes[i] = (Note){SPAWN_Y, lane, true, false};
            return;
        }
    }
}

static void hit_lane(int lane) {
    int best_dist = HIT_GOOD * 3;
    int best_i    = -1;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!s_notes[i].active || s_notes[i].scored || s_notes[i].lane != lane) continue;
        int d = s_notes[i].y - TARGET_Y;
        if (d < 0) d = -d;
        if (d < best_dist) { best_dist = d; best_i = i; }
    }
    if (best_i < 0) return;  // nothing within range

    s_notes[best_i].scored = true;
    s_notes[best_i].active = false;
    s_lane_lit[lane] = 10;

    if (best_dist <= HIT_PERFECT) {
        set_feedback("PERFECT!", LANE_CLR[lane]);
        s_score += 300;
        s_combo++;
        s_perfects++;
    } else if (best_dist <= HIT_GOOD) {
        set_feedback("GOOD!", GColorGreen);
        s_score += 100;
        s_combo++;
        s_goods++;
    } else {
        set_feedback("MISS!", GColorRed);
        s_combo = 0;
        s_misses++;
    }
    if (s_combo > s_max_combo) s_max_combo = s_combo;
}

static void reset_game(void) {
    for (int i = 0; i < MAX_NOTES; i++) s_notes[i].active = false;
    s_frame = s_spawn_idx = s_score = s_combo = s_max_combo = 0;
    s_perfects = s_goods = s_misses = 0;
    s_fb_frames = 0;
    for (int l = 0; l < 3; l++) s_lane_lit[l] = 0;
    s_state = ST_PLAY;
    speaker_play_notes(MUSIC, ARRAY_LENGTH(MUSIC), 200);
}

// ── Game loop ─────────────────────────────────────────────────────────────

static void game_tick(void *data) {
    s_timer = app_timer_register(FRAME_MS, game_tick, NULL);
    s_anim  = (s_anim + 1) & 255;

    if (s_state != ST_PLAY) {
        layer_mark_dirty(s_canvas);
        return;
    }

    s_frame++;

    // Spawn notes: each beat, look up SONG pattern and spawn
    if ((s_frame % BEAT_FRAMES) == 1) {
        if (s_spawn_idx < NUM_BEATS) {
            for (int l = 0; l < 3; l++) {
                if (SONG[s_spawn_idx][l]) spawn_note(l);
            }
            s_spawn_idx++;
        }
        // Haptic beat (starts once notes begin arriving at target)
        if (s_spawn_idx > LOOKAHEAD) {
            vibes_short_pulse();
        }
    }

    // Move notes and check misses
    bool any_active = false;
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!s_notes[i].active) continue;
        s_notes[i].y -= NOTE_SPEED;
        any_active = true;

        // Note slipped above the hit window without being scored
        if (!s_notes[i].scored && s_notes[i].y < TARGET_Y - HIT_GOOD) {
            s_notes[i].active = false;
            s_combo = 0;
            s_misses++;
            set_feedback("MISS!", GColorRed);
        }
        // Gone off top of screen
        if (s_notes[i].y < 0) {
            s_notes[i].active = false;
        }
    }

    // Decay timers
    for (int l = 0; l < 3; l++) {
        if (s_lane_lit[l] > 0) s_lane_lit[l]--;
    }
    if (s_fb_frames > 0) s_fb_frames--;

    // Song complete
    if (s_spawn_idx >= NUM_BEATS && !any_active) {
        speaker_stop();
        s_state = ST_RESULT;
    }

    layer_mark_dirty(s_canvas);
}

// ── Drawing ───────────────────────────────────────────────────────────────

static void draw_hud(GContext *ctx, int w) {
    // Background bar
    graphics_context_set_fill_color(ctx, GColorOxfordBlue);
    graphics_fill_rect(ctx, GRect(0, 0, w, HUD_H), 0, GCornerNone);

    // Score (left)
    char buf[20];
    snprintf(buf, sizeof(buf), "%d", s_score);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, buf,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
        GRect(4, 3, 90, 22), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    // Combo (center)
    if (s_combo > 1) {
        snprintf(buf, sizeof(buf), "x%d", s_combo);
        graphics_context_set_text_color(ctx, GColorYellow);
        graphics_draw_text(ctx, buf,
            fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
            GRect(w/2 - 22, 3, 44, 22), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }

    // Progress bar (bottom of HUD)
    int pw = (s_spawn_idx * w) / NUM_BEATS;
    graphics_context_set_fill_color(ctx, GColorGreen);
    graphics_fill_rect(ctx, GRect(0, HUD_H - 3, pw, 3), 0, GCornerNone);
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect b = layer_get_bounds(layer);

    // Black background
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, b, 0, GCornerNone);

    // ── Title screen ──────────────────────────────────────────────────────
    if (s_state == ST_TITLE) {
        graphics_context_set_text_color(ctx, GColorRed);
        graphics_draw_text(ctx, "Dance+",
            fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
            GRect(0, 40, b.size.w, 50), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

        // Bouncing preview arrows
        for (int l = 0; l < 3; l++) {
            int off = (int)(sin_lookup(((s_anim * 10 + l * 85) % TRIG_MAX_ANGLE)) * 10 / TRIG_MAX_RATIO);
            gpath_move_to(s_arrows[l], GPoint(LANE_CX[l], 118 + off));
            graphics_context_set_fill_color(ctx, LANE_CLR[l]);
            gpath_draw_filled(ctx, s_arrows[l]);
            graphics_context_set_stroke_color(ctx, GColorWhite);
            graphics_context_set_stroke_width(ctx, 1);
            gpath_draw_outline(ctx, s_arrows[l]);
        }

        graphics_context_set_text_color(ctx, GColorLightGray);
        graphics_draw_text(ctx,
            "UP / SEL / DOWN\nfor Left, Center, Right\n\nSELECT to start",
            fonts_get_system_font(FONT_KEY_GOTHIC_14),
            GRect(10, 148, b.size.w - 20, 76),
            GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
        return;
    }

    // ── Result screen ─────────────────────────────────────────────────────
    if (s_state == ST_RESULT) {
        graphics_context_set_text_color(ctx, GColorYellow);
        graphics_draw_text(ctx, "Results",
            fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
            GRect(0, 28, b.size.w, 36), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);

        char buf[40];
        graphics_context_set_text_color(ctx, GColorWhite);
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
            GRect(20, 72, b.size.w - 40, 22), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

        snprintf(buf, sizeof(buf), "Best combo: x%d", s_max_combo);
        graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(20, 98, b.size.w - 40, 22), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

        graphics_context_set_text_color(ctx, GColorCyan);
        snprintf(buf, sizeof(buf), "Perfect: %d", s_perfects);
        graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(20, 124, b.size.w - 40, 22), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

        graphics_context_set_text_color(ctx, GColorGreen);
        snprintf(buf, sizeof(buf), "Good:    %d", s_goods);
        graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(20, 148, b.size.w - 40, 22), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

        graphics_context_set_text_color(ctx, GColorRed);
        snprintf(buf, sizeof(buf), "Miss:    %d", s_misses);
        graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18),
            GRect(20, 172, b.size.w - 40, 22), GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

        graphics_context_set_text_color(ctx, GColorDarkGray);
        graphics_draw_text(ctx, "SELECT to play again",
            fonts_get_system_font(FONT_KEY_GOTHIC_14),
            GRect(10, 202, b.size.w - 20, 22), GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
        return;
    }

    // ── Gameplay ──────────────────────────────────────────────────────────

    // Lane dividers
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(DIVIDER_1, HUD_H), GPoint(DIVIDER_1, b.size.h));
    graphics_draw_line(ctx, GPoint(DIVIDER_2, HUD_H), GPoint(DIVIDER_2, b.size.h));

    // Target arrows: dark fill + lane-color outline, flash on hit
    for (int l = 0; l < 3; l++) {
        gpath_move_to(s_arrows[l], GPoint(LANE_CX[l], TARGET_Y));

        if (s_lane_lit[l] > 0) {
            // Lit: filled in lane color
            graphics_context_set_fill_color(ctx, LANE_CLR[l]);
        } else {
            // Normal: dark fill so outline stands out
            graphics_context_set_fill_color(ctx, GColorOxfordBlue);
        }
        gpath_draw_filled(ctx, s_arrows[l]);

        graphics_context_set_stroke_color(ctx, LANE_CLR[l]);
        graphics_context_set_stroke_width(ctx, 2);
        gpath_draw_outline(ctx, s_arrows[l]);
    }

    // Falling notes
    for (int i = 0; i < MAX_NOTES; i++) {
        Note *n = &s_notes[i];
        if (!n->active) continue;
        if (n->y < HUD_H || n->y > b.size.h + NOTE_SPEED) continue;

        gpath_move_to(s_arrows[n->lane], GPoint(LANE_CX[n->lane], n->y));
        graphics_context_set_fill_color(ctx, LANE_CLR[n->lane]);
        gpath_draw_filled(ctx, s_arrows[n->lane]);
        graphics_context_set_stroke_color(ctx, GColorWhite);
        graphics_context_set_stroke_width(ctx, 1);
        gpath_draw_outline(ctx, s_arrows[n->lane]);
    }

    // Feedback text (center screen, overlaying notes)
    if (s_fb_frames > 0) {
        graphics_context_set_text_color(ctx, s_fb_color);
        graphics_draw_text(ctx, s_feedback,
            fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
            GRect(0, 116, b.size.w, 26),
            GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }

    // HUD drawn last so it covers anything that drifts into it
    draw_hud(ctx, b.size.w);
}

// ── Buttons ───────────────────────────────────────────────────────────────

static void up_click(ClickRecognizerRef r, void *ctx)     { if (s_state == ST_PLAY) hit_lane(0); }
static void down_click(ClickRecognizerRef r, void *ctx)   { if (s_state == ST_PLAY) hit_lane(2); }
static void select_click(ClickRecognizerRef r, void *ctx) {
    if (s_state == ST_TITLE || s_state == ST_RESULT) {
        reset_game();
    } else if (s_state == ST_PLAY) {
        hit_lane(1);
    }
}

static void click_provider(void *ctx) {
    window_single_click_subscribe(BUTTON_ID_UP,     up_click);
    window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
    window_single_click_subscribe(BUTTON_ID_DOWN,   down_click);
}

// ── Window lifecycle ──────────────────────────────────────────────────────

static void win_load(Window *win) {
    Layer *root = window_get_root_layer(win);
    GRect  b    = layer_get_bounds(root);

    s_canvas = layer_create(b);
    layer_set_update_proc(s_canvas, canvas_update_proc);
    layer_add_child(root, s_canvas);

    for (int l = 0; l < 3; l++) {
        s_arrows[l] = gpath_create(&s_arrow_info[l]);
    }

    s_timer = app_timer_register(FRAME_MS, game_tick, NULL);
}

static void win_unload(Window *win) {
    speaker_stop();
    if (s_timer) { app_timer_cancel(s_timer); s_timer = NULL; }
    for (int l = 0; l < 3; l++) {
        if (s_arrows[l]) { gpath_destroy(s_arrows[l]); s_arrows[l] = NULL; }
    }
    layer_destroy(s_canvas);
    s_canvas = NULL;
}

// ── App lifecycle ─────────────────────────────────────────────────────────

static void init(void) {
    s_win = window_create();
    window_set_background_color(s_win, GColorBlack);
    window_set_click_config_provider(s_win, click_provider);
    window_set_window_handlers(s_win, (WindowHandlers){
        .load   = win_load,
        .unload = win_unload,
    });
    window_stack_push(s_win, true);
}

static void deinit(void) {
    window_destroy(s_win);
}

int main(void) { init(); app_event_loop(); deinit(); return 0; }
