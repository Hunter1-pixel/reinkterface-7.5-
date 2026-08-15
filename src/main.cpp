// vim: foldmethod=marker:foldmarker={{{,}}}
#include <iomanip>
#include <limits>
#include <sstream>

#include <GxEPD2_BW.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>

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

// Layout sizing for the 7.5" 800x480 GDEY075T7 panel.
// The 5.83" build used 209x100 sparkboxes with size-2 (12 px) text. On the
// wider, larger 7.5" panel that's undersized for desk-distance viewing, so
// we scaled up proportionally:
//   - sparkbox width  209 -> 260 px  (matches 800 / 3 columns + gutters)
//   - sparkbox height 100 -> 150 px  (1.5x, gives graphs room under title)
//   - discrete box    26  -> 36 px   (taller to host size-3 text)
//   - title font      2   -> 3       (12 px -> 18 px)
//   - top-line font   3   -> 4       (18 px -> 24 px)
//   - gutter          5   -> 5 px    (kept; panel is wider so absolute
//                                   gutter already feels roomier)
//
// Vertical budget for box rows: 480 - 120 (header) - 14 (bottom margin) = 346 px
//   discrete(36) + gutter(5) + spark(150) + gutter(5) + spark(150) = 346 px ✓
//
// Width math (must tile edge-to-edge):
//   MARGIN(5) + 260 + GUTTER(5) + 260 + GUTTER(5) + 260 + MARGIN(5) = 800 ✓
#define SPARKBOX_HEIGHT     150
#define SPARKBOX_WIDTH      260
#define DISCRETEBOX_WIDTH   260
#define DISCRETEBOX_HEIGHT  36
#define GUTTER              5
#define MARGIN              5
// Title bar height inside each box; sparkbox graph area = (height - title_h - 32).
#define SPARKBOX_TITLE_H    32

#define INTERFACE_VERSION "IFv01"
#define GIT_REVISION "INKTF 0.1.0"

NimBLEServer *BLE_SERVER = nullptr;
std::string BLE_NAME = "INKTF";

// Debug logging goes to the USB serial port. Aliased so we can swap it out
// (e.g. to a disabled no-op) in one place if we ever want a quiet build.
#define Debug Serial

// Dirty-region tracker.
//
// The host BLE frame (status lines / keyvals / vectors / flush) redraws
// the whole panel, which is the natural way to handle a structural
// change. But the battery readout updates on its own 5 s timer and the
// only thing that actually changed is a ~120 x 14 px patch in the
// bottom-right corner. Forcing a full refresh every 5 s for that is
// wasteful (~1.2 s flicker every poll) and shortens panel life.
//
// Instead we keep a bounding box of "regions that have changed since the
// last refresh". If the host didn't push anything we may have only the
// battery patch dirty — in that case we use displayWindow() to do a
// ~450 ms fast partial refresh of just that rect. If the host pushed a
// new frame we mark the whole screen dirty and do a fast full refresh.
struct DirtyRegion {
    bool     full    = true; // true => redraw entire screen
    // Partial-rect bounds (x0,y0)..(x1,y1) BOTH inclusive. A rect is
    // "empty" when x0 > x1 or y0 > y1.
    int16_t  x0 = 0, y0 = 0;
    int16_t  x1 = -1, y1 = -1;

    void markFull() {
        full = true;
    }

    void markPartial(int16_t rx0, int16_t ry0, int16_t rx1, int16_t ry1) {
        if (full) {
            // already going to redraw the whole screen — don't bother
            // expanding a partial rect.
            return;
        }
        if (empty()) {
            x0 = rx0; y0 = ry0;
            x1 = rx1; y1 = ry1;
        } else {
            if (rx0 < x0) x0 = rx0;
            if (ry0 < y0) y0 = ry0;
            if (rx1 > x1) x1 = rx1;
            if (ry1 > y1) y1 = ry1;
        }
    }

    bool empty() const { return x0 > x1 || y0 > y1; }

    void clear() {
        full = false;
        x0 = 0; y0 = 0;
        x1 = -1; y1 = -1;
    }
} DIRTY;

bool INVERTED = false;
#define FG_COLOR (INVERTED ? GxEPD_WHITE : GxEPD_BLACK)
#define BG_COLOR (INVERTED ? GxEPD_BLACK : GxEPD_WHITE)

// Idle / sleep screen behavior, merged from upstream.
// After 5 minutes without a BLE connection the board switches to a low-power
// idle mode: it draws TRMNL's sleep bitmap once, then hibernates the panel.
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

