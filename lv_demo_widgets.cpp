/**
 * @file lv_demo_widgets.c
 * Spotify Cassette Player — LVGL demo with Spotify Web API integration
 *
 * Spotify API integration: requires Client ID, Client Secret, and Refresh Token.
 * Replace placeholders in secrets.h before flashing.
 *
 * Polls Spotify for current track every 5 seconds.
 * Controls playback via Web API commands.
 *
 * Requires:
 *   #include <HTTPClient.h>          (ESP32 Arduino)
 *   #include <ArduinoJson.h>         (ArduinoJson v6/v7)
 *   #include <Preferences.h>         (ESP32 NVS)
 *   LVGL ≥ 8.x
 */

#include "lv_demo_widgets.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include "secrets.h"

/* ── User config ──────────────────────────────────────────── */

// Calgary Timezone: Mountain Standard Time (MST) / Mountain Daylight Time (MDT)
// MST is 7 hours behind UTC, MDT is 6 hours behind.
// Rules: Starts March (M3) 2nd Sunday (.2.0), ends Nov (M11) 1st Sunday (.1.0)
#define CALGARY_TZ "MST7MDT,M3.2.0,M11.1.0"
/* ────────────────────────────────────────────────────────── */

/* ── Dark colour palette ─────────────────────────────────── */
#define C_BG lv_color_hex(0x121212)
#define C_SURFACE lv_color_hex(0x1E1E1E)
#define C_CARD lv_color_hex(0x252525)
#define C_ACCENT lv_color_hex(0xEF0107)  /* Arsenal red                */
#define C_ACCENT2 lv_color_hex(0x9B0000) /* darker red                 */
#define C_TEXT lv_color_hex(0xF0F0F0)
#define C_TEXT_DIM lv_color_hex(0x888888)
#define C_GOLD lv_color_hex(0xFFD700)
#define C_GREEN lv_color_hex(0x4CAF50)
#define C_WHITE lv_color_hex(0xFFFFFF)
#define C_DIVIDER lv_color_hex(0x333333)
/* ────────────────────────────────────────────────────────── */

/* ── Forward declarations ────────────────────────────────── */
static void create_cassette_screen(lv_obj_t *parent);
static void create_widgets_tab(lv_obj_t *parent);
static void spotify_control(const char *command);
static void fetch_spotify_data(void);
static void update_spotify_ui(void);
static void reel_anim_cb(void *target, int32_t value);
static bool fetch_spotify_access_token(char *access_token, size_t len, int *expires_in);
static bool parse_google_datetime(const char *input, char *out, size_t out_len, time_t *out_time);
static void init_styles(void);

static lv_style_t style_bg, style_card, style_title, style_body, style_dim, style_accent, style_divider, style_tab_btn;

/* ── NVS / Preferences ───────────────────────────────────── */
static Preferences prefs;

/* ── Parsed data ─────────────────────────────────────────── */
struct SpotifyTrack
{
    char name[128];
    char artist[128];
    char album[128];
    bool is_playing;
};

static SpotifyTrack g_current_track;
static bool g_spotify_data_ready = false;
static bool g_spotify_data_error = false;
static char g_spotify_error_msg[128] = {0};

