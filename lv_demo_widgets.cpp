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
#include "secrets.h"

/* ── User config ──────────────────────────────────────────── */
#define ARSENAL_ID 57
#define LEAGUE_CODE "PL"

// Calgary Timezone: Mountain Standard Time (MST) / Mountain Daylight Time (MDT)
// MST is 7 hours behind UTC, MDT is 6 hours behind.
// Rules: Starts March (M3) 2nd Sunday (.2.0), ends Nov (M11) 1st Sunday (.1.0)
#define CALGARY_TZ "MST7MDT,M3.2.0,M11.1.0"
/* ────────────────────────────────────────────────────────── */

/* ── Google Calendar config ─────────────────────────────── */
#define CALENDAR_MAX_EVENTS 5
#define CALENDAR_LOOKAHEAD_HOURS 168
#define CALENDAR_IDS {"primary", "2605dedd02f46bc2b31a09350f488621ae76510324ae4bc6a4e33682ae15b9af@group.calendar.google.com"}
#define CALENDAR_NAMES {"Personal", "Family"}
#define CALENDAR_COUNT 2
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
static void create_calendar_tab(lv_obj_t *parent);
static void fetch_arsenal_data(void);
static void update_fixture_ui(void);
static void update_standings_ui(void);
static void calendar_tab_event_handler(lv_event_t *e);
static void fetch_calendar_data(void);
static void update_calendar_ui(void);
static bool fetch_access_token(char *access_token, size_t len, int *expires_in);
static bool parse_google_datetime(const char *input, char *out, size_t out_len, time_t *out_time);
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

