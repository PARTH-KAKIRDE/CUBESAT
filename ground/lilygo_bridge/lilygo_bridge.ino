/*
 * LilyGO LoRa32 (433 MHz, SX1278) <-> USB serial bridge for ground/gs_tool.py.
 *
 * Flashing this replaces TinyGS on the board (you can re-flash TinyGS later
 * from https://installer.tinygs.com). Arduino IDE: board "ESP32 Dev Module"
 * (or "TTGO LoRa32-OLED"), library "RadioLib" (tested API: 6.x / 7.x).
 *
 * Serial 115200, one line per message:
 *   PC -> board   TX <hex>                         transmit a packet
 *                 CFG <freq_hz> <sf> <bw_khz> <cr> <power_dbm> <sync_hex>
 *   board -> PC   RX <rssi_dbm> <snr_db> <hex>     packet received
 *                 TXDONE | OK ... | ERR ...
 *
 * The radio settings MUST match the satellite (TTC_*_DEFAULT in icd/cubesat_icd.h):
 * 436.5 MHz, SF9, 125 kHz, CR 4/5, sync 0x12, preamble 8, CRC on.
 * 436.5 MHz is a placeholder: use your coordinated amateur frequency.
 */
#include <RadioLib.h>

// LilyGO T3 v1.6.1 pin map. Check your board revision.
#define LORA_SCK 5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_NSS 18
#define LORA_RST 23
#define LORA_DIO0 26
#define LORA_DIO1 33

SX1278 radio = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1);

static float g_freq = 436.5, g_bw = 125.0;
static uint8_t g_sf = 9, g_cr = 5, g_sync = 0x12;
static int8_t g_power = 17;
static volatile bool g_rx_flag = false;

#if defined(ESP32)
ICACHE_RAM_ATTR
#endif
void on_rx(void) { g_rx_flag = true; }

static bool apply_config(void)
{
    int st = radio.begin(g_freq, g_bw, g_sf, g_cr, g_sync, g_power, 8, 0);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("ERR begin %d\n", st);
        return false;
    }
    radio.setCRC(true);
    radio.setPacketReceivedAction(on_rx);
    radio.startReceive();
    Serial.printf("OK freq %.3f sf %u bw %.1f cr %u pwr %d sync 0x%02X\n", g_freq, g_sf, g_bw, g_cr, g_power, g_sync);
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void handle_line(String line)
{
    line.trim();
    if (line.startsWith("TX ")) {
        String h = line.substring(3);
        uint8_t buf[255];
        size_t n = 0;
        for (size_t i = 0; i + 1 < h.length() && n < sizeof buf; i += 2) {
            int a = hexval(h[i]), b = hexval(h[i + 1]);
            if (a < 0 || b < 0) { Serial.println("ERR hex"); return; }
            buf[n++] = (uint8_t)(a * 16 + b);
        }
        int st = radio.transmit(buf, n);
        Serial.println(st == RADIOLIB_ERR_NONE ? "TXDONE" : "ERR tx");
        radio.startReceive();
    } else if (line.startsWith("CFG ")) {
        unsigned long f;
        unsigned sf, cr, sync;
        float bw;
        int p;
        if (sscanf(line.c_str() + 4, "%lu %u %f %u %d %x", &f, &sf, &bw, &cr, &p, &sync) == 6) {
            g_freq = f / 1e6f; g_sf = sf; g_bw = bw; g_cr = cr; g_power = p; g_sync = sync;
            apply_config();
        } else {
            Serial.println("ERR cfg");
        }
    } else if (line.length()) {
        Serial.println("ERR unknown");
    }
}

void setup()
{
    Serial.begin(115200);
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
    apply_config();
}

void loop()
{
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') { handle_line(line); line = ""; }
        else if (line.length() < 600) line += c;
    }
    if (g_rx_flag) {
        g_rx_flag = false;
        uint8_t buf[255];
        size_t n = radio.getPacketLength();
        int st = radio.readData(buf, n);
        if (st == RADIOLIB_ERR_NONE) {
            Serial.printf("RX %d %.2f ", (int)radio.getRSSI(), radio.getSNR());
            for (size_t i = 0; i < n; i++) Serial.printf("%02x", buf[i]);
            Serial.println();
        } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
            Serial.println("ERR crc");
        }
        radio.startReceive();
    }
}
