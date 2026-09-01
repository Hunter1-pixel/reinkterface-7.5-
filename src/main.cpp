// vim: foldmethod=marker:foldmarker={{{,}}}
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>

#include <GxEPD2_BW.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "bazzite_logo.h"
#include "sleep_screen.h"

#define SERVICE_UUID                                                                               \
    NimBLEUUID { "95c7b479-8e84-4ce7-a121-faf74bf48c84" }
#define TOPLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871900" }
#define MIDLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871901" }
#define BOTLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871902" }
#define KEYVAL_UUID                                                                                \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871903" }
#define VECTOR_UUID                                                                                \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871904" }
#define FLUSH_UUID                                                                                 \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a8719FF" }

// Seeed XIAO EE04 board pin definitions
#define EPD_CS   44
#define EPD_DC   10
#define EPD_RST  38
#define EPD_BUSY 4

#define SPARKBOX_HEIGHT 150
#define SPARKBOX_WIDTH  209

#define INTERFACE_VERSION "IFv01"
#define GIT_REVISION "NICNIC 0.0.3"

NimBLEServer *BLE_SERVER = nullptr;
std::string BLE_NAME = "INKTF";

#define Debug Serial

bool INVERTED = false;
#define FG_COLOR (INVERTED ? GxEPD_WHITE : GxEPD_BLACK)
#define BG_COLOR (INVERTED ? GxEPD_BLACK : GxEPD_WHITE)

// 5 minutes disconnected enters enter idle mode
static constexpr unsigned long IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;

// separate BLE advertising profiles
// NimBLE intervals are in 0.625 ms units
// active: 100-200 ms, idle: 1000-2000 ms
static constexpr uint16_t ADV_ACTIVE_MIN = 160;
static constexpr uint16_t ADV_ACTIVE_MAX = 320;
static constexpr uint16_t ADV_IDLE_MIN   = 1600;
static constexpr uint16_t ADV_IDLE_MAX   = 3200;

// track idle state and disconnect timing
static bool IDLE_MODE = false;
static unsigned long LAST_DISCONNECT_MS = 0;

// forces a full-panel redraw instead of a per-widget partial one
static bool FORCE_FULL_REFRESH = true;
static uint8_t PARTIAL_REFRESH_COUNT = 0;
static constexpr uint8_t FULL_REFRESH_INTERVAL = 20;

// guards STATE between BLE callbacks (their own FreeRTOS task) and loop()
static SemaphoreHandle_t STATE_MUTEX = nullptr;

