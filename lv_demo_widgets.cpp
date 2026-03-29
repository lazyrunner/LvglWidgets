/**
 * @file lv_demo_widgets.c
 * Arsenal FC Dashboard — LVGL demo with api-football.com integration
 *
 * API key placeholder: replace API_FOOTBALL_KEY below before flashing.
 *
 * Data is fetched once on boot and then at most once every 24 hours.
 * The last-fetch Unix timestamp is persisted in NVS via Preferences so
 * it survives deep-sleep / soft resets.
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

/* ── User config ──────────────────────────────────────────── */
#define FOOTBALL_API_TOKEN "3b4a18c3bf554701811c4066d682133d"
#define ARSENAL_ID 57
#define LEAGUE_CODE "PL"

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
static void create_arsenal_tab(lv_obj_t *parent);
static void create_widgets_tab(lv_obj_t *parent);
static void create_other_tab(lv_obj_t *parent);
static void fetch_arsenal_data(void);
static void update_fixture_ui(void);
static void update_standings_ui(void);
static void init_styles(void);

static lv_style_t style_bg, style_card, style_title, style_body, style_dim, style_accent, style_divider, style_tab_btn;

/* ── NVS / Preferences ───────────────────────────────────── */
static Preferences prefs;

/* ── Parsed data ─────────────────────────────────────────── */
struct Fixture
{
    char date[32];
    char home_name[64];
    char away_name[64];
    char league_name[64];
    int days_until;
};

struct Standing
{
    int rank;
    char team[40];
    int played;
    int goal_diff;
    int points;
};

static Fixture g_fixture;
static Standing g_standings[20];
static int g_standing_count = 0;
static bool g_data_ready = false;
static bool g_data_error = false;
static char g_error_msg[128] = {0};
static time_t g_fixture_time_utc = 0; /* UTC time_t of the fixture */