void drawSparkbox(int16_t &x, const int16_t &y, std::string &title, const std::string &value,
                  const Points &points)
{ // {{{
    const int16_t w = SPARKBOX_WIDTH;
    const int16_t h = SPARKBOX_HEIGHT;
    const int16_t hpad = 8;
    const int16_t vpad = 6;
    const int16_t title_h = SPARKBOX_TITLE_H;
    const int16_t graph_h = (h - title_h) - 32;
    const int16_t graph_w = w - 20;
    const int16_t graph_x = x + 10;
    const int16_t graph_y = (y + h) - 16;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        MF_DISPLAY.fillRect(x, y + title_h, w, 1, FG_COLOR);
        // Title text bumped to size 3 (18 px) so it reads from desk distance.
        drawText(title.c_str(), x + hpad, y + vpad, 3);
        // Value uses size 3; width-per-char for size 3 is ~18 px (vs 12 for size 2).
        drawText(value.c_str(),
                 (x + (w - hpad)) - (18 * strlen(value.c_str())), y + vpad, 3);

        std::stringstream maxstrm;
        maxstrm << std::fixed << std::setprecision(0) << points.yMax;
        auto maxstr = maxstrm.str();
        drawText(maxstr.c_str(), x + hpad, y + title_h + vpad, 2);

        std::stringstream minstrm;
        minstrm << std::fixed << std::setprecision(0) << points.yMin;
        auto minstr = minstrm.str();
        drawText(minstr.c_str(), x + hpad, y + h - (vpad + 7), 2);

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
    const int16_t hpad = 8;
    const int16_t vpad = 8;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        // Size 3 text needs more vertical padding to stay optically centered
        // in the 36 px box height.
        drawText(title.c_str(), x + hpad, y + vpad, 3);
        drawText(value.c_str(),
                 (x + (w - hpad)) - (18 * strlen(value.c_str())), y + vpad, 3);
    }

    x += w;
} // }}}