// Waveshare 5.83" 648x480, SSD1677 controller
// Full HEIGHT buffer is safe on ESP32-S3 Plus with 8MB PSRAM (~48KB for 1bpp)
GxEPD2_BW<GxEPD2_583_GDEQ0583T31, GxEPD2_583_GDEQ0583T31::HEIGHT>
    MF_DISPLAY(GxEPD2_583_GDEQ0583T31(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static unsigned long DISP_DEBOUNCE = 0;

struct Point { // {{{
    float x;
    float y;

    Point()
        : x(0)
        , y(0)
    {
    }
    Point(float _x, float _y)
        : x(_x)
        , y(_y)
    {
    }
}; // }}}

struct Points { // {{{
    float yMin;
    float yMax;
    std::vector<Point> points;

    Points()
        : yMin(0)
        , yMax(0)
        , points()
    {
    }

    void clear()
    {
        yMin = 0;
        yMax = 0;
        points.clear();
    }
}; // }}}

struct KeyVal { // {{{
    std::string key;
    std::string val;

    KeyVal()
        : key{""}
        , val{""}
    {
    }
}; // }}}
typedef std::vector<KeyVal> KeyVals;

struct State { // {{{
    bool connected = false;
    std::string topLine{"Starting up..."};
    std::string midLine{"No User"};
    std::string botLine{"No Activity"};
    std::string hostMsg{""};

    KeyVals keyvals{9};
    std::vector<Points> sparks{6};

    void reset()
    {
        keyvals.clear();
        keyvals.resize(9);
        sparks.clear();
        sparks.resize(6);

        uint32_t addr = (uint64_t)NimBLEDevice::getAddress() & 0xFFFFFF;
        std::stringstream name;
        name << "INKTF-";
        name << std::uppercase << std::hex << std::setfill('0') << std::setw(6) << addr;
        BLE_NAME = name.str();

        connected = false;
        topLine = "Waiting on connection...";
        midLine = BLE_NAME;
        botLine = "";
        hostMsg = "";
        keyvals[0].key = "OS";
        keyvals[0].val = "--";
        keyvals[1].key = "BIOS";
        keyvals[1].val = "--";
        keyvals[2].key = "STEAM";
        keyvals[2].val = "--";
        keyvals[3].key = "CPU";
        keyvals[3].val = "-- dC";
        keyvals[4].key = "GPU";
        keyvals[4].val = "-- dC";
        keyvals[5].key = "FAN";
        keyvals[5].val = "-- RPM";
        keyvals[6].key = "CPU";
        keyvals[6].val = "--%";
        keyvals[7].key = "GPU";
        keyvals[7].val = "--%";
        keyvals[8].key = "MEM";
        keyvals[8].val = "--%";
    }
} STATE; // }}}

struct Rect { // {{{
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
}; // }}}

// widget rects mirroring drawStatic()'s layout - shared by drawing and partial refresh
static const Rect HEADER_RECTS[3] = {
    {115, 10, 533, 40}, // topLine
    {115, 48, 533, 32}, // midLine
    {115, 78, 533, 37}, // botLine
};
static const Rect DISCRETE_RECTS[3] = {
    {5, 115, SPARKBOX_WIDTH, 26},
    {219, 115, SPARKBOX_WIDTH, 26},
    {433, 115, SPARKBOX_WIDTH, 26},
};
static const Rect SPARK_RECTS[6] = {
    {5, 146, SPARKBOX_WIDTH, SPARKBOX_HEIGHT},
    {219, 146, SPARKBOX_WIDTH, SPARKBOX_HEIGHT},
    {433, 146, SPARKBOX_WIDTH, SPARKBOX_HEIGHT},
    {5, 301, SPARKBOX_WIDTH, SPARKBOX_HEIGHT},
    {219, 301, SPARKBOX_WIDTH, SPARKBOX_HEIGHT},
    {433, 301, SPARKBOX_WIDTH, SPARKBOX_HEIGHT},
};
static const Rect HOSTMSG_RECT = {324, 460, 320, 20};

bool sparksEqual(const Points &a, const Points &b)
{ // {{{
    if (a.yMin != b.yMin || a.yMax != b.yMax) {
        return false;
    }
    if (a.points.size() != b.points.size()) {
        return false;
    }
    return std::equal(a.points.begin(), a.points.end(), b.points.begin(),
                      [](const Point &p1, const Point &p2) { return p1.x == p2.x && p1.y == p2.y; });
} // }}}

// last-rendered snapshot, diffed against STATE to find dirty widgets
struct RenderCache { // {{{
    std::string headerLines[3];
    std::string keyvalVals[9];
    Points sparks[6];
    std::string hostMsg;
} RENDER_CACHE; // }}}

void drawText(const char *text, const int16_t &x = -1, const int16_t &y = -1,
              const uint8_t &size = 1, const bool &wrap = false)
{ // {{{
    if (x >= 0 && y >= 0) {
        MF_DISPLAY.setCursor(x, y);
    }
    MF_DISPLAY.setTextSize(size);
    MF_DISPLAY.setTextColor(FG_COLOR);
    MF_DISPLAY.setTextWrap(wrap);
    MF_DISPLAY.print(text);
} // }}}

void drawLogo(int16_t &x, const int16_t &y = 0)
{ // {{{
    MF_DISPLAY.drawBitmap(x, y, bazzite_logo_bitmap, 100, 100, FG_COLOR);
    x += 101;
} // }}}

void drawSparkbox(int16_t &x, const int16_t &y, const std::string &title, const std::string &value,
                  const Points &points)
{ // {{{
    const int16_t w = SPARKBOX_WIDTH;
    const int16_t h = SPARKBOX_HEIGHT;
    const int16_t hpad = 8;
    const int16_t vpad = 6;
    const int16_t title_h = 26;
    const int16_t graph_h = (h - title_h) - 32;
    const int16_t graph_w = w - 20;
    const int16_t graph_x = x + 10;
    const int16_t graph_y = (y + h) - 16;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        MF_DISPLAY.fillRect(x, y + title_h, w, 1, FG_COLOR);
        drawText(title.c_str(), x + hpad, y + vpad, 2);
        drawText(value.c_str(), (x + (w - hpad)) - (12 * strlen(value.c_str())), y + vpad, 2);

        std::stringstream maxstrm;
        maxstrm << std::fixed << std::setprecision(0) << points.yMax;
        auto maxstr = maxstrm.str();
        drawText(maxstr.c_str(), x + hpad, y + title_h + vpad);

        std::stringstream minstrm;
        minstrm << std::fixed << std::setprecision(0) << points.yMin;
        auto minstr = minstrm.str();
        drawText(minstr.c_str(), x + hpad, y + h - (vpad + 7));

        if (points.points.size() >= 2) {
            int16_t s_x = 0.0, s_y = 0.0, e_x = 0.0, e_y = 0.0;
            for (auto p = points.points.cbegin(); p != points.points.cend() - 1; ++p) {
                s_x = graph_x + (p->x * graph_w);
                e_x = graph_x + ((p + 1)->x * graph_w);
                s_y = graph_y + (p->y * graph_h * -1.0);
                e_y = graph_y + ((p + 1)->y * graph_h * -1.0);
                MF_DISPLAY.drawLine(s_x, s_y, e_x, e_y, FG_COLOR);
                MF_DISPLAY.drawLine(s_x, s_y - 1, e_x, e_y - 1, FG_COLOR);
                MF_DISPLAY.drawLine(s_x, s_y + 1, e_x, e_y + 1, FG_COLOR);
                MF_DISPLAY.drawLine(s_x - 1, s_y, e_x - 1, e_y, FG_COLOR);
                MF_DISPLAY.drawLine(s_x + 1, s_y, e_x + 1, e_y, FG_COLOR);
            }
        }
    }

    x += w;
} // }}}

void drawDiscreteBox(int16_t &x, const int16_t &y, const std::string &title,
                     const std::string &value)
{ // {{{
    const int16_t w = 209;
    const int16_t h = 26;
    const int16_t hpad = 8;
    const int16_t vpad = 6;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        drawText(title.c_str(), x + hpad, y + vpad, 2);
        drawText(value.c_str(), (x + (w - hpad)) - (12 * strlen(value.c_str())), y + vpad, 2);
    }

    x += w;
} // }}}

