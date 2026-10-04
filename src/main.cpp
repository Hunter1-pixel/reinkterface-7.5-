// vim: foldmethod=marker:foldmarker={{{,}}}
#include <iomanip>
#include <limits>
#include <sstream>

#include <GxEPD2_BW.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>

#include "bazzite_logo.h"
#include "gaben_sleep_screen.h"

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

// Pin definitions for the TRMNL 7.5" (OG) DIY Kit
// (Seeed Studio XIAO ePaper Display Board EE04 + Good Display GDEY075T7 panel)
// Pinout verified against usetrmnl/trmnl-firmware BOARD_XIAO_EPAPER_DISPLAY.
// SPI is wired to the ESP32-S3's hardware SPI peripheral; CS lives on GPIO44
// (strapping pin - safe after boot). All control lines are on RTC-capable
// GPIOs except BUSY (input only, doesn't need RTC).
#define EPD_SCK  7
#define EPD_MOSI 9
#define EPD_CS   44
#define EPD_DC   10
#define EPD_RST  38
#define EPD_BUSY 4

// Battery monitoring on the EE04 board:
//   BAT_ADC   -> GPIO1 (A0)  : voltage divider tap, fed through a load switch
//   ADC_EN    -> GPIO6 (A5)  : load-switch enable, active HIGH
// Both are required because the divider is powered through a switch to keep
// quiescent draw near zero when not measuring.
#define PIN_BAT_ADC  1
#define PIN_BAT_EN   6

// 800x480 panel layout.
#define SPARKBOX_HEIGHT     150
#define SPARKBOX_WIDTH      260
#define DISCRETEBOX_WIDTH   260
#define DISCRETEBOX_HEIGHT  36
#define GUTTER              5
#define MARGIN              5
// Title bar height inside each box; sparkbox graph area = (height - title_h - 32).
#define SPARKBOX_TITLE_H    32
#define HEADER_X            130
#define HEADER_Y            12
#define BOX_TOP_Y           120
#define BOX_PAD_X           8
#define FOOTER_Y_OFFSET     14

#define INTERFACE_VERSION "IFv01"
#define GIT_REVISION "INKTF 0.1.0"

NimBLEServer *BLE_SERVER = nullptr;
std::string BLE_NAME = "INKTF";

// Debug logging goes to the USB serial port. Aliased so we can swap it out
// (e.g. to a disabled no-op) in one place if we ever want a quiet build.
#define Debug Serial

// Any dashboard change repaints the full panel. This avoids stale regions
// and keeps the 7.5" layout predictable.
static bool DISPLAY_DIRTY = true;

bool INVERTED = false;
#define FG_COLOR (INVERTED ? GxEPD_WHITE : GxEPD_BLACK)
#define BG_COLOR (INVERTED ? GxEPD_BLACK : GxEPD_WHITE)

// Idle / sleep screen behavior, merged from upstream.
// After 5 minutes without a BLE connection the board switches to a low-power
// idle mode: it draws the Gaben sleep bitmap once, then hibernates the panel.
// Bluetooth advertising stays enabled so the device remains discoverable, but
// the advertising interval is widened to lower power usage. Any connection or
// host-side BLE write exits idle and forces a fresh dashboard redraw.
static constexpr unsigned long IDLE_TIMEOUT_MS = 5UL * 60UL * 1000UL;

// NimBLE advertising intervals are specified in 0.625 ms units.
//   active: 100-200 ms (responsive when the user just walked up to the case)
//   idle:   1000-2000 ms (still discoverable, but ~10x less radio time)
static constexpr uint16_t ADV_ACTIVE_MIN = 160;
static constexpr uint16_t ADV_ACTIVE_MAX = 320;
static constexpr uint16_t ADV_IDLE_MIN   = 1600;
static constexpr uint16_t ADV_IDLE_MAX   = 3200;
// track idle state and disconnect timing
static bool IDLE_MODE = false;
static unsigned long LAST_DISCONNECT_MS = 0;