/* ── LVGL widget handles ─────────────────────────────────── */
static lv_obj_t *g_fixture_card = NULL;
static lv_obj_t *g_home_name_lbl = NULL;
static lv_obj_t *g_away_name_lbl = NULL;
static lv_obj_t *g_vs_lbl = NULL;
static lv_obj_t *g_date_lbl = NULL;
static lv_obj_t *g_days_lbl = NULL;
static lv_obj_t *g_league_lbl = NULL;
static lv_obj_t *g_standing_cont = NULL;
static lv_obj_t *g_status_lbl = NULL;
static bool g_ui_update_ready = false; /* Flag set by background task to signal UI update */

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
        update_fixture_ui();
        update_standings_ui();
        g_ui_update_ready = false;
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

    /* Tab view */
    lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 40);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(tv, C_BG, 0);
    lv_obj_set_style_bg_color(lv_tabview_get_tab_btns(tv), C_SURFACE, 0);
    lv_obj_set_style_border_side(lv_tabview_get_tab_btns(tv),
                                 LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(lv_tabview_get_tab_btns(tv), C_ACCENT, 0);
    lv_obj_set_style_border_width(lv_tabview_get_tab_btns(tv), 2, 0);

    /* Style tab buttons */
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_text_color(tab_btns, C_TEXT_DIM, 0);
    lv_obj_set_style_text_color(tab_btns, C_ACCENT,
                                LV_STATE_CHECKED);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(tab_btns, C_SURFACE, LV_STATE_CHECKED);

    /* Create tabs */
    lv_obj_t *tab_arsenal = lv_tabview_add_tab(tv, LV_SYMBOL_HOME " Arsenal");
    lv_obj_t *tab_widgets = lv_tabview_add_tab(tv, LV_SYMBOL_LIST " Widgets");
    lv_obj_t *tab_other = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS " More");

    lv_obj_set_style_bg_color(tab_arsenal, C_BG, 0);
    lv_obj_set_style_bg_color(tab_widgets, C_BG, 0);
    lv_obj_set_style_bg_color(tab_other, C_BG, 0);

    create_arsenal_tab(tab_arsenal);
    create_widgets_tab(tab_widgets);
    create_other_tab(tab_other);

    /* Create timer that checks for deferred UI updates from background task */
    lv_timer_create(ui_update_timer_cb, 100, NULL); /* Check every 100ms */

    /* Kick off data fetch on a background FreeRTOS task.
     * HTTPClient uses its own semaphores that must NOT be called
     * from the LVGL task — running it on a separate task avoids
     * the xQueueSemaphoreTake assert crash.                        */
    xTaskCreatePinnedToCore(
        [](void *)
        {
            fetch_arsenal_data();
            vTaskDelete(NULL); /* self-delete when done */
        },
        "arsenal_fetch", /* task name */
        16384,           /* stack size in bytes — HTTP + JSON need plenty */
        NULL,            /* parameter */
        1,               /* priority (low — LVGL runs at higher) */
        NULL,            /* handle — not needed */
        0                /* core 0; LVGL typically runs on core 1 */
    );
    Serial.println("[Arsenal] Background fetch task spawned");
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Arsenal tab layout
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void create_arsenal_tab(lv_obj_t *parent)
{
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 10, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* ── Section title: Next Match ── */
    lv_obj_t *header = lv_label_create(parent);
    lv_label_set_text(header, LV_SYMBOL_BELL "  NEXT MATCH");
    lv_obj_add_style(header, &style_dim, 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(header, 2, 0);

    /* ── Fixture card (using flex layout) ── */
    g_fixture_card = lv_obj_create(parent);
    lv_obj_add_style(g_fixture_card, &style_card, 0);
    lv_obj_set_width(g_fixture_card, LV_PCT(100));
    lv_obj_set_height(g_fixture_card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(g_fixture_card, 14, 0);
    lv_obj_set_flex_flow(g_fixture_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_fixture_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* League name */
    g_league_lbl = lv_label_create(g_fixture_card);
    lv_label_set_text(g_league_lbl, "Loading…");
    lv_obj_add_style(g_league_lbl, &style_dim, 0);
    lv_obj_set_style_text_color(g_league_lbl, C_ACCENT, 0);
    lv_obj_set_style_text_font(g_league_lbl, &lv_font_montserrat_14, 0);

    /* ── VS label ── */
    lv_obj_t *row = lv_obj_create(g_fixture_card);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_vs_lbl = lv_label_create(row);
    lv_label_set_text(g_vs_lbl, "VS");
    lv_obj_add_style(g_vs_lbl, &style_accent, 0);
    lv_obj_set_style_text_font(g_vs_lbl, &lv_font_montserrat_14, 0);

    /* Team names row */
    lv_obj_t *names_row = lv_obj_create(g_fixture_card);
    lv_obj_set_size(names_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(names_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(names_row, 0, 0);
    lv_obj_set_style_pad_all(names_row, 0, 0);
    lv_obj_set_flex_flow(names_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(names_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_home_name_lbl = lv_label_create(names_row);
    lv_label_set_text(g_home_name_lbl, "—");
    lv_obj_add_style(g_home_name_lbl, &style_body, 0);
    lv_label_set_long_mode(g_home_name_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_home_name_lbl, 90);
    lv_obj_set_style_text_align(g_home_name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *spacer = lv_obj_create(names_row);
    lv_obj_set_size(spacer, 20, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    g_away_name_lbl = lv_label_create(names_row);
    lv_label_set_text(g_away_name_lbl, "—");
    lv_obj_add_style(g_away_name_lbl, &style_body, 0);
    lv_label_set_long_mode(g_away_name_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_away_name_lbl, 90);
    lv_obj_set_style_text_align(g_away_name_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* Date label */
    g_date_lbl = lv_label_create(g_fixture_card);
    lv_label_set_text(g_date_lbl, "Date: —");
    lv_obj_add_style(g_date_lbl, &style_dim, 0);
    lv_obj_set_style_text_font(g_date_lbl, &lv_font_montserrat_14, 0);

    /* Divider */
    lv_obj_t *div = lv_obj_create(g_fixture_card);
    lv_obj_add_style(div, &style_divider, 0);
    lv_obj_set_width(div, LV_PCT(80));

    /* Days countdown */
    g_days_lbl = lv_label_create(g_fixture_card);
    lv_label_set_text(g_days_lbl, "");
    lv_obj_set_style_text_color(g_days_lbl, C_GOLD, 0);
    lv_obj_set_style_text_font(g_days_lbl, &lv_font_montserrat_14, 0);

    /* Status / error label */
    g_status_lbl = lv_label_create(parent);
    lv_label_set_text(g_status_lbl, "Fetching data…");
    lv_obj_add_style(g_status_lbl, &style_dim, 0);
    lv_obj_set_style_text_font(g_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(g_status_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(g_status_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* ── Section title: Premier League Table ── */
    lv_obj_t *tbl_header = lv_label_create(parent);
    lv_label_set_text(tbl_header, LV_SYMBOL_LIST "  PREMIER LEAGUE TABLE");
    lv_obj_add_style(tbl_header, &style_dim, 0);
    lv_obj_set_style_text_font(tbl_header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(tbl_header, 2, 0);

    /* ── Standings window (container for table header + rows) ── */
    lv_obj_t *standings_window = lv_obj_create(parent);
    lv_obj_set_size(standings_window, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(standings_window, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(standings_window, 0, 0);
    lv_obj_set_style_pad_all(standings_window, 0, 0);
    lv_obj_set_style_pad_row(standings_window, 0, 0);
    lv_obj_set_flex_flow(standings_window, LV_FLEX_FLOW_COLUMN);

    /* Table header row */
    lv_obj_t *tbl_hdr = lv_obj_create(standings_window);
    lv_obj_set_size(tbl_hdr, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(tbl_hdr, C_ACCENT2, 0);
    lv_obj_set_style_bg_opa(tbl_hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tbl_hdr, 6, 0);
    lv_obj_set_style_border_width(tbl_hdr, 0, 0);
    lv_obj_set_style_pad_hor(tbl_hdr, 8, 0);
    lv_obj_set_style_pad_ver(tbl_hdr, 4, 0);
    lv_obj_set_flex_flow(tbl_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tbl_hdr, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char *col_headers[] = {"#", "Club", "P", "GD", "Pts"};
    const lv_coord_t col_w[] = {22, 110, 28, 35, 35};
    for (int i = 0; i < 5; i++)
    {
        lv_obj_t *lbl = lv_label_create(tbl_hdr);
        lv_label_set_text(lbl, col_headers[i]);
        lv_obj_set_width(lbl, col_w[i]);
        lv_obj_set_style_text_color(lbl, C_WHITE, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    }

    /* Standings container — populated after fetch */
    g_standing_cont = lv_obj_create(standings_window);
    lv_obj_set_width(g_standing_cont, LV_PCT(100));
    lv_obj_set_height(g_standing_cont, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(g_standing_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_standing_cont, 0, 0);
    lv_obj_set_style_pad_all(g_standing_cont, 0, 0);
    lv_obj_set_style_pad_row(g_standing_cont, 2, 0);
    lv_obj_set_flex_flow(g_standing_cont, LV_FLEX_FLOW_COLUMN);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   API fetch — call once per boot, then once per day via NVS
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
void fetch_arsenal_data()
{
    Serial.println("\n[Arsenal] --- Starting Data Fetch ---");

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[Arsenal] Error: WiFi not connected");
        return;
    }

    HTTPClient http;
    time_t now = time(NULL);
    g_data_error = false;
    g_data_ready = false;

    // ── 1. FETCH FIXTURE ──
    // We use status=SCHEDULED & limit=1 to get the single next game
    String fixtureUrl = "https://api.football-data.org/v4/teams/" + String(ARSENAL_ID) + "/matches?status=SCHEDULED&limit=1";

    http.begin(fixtureUrl);
    http.addHeader("X-Auth-Token", FOOTBALL_API_TOKEN);
    http.addHeader("Accept-Encoding", "identity"); // CRITICAL: Forces plain text instead of GZIP

    int httpCode = http.GET();
    if (httpCode == 200)
    {
        String payload = http.getString();

        // Use a filter to keep memory usage low
        StaticJsonDocument<200> filter;
        filter["matches"][0]["competition"]["name"] = true;
        filter["matches"][0]["homeTeam"]["shortName"] = true;
        filter["matches"][0]["awayTeam"]["shortName"] = true;
        filter["matches"][0]["utcDate"] = true;

        DynamicJsonDocument doc(2048);
        DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));

        if (!err && doc["matches"].size() > 0)
        {
            JsonObject match = doc["matches"][0];

            strlcpy(g_fixture.league_name, match["competition"]["name"] | "Premier League", sizeof(g_fixture.league_name));
            strlcpy(g_fixture.home_name, match["homeTeam"]["shortName"] | "Home", sizeof(g_fixture.home_name));
            strlcpy(g_fixture.away_name, match["awayTeam"]["shortName"] | "Away", sizeof(g_fixture.away_name));
            strlcpy(g_fixture.date, match["utcDate"] | "", sizeof(g_fixture.date));

            // Parse ISO date (e.g. 2026-04-11T05:30:00Z)
            struct tm tm_match = {0};
            if (strptime(g_fixture.date, "%Y-%m-%dT%H:%M:%SZ", &tm_match))
            {
                /* Convert UTC struct tm to time_t using helper */
                g_fixture_time_utc = utc_to_time_t(&tm_match);
                g_fixture.days_until = (int)(difftime(g_fixture_time_utc, now) / 86400.0);
                if (g_fixture.days_until < 0)
                    g_fixture.days_until = 0;
            }
            g_data_ready = true;
            Serial.printf("[Fixture] Found: %s vs %s\n", g_fixture.home_name, g_fixture.away_name);
        }
        else
        {
            Serial.printf("[Fixture] Parse/Data Error: %s\n", err.c_str());
            if (payload.length() < 10)
                Serial.println("[Fixture] Warning: Empty or very short response.");
            g_data_error = true;
        }
    }
    else
    {
        Serial.printf("[Fixture] HTTP Failed: %d\n", httpCode);
        g_data_error = true;
    }
    http.end();

    // ── 2. FETCH STANDINGS ──
    if (!g_data_error)
    {
        Serial.printf("[standings] Fetching standings now ");
        String standingsUrl = "https://api.football-data.org/v4/competitions/" + String(LEAGUE_CODE) + "/standings";
        http.begin(standingsUrl);
        http.addHeader("X-Auth-Token", FOOTBALL_API_TOKEN);
        http.addHeader("Accept-Encoding", "identity");

        httpCode = http.GET();
        if (httpCode == 200)
        {
            String payload = http.getString();
            Serial.printf("[standings] Successfully fetched standings ");
            // Standings can be large, we filter strictly to the 'table' array
            DynamicJsonDocument doc(12288);
            DeserializationError err = deserializeJson(doc, payload);

            if (!err)
            {
                JsonArray table = doc["standings"][0]["table"];
                g_standing_count = 0;
                for (JsonObject row : table)
                {
                    if (g_standing_count >= 20)
                        break;
                    Standing &s = g_standings[g_standing_count++];
                    s.rank = row["position"];
                    strlcpy(s.team, row["team"]["shortName"] | "Team", sizeof(s.team));
                    s.played = row["playedGames"];
                    s.goal_diff = row["goalDifference"];
                    s.points = row["points"];
                }
                Serial.printf("[Standings] Loaded %d teams\n", g_standing_count);
            }
            else
            {
                Serial.printf("[Standings] Parse/Data Error: %s\n", err.c_str());
                if (payload.length() < 10)
                    Serial.println("[Standings] Warning: Empty or very short response.");
                g_data_error = true;
            }
        }
        http.end();
    }

    // ── 3. SIGNAL UI UPDATE ──
    // Set flag for deferred update on LVGL task (thread-safe)
    if (g_data_ready)
    {
        Serial.println("[Arsenal] Data ready — flagging UI update...");
        g_ui_update_ready = true; /* Timer callback will call update functions */
    }
}
/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   UI update — fixture card
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void update_fixture_ui(void)
{
    if (!g_fixture_card)
        return;

    if (g_data_error)
    {
        lv_label_set_text(g_status_lbl, g_error_msg);
        lv_label_set_text(g_league_lbl, "Error");
        return;
    }
    if (!g_data_ready)
        return;

    lv_label_set_text(g_status_lbl, ""); /* clear loading text */

    /* League */
    lv_label_set_text(g_league_lbl, g_fixture.league_name);

    /* Team names */
    lv_label_set_text(g_home_name_lbl, g_fixture.home_name);
    lv_label_set_text(g_away_name_lbl, g_fixture.away_name);

    /* Date — convert UTC to Calgary time and format */
    char date_str[64] = "Date: —";
    if (g_fixture_time_utc > 0)
    {
        /* Convert UTC time_t to Calgary local time */
        struct tm *tm_calgary = localtime(&g_fixture_time_utc);
        if (tm_calgary)
        {
            char calgary_time[40];
            strftime(calgary_time, sizeof(calgary_time), "%Y-%m-%d %H:%M", tm_calgary);
            snprintf(date_str, sizeof(date_str), "Date: %s", calgary_time);
        }
    }
    lv_label_set_text(g_date_lbl, date_str);

    /* Days countdown */
    if (g_fixture.days_until == 0)
    {
        lv_label_set_text(g_days_lbl, LV_SYMBOL_BELL "  TODAY!");
        lv_obj_set_style_text_color(g_days_lbl, C_ACCENT, 0);
    }
    else if (g_fixture.days_until == 1)
    {
        lv_label_set_text(g_days_lbl, "1 day to go");
        lv_obj_set_style_text_color(g_days_lbl, C_GOLD, 0);
    }
    else if (g_fixture.days_until > 0)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d days to go",
                 g_fixture.days_until);
        lv_label_set_text(g_days_lbl, buf);
        lv_obj_set_style_text_color(g_days_lbl, C_GOLD, 0);
    }
    else
    {
        lv_label_set_text(g_days_lbl, "Date unavailable");
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   UI update — standings table
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void update_standings_ui(void)
{
    if (!g_standing_cont || g_standing_count == 0)
        return;

    lv_obj_clean(g_standing_cont); /* remove old rows */

    const lv_coord_t col_w[] = {22, 110, 28, 35, 35};

    for (int i = 0; i < g_standing_count; i++)
    {
        Standing &s = g_standings[i];

        lv_obj_t *row = lv_obj_create(g_standing_cont);
        lv_obj_set_size(row, LV_PCT(100), 30);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* Alternating row colours; highlight Arsenal */
        bool is_arsenal = (strstr(s.team, "Arsenal") != NULL);
        if (is_arsenal)
        {
            lv_obj_set_style_bg_color(row, lv_color_hex(0x3A0000), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }
        else if (i % 2 == 0)
        {
            lv_obj_set_style_bg_color(row, C_CARD, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }
        else
        {
            lv_obj_set_style_bg_color(row, C_SURFACE, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        }

        /* Rank */
        char rank_str[8];
        snprintf(rank_str, sizeof(rank_str), "%d", s.rank);
        lv_obj_t *lbl_rank = lv_label_create(row);
        lv_label_set_text(lbl_rank, rank_str);
        lv_obj_set_width(lbl_rank, col_w[0]);
        lv_obj_set_style_text_color(lbl_rank,
                                    (s.rank <= 4) ? C_GREEN : (s.rank <= 6) ? C_GOLD
                                                          : (s.rank >= 18)  ? C_ACCENT
                                                                            : C_TEXT_DIM,
                                    0);
        lv_obj_set_style_text_font(lbl_rank, &lv_font_montserrat_14, 0);

        /* Team name */
        lv_obj_t *lbl_team = lv_label_create(row);
        lv_label_set_text(lbl_team, s.team);
        lv_obj_set_width(lbl_team, col_w[1]);
        lv_obj_set_style_text_color(lbl_team,
                                    is_arsenal ? C_ACCENT : C_TEXT, 0);
        lv_obj_set_style_text_font(lbl_team, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(lbl_team, LV_LABEL_LONG_DOT);

        /* Played */
        char p_str[8];
        snprintf(p_str, sizeof(p_str), "%d", s.played);
        lv_obj_t *lbl_p = lv_label_create(row);
        lv_label_set_text(lbl_p, p_str);
        lv_obj_set_width(lbl_p, col_w[2]);
        lv_obj_set_style_text_color(lbl_p, C_TEXT_DIM, 0);
        lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_14, 0);

        /* Goal diff */
        char gd_str[10];
        snprintf(gd_str, sizeof(gd_str), "%+d", s.goal_diff);
        lv_obj_t *lbl_gd = lv_label_create(row);
        lv_label_set_text(lbl_gd, gd_str);
        lv_obj_set_width(lbl_gd, col_w[3]);
        lv_obj_set_style_text_color(lbl_gd,
                                    s.goal_diff > 0 ? C_GREEN : s.goal_diff < 0 ? C_ACCENT
                                                                                : C_TEXT_DIM,
                                    0);
        lv_obj_set_style_text_font(lbl_gd, &lv_font_montserrat_14, 0);

        /* Points */
        char pts_str[8];
        snprintf(pts_str, sizeof(pts_str), "%d", s.points);
        lv_obj_t *lbl_pts = lv_label_create(row);
        lv_label_set_text(lbl_pts, pts_str);
        lv_obj_set_width(lbl_pts, col_w[4]);
        lv_obj_set_style_text_color(lbl_pts, C_WHITE, 0);
        lv_obj_set_style_text_font(lbl_pts, &lv_font_montserrat_14, 0);
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Original demo tab (Widgets) — preserved
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void create_widgets_tab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 10, 0);

    /* ── Slider ── */
    lv_obj_t *lbl_sl = lv_label_create(parent);
    lv_label_set_text(lbl_sl, "Slider");
    lv_obj_add_style(lbl_sl, &style_dim, 0);

    lv_obj_t *slider = lv_slider_create(parent);
    lv_obj_set_width(slider, LV_PCT(90));
    lv_obj_set_style_bg_color(slider, C_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(slider, C_ACCENT,
                              LV_PART_KNOB | LV_STATE_DEFAULT);

    /* ── Switch ── */
    lv_obj_t *lbl_sw = lv_label_create(parent);
    lv_label_set_text(lbl_sw, "Switch");
    lv_obj_add_style(lbl_sw, &style_dim, 0);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_style_bg_color(sw, C_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* ── Arc ── */
    lv_obj_t *lbl_arc = lv_label_create(parent);
    lv_label_set_text(lbl_arc, "Arc");
    lv_obj_add_style(lbl_arc, &style_dim, 0);

    lv_obj_t *arc = lv_arc_create(parent);
    lv_arc_set_value(arc, 70);
    lv_obj_set_size(arc, 80, 80);
    lv_obj_set_style_arc_color(arc, C_ACCENT,
                               LV_PART_INDICATOR | LV_STATE_DEFAULT);

    /* ── Button ── */
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, C_ACCENT, 0);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Button");
    lv_obj_add_style(btn_lbl, &style_body, 0);

    /* ── Checkbox ── */
    lv_obj_t *cb = lv_checkbox_create(parent);
    lv_checkbox_set_text(cb, "Enable feature");
    lv_obj_set_style_text_color(cb, C_TEXT, 0);
    lv_obj_set_style_bg_color(cb, C_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);

    /* ── Dropdown ── */
    lv_obj_t *lbl_dd = lv_label_create(parent);
    lv_label_set_text(lbl_dd, "Dropdown");
    lv_obj_add_style(lbl_dd, &style_dim, 0);

    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, "Option 1\nOption 2\nOption 3");
    lv_obj_set_width(dd, LV_PCT(80));
    lv_obj_set_style_bg_color(dd, C_CARD, 0);
    lv_obj_set_style_text_color(dd, C_TEXT, 0);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   "More" tab — placeholder
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void create_other_tab(lv_obj_t *parent)
{
    lv_obj_set_style_pad_all(parent, 16, 0);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_add_style(title, &style_title, 0);

    lv_obj_t *info = lv_label_create(card);
    lv_label_set_text(info,
                      "Add your own content here.\n\n"
                      "API key: edit API_FOOTBALL_KEY\n"
                      "in lv_demo_widgets.c");
    lv_obj_add_style(info, &style_dim, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, LV_PCT(90));
}