const std::string &headerLineText(const State &s, uint8_t idx)
{ // {{{
    if (idx == 0) {
        return s.topLine;
    } else if (idx == 1) {
        return s.midLine;
    } else {
        return s.botLine;
    }
} // }}}

void drawHeaderLine(const State &s, uint8_t idx)
{ // {{{
    static const int16_t X = 120;
    static const int16_t Y[3] = {15, 50, 80};
    static const uint8_t SIZE[3] = {3, 2, 2};
    drawText(headerLineText(s, idx).c_str(), X, Y[idx], SIZE[idx]);
} // }}}

void drawHostMsg(const State &s)
{ // {{{
    int16_t x = MF_DISPLAY.width() - (6 * strlen(s.hostMsg.c_str())) - 5;
    int16_t y = MF_DISPLAY.height() - 12;
    drawText(s.hostMsg.c_str(), x, y);
} // }}}

void drawStatic(const State &s)
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    // logo in top left corner
    x = 5;
    y = 5;
    drawLogo(x, y);

    // show connected fremont hostname/serial or connecting status
    drawHeaderLine(s, 0);
    drawHeaderLine(s, 1);
    drawHeaderLine(s, 2);

    // first row of boxes with no sparklines
    x = 5;
    y = 115;
    drawDiscreteBox(x, y, s.keyvals[0].key, s.keyvals[0].val);
    x += 5;
    drawDiscreteBox(x, y, s.keyvals[1].key, s.keyvals[1].val);
    x += 5;
    drawDiscreteBox(x, y, s.keyvals[2].key, s.keyvals[2].val);

    // second row
    x = 5;
    y += 26 + 5;
    drawSparkbox(x, y, s.keyvals[3].key, s.keyvals[3].val, s.sparks[0]);
    x += 5;
    drawSparkbox(x, y, s.keyvals[4].key, s.keyvals[4].val, s.sparks[1]);
    x += 5;
    drawSparkbox(x, y, s.keyvals[5].key, s.keyvals[5].val, s.sparks[2]);

    // third row
    x = 5;
    y += SPARKBOX_HEIGHT + 5;
    drawSparkbox(x, y, s.keyvals[6].key, s.keyvals[6].val, s.sparks[3]);
    x += 5;
    drawSparkbox(x, y, s.keyvals[7].key, s.keyvals[7].val, s.sparks[4]);
    x += 5;
    drawSparkbox(x, y, s.keyvals[8].key, s.keyvals[8].val, s.sparks[5]);

    // version tag
    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    x = 5;
    y = MF_DISPLAY.height() - 12;
    drawText(tag.str().c_str(), x, y);

    // host message if provided (usually a timestamp)
    drawHostMsg(s);
} // }}}