// TRMNL 7.5" OG DIY Kit uses the Good Display GDEY075T7 (800x480,
// UC8179 / GD7965 controller). The XIAO ESP32-S3 Plus has 8MB OPI
// PSRAM which is what lets us keep a full-height 1bpp framebuffer
// (48000 bytes) in memory without paging.
GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT>
    MF_DISPLAY(GxEPD2_750_GDEY075T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

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

    // Battery / charging status read off the EE04's divider. The Valve
    // Inkterface host doesn't send battery data over BLE, so we sample
    // it ourselves and report it as part of the bottom-line string.
    int   batteryMv = -1;          // last raw ADC reading, mV
    bool  batteryCharging = false; // best-effort, USB plugged heuristic
    bool  batteryPresent = false;  // false when the divider is floating

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
        // midLine is now the host-controlled Steam display name slot.
        // The firmware renders the BLE device name directly from
        // BLE_NAME in drawStatic(), so we don't put BLE_NAME here
        // anymore — if we did, the very first BLE write from the
        // host would clobber it anyway.
        midLine = "";
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

void drawText(const char *text, const int16_t &x = -1, const int16_t &y = -1,
              const uint8_t &size = 1, const bool &wrap = false);
void exitIdleMode();

static int16_t textWidth(const std::string &text, uint8_t size)
{
    return (int16_t)(text.length() * 6 * size);
}

static std::string boundedString(const char *text, size_t maxLen)
{
    size_t len = 0;
    while (len < maxLen && text[len] != '\0') {
        len++;
    }
    return std::string(text, len);
}

static std::string truncateToWidth(const std::string &text, int16_t maxW, uint8_t size)
{
    if (maxW <= 0 || size == 0) {
        return "";
    }

    size_t maxChars = (size_t)(maxW / (6 * size));
    if (text.length() <= maxChars) {
        return text;
    }
    if (maxChars == 0) {
        return "";
    }
    if (maxChars == 1) {
        return text.substr(0, 1);
    }

    return text.substr(0, maxChars - 1) + "~";
}

static uint8_t fitTextSize(const std::string &text, int16_t maxW, uint8_t preferred,
                           uint8_t minimum)
{
    uint8_t size = preferred;
    while (size > minimum && textWidth(text, size) > maxW) {
        size--;
    }
    return size;
}

void drawTextFit(const std::string &text, int16_t x, int16_t y, int16_t maxW,
                 uint8_t preferredSize, uint8_t minimumSize = 1)
{
    uint8_t size = fitTextSize(text, maxW, preferredSize, minimumSize);
    drawText(truncateToWidth(text, maxW, size).c_str(), x, y, size);
}

void drawTextFitRight(const std::string &text, int16_t rightX, int16_t y, int16_t maxW,
                      uint8_t preferredSize, uint8_t minimumSize = 1)
{
    uint8_t size = fitTextSize(text, maxW, preferredSize, minimumSize);
    std::string fitted = truncateToWidth(text, maxW, size);
    drawText(fitted.c_str(), rightX - textWidth(fitted, size), y, size);
}

static void boxRectForIndex(uint8_t index, int16_t &x, int16_t &y, int16_t &w, int16_t &h)
{
    uint8_t col = index;
    y = BOX_TOP_Y;
    h = DISCRETEBOX_HEIGHT;

    if (index >= 3) {
        col = (uint8_t)((index - 3) % 3);
        y = (int16_t)(BOX_TOP_Y + DISCRETEBOX_HEIGHT + GUTTER +
                      (((index - 3) / 3) * (SPARKBOX_HEIGHT + GUTTER)));
        h = SPARKBOX_HEIGHT;
    }

    x = (int16_t)(MARGIN + col * (DISCRETEBOX_WIDTH + GUTTER));
    w = DISCRETEBOX_WIDTH;
}

static void markDashboardDirty()
{
    DISPLAY_DIRTY = true;
}

static void scheduleDashboardRefresh(unsigned long delayMs = 100)
{
    exitIdleMode();
    markDashboardDirty();
    DISP_DEBOUNCE = delayMs;
}

static void scheduleActiveRefresh(unsigned long delayMs = 100)
{
    if (!IDLE_MODE) {
        markDashboardDirty();
        DISP_DEBOUNCE = delayMs;
    }
}

// Read the battery divider through the EE04's load switch. Returns the
// measured cell voltage in millivolts, or -1 if the divider reads as
// floating (no battery connected, switch stuck off, etc.).
//
// The reference EE04 schematic puts a 2:1 divider (R1 == R2) between VBAT
// and the ADC pin. With the ESP32-S3's eFuse-calibrated ADC and a
// 3.3V VREF we read mv = raw * 3300 / 4096 * 2.
static int readBatteryMv()
{
    digitalWrite(PIN_BAT_EN, HIGH); // enable divider load switch
    delay(10);                       // let the switch + cap settle

    long sum = 0;
    int good = 0;
    for (int i = 0; i < 16; i++) {
        int raw = analogRead(PIN_BAT_ADC);
        // analogRead on S3 returns 0..4095 for 12-bit reads; discard
        // the obviously-bogus readings the divider occasionally produces
        // before the switch fully closes.
        if (raw > 50) {
            sum += raw;
            good++;
        }
        delayMicroseconds(200);
    }
    digitalWrite(PIN_BAT_EN, LOW); // disable divider to save quiescent draw

    if (good < 4) {
        return -1;
    }
    float avg = (float)sum / (float)good;
    float mv = (avg / 4095.0f) * 3300.0f * 2.0f;
    return (int)(mv + 0.5f);
}

void drawText(const char *text, const int16_t &x, const int16_t &y,
              const uint8_t &size, const bool &wrap)
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

static void drawBoxFrame(int16_t x, int16_t y, int16_t w, int16_t h)
{
    MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
    MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
}

static void drawBoxTitleValue(int16_t x, int16_t y, int16_t w, const std::string &title,
                              const std::string &value, uint8_t valueSize)
{
    drawTextFit(title, x + BOX_PAD_X, y + 6, 94, 3, 2);
    drawTextFitRight(value, x + w - BOX_PAD_X, y + 6, w - 118, valueSize, 1);
}

void drawSparkbox(int16_t &x, const int16_t &y, std::string &title, const std::string &value,
                  const Points &points)
{ // {{{
    const int16_t w = SPARKBOX_WIDTH;
    const int16_t h = SPARKBOX_HEIGHT;
    const int16_t title_h = SPARKBOX_TITLE_H;
    const int16_t graph_h = (h - title_h) - 32;
    const int16_t graph_w = w - 20;
    const int16_t graph_x = x + 10;
    const int16_t graph_y = (y + h) - 16;

    if (!title.empty()) {
        drawBoxFrame(x, y, w, h);
        MF_DISPLAY.fillRect(x, y + title_h, w, 1, FG_COLOR);
        drawBoxTitleValue(x, y, w, title, value, 3);

        std::stringstream maxstrm;
        maxstrm << std::fixed << std::setprecision(0) << points.yMax;
        auto maxstr = maxstrm.str();
        drawTextFit(maxstr, x + BOX_PAD_X, y + title_h + 6, 64, 2, 1);

        std::stringstream minstrm;
        minstrm << std::fixed << std::setprecision(0) << points.yMin;
        auto minstr = minstrm.str();
        drawTextFit(minstr, x + BOX_PAD_X, y + h - 13, 64, 2, 1);

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
    const int16_t w = DISCRETEBOX_WIDTH;
    const int16_t h = DISCRETEBOX_HEIGHT;

    if (!title.empty()) {
        drawBoxFrame(x, y, w, h);
        drawBoxTitleValue(x, y + 2, w, title, value, 2);
    }

    x += w;
} // }}}

static void drawHeader()
{
    const int16_t w = MF_DISPLAY.width() - HEADER_X - MARGIN;
    drawTextFit(STATE.topLine, HEADER_X, HEADER_Y, w, 3, 2);
    drawTextFit(BLE_NAME, HEADER_X, HEADER_Y + 30, w, 2, 1);
    drawTextFit(STATE.midLine, HEADER_X, HEADER_Y + 51, w, 2, 1);
    drawTextFit(STATE.botLine, HEADER_X, HEADER_Y + 72, w, 2, 1);
}

static void drawMetricGrid()
{
    int16_t x, y, w, h;
    for (uint8_t i = 0; i < 9; i++) {
        boxRectForIndex(i, x, y, w, h);
        if (i < 3) {
            drawDiscreteBox(x, y, STATE.keyvals[i].key, STATE.keyvals[i].val);
        } else {
            drawSparkbox(x, y, STATE.keyvals[i].key, STATE.keyvals[i].val, STATE.sparks[i - 3]);
        }
    }
}

static std::string footerRightText()
{
    if (!STATE.hostMsg.empty() || !STATE.batteryPresent) {
        return STATE.hostMsg;
    }

    std::stringstream bat;
    bat << "BAT " << std::fixed << std::setprecision(2)
        << ((float)STATE.batteryMv / 1000.0f) << "V";
    if (STATE.batteryCharging) {
        bat << " +";
    }
    return bat.str();
}

void drawStatic()
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    x = MARGIN;
    y = MARGIN;
    drawLogo(x, y);
    drawHeader();
    drawMetricGrid();

    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    y = MF_DISPLAY.height() - FOOTER_Y_OFFSET;
    drawTextFit(tag.str(), MARGIN, y, 400, 1, 1);
    drawTextFitRight(footerRightText(), MF_DISPLAY.width() - MARGIN, y, 360, 1, 1);
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

    const int16_t bmpW = GABEN_SLEEP_SCREEN_WIDTH;
    const int16_t bmpH = GABEN_SLEEP_SCREEN_HEIGHT;
    const int16_t x = (MF_DISPLAY.width() - bmpW) / 2;
    const int16_t y = 0;

    MF_DISPLAY.drawBitmap(x, y, gaben_sleep_screen_bitmap, bmpW, bmpH, FG_COLOR);
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
    markDashboardDirty();
    setAdvertisingProfile(false);
    DISP_DEBOUNCE = 10; // redraw normal dashboard soon
} // }}}

class ServerCallbacks : public NimBLEServerCallbacks
{ // {{{
    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn) override
    {
        Debug.println("got connection");
        NimBLEDevice::stopAdvertising();
        STATE.connected = true;

        // any connection exits idle immediately and restores the normal dashboard.
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
            STATE.reset();
            // Connection state changed; the connecting-screen is a
            // full-screen image, so mark the whole screen dirty.
            markDashboardDirty();

            // start the idle timeout when the device becomes disconnected.
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
        bool changed = false;
        if (uuid == TOPLINE_UUID && STATE.topLine != value) {
            STATE.topLine = value;
            changed = true;
        } else if (uuid == MIDLINE_UUID && STATE.midLine != value) {
            STATE.midLine = value;
            changed = true;
        } else if (uuid == BOTLINE_UUID && STATE.botLine != value) {
            STATE.botLine = value;
            changed = true;
        } else if (uuid != TOPLINE_UUID && uuid != MIDLINE_UUID && uuid != BOTLINE_UUID) {
            Debug.print("Got value (");
            Debug.print(value.c_str());
            Debug.print(") for unknown UUID (");
            Debug.print(uuid.toString().c_str());
            Debug.println("), ignoring.");
            return;
        }
        if (changed) {
            scheduleDashboardRefresh();
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
            if (msg.index >= STATE.keyvals.size()) {
                Debug.print("got bad keyval index: ");
                Debug.println(msg.index);
                return;
            }
            STATE.keyvals[msg.index].key = boundedString(msg.key, sizeof(msg.key));
            STATE.keyvals[msg.index].val = boundedString(msg.val, sizeof(msg.val));
            scheduleDashboardRefresh();
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
        Msg msg{};
        const size_t headerSize = sizeof(Msg) - sizeof(msg.values);
        if (value.length() >= headerSize) {
            memcpy(&msg, value.data(), headerSize);
            if (msg.index >= STATE.sparks.size() || (msg.count % 2) != 0 ||
                msg.count > sizeof(msg.values) || value.length() < headerSize + msg.count) {
                Debug.print("got bad vector metadata, index/count: ");
                Debug.print(msg.index);
                Debug.print("/");
                Debug.println(msg.count);
                return;
            }
            memcpy(msg.values, value.data() + headerSize, msg.count);
            Debug.print("got vector for index (");
            Debug.print(msg.index);
            Debug.print(") with ");
            Debug.print(msg.count);
            Debug.print(" values, min ");
            Debug.print(msg.minVal);
            Debug.print(", max ");
            Debug.println(msg.maxVal);
            STATE.sparks[msg.index].clear();
            STATE.sparks[msg.index].yMin = msg.minVal;
            STATE.sparks[msg.index].yMax = msg.maxVal;
            for (int i = 0; i < msg.count; i += 2) {
                STATE.sparks[msg.index].points.emplace_back(msg.values[i] / 255.0,
                                                            msg.values[i + 1] / 255.0);
            }
            scheduleDashboardRefresh();
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
        STATE.hostMsg = characteristic->getValue();
        // any incoming data implies active use, so leave idle mode and
        // redraw the dashboard instead of staying on the sleep screen.
        scheduleDashboardRefresh();
    }
} FLUSH_CALLBACKS; // }}}

void setup()
{ // {{{
    Serial.begin(115200);

#if defined(STARTUP_DELAY_MS)
    delay(STARTUP_DELAY_MS);
#endif

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

    // Battery monitor on the EE04: configure ADC pin + load-switch enable,
    // but leave the switch off so it doesn't leak current until we read.
    pinMode(PIN_BAT_EN, OUTPUT);
    digitalWrite(PIN_BAT_EN, LOW);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);

    Debug.println("initializing display");
    STATE.reset();
    // init(serial_diag_bitrate, initial, reset_duration_ms, pulldown_rst_mode)
    MF_DISPLAY.init(115200, true, 2, false);
    MF_DISPLAY.setFullWindow();
    MF_DISPLAY.fillScreen(BG_COLOR);
    drawStatic();
    MF_DISPLAY.display();
    MF_DISPLAY.hibernate();
    DISPLAY_DIRTY = false;
    DISP_DEBOUNCE = 0;

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

    // start in normal/active advertising profile.
    setAdvertisingProfile(false);

    // device starts disconnected, so begin idle timeout from boot.
    LAST_DISCONNECT_MS = millis();

} // }}}

void loop()
{ // {{{
    static unsigned long LAST_MS = 0;
    static unsigned long CONN_DEBOUNCE = 5000;
    // Sample the battery occasionally (every ~5s). Doing it on every loop
    // would wake the ADC + divider too often and defeat the load switch.
    static unsigned long BAT_POLL = 0;
    static unsigned long BAT_INTERVAL_MS = 5000;

    auto now = millis();
    auto delta = now - LAST_MS;
    if (now < LAST_MS) {
        Debug.println("handling time rollover");
        delta = (std::numeric_limits<unsigned long>::max() - LAST_MS) + now;
    }

    if (BAT_POLL > 0 && BAT_POLL > delta) {
        BAT_POLL -= delta;
    } else if (BAT_POLL == 0) {
        int mv = readBatteryMv();
        bool was_present = STATE.batteryPresent;
        int  was_mv     = STATE.batteryMv;
        if (mv < 0) {
            STATE.batteryPresent = false;
            STATE.batteryMv = -1;
        } else {
            // Only flip "present" once we're confident; first reading after
            // boot is sometimes flaky on a freshly-enabled divider.
            STATE.batteryPresent = true;
            STATE.batteryMv = mv;
        }
        if (was_present != STATE.batteryPresent ||
            (STATE.batteryPresent && abs(was_mv - STATE.batteryMv) >= 50)) {
            // >=50 mV change is worth redrawing for. Smaller ripples are
            // just ADC noise.
            scheduleActiveRefresh();
        }
        BAT_POLL = BAT_INTERVAL_MS;
    } else {
        // BAT_POLL <= delta: deadline passed this iteration; do a sample
        // on the next tick (the == 0 branch) by zeroing it now.
        BAT_POLL = 0;
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
        Debug.println("drawing full dashboard to display");
        DISP_DEBOUNCE = 0;

        // while idle, the sleep screen is the source of truth; skip the
        // dashboard redraw entirely (and clear the dirty flag so any
        // battery-only change doesn't keep accumulating). exitIdleMode()
        // sets DISP_DEBOUNCE = 10, which forces the next iteration to
        // repaint the dashboard.
        if (!IDLE_MODE && DISPLAY_DIRTY) {
            MF_DISPLAY.init(115200, false, 2, false);
            MF_DISPLAY.setFullWindow();
            MF_DISPLAY.fillScreen(BG_COLOR);
            drawStatic();
            MF_DISPLAY.display();
            MF_DISPLAY.hibernate();
        }
        DISPLAY_DIRTY = false;
        Debug.println("drew to display");
    }

    LAST_MS = now;
    delay(10);
} //