void drawStatic()
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    // logo in top left corner
    x = MARGIN;
    y = MARGIN;
    drawLogo(x, y);

    // show connected fremont hostname/serial or connecting status
    // Sized for the bigger 7.5" canvas:
    //   - topline: size 4 (24 px tall) — the headline
    //   - midline: size 3 (18 px tall) — secondary info
    //   - botline: size 3 (18 px tall) — tertiary info
    x = 130;
    y = 18;
    drawText(STATE.topLine.c_str(), x, y, 4);
    y += 32;
    drawText(STATE.midLine.c_str(), x, y, 3);
    y += 28;
    drawText(STATE.botLine.c_str(), x, y, 3);

    // first row of boxes with no sparklines
    x = MARGIN;
    y = 120;
    drawDiscreteBox(x, y, STATE.keyvals[0].key, STATE.keyvals[0].val);
    x += GUTTER;
    drawDiscreteBox(x, y, STATE.keyvals[1].key, STATE.keyvals[1].val);
    x += GUTTER;
    drawDiscreteBox(x, y, STATE.keyvals[2].key, STATE.keyvals[2].val);

    // second row
    x = MARGIN;
    y += DISCRETEBOX_HEIGHT + GUTTER;
    drawSparkbox(x, y, STATE.keyvals[3].key, STATE.keyvals[3].val, STATE.sparks[0]);
    x += GUTTER;
    drawSparkbox(x, y, STATE.keyvals[4].key, STATE.keyvals[4].val, STATE.sparks[1]);
    x += GUTTER;
    drawSparkbox(x, y, STATE.keyvals[5].key, STATE.keyvals[5].val, STATE.sparks[2]);

    // third row
    x = MARGIN;
    y += SPARKBOX_HEIGHT + GUTTER;
    drawSparkbox(x, y, STATE.keyvals[6].key, STATE.keyvals[6].val, STATE.sparks[3]);
    x += GUTTER;
    drawSparkbox(x, y, STATE.keyvals[7].key, STATE.keyvals[7].val, STATE.sparks[4]);
    x += GUTTER;
    drawSparkbox(x, y, STATE.keyvals[8].key, STATE.keyvals[8].val, STATE.sparks[5]);

    // version tag (left) and host message / battery (right)
    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    x = MARGIN;
    y = MF_DISPLAY.height() - 14;
    drawText(tag.str().c_str(), x, y, 1);

    // right side: host message (timestamp) when present, otherwise our own
    // battery reading. We only show battery when there's nothing from the
    // host so we don't double-print text.
    std::string right = STATE.hostMsg;
    if (right.empty() && STATE.batteryPresent) {
        std::stringstream bat;
        bat << "BAT " << std::fixed << std::setprecision(2)
            << ((float)STATE.batteryMv / 1000.0f) << "V";
        if (STATE.batteryCharging) {
            bat << " +";
        }
        right = bat.str();
    }
    // size 1 = 6 px wide per char.
    x = MF_DISPLAY.width() - (6 * right.length()) - MARGIN;
    drawText(right.c_str(), x, y, 1);
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
            DIRTY.markFull();

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
            // Status lines span most of the right half of the header.
            // Conservatively mark a generous rect: x from the logo's
            // right edge through the right margin, y covering all three
            // lines of header text.
            DIRTY.markPartial(130, 12, MF_DISPLAY.width() - MARGIN, 110);
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
            STATE.keyvals[msg.index].key = msg.key;
            STATE.keyvals[msg.index].val = msg.val;
            // A single keyval changed — could be a single box. If we
            // knew the prior index/value we could compute a tight rect;
            // for simplicity we conservatively mark the whole screen
            // dirty so the partial-refresh fast path doesn't risk
            // ghosting from overlapping rectangles after several updates.
            DIRTY.markFull();
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
            // New vector data is a structural change to the sparkline
            // graph; mark the whole screen dirty to avoid ghosting where
            // the old graph lines cross the new ones.
            DIRTY.markFull();
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
        // The host message goes into the bottom-right corner (same slot
        // as the battery readout). We only know the new value's *length*
        // here, not its pixel width, so conservatively mark a generous
        // strip from where the hostMsg text starts through the right
        // margin, plus enough height for size-1 text + padding.
        int16_t msgPxW = (int16_t)(6 * STATE.hostMsg.length());
        int16_t rightX = (int16_t)MF_DISPLAY.width() - msgPxW - MARGIN;
        if (rightX < 130) rightX = 130; // don't extend into the BLE_NAME tag
        DIRTY.markPartial(rightX - 8, MF_DISPLAY.height() - 22,
                          MF_DISPLAY.width() - MARGIN,
                          MF_DISPLAY.height() - 2);

        // any incoming data implies active use, so leave idle mode and
        // redraw the dashboard instead of staying on the sleep screen.
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
    DIRTY.clear();        // first paint is always full
    DIRTY.markFull();
    // init(serial_diag_bitrate, initial, reset_duration_ms, pulldown_rst_mode)
    MF_DISPLAY.init(115200, true, 2, false);
    MF_DISPLAY.setFullWindow();
    MF_DISPLAY.fillScreen(BG_COLOR);
    drawStatic();
    MF_DISPLAY.display();
    MF_DISPLAY.hibernate();
    DIRTY.clear();
    DISP_DEBOUNCE = 10;

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
            // just ADC noise. The battery text sits in the bottom-right
            // corner; mark a tight rect there for fast partial refresh.
            int16_t msgPxW = (int16_t)(6 * 18); // "BAT 4.XXV +" worst case
            int16_t rightX = (int16_t)MF_DISPLAY.width() - msgPxW - MARGIN;
            if (rightX < 130) rightX = 130;
            DIRTY.markPartial(rightX - 8, MF_DISPLAY.height() - 22,
                              MF_DISPLAY.width() - MARGIN,
                              MF_DISPLAY.height() - 2);
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
        Debug.print("drawing to display, mode=");
        Debug.println(DIRTY.full ? "FULL" : "PARTIAL");
        DISP_DEBOUNCE = 0;

        // while idle, the sleep screen is the source of truth; skip the
        // dashboard redraw entirely (and clear the dirty state so any
        // battery-only change doesn't keep accumulating). exitIdleMode()
        // sets DISP_DEBOUNCE = 10, which forces the next iteration to
        // repaint the dashboard.
        if (!IDLE_MODE) {
            // init() must be called again after hibernate() to wake the panel
            MF_DISPLAY.init(115200, false, 2, false);

            if (DIRTY.full) {
                // Full refresh path. Repaint everything in the buffer, push
                // it to the panel, then clear the dirty state.
                MF_DISPLAY.setFullWindow();
                MF_DISPLAY.fillScreen(BG_COLOR);
                drawStatic();
                MF_DISPLAY.display();
            } else {
                // Partial refresh path. The buffer is repainted in full so
                // its contents under the dirty rect are correct, but only
                // the dirty slice is pushed to the panel for the ~450 ms
                // fast partial refresh instead of the ~1.2 s fast full one.
                int16_t w = (int16_t)(DIRTY.x1 - DIRTY.x0 + 1);
                int16_t h = (int16_t)(DIRTY.y1 - DIRTY.y0 + 1);
                int16_t xRounded = DIRTY.x0 & ~0x07;                   // round down to 8
                int16_t wRounded = (int16_t)(((w + (DIRTY.x0 - xRounded) + 7) & ~0x07)); // round up
                // Repaint the whole buffer (GxEPD2 needs the in-RAM pixels
                // under the dirty rect to be correct, since it pushes that
                // slice directly out of the buffer).
                MF_DISPLAY.setFullWindow();
                MF_DISPLAY.fillScreen(BG_COLOR);
                drawStatic();
                MF_DISPLAY.setPartialWindow(xRounded, DIRTY.y0, wRounded, h);
                MF_DISPLAY.displayWindow(xRounded, DIRTY.y0, wRounded, h);
            }
            MF_DISPLAY.hibernate();
        }
        DIRTY.clear();
        Debug.println("drew to display");
    }

    LAST_MS = now;
    delay(10);
} //