// redraws only the changed widgets via partial refresh; full refresh when forced
// (boot/connect/disconnect/idle-exit) or periodically, to bound e-ink ghosting
void redrawDashboard()
{ // {{{
    // init() must be called again after hibernate() to wake the panel
    MF_DISPLAY.init(115200, false, 2, false);

    // drawPixel() is relative to the last setPartialWindow() call, so stay in
    // full-window mode for all drawing; displayWindow() alone scopes the panel refresh
    MF_DISPLAY.setFullWindow();

    // snapshot STATE under the lock so the slow draw below can't race a BLE write
    xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
    State snapshot = STATE;
    xSemaphoreGive(STATE_MUTEX);

    std::string dbg = "STATE at draw time: top=[" + snapshot.topLine + "] mid=[" + snapshot.midLine
        + "] bot=[" + snapshot.botLine + "] OS=[" + snapshot.keyvals[0].val + "] BIOS=["
        + snapshot.keyvals[1].val + "] STEAM=[" + snapshot.keyvals[2].val + "] GPUtemp=["
        + snapshot.keyvals[4].val + "]";
    Debug.println(dbg.c_str());

    bool doFull = FORCE_FULL_REFRESH || (PARTIAL_REFRESH_COUNT >= FULL_REFRESH_INTERVAL);

    if (doFull) {
        Debug.println("full refresh");
        MF_DISPLAY.fillScreen(BG_COLOR);
        drawStatic(snapshot);
        MF_DISPLAY.display(false);

        for (uint8_t i = 0; i < 3; i++) {
            RENDER_CACHE.headerLines[i] = headerLineText(snapshot, i);
        }
        for (uint8_t i = 0; i < 9; i++) {
            RENDER_CACHE.keyvalVals[i] = snapshot.keyvals[i].val;
        }
        for (uint8_t i = 0; i < 6; i++) {
            RENDER_CACHE.sparks[i] = snapshot.sparks[i];
        }
        RENDER_CACHE.hostMsg = snapshot.hostMsg;

        FORCE_FULL_REFRESH = false;
        PARTIAL_REFRESH_COUNT = 0;
    } else {
        Debug.println("partial refresh");
        bool any = false;

        for (uint8_t i = 0; i < 3; i++) {
            const std::string &text = headerLineText(snapshot, i);
            if (text != RENDER_CACHE.headerLines[i]) {
                const Rect &r = HEADER_RECTS[i];
                MF_DISPLAY.fillRect(r.x, r.y, r.w, r.h, BG_COLOR);
                drawHeaderLine(snapshot, i);
                MF_DISPLAY.displayWindow(r.x, r.y, r.w, r.h);
                RENDER_CACHE.headerLines[i] = text;
                any = true;
                Debug.println(("dirty: header" + std::to_string(i)).c_str());
            }
        }

        for (uint8_t i = 0; i < 3; i++) {
            if (snapshot.keyvals[i].val != RENDER_CACHE.keyvalVals[i]) {
                const Rect &r = DISCRETE_RECTS[i];
                int16_t x = r.x;
                MF_DISPLAY.fillRect(r.x, r.y, r.w, r.h, BG_COLOR);
                drawDiscreteBox(x, r.y, snapshot.keyvals[i].key, snapshot.keyvals[i].val);
                MF_DISPLAY.displayWindow(r.x, r.y, r.w, r.h);
                RENDER_CACHE.keyvalVals[i] = snapshot.keyvals[i].val;
                any = true;
                Debug.println(("dirty: discrete" + std::to_string(i)).c_str());
            }
        }

        for (uint8_t i = 0; i < 6; i++) {
            uint8_t kvIdx = i + 3;
            if (snapshot.keyvals[kvIdx].val != RENDER_CACHE.keyvalVals[kvIdx]
                || !sparksEqual(snapshot.sparks[i], RENDER_CACHE.sparks[i])) {
                const Rect &r = SPARK_RECTS[i];
                int16_t x = r.x;
                MF_DISPLAY.fillRect(r.x, r.y, r.w, r.h, BG_COLOR);
                drawSparkbox(x, r.y, snapshot.keyvals[kvIdx].key, snapshot.keyvals[kvIdx].val, snapshot.sparks[i]);
                MF_DISPLAY.displayWindow(r.x, r.y, r.w, r.h);
                RENDER_CACHE.keyvalVals[kvIdx] = snapshot.keyvals[kvIdx].val;
                RENDER_CACHE.sparks[i] = snapshot.sparks[i];
                any = true;
                Debug.println(("dirty: spark" + std::to_string(i)).c_str());
            }
        }

        if (snapshot.hostMsg != RENDER_CACHE.hostMsg) {
            const Rect &r = HOSTMSG_RECT;
            MF_DISPLAY.fillRect(r.x, r.y, r.w, r.h, BG_COLOR);
            drawHostMsg(snapshot);
            MF_DISPLAY.displayWindow(r.x, r.y, r.w, r.h);
            RENDER_CACHE.hostMsg = snapshot.hostMsg;
            any = true;
            Debug.println("dirty: hostMsg");
        }

        if (any) {
            PARTIAL_REFRESH_COUNT++;
        }
    }

    // powerOff(), not hibernate(): hibernate() resets the panel on wake, wiping the
    // previous-frame RAM partial refresh diffs against. hibernate() is still used to
    // enter idle mode, which always forces a full refresh on wake so it's unaffected
    MF_DISPLAY.powerOff();
} // }}}

