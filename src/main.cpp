// vim: foldmethod=marker:foldmarker={{{,}}}
#include <iomanip>
#include <limits>
#include <sstream>

#include <GxEPD2_BW.h>
#include <NimBLEDevice.h>
#include <esp_sleep.h>

#include "bazzite_logo.h"

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

// Sparkbox width is tuned so three columns + three 5 px gutters + 5 px left
// and right margins tile the 800 px wide GDEY075T7 panel exactly:
// 5 + 260 + 5 + 260 + 5 + 260 + 5 = 800.
#define SPARKBOX_HEIGHT 150
#define SPARKBOX_WIDTH  260
#define DISCRETEBOX_WIDTH 260

#define INTERFACE_VERSION "IFv01"
#define GIT_REVISION "INKTF 0.1.0"

NimBLEServer *BLE_SERVER = nullptr;
std::string BLE_NAME = "INKTF";

bool INVERTED = false;
#define FG_COLOR (INVERTED ? GxEPD_WHITE : GxEPD_BLACK)
#define BG_COLOR (INVERTED ? GxEPD_BLACK : GxEPD_WHITE)

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
    const int16_t w = DISCRETEBOX_WIDTH;
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

void drawStatic()
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    // logo in top left corner
    x = 5;
    y = 5;
    drawLogo(x, y);

    // show connected fremont hostname/serial or connecting status
    x = 120;
    y = 15;
    drawText(STATE.topLine.c_str(), x, y, 3);
    y += 35;
    drawText(STATE.midLine.c_str(), x, y, 2);
    y += 30;
    drawText(STATE.botLine.c_str(), x, y, 2);

    // first row of boxes with no sparklines
    x = 5;
    y = 115;
    drawDiscreteBox(x, y, STATE.keyvals[0].key, STATE.keyvals[0].val);
    x += 5;
    drawDiscreteBox(x, y, STATE.keyvals[1].key, STATE.keyvals[1].val);
    x += 5;
    drawDiscreteBox(x, y, STATE.keyvals[2].key, STATE.keyvals[2].val);

    // second row
    x = 5;
    y += 26 + 5;
    drawSparkbox(x, y, STATE.keyvals[3].key, STATE.keyvals[3].val, STATE.sparks[0]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[4].key, STATE.keyvals[4].val, STATE.sparks[1]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[5].key, STATE.keyvals[5].val, STATE.sparks[2]);

    // third row
    x = 5;
    y += SPARKBOX_HEIGHT + 5;
    drawSparkbox(x, y, STATE.keyvals[6].key, STATE.keyvals[6].val, STATE.sparks[3]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[7].key, STATE.keyvals[7].val, STATE.sparks[4]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[8].key, STATE.keyvals[8].val, STATE.sparks[5]);

    // version tag (left) and host message / battery (right)
    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    x = 5;
    y = MF_DISPLAY.height() - 12;
    drawText(tag.str().c_str(), x, y);

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
    x = MF_DISPLAY.width() - (6 * right.length()) - 5;
    drawText(right.c_str(), x, y);
} // }}}

class ServerCallbacks : public NimBLEServerCallbacks
{ // {{{
    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn) override
    {
        Debug.println("got connection");
        NimBLEDevice::stopAdvertising();
        STATE.connected = true;
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
            STATE.topLine = value;
        } else if (uuid == MIDLINE_UUID && STATE.midLine != value) {
            STATE.midLine = value;
        } else if (uuid == BOTLINE_UUID && STATE.botLine != value) {
            STATE.botLine = value;
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
            STATE.keyvals[msg.index].key = msg.key;
            STATE.keyvals[msg.index].val = msg.val;
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
    // init(serial_diag_bitrate, initial, reset_duration_ms, pulldown_rst_mode)
    MF_DISPLAY.init(115200, true, 2, false);
    MF_DISPLAY.setFullWindow();
    MF_DISPLAY.fillScreen(BG_COLOR);
    drawStatic();
    MF_DISPLAY.display();
    MF_DISPLAY.hibernate();
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
    NimBLEDevice::startAdvertising();

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
            DISP_DEBOUNCE = 100;
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

    if (DISP_DEBOUNCE > 0 && DISP_DEBOUNCE > delta) {
        DISP_DEBOUNCE -= delta;
    } else if (DISP_DEBOUNCE > 0) {
        Debug.println("drawing to display");
        DISP_DEBOUNCE = 0;
        // init() must be called again after hibernate() to wake the panel
        MF_DISPLAY.init(115200, false, 2, false);
        MF_DISPLAY.setFullWindow();
        MF_DISPLAY.fillScreen(BG_COLOR);
        drawStatic();
        MF_DISPLAY.display();
        MF_DISPLAY.hibernate();
        Debug.println("drew to display");
    }

    LAST_MS = now;
    delay(10);
} //