struct CalendarEvent
{
    char summary[64];
    char local_time[32];
    char calendar_name[48];
    time_t event_time_t; /* For sorting */
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

/* ── Calendar state ──────────────────────────────────────── */
static CalendarEvent g_calendar_events[CALENDAR_MAX_EVENTS * 2]; /* Buffer for multi-calendar merge before trimming */
static int g_calendar_event_count = 0;
static bool g_calendar_data_ready = false;
static bool g_calendar_data_error = false;
static char g_calendar_error_msg[128] = {0};
static lv_obj_t *g_calendar_container = NULL;
static lv_obj_t *g_calendar_status_lbl = NULL;
static bool g_calendar_view_initialized = false;
static bool g_calendar_ui_update_ready = false;

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
    if (g_calendar_ui_update_ready)
    {
        update_calendar_ui();
        g_calendar_ui_update_ready = false;
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
    lv_obj_t *tab_arsenal = lv_tabview_add_tab(tv, LV_SYMBOL_LIST " Arsenal");
    lv_obj_t *tab_widgets = lv_tabview_add_tab(tv, LV_SYMBOL_HOME " Widgets");
    lv_obj_t *tab_calendar = lv_tabview_add_tab(tv, LV_SYMBOL_BELL " Calendar");

    lv_obj_set_style_bg_color(tab_arsenal, C_BG, 0);
    lv_obj_set_style_bg_color(tab_widgets, C_BG, 0);
    lv_obj_set_style_bg_color(tab_calendar, C_BG, 0);

    create_arsenal_tab(tab_arsenal);
    create_widgets_tab(tab_widgets);
    create_calendar_tab(tab_calendar);

    /* Register tab change handler for calendar tab refresh on open */
    lv_obj_add_event_cb(tv, calendar_tab_event_handler, LV_EVENT_VALUE_CHANGED, NULL);

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

    /* Sync time with NTP server (UTC; timezone already set in lv_demo_widgets()) */
    Serial.println("[Arsenal] Syncing time with NTP...");
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    /* Set timezone for this background task */
    setenv("TZ", CALGARY_TZ, 1);
    tzset();

    /* Wait for time to be synced (with timeout) */
    time_t now = time(NULL);
    int ntp_attempts = 0;
    const int NTP_TIMEOUT = 100;                      /* ~20 seconds at 200ms intervals */
    while (now < 86400 && ntp_attempts < NTP_TIMEOUT) /* 86400 = 1 day in seconds; sanity check */
    {
        delay(200);
        now = time(NULL);
        ntp_attempts++;
    }

    if (now < 86400)
    {
        Serial.println("[Arsenal] Error: Failed to sync time with NTP");
        g_data_error = true;
        return;
    }

    Serial.printf("[Arsenal] Time synced to: %ld\n", now);

    HTTPClient http;
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

                /* Ensure timezone is set before converting */
                tzset();

                /* Convert UTC time_t to Calgary local time (MDT/MST) and store formatted date */
                struct tm *tm_calgary = localtime(&g_fixture_time_utc);
                if (tm_calgary)
                {
                    char calgary_date_str[32];
                    strftime(calgary_date_str, sizeof(calgary_date_str), "%Y-%m-%d %H:%M", tm_calgary);
                    strlcpy(g_fixture.date, calgary_date_str, sizeof(g_fixture.date));
                    Serial.printf("[Fixture] Match time converted to Calgary: %s\n", g_fixture.date);
                }
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

    /* Date & Time — g_fixture.date is already in Calgary local time (MDT/MST) */
    char date_str[64] = "Date: —";
    if (g_fixture.date[0] != '\0')
    {
        snprintf(date_str, sizeof(date_str), "Date: %s", g_fixture.date);
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
   Calendar tab
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void create_calendar_tab(lv_obj_t *parent)
{
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);
    lv_obj_set_style_pad_all(parent, 8, 0);
    lv_obj_set_style_pad_row(parent, 10, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Header */
    lv_obj_t *header = lv_label_create(parent);
    lv_label_set_text(header, LV_SYMBOL_BELL "  NEXT EVENTS");
    lv_obj_add_style(header, &style_dim, 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(header, 2, 0);

    /* Status label */
    g_calendar_status_lbl = lv_label_create(parent);
    lv_label_set_text(g_calendar_status_lbl, "Pull to load events...");
    lv_obj_add_style(g_calendar_status_lbl, &style_dim, 0);
    lv_obj_set_style_text_font(g_calendar_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(g_calendar_status_lbl, LV_PCT(100));

    /* Events container */
    g_calendar_container = lv_obj_create(parent);
    lv_obj_set_width(g_calendar_container, LV_PCT(100));
    lv_obj_set_height(g_calendar_container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(g_calendar_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_calendar_container, 0, 0);
    lv_obj_set_style_pad_all(g_calendar_container, 0, 0);
    lv_obj_set_style_pad_row(g_calendar_container, 0, 0);
    lv_obj_set_flex_flow(g_calendar_container, LV_FLEX_FLOW_COLUMN);
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Calendar tab event handler — fetches data on tab open
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void calendar_tab_event_handler(lv_event_t *e)
{
    lv_obj_t *tv = lv_event_get_target(e);
    uint16_t active_tab = lv_tabview_get_tab_act(tv);

    /* Check if Calendar is the active tab (assuming it's index 2 after Arsenal=0, Widgets=1) */
    if (active_tab == 2 && !g_calendar_view_initialized)
    {
        g_calendar_view_initialized = true;
        Serial.println("[Calendar] Tab opened — spawning fetch task");

        /* Spawn background task to fetch events */
        xTaskCreatePinnedToCore(
            [](void *)
            {
                fetch_calendar_data();
                vTaskDelete(NULL);
            },
            "calendar_fetch",
            16384,
            NULL,
            1,
            NULL,
            0);
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Fetch Google access token using refresh_token
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static bool fetch_access_token(char *access_token, size_t len, int *expires_in)
{
    if (!access_token || len == 0)
        return false;

    HTTPClient http;
    http.begin("https://oauth2.googleapis.com/token");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    /* Build POST body */
    String body = "client_id=" + String(GOOGLE_CLIENT_ID) +
                  "&client_secret=" + String(GOOGLE_CLIENT_SECRET) +
                  "&refresh_token=" + String(GOOGLE_REFRESH_TOKEN) +
                  "&grant_type=refresh_token";

    int httpCode = http.POST(body);
    bool success = false;

    if (httpCode == 200)
    {
        String payload = http.getString();
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, payload);

        if (!err && doc.containsKey("access_token"))
        {
            strlcpy(access_token, doc["access_token"] | "", len);
            *expires_in = doc["expires_in"] | 3600;
            success = true;
            Serial.printf("[Calendar] Access token obtained, expires in %d seconds\n", *expires_in);
        }
        else
        {
            Serial.printf("[Calendar] Token parse error: %s\n", err.c_str());
        }
    }
    else
    {
        Serial.printf("[Calendar] Token request failed: HTTP %d\n", httpCode);
    }

    http.end();
    return success;
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

/* Compare events by time for sorting */
static int compare_calendar_events(const void *a, const void *b)
{
    const CalendarEvent *evt_a = (const CalendarEvent *)a;
    const CalendarEvent *evt_b = (const CalendarEvent *)b;

    if (evt_a->event_time_t < evt_b->event_time_t)
        return -1;
    else if (evt_a->event_time_t > evt_b->event_time_t)
        return 1;
    return 0;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Fetch Google Calendar events
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void fetch_calendar_data(void)
{
    Serial.println("\n[Calendar] --- Starting fetch ---");

    /* Check WiFi */
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[Calendar] Error: WiFi not connected");
        g_calendar_data_error = true;
        strlcpy(g_calendar_error_msg, "WiFi not connected", sizeof(g_calendar_error_msg));
        g_calendar_ui_update_ready = true;
        return;
    }

    /* Ensure time is synced */
    time_t now = time(NULL);
    if (now < 86400)
    {
        Serial.println("[Calendar] Syncing time...");
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        int tries = 0;
        while (now < 86400 && tries < 100)
        {
            delay(200);
            now = time(NULL);
            tries++;
        }
    }

    if (now < 86400)
    {
        Serial.println("[Calendar] Error: Failed to sync time");
        g_calendar_data_error = true;
        strlcpy(g_calendar_error_msg, "Time sync failed", sizeof(g_calendar_error_msg));
        g_calendar_ui_update_ready = true;
        return;
    }

    /* Set timezone */
    setenv("TZ", CALGARY_TZ, 1);
    tzset();

    /* Fetch access token */
    char access_token[512] = {0};
    int expires_in = 0;
    if (!fetch_access_token(access_token, sizeof(access_token), &expires_in))
    {
        Serial.println("[Calendar] Failed to get access token");
        g_calendar_data_error = true;
        strlcpy(g_calendar_error_msg, "Auth failed", sizeof(g_calendar_error_msg));
        g_calendar_ui_update_ready = true;
        return;
    }

    /* Build timeMin (now in ISO 8601 UTC) */
    struct tm *tm_utc = gmtime(&now);
    char timeMin[32] = {0};
    strftime(timeMin, sizeof(timeMin), "%Y-%m-%dT%H:%M:%SZ", tm_utc);

    /* Setup calendar IDs array */
    const char *calendar_ids[CALENDAR_COUNT] = CALENDAR_IDS;
    const char *calendar_names[CALENDAR_COUNT] = CALENDAR_NAMES;

    g_calendar_event_count = 0;
    g_calendar_data_ready = false;
    g_calendar_data_error = false;

    HTTPClient http;

    /* Loop through each calendar and fetch events */
    for (int cal_idx = 0; cal_idx < CALENDAR_COUNT; cal_idx++)
    {
        const char *cal_id = calendar_ids[cal_idx];
        Serial.printf("[Calendar] Fetching from: %s\n", cal_id);

        /* Build calendar API URL */
        String calendarUrl = "https://www.googleapis.com/calendar/v3/calendars/" +
                             String(cal_id) +
                             "/events?orderBy=startTime&singleEvents=true&timeMin=" +
                             String(timeMin) +
                             "&maxResults=" + String(CALENDAR_MAX_EVENTS);

        http.begin(calendarUrl);
        http.addHeader("Authorization", "Bearer " + String(access_token));
        http.addHeader("Accept-Encoding", "identity");

        int httpCode = http.GET();

        if (httpCode == 200)
        {
            String payload = http.getString();
            DynamicJsonDocument doc(8192);
            DeserializationError err = deserializeJson(doc, payload);

            if (!err && doc.containsKey("items"))
            {
                JsonArray items = doc["items"];
                for (JsonObject item : items)
                {
                    if (g_calendar_event_count >= CALENDAR_MAX_EVENTS * 2) /* Allow temporary overflow for multi-calendar merge */
                        break;

                    CalendarEvent &evt = g_calendar_events[g_calendar_event_count];

                    /* Summary */
                    strlcpy(evt.summary, item["summary"] | "(No title)", sizeof(evt.summary));

                    /* Calendar name (display name) */
                    strlcpy(evt.calendar_name, calendar_names[cal_idx], sizeof(evt.calendar_name));

                    /* DateTime or Date */
                    const char *dt_str = NULL;
                    if (item.containsKey("start") && item["start"].containsKey("dateTime"))
                    {
                        dt_str = item["start"]["dateTime"];
                    }
                    else if (item.containsKey("start") && item["start"].containsKey("date"))
                    {
                        dt_str = item["start"]["date"];
                    }

                    if (dt_str)
                    {
                        parse_google_datetime(dt_str, evt.local_time, sizeof(evt.local_time), &evt.event_time_t);
                    }
                    else
                    {
                        strlcpy(evt.local_time, "Time TBD", sizeof(evt.local_time));
                        evt.event_time_t = 0;
                    }

                    g_calendar_event_count++;
                    Serial.printf("[Calendar] Event: %s @ %s\n", evt.summary, evt.local_time);
                }

                Serial.printf("[Calendar] From %s: %d events\n", cal_id, g_calendar_event_count);
            }
            else
            {
                Serial.printf("[Calendar] Parse error from %s: %s\n", cal_id, err.c_str());
            }
        }
        else
        {
            Serial.printf("[Calendar] HTTP %d from %s\n", httpCode, cal_id);
        }

        http.end();
    }

    /* Sort all events by date/time */
    if (g_calendar_event_count > 1)
    {
        qsort(g_calendar_events, g_calendar_event_count, sizeof(CalendarEvent), compare_calendar_events);
        Serial.println("[Calendar] Events sorted by time");
    }

    /* Trim to max displayable events */
    if (g_calendar_event_count > CALENDAR_MAX_EVENTS)
    {
        g_calendar_event_count = CALENDAR_MAX_EVENTS;
        Serial.printf("[Calendar] Trimmed to %d events\n", CALENDAR_MAX_EVENTS);
    }

    if (g_calendar_event_count > 0)
    {
        g_calendar_data_ready = true;
        Serial.printf("[Calendar] Total loaded: %d events\n", g_calendar_event_count);
    }
    else
    {
        g_calendar_data_error = true;
        strlcpy(g_calendar_error_msg, "No events found", sizeof(g_calendar_error_msg));
    }

    /* Signal UI update */
    g_calendar_ui_update_ready = true;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Update calendar UI with events
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void update_calendar_ui(void)
{
    if (!g_calendar_container || !g_calendar_status_lbl)
        return;

    lv_obj_clean(g_calendar_container);

    if (g_calendar_data_error)
    {
        lv_label_set_text(g_calendar_status_lbl, g_calendar_error_msg);
        return;
    }

    if (!g_calendar_data_ready)
        return;

    if (g_calendar_event_count == 0)
    {
        lv_label_set_text(g_calendar_status_lbl, "No events found");
        return;
    }

    lv_label_set_text(g_calendar_status_lbl, "");

    /* Create event rows */
    for (int i = 0; i < g_calendar_event_count; i++)
    {
        CalendarEvent &evt = g_calendar_events[i];

        /* Event card */
        lv_obj_t *card = lv_obj_create(g_calendar_container);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_add_style(card, &style_card, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        /* DateTime */
        lv_obj_t *dt_lbl = lv_label_create(card);
        lv_label_set_text(dt_lbl, evt.local_time);
        lv_obj_add_style(dt_lbl, &style_accent, 0);
        lv_obj_set_style_text_font(dt_lbl, &lv_font_montserrat_14, 0);

        /* Summary / Title */
        lv_obj_t *title_lbl = lv_label_create(card);
        lv_label_set_text(title_lbl, evt.summary);
        lv_obj_add_style(title_lbl, &style_body, 0);
        lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(title_lbl, LV_PCT(100));

        /* Calendar name (source) */
        lv_obj_t *cal_lbl = lv_label_create(card);
        lv_label_set_text(cal_lbl, evt.calendar_name);
        lv_obj_add_style(cal_lbl, &style_dim, 0);
        lv_obj_set_style_text_font(cal_lbl, &lv_font_montserrat_14, 0);
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   Widgets tab — empty for now
   ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static void create_widgets_tab(lv_obj_t *parent)
{
    (void)parent; /* Placeholder for future widgets */
}