// helper to switch advertising speed
// keeps device discoverable while saving power
void setAdvertisingProfile(bool idle)
{ // {{{
    BLEAdvertising *advert = NimBLEDevice::getAdvertising();
    bool wasAdvertising = advert->isAdvertising();

    if (wasAdvertising) {
        NimBLEDevice::stopAdvertising();
    }

    if (idle) {
        advert->setMinInterval(ADV_IDLE_MIN);
        advert->setMaxInterval(ADV_IDLE_MAX);
    } else {
        advert->setMinInterval(ADV_ACTIVE_MIN);
        advert->setMaxInterval(ADV_ACTIVE_MAX);
    }

    NimBLEDevice::startAdvertising();
} // }}}

// draw the dedicated idle bitmap once, then hibernate the panel
void drawSleepScreen()
{ // {{{
    MF_DISPLAY.init(115200, false, 2, false);
    MF_DISPLAY.setFullWindow();
    MF_DISPLAY.fillScreen(BG_COLOR);

    const int16_t bmpW = 640;
    const int16_t bmpH = 480;
    const int16_t x = (MF_DISPLAY.width() - bmpW) / 2;
    const int16_t y = 0;

    MF_DISPLAY.drawBitmap(x, y, sleep_screen_bitmap, bmpW, bmpH, FG_COLOR);
    MF_DISPLAY.display();
    MF_DISPLAY.hibernate();
} // }}}