/* ── LVGL widget handles ─────────────────────────────────── */
static lv_obj_t *g_cassette_img = NULL;
static lv_obj_t *g_left_reel = NULL;
static lv_obj_t *g_right_reel = NULL;
static lv_obj_t *g_track_label = NULL;
static lv_obj_t *g_artist_label = NULL;
static lv_obj_t *g_album_label = NULL;
static lv_obj_t *g_play_pause_btn = NULL;
static lv_obj_t *g_prev_btn = NULL;
static lv_obj_t *g_next_btn = NULL;
static lv_anim_t g_left_anim;
static lv_anim_t g_right_anim;
static bool g_ui_update_ready = false;    /* Flag set by background task to signal UI update */
static bool g_poll_spotify_ready = false; /* Flag set by timer to signal polling */

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Style helpers
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Deferred UI update timer (runs on LVGL task for thread-safety)
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
/* UTC to time_t helper — converts UTC struct tm to seconds since epoch */
static time_t utc_to_time_t(struct tm *utc_tm)
{
    // Days in each month (non-leap year)
    int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    // Calculate total days since epoch (1970-01-01)
    int days = 0;
    int year = utc_tm->tm_year + 1900;

    // Count leap years since 1970
    for (int y = 1970; y < year; y++)
    {
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    }

    // Add days for this year
    for (int m = 0; m < utc_tm->tm_mon; m++)
    {
        days += dim[m];
        if (m == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        {
            days++; // leap year February
        }
    }

    days += utc_tm->tm_mday - 1;

    // Calculate seconds
    return (time_t)days * 86400 + utc_tm->tm_hour * 3600 + utc_tm->tm_min * 60 + utc_tm->tm_sec;
}

static void ui_update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (g_ui_update_ready)
    {
        update_spotify_ui();
        g_ui_update_ready = false;
    }
    if (g_poll_spotify_ready)
    {
        g_poll_spotify_ready = false;
        // Trigger background poll
        xTaskCreatePinnedToCore(
            [](void *)
            {
                fetch_spotify_data();
                vTaskDelete(NULL);
            },
            "spotify_poll", 16384, NULL, 1, NULL, 0);
    }
}

