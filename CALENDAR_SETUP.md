# Google Calendar Tab Setup Guide

## Overview
The Calendar tab displays up to 5 upcoming events from **one or more Google Calendars**, merged into a single sorted list with date/time and title. Each event shows which calendar it came from. The tab refreshes **only when you open it** (no continuous polling).

### Features
- ✅ **Multi-calendar support**: Fetch from 2, 3, or more calendars at once
- ✅ **Automatic sorting**: Events sorted by date/time (earliest first)
- ✅ **Timezone-aware**: Converts UTC to your local timezone
- ✅ **On-demand refresh**: Fetches data only when you open the tab
- ✅ **Private calendar access**: Uses OAuth2 refresh tokens (no exposed credentials)
- ✅ **Graceful error handling**: Shows auth/network errors in UI

---

## 1. Google Cloud Project Setup

### Step 1: Create a Google Cloud Project
1. Go to [Google Cloud Console](https://console.cloud.google.com/)
2. Create a **New Project**
3. Name it (e.g., "ESP32 Calendar")
4. Wait for creation to complete

### Step 2: Enable Google Calendar API
1. In the Console, search for **"Calendar API"**
2. Click **Enable** to activate it for your project

### Step 3: Create OAuth 2.0 Credentials
1. Go to **APIs & Services > Credentials**
2. Click **Create Credentials > OAuth client ID**
3. If prompted, configure the OAuth consent screen first:
   - **User Type**: Select Internal (or External if you prefer)
   - **App name**: "ESP32 Calendar"
   - **Scopes**: Add `https://www.googleapis.com/auth/calendar.readonly`
   - Save and continue
4. For Application type, select **Desktop** (or Web if Desktop is unavailable)
5. Create the credentials
6. Save both:
   - `Client ID`
   - `Client Secret`

---

## 2. Obtain the Refresh Token

### Option A: Using Browser + cURL (Recommended)
1. **Generate authorization URL** (replace `<CLIENT_ID>`):
   ```
   https://accounts.google.com/o/oauth2/v2/auth?client_id=<CLIENT_ID>&redirect_uri=http://localhost&response_type=code&scope=https://www.googleapis.com/auth/calendar.readonly&access_type=offline&prompt=consent
   ```

2. **Open the URL in your browser**, log in, and grant permission

3. **Copy the authorization code** from the redirect URL:
   - You'll be redirected to `http://localhost/?code=<CODE_HERE>&scope=...`
   - Copy the `code` value

4. **Exchange code for refresh token** using cURL or Postman:
   ```bash
   curl -X POST https://oauth2.googleapis.com/token \
     -d "client_id=<CLIENT_ID>" \
     -d "client_secret=<CLIENT_SECRET>" \
     -d "code=<CODE_FROM_STEP_3>" \
     -d "redirect_uri=http://localhost" \
     -d "grant_type=authorization_code"
   ```

5. **Save the `refresh_token`** from the JSON response:
   ```json
   {
     "access_token": "...",
     "expires_in": 3599,
     "refresh_token": "1//...",
     "scope": "...",
     "token_type": "Bearer"
   }
   ```

### Option B: Using Google OAuth 2.0 Playground
1. Go to [Google OAuth 2.0 Playground](https://developers.google.com/oauthplayground/)
2. Select **Google Calendar API v3** from the list
3. Select scope: `https://www.googleapis.com/auth/calendar.readonly`
4. Click **Authorize APIs**
5. Approve the request
6. Click **Exchange authorization code for tokens**
7. Copy the `refresh_token`

---

## 3. Configure the ESP32 Code

### Edit `lv_demo_widgets.cpp`

Find these lines near the top (around line 37-45):
```cpp
/* ── Google Calendar config ──────────────────────────────── */
#define CALENDAR_MAX_EVENTS 5
#define CALENDAR_LOOKAHEAD_HOURS 168
#define GOOGLE_CLIENT_ID "YOUR_CLIENT_ID_HERE"
#define GOOGLE_CLIENT_SECRET "YOUR_CLIENT_SECRET_HERE"
#define GOOGLE_REFRESH_TOKEN "YOUR_REFRESH_TOKEN_HERE"
/* Multiple calendars: add comma-separated calendar IDs */
#define CALENDAR_COUNT 2
#define CALENDAR_IDS { "primary", "your_second_calendar@gmail.com" }
```

**Basic setup (1 calendar):**
```cpp
#define GOOGLE_CLIENT_ID "1234567890-abc...xyz.apps.googleusercontent.com"
#define GOOGLE_CLIENT_SECRET "GOCSPX-abc...xyz"
#define GOOGLE_REFRESH_TOKEN "1//0gV...XYZ"
#define CALENDAR_COUNT 1
#define CALENDAR_IDS { "primary" }
```

**Multiple calendars (recommended):**
```cpp
#define GOOGLE_CLIENT_ID "1234567890-abc...xyz.apps.googleusercontent.com"
#define GOOGLE_CLIENT_SECRET "GOCSPX-abc...xyz"
#define GOOGLE_REFRESH_TOKEN "1//0gV...XYZ"
#define CALENDAR_COUNT 2
#define CALENDAR_IDS { "primary", "work@gmail.com" }
```

Or 3 calendars:
```cpp
#define CALENDAR_COUNT 3
#define CALENDAR_IDS { "primary", "work@gmail.com", "family@gmail.com" }
```

### Configurable Parameters
- **`CALENDAR_MAX_EVENTS`**: Total events to display (default 5). Shows up to this many **combined** from all calendars
- **`CALENDAR_LOOKAHEAD_HOURS`**: Change `168` (7 days) to look ahead a different duration
- **`CALENDAR_COUNT`**: Number of calendars to fetch (1-5 recommended; each adds latency)
- **`CALENDAR_IDS`**: Array of calendar IDs:
  - `"primary"` = your main Google Calendar
  - `"email@gmail.com"` = specific shared or secondary calendar
  - Find calendar IDs in Google Calendar Settings → Calendar name → Integrate calendar

---

## 4. Build & Flash

1. **Ensure WiFi credentials** are set in `LvglWidgets.ino`:
   ```cpp
   const char *ssid = "Your_WiFi_SSID";
   const char *password = "Your_WiFi_Password";
   ```

2. **Compile** in Arduino IDE
   - Select your board (e.g., ESP32)
   - Set COM port

3. **Upload** to device

---

## 5. Testing

### First Boot
1. Device connects to WiFi
2. **Arsenal tab** loads (as before)
3. Switch to **Calendar tab** → Status shows "Pull to load events..."
4. Tap the Calendar tab again or let it refresh → **Background task spawns and fetches from all calendars**
5. Watch Serial Monitor for debug output:
   ```
   [Calendar] Tab opened — spawning fetch task
   [Calendar] --- Starting fetch ---
   [Calendar] Syncing time...
   [Calendar] Access token obtained, expires in 3600 seconds
   [Calendar] Fetching from: primary
   [Calendar] Event: Team Meeting @ Mar 31, 14:30
   [Calendar] Event: Personal Reminder @ Apr 01, 09:00
   [Calendar] From primary: 2 events
   [Calendar] Fetching from: work@gmail.com
   [Calendar] Event: Standup @ Mar 31, 10:00
   [Calendar] From work@gmail.com: 1 event
   [Calendar] Events sorted by time
   [Calendar] Total loaded: 3 events
   ```

6. **Calendar tab displays events** with date/time, title, and calendar source name

---

## 6. Troubleshooting

### "Auth failed"
- Check `GOOGLE_CLIENT_ID`, `GOOGLE_CLIENT_SECRET`, `GOOGLE_REFRESH_TOKEN` — ensure no extra spaces
- **Refresh token may have expired** — re-authorize using the browser flow above

### "HTTP 401"
- Access token refresh failed
- WiFi may be unstable; check connection

### "HTTP 403"
- Calendar API may not be enabled or refresh token lacks permission
- Verify scopes include `calendar.readonly` in OAuth consent screen

### No events displayed
- Calendar may be private or empty
- Check `GOOGLE_CALENDAR_ID` — if not "primary", verify the ID exists
- Scroll down in tab to see all events (up to 5)

### Serial monitor shows nothing
- Ensure **Tools > Core Debug Level** is set to "Info" or higher

---

## 7. Security Notes

⚠️ **Important:**
- **Never commit** `GOOGLE_CLIENT_SECRET` or `GOOGLE_REFRESH_TOKEN` to public repositories
- Consider using `Preferences` (ESP32 NVS) to store secrets instead of hardcoding:
  ```cpp
  Preferences prefs;
  prefs.begin("calendar", false);
  String token = prefs.getString("refresh_token", "");
  prefs.end();
  ```

---

## 8. API Refresh Behavior

- **Access tokens** expire in ~1 hour (`3600` seconds)
- Code automatically refreshes the access token **before each fetch**
- **Refresh tokens** are long-lived (until revoked or password changed)
- Each time you open the Calendar tab, a **new background task fetches fresh events**

---

## FAQ

**Q: Can I see events from multiple calendars?**  
A: Yes! Set `CALENDAR_COUNT` and add multiple IDs to `CALENDAR_IDS` array. All events are merged, sorted by date/time, and capped at `CALENDAR_MAX_EVENTS` total. Each event shows which calendar it came from.

**Q: How are multi-calendar events sorted?**  
A: All events from all calendars are automatically sorted by date/time (earliest first). No calendar takes priority.

**Q: How often does it refresh?**  
A: Only when you open the Calendar tab. Set `CALENDAR_MAX_EVENTS` to lower value if you want faster initial display.

**Q: What if the device loses WiFi while fetching?**  
A: The background task will timeout gracefully, and the tab will show "WiFi not connected" on next open attempt.

**Q: Can I display all-day events?**  
A: Yes — the code parses both `start.dateTime` (timed events) and `start.date` (all-day).

**Q: How do I find a secondary calendar ID?**  
A: In Google Calendar:
1. Right-click the calendar name
2. Select **Settings**
3. Under **Integrate calendar**, copy the **Calendar ID** (looks like `email@gmail.com` or `abc123#....@group.calendar.google.com`)
4. Add to `CALENDAR_IDS` array with quotes: `"calendar-id@gmail.com"`

**Q: Can I have more than 5 events?**  
A: Increase `CALENDAR_MAX_EVENTS` to any number. Note: each additional calendar adds ~1 second of fetch time.

---

## Next Steps

1. ✅ Fill in your credentials in `lv_demo_widgets.cpp`
2. ✅ Build and flash
3. ✅ Open Calendar tab and watch Serial Monitor
4. ✅ Enjoy your upcoming events on the TFT display!

---

**Questions?** Check Serial Monitor output for debug messages.