void enterIdleMode()
{ // {{{
    if (IDLE_MODE) {
        return;
    }

    Debug.println("entering idle mode");
    IDLE_MODE = true;
    DISP_DEBOUNCE = 0; // normal dashboard redraw no longer needed in idle
    setAdvertisingProfile(true);
    drawSleepScreen();
} // }}}

void exitIdleMode()
{ // {{{
    if (!IDLE_MODE) {
        return;
    }

    Debug.println("leaving idle mode");
    IDLE_MODE = false;
    setAdvertisingProfile(false);
    FORCE_FULL_REFRESH = true; // sleep bitmap needs a clean full-refresh baseline
    DISP_DEBOUNCE = 10; // redraw normal dashboard soon
} // }}}

class ServerCallbacks : public NimBLEServerCallbacks
{ // {{{
    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn) override
    {
        Debug.println("got connection");
        NimBLEDevice::stopAdvertising();
        xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
        STATE.connected = true;
        xSemaphoreGive(STATE_MUTEX);
        FORCE_FULL_REFRESH = true; // so one-time data (OS/BIOS/STEAM, etc) is never missed

        // any connection exits idle immediately and restores the normal dashboard
        exitIdleMode();
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &conn, int reason) override
    {
        Debug.print("got disconnect event, connected count: ");
        Debug.println(server->getConnectedCount());
        if (server->getConnectedCount() <= 1) {
            if (STATE.connected) {
                DISP_DEBOUNCE = 100;
            }
            xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
            STATE.reset();
            xSemaphoreGive(STATE_MUTEX);
            FORCE_FULL_REFRESH = true; // clean baseline for the "waiting on connection" screen

            // start the idle timeout when the device becomes disconnected
            LAST_DISCONNECT_MS = millis();
        }
        NimBLEDevice::startAdvertising();
    }
} SERVER_CALLBACKS; // }}}

class StatusLineCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        auto uuid = characteristic->getUUID();
        if (uuid == TOPLINE_UUID && STATE.topLine != value) {
            xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
            STATE.topLine = value;
            xSemaphoreGive(STATE_MUTEX);
            Debug.println(("got topLine: " + value).c_str());
        } else if (uuid == MIDLINE_UUID && STATE.midLine != value) {
            xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
            STATE.midLine = value;
            xSemaphoreGive(STATE_MUTEX);
            Debug.println(("got midLine: " + value).c_str());
        } else if (uuid == BOTLINE_UUID && STATE.botLine != value) {
            xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
            STATE.botLine = value;
            xSemaphoreGive(STATE_MUTEX);
            Debug.println(("got botLine: " + value).c_str());
        } else if (uuid != TOPLINE_UUID && uuid != MIDLINE_UUID && uuid != BOTLINE_UUID) {
            Debug.print("Got value (");
            Debug.print(value.c_str());
            Debug.print(") for unknown UUID (");
            Debug.print(uuid.toString().c_str());
            Debug.println("), ignoring.");
            return;
        }
    }
} STATUS_CALLBACKS; // }}}

class KeyValCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    typedef struct __attribute__((packed)) {
        uint8_t index;
        char key[32];
        char val[32];
    } Msg;

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        Msg msg;
        if (value.length() == sizeof(Msg)) {
            memcpy(&msg, value.data(), sizeof(Msg));
            xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
            STATE.keyvals[msg.index].key = msg.key;
            STATE.keyvals[msg.index].val = msg.val;
            xSemaphoreGive(STATE_MUTEX);
            Debug.println(("got keyval index " + std::to_string(msg.index) + ": " + msg.key + " = "
                           + msg.val)
                              .c_str());
        } else {
            Debug.print("got bad keyval write, size: ");
            Debug.println(value.length());
        }
    }
} KEYVAL_CALLBACKS; // }}}

class VectorCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    typedef struct __attribute__((packed)) {
        uint8_t index;
        uint8_t count;
        float minVal;
        float maxVal;
        uint8_t values[32 * 2];
    } Msg;

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        Msg msg;
        if (value.length() >= 2) {
            memcpy(&msg, value.data(), sizeof(Msg));
            Debug.println(("got vector for index (" + std::to_string(msg.index) + ") with "
                           + std::to_string(msg.count) + " values, min " + std::to_string(msg.minVal)
                           + ", max " + std::to_string(msg.maxVal))
                              .c_str());
            xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
            STATE.sparks[msg.index].clear();
            STATE.sparks[msg.index].yMin = msg.minVal;
            STATE.sparks[msg.index].yMax = msg.maxVal;
            for (int i = 0; i < msg.count; i += 2) {
                STATE.sparks[msg.index].points.emplace_back(msg.values[i] / 255.0,
                                                            msg.values[i + 1] / 255.0);
            }
            xSemaphoreGive(STATE_MUTEX);
        } else {
            Debug.print("got bad vectors write, size: ");
            Debug.println(value.length());
        }
    }
} VECTOR_CALLBACKS; // }}}

class FlushCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        xSemaphoreTake(STATE_MUTEX, portMAX_DELAY);
        STATE.hostMsg = characteristic->getValue();
        xSemaphoreGive(STATE_MUTEX);

        // any incoming data implies active use, so leave idle mode and redraw dashboard
        exitIdleMode();

        DISP_DEBOUNCE = 100;
    }
} FLUSH_CALLBACKS; // }}}