static void init_styles(void)
{
    /* Background */
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, C_BG);
    lv_style_set_bg_opa(&style_bg, LV_OPA_COVER);
    lv_style_set_border_width(&style_bg, 0);
    lv_style_set_pad_all(&style_bg, 0);

    /* Card */
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, C_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_radius(&style_card, 10);
    lv_style_set_border_width(&style_card, 0);
    lv_style_set_pad_all(&style_card, 12);

    /* Title text */
    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, C_TEXT);
    lv_style_set_text_font(&style_title, &lv_font_montserrat_14);

    /* Body text */
    lv_style_init(&style_body);
    lv_style_set_text_color(&style_body, C_TEXT);
    lv_style_set_text_font(&style_body, &lv_font_montserrat_14);

    /* Dim text */
    lv_style_init(&style_dim);
    lv_style_set_text_color(&style_dim, C_TEXT_DIM);
    lv_style_set_text_font(&style_dim, &lv_font_montserrat_14);

    /* Accent text (red) */
    lv_style_init(&style_accent);
    lv_style_set_text_color(&style_accent, C_ACCENT);
    lv_style_set_text_font(&style_accent, &lv_font_montserrat_14);

    /* Tab button */
    lv_style_init(&style_tab_btn);
    lv_style_set_bg_color(&style_tab_btn, C_SURFACE);
    lv_style_set_text_color(&style_tab_btn, C_TEXT);

    /* Divider */
    lv_style_init(&style_divider);
    lv_style_set_bg_color(&style_divider, C_DIVIDER);
    lv_style_set_bg_opa(&style_divider, LV_OPA_COVER);
    lv_style_set_border_width(&style_divider, 0);
    lv_style_set_pad_all(&style_divider, 0);
    lv_style_set_height(&style_divider, 1);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Cassette screen layout
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void create_cassette_screen(lv_obj_t *parent)
{
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_style_pad_all(parent, 20, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Cassette container */
    lv_obj_t *cassette_cont = lv_obj_create(parent);
    lv_obj_add_style(cassette_cont, &style_card, 0);
    lv_obj_set_size(cassette_cont, 300, 200);
    lv_obj_set_style_pad_all(cassette_cont, 10, 0);
    lv_obj_set_flex_flow(cassette_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cassette_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Cassette body (simple rectangle for now) */
    lv_obj_t *cassette_body = lv_obj_create(cassette_cont);
    lv_obj_set_size(cassette_body, 250, 120);
    lv_obj_set_style_bg_color(cassette_body, lv_color_hex(0x8B4513), 0); // Brown color
    lv_obj_set_style_border_width(cassette_body, 2, 0);
    lv_obj_set_style_border_color(cassette_body, lv_color_hex(0x654321), 0);
    lv_obj_set_style_radius(cassette_body, 5, 0);

    /* Left reel */
    g_left_reel = lv_obj_create(cassette_body);
    lv_obj_set_size(g_left_reel, 40, 40);
    lv_obj_set_style_bg_color(g_left_reel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(g_left_reel, 20, 0);
    lv_obj_set_style_border_width(g_left_reel, 2, 0);
    lv_obj_set_style_border_color(g_left_reel, lv_color_hex(0x333333), 0);
    lv_obj_align(g_left_reel, LV_ALIGN_LEFT_MID, 20, 0);

    /* Right reel */
    g_right_reel = lv_obj_create(cassette_body);
    lv_obj_set_size(g_right_reel, 40, 40);
    lv_obj_set_style_bg_color(g_right_reel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(g_right_reel, 20, 0);
    lv_obj_set_style_border_width(g_right_reel, 2, 0);
    lv_obj_set_style_border_color(g_right_reel, lv_color_hex(0x333333), 0);
    lv_obj_align(g_right_reel, LV_ALIGN_RIGHT_MID, -20, 0);

    /* Track info below cassette */
    lv_obj_t *info_cont = lv_obj_create(parent);
    lv_obj_set_size(info_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_cont, 0, 0);
    lv_obj_set_style_pad_all(info_cont, 0, 0);
    lv_obj_set_flex_flow(info_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_track_label = lv_label_create(info_cont);
    lv_label_set_text(g_track_label, "Loading...");
    lv_obj_add_style(g_track_label, &style_title, 0);
    lv_obj_set_style_text_align(g_track_label, LV_TEXT_ALIGN_CENTER, 0);

    g_artist_label = lv_label_create(info_cont);
    lv_label_set_text(g_artist_label, "—");
    lv_obj_add_style(g_artist_label, &style_body, 0);
    lv_obj_set_style_text_align(g_artist_label, LV_TEXT_ALIGN_CENTER, 0);

    g_album_label = lv_label_create(info_cont);
    lv_label_set_text(g_album_label, "—");
    lv_obj_add_style(g_album_label, &style_dim, 0);
    lv_obj_set_style_text_align(g_album_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Control buttons */
    lv_obj_t *btn_cont = lv_obj_create(parent);
    lv_obj_set_size(btn_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_style_pad_all(btn_cont, 0, 0);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_prev_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(g_prev_btn, 60, 60);
    lv_obj_add_style(g_prev_btn, &style_card, 0);
    lv_obj_t *prev_label = lv_label_create(g_prev_btn);
    lv_label_set_text(prev_label, LV_SYMBOL_PREV);
    lv_obj_add_style(prev_label, &style_body, 0);
    lv_obj_add_event_cb(g_prev_btn, [](lv_event_t *e)
                        { spotify_control("previous"); }, LV_EVENT_CLICKED, NULL);

    g_play_pause_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(g_play_pause_btn, 80, 80);
    lv_obj_add_style(g_play_pause_btn, &style_card, 0);
    lv_obj_t *play_label = lv_label_create(g_play_pause_btn);
    lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    lv_obj_add_style(play_label, &style_body, 0);
    lv_obj_add_event_cb(g_play_pause_btn, [](lv_event_t *e)
                        { spotify_control("play_pause"); }, LV_EVENT_CLICKED, NULL);

    g_next_btn = lv_btn_create(btn_cont);
    lv_obj_set_size(g_next_btn, 60, 60);
    lv_obj_add_style(g_next_btn, &style_card, 0);
    lv_obj_t *next_label = lv_label_create(g_next_btn);
    lv_label_set_text(next_label, LV_SYMBOL_NEXT);
    lv_obj_add_style(next_label, &style_body, 0);
    lv_obj_add_event_cb(g_next_btn, [](lv_event_t *e)
                        { spotify_control("next"); }, LV_EVENT_CLICKED, NULL);

    /* Initialize animation for reels */
    lv_anim_init(&g_left_anim);
    lv_anim_set_var(&g_left_anim, g_left_reel);
    lv_anim_set_exec_cb(&g_left_anim, reel_anim_cb);
    lv_anim_set_time(&g_left_anim, 2000);
    lv_anim_set_repeat_count(&g_left_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&g_left_anim, 0, 3600); // 3600 = 360 degrees * 10

    lv_anim_init(&g_right_anim);
    lv_anim_set_var(&g_right_anim, g_right_reel);
    lv_anim_set_exec_cb(&g_right_anim, reel_anim_cb);
    lv_anim_set_time(&g_right_anim, 2000);
    lv_anim_set_repeat_count(&g_right_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_values(&g_right_anim, 0, -3600); // Opposite direction
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Spotify control commands
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void spotify_control(const char *command)
{
    Serial.printf("[Spotify] Sending command: %s\n", command);

    // Get fresh access token
    char access_token[1024] = {0};
    int expires_in = 0;
    if (!fetch_spotify_access_token(access_token, sizeof(access_token), &expires_in))
    {
        Serial.println("[Spotify] Failed to get access token for control");
        return;
    }

    HTTPClient http;
    String url;
    String method;

    if (strcmp(command, "play_pause") == 0)
    {
        url = g_current_track.is_playing ? "https://api.spotify.com/v1/me/player/pause" : "https://api.spotify.com/v1/me/player/play";
        method = "PUT";
    }
    else if (strcmp(command, "next") == 0)
    {
        url = "https://api.spotify.com/v1/me/player/next";
        method = "POST";
    }
    else if (strcmp(command, "previous") == 0)
    {
        url = "https://api.spotify.com/v1/me/player/previous";
        method = "POST";
    }
    else
    {
        Serial.println("[Spotify] Unknown command");
        return;
    }

    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + access_token);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Content-Length", "0");

    Serial.printf("[Spotify] Control request URL: %s\n", url.c_str());
    Serial.printf("[Spotify] Control request method: %s\n", method.c_str());

    int httpCode = http.sendRequest(method.c_str(), "");
    String controlPayload = http.getString();
    Serial.printf("[Spotify] Control HTTP response code: %d\n", httpCode);
    Serial.printf("[Spotify] Control response payload: %s\n", controlPayload.c_str());
    if (httpCode == 204 || httpCode == 202)
    {
        Serial.printf("[Spotify] Command %s sent successfully\n", command);
        // Trigger a data refresh after a short delay
        xTaskCreatePinnedToCore(
            [](void *)
            {
                delay(500); // Wait for Spotify to process
                fetch_spotify_data();
                vTaskDelete(NULL);
            },
            "refresh_after_control", 4096, NULL, 1, NULL, 0);
    }
    else
    {
        Serial.printf("[Spotify] Command failed: HTTP %d\n", httpCode);
    }
    http.end();
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Entry point
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void lv_demo_widgets(void)
{
    init_styles();

    /* Set Calgary timezone for all time operations */
    setenv("TZ", CALGARY_TZ, 1);
    tzset();

    /* Root screen */
    lv_obj_t *scr = lv_scr_act();
    lv_obj_add_style(scr, &style_bg, 0);

    /* Create single screen cassette player */
    create_cassette_screen(scr);

    /* Create timer that checks for deferred UI updates from background task */
    lv_timer_create(ui_update_timer_cb, 100, NULL); /* Check every 100ms */

    /* Create timer for periodic Spotify data polling */
    lv_timer_create([](lv_timer_t *timer)
                    { g_poll_spotify_ready = true; }, 5000, NULL); /* Set poll flag every 5 seconds */

    /* Kick off initial Spotify data fetch on a background FreeRTOS task. */
    xTaskCreatePinnedToCore(
        [](void *)
        {
            fetch_spotify_data();
            vTaskDelete(NULL); /* self-delete when done */
        },
        "spotify_fetch", /* task name */
        16384,           /* stack size in bytes */
        NULL,            /* parameter */
        1,               /* priority */
        NULL,            /* handle */
        0                /* core 0 */
    );
    Serial.println("[Spotify] Background fetch task spawned");
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   API fetch — poll Spotify for current track
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void fetch_spotify_data()
{
    // Serial.println("\n[Spotify] --- Starting Data Fetch ---");

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[Spotify] Error: WiFi not connected");
        return;
    }

    HTTPClient http;
    g_spotify_data_error = false;
    g_spotify_data_ready = false;

    // ── 1. GET ACCESS TOKEN ──
    char access_token[1024] = {0};
    int expires_in = 0;
    if (!fetch_spotify_access_token(access_token, sizeof(access_token), &expires_in))
    {
        Serial.println("[Spotify] Failed to get access token");
        g_spotify_data_error = true;
        return;
    }

    // ── 2. FETCH CURRENTLY PLAYING ──
    http.begin("https://api.spotify.com/v1/me/player/currently-playing");
    http.addHeader("Authorization", String("Bearer ") + access_token);
    http.addHeader("Accept-Encoding", "identity");

    int httpCode = http.GET();
    if (httpCode == 200)
    {
        String payload = http.getString();

        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(doc, payload);

        if (!err)
        {
            g_current_track.is_playing = doc["is_playing"] | false;
            strlcpy(g_current_track.name, doc["item"]["name"] | "Unknown Track", sizeof(g_current_track.name));
            strlcpy(g_current_track.artist, doc["item"]["artists"][0]["name"] | "Unknown Artist", sizeof(g_current_track.artist));
            strlcpy(g_current_track.album, doc["item"]["album"]["name"] | "Unknown Album", sizeof(g_current_track.album));

            g_spotify_data_ready = true;
            // Serial.printf("[Spotify] Current: %s - %s\n", g_current_track.name, g_current_track.artist);
        }
        else
        {
            // Serial.printf("[Spotify] Parse Error: %s\n", err.c_str());
            g_spotify_data_error = true;
        }
    }
    else if (httpCode == 204)
    {
        // No content - nothing playing
        g_current_track.is_playing = false;
        strlcpy(g_current_track.name, "Nothing playing", sizeof(g_current_track.name));
        strlcpy(g_current_track.artist, "", sizeof(g_current_track.artist));
        strlcpy(g_current_track.album, "", sizeof(g_current_track.album));
        g_spotify_data_ready = true;
        // Serial.println("[Spotify] Nothing currently playing");
    }
    else
    {
        Serial.printf("[Spotify] HTTP Failed: %d\n", httpCode);
        g_spotify_data_error = true;
    }
    http.end();

    // ── 3. SIGNAL UI UPDATE ──
    if (g_spotify_data_ready)
    {
        // Serial.println("[Spotify] Data ready — flagging UI update...");
        g_ui_update_ready = true;
    }
}
/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   UI update — Spotify track info
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void update_spotify_ui(void)
{
    if (!g_track_label)
        return;

    if (g_spotify_data_error)
    {
        lv_label_set_text(g_track_label, "Error loading track");
        lv_label_set_text(g_artist_label, "");
        lv_label_set_text(g_album_label, "");
        return;
    }
    if (!g_spotify_data_ready)
        return;

    /* Update labels */
    lv_label_set_text(g_track_label, g_current_track.name);
    lv_label_set_text(g_artist_label, g_current_track.artist);
    lv_label_set_text(g_album_label, g_current_track.album);

    /* Update play/pause button */
    if (g_play_pause_btn)
    {
        lv_obj_t *btn_label = lv_obj_get_child(g_play_pause_btn, 0);
        if (btn_label)
        {
            lv_label_set_text(btn_label, g_current_track.is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        }
    }

    /* Control reel animation */
    if (g_current_track.is_playing)
    {
        lv_anim_start(&g_left_anim);
        lv_anim_start(&g_right_anim);
    }
    else
    {
        lv_anim_del(g_left_reel, reel_anim_cb);
        lv_anim_del(g_right_reel, reel_anim_cb);
        lv_obj_set_style_transform_angle(g_left_reel, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_angle(g_right_reel, 0, LV_PART_MAIN);
    }
}

static void reel_anim_cb(void *target, int32_t value)
{
    if (!target)
        return;

    lv_obj_t *obj = (lv_obj_t *)target;
    lv_obj_set_style_transform_angle(obj, value, LV_PART_MAIN);
}

static bool parse_google_datetime(const char *input, char *out, size_t out_len, time_t *out_time)
{
    if (!input || !out || out_len == 0)
        return false;

    struct tm tm_dt = {0};

    /* Try parsing full datetime: "2026-03-30T17:30:00-06:00" or "2026-03-30T17:30:00Z" */
    if (strptime(input, "%Y-%m-%dT%H:%M:%S", &tm_dt))
    {
        /* Convert to time_t and back via localtime for timezone handling */
        time_t t = utc_to_time_t(&tm_dt);

        if (out_time)
            *out_time = t; /* Store for sorting */

        /* Ensure timezone is set */
        tzset();

        struct tm *tm_local = localtime(&t);
        if (tm_local)
        {
            strftime(out, out_len, "%b %d, %H:%M", tm_local);
            return true;
        }
    }
    /* Try parsing all-day date: "2026-03-30" */
    else if (strptime(input, "%Y-%m-%d", &tm_dt))
    {
        /* Convert struct tm to time_t, then back via localtime for timezone */
        time_t t_allday = utc_to_time_t(&tm_dt);

        if (out_time)
            *out_time = t_allday; /* Store for sorting */

        struct tm *tm_local = localtime(&t_allday);
        if (tm_local)
        {
            strftime(out, out_len, "%b %d (all-day)", tm_local);
            return true;
        }
    }

    if (out_time)
        *out_time = 0;            /* No valid time parsed */
    strlcpy(out, input, out_len); /* Fallback: show raw input */
    return false;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Fetch Spotify access token using refresh_token
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static bool fetch_spotify_access_token(char *access_token, size_t len, int *expires_in)
{
    if (!access_token || len == 0)
        return false;

    HTTPClient http;
    http.begin("https://accounts.spotify.com/api/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    /* Build POST body */
    String body = "client_id=" + String(SPOTIFY_CLIENT_ID) +
                  "&client_secret=" + String(SPOTIFY_CLIENT_SECRET) +
                  "&refresh_token=" + String(SPOTIFY_REFRESH_TOKEN) +
                  "&grant_type=refresh_token";

    Serial.println("[Spotify] Requesting access token...");
    Serial.printf("[Spotify] Token endpoint body: %s\n", body.c_str());

    int httpCode = http.POST(body);
    bool success = false;
    Serial.printf("[Spotify] Token request HTTP code: %d\n", httpCode);

    String payload = http.getString();
    Serial.printf("[Spotify] Token response payload: %s\n", payload.c_str());

    if (httpCode == 200)
    {
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, payload);

        if (!err && doc.containsKey("access_token"))
        {
            strlcpy(access_token, doc["access_token"] | "", len);
            *expires_in = doc["expires_in"] | 3600;
            success = true;
            Serial.printf("[Spotify] Access token obtained, expires in %d seconds\n", *expires_in);
            int token_len = min(8, (int)strlen(access_token));
            String token_snippet = String(access_token).substring(0, token_len);
            Serial.printf("[Spotify] Access token prefix: %s...\n", token_snippet.c_str());
            if (doc.containsKey("scope"))
            {
                Serial.printf("[Spotify] Access token scopes: %s\n", doc["scope"].as<const char *>());
            }
        }
        else
        {
            Serial.printf("[Spotify] Token parse error: %s\n", err.c_str());
            if (doc.containsKey("error_description"))
            {
                Serial.printf("[Spotify] Error description: %s\n", doc["error_description"].as<const char *>());
            }
            else if (doc.containsKey("error"))
            {
                Serial.printf("[Spotify] Error field: %s\n", doc["error"].as<const char *>());
            }
        }
    }
    else
    {
        Serial.printf("[Spotify] Token request failed: HTTP %d\n", httpCode);
        Serial.printf("[Spotify] Token request details: %s\n", payload.c_str());
    }

    http.end();
    return success;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Widgets tab — empty for now
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void create_widgets_tab(lv_obj_t *parent)
{
    (void)parent; /* Placeholder for future widgets */
}