void setup()
{ // {{{
    Serial.begin(115200);

#if defined(STARTUP_DELAY_MS)
    delay(STARTUP_DELAY_MS);
#endif

    STATE_MUTEX = xSemaphoreCreateMutex();

    Debug.println("setting up ble device and service");
    NimBLEDevice::init("");
    NimBLEDevice::setPower(2);
    NimBLEDevice::setMTU(256);
    BLE_SERVER = NimBLEDevice::createServer();
    BLE_SERVER->setCallbacks(&SERVER_CALLBACKS);
    BLEService *service = BLE_SERVER->createService(SERVICE_UUID);
    BLECharacteristic *characteristic = nullptr;

    characteristic =
        service->createCharacteristic(TOPLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.topLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);
    characteristic =
        service->createCharacteristic(MIDLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.midLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);
    characteristic =
        service->createCharacteristic(BOTLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.botLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);

    characteristic = service->createCharacteristic(KEYVAL_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&KEYVAL_CALLBACKS);
    characteristic = service->createCharacteristic(VECTOR_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&VECTOR_CALLBACKS);

    characteristic = service->createCharacteristic(FLUSH_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&FLUSH_CALLBACKS);

    BLE_SERVER->start();

    // advertising hasn't started yet, so no BLE callback can race STATE here
    Debug.println("initializing display");
    STATE.reset();
    MF_DISPLAY.init(115200, true, 2, false);
    MF_DISPLAY.setFullWindow();
    MF_DISPLAY.fillScreen(BG_COLOR);
    drawStatic(STATE);
    MF_DISPLAY.display(false);
    MF_DISPLAY.powerOff();

    // sync cache/flag manually since this bypasses redrawDashboard() (needs its own
    // one-time init(..., true, ...) call)
    for (uint8_t i = 0; i < 3; i++) {
        RENDER_CACHE.headerLines[i] = headerLineText(STATE, i);
    }
    for (uint8_t i = 0; i < 9; i++) {
        RENDER_CACHE.keyvalVals[i] = STATE.keyvals[i].val;
    }
    for (uint8_t i = 0; i < 6; i++) {
        RENDER_CACHE.sparks[i] = STATE.sparks[i];
    }
    RENDER_CACHE.hostMsg = STATE.hostMsg;
    FORCE_FULL_REFRESH = false;

    Debug.println("starting ble advert");
    uint32_t addr = (uint64_t)NimBLEDevice::getAddress() & 0xFFFFFF;
    std::stringstream name;
    name << "INKTF-";
    name << std::uppercase << std::hex << std::setfill('0') << std::setw(6) << addr;
    BLE_NAME = name.str();
    BLEAdvertising *advert = NimBLEDevice::getAdvertising();
    BLEAdvertisementData ad_data{};
    ad_data.setName(BLE_NAME);
    ad_data.setManufacturerData("\x5d\x05" INTERFACE_VERSION);
    advert->setAdvertisementData(ad_data);
    advert->addServiceUUID(SERVICE_UUID);
    advert->enableScanResponse(false);

    // start in normal/active advertising profile
    setAdvertisingProfile(false);

    // device starts disconnected, so begin idle timeout from boot
    LAST_DISCONNECT_MS = millis();

} // }}}

void loop()
{ // {{{
    static unsigned long LAST_MS = 0;
    static unsigned long CONN_DEBOUNCE = 5000;

    auto now = millis();
    auto delta = now - LAST_MS;
    if (now < LAST_MS) {
        Debug.println("handling time rollover");
        delta = (std::numeric_limits<unsigned long>::max() - LAST_MS) + now;
    }

    if (CONN_DEBOUNCE > 0 && CONN_DEBOUNCE > delta) {
        CONN_DEBOUNCE -= delta;
    } else if (CONN_DEBOUNCE > 0) {
        bool advertising = NimBLEDevice::getAdvertising()->isAdvertising();
        uint8_t connections = BLE_SERVER->getConnectedCount();
        if (!advertising && connections == 0) {
            Debug.println("starting advertisement, we have no connections");
            NimBLEDevice::startAdvertising();
        } else if (advertising && connections > 0) {
            Debug.println("stopping advertisement, we have connections");
            NimBLEDevice::stopAdvertising();
        }
        CONN_DEBOUNCE = 5000;
    }

    // after 5 minutes with no BLE connection, enter idle mode.
    if (!IDLE_MODE && BLE_SERVER->getConnectedCount() == 0) {
        unsigned long disconnectedFor = now - LAST_DISCONNECT_MS;
        if (now < LAST_DISCONNECT_MS) {
            disconnectedFor = (std::numeric_limits<unsigned long>::max() - LAST_DISCONNECT_MS) + now;
        }

        if (disconnectedFor >= IDLE_TIMEOUT_MS) {
            enterIdleMode();
        }
    }

    if (DISP_DEBOUNCE > 0 && DISP_DEBOUNCE > delta) {
        DISP_DEBOUNCE -= delta;
    } else if (DISP_DEBOUNCE > 0) {
        Debug.println("drawing to display");
        DISP_DEBOUNCE = 0;

        // if idle mode is active, keep the dedicated sleep bitmap instead of redrawing dashboard
        if (!IDLE_MODE) {
            redrawDashboard();
        }

        Debug.println("drew to display");
    }

    LAST_MS = now;
    delay(10);
} //