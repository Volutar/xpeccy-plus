// The emulated ESP-01 with Espressif's NONOS AT 1.7.6 (SDK 3.0.6) on the Next's UART 0: this
// part is the module itself - its line at its own rate, the receiver and the dispatcher as
// the firmware has them, boot, flash and the two pins the Next sees. The commands are in
// esp_basic.cpp, esp_wifi.cpp and esp_tcp.cpp. Replies go onto the line whole, back to back;
// a guest at another rate reads garbage, as on the board. Its flash is nvram/esp01.conf.
// Time is the guest's: CLK_28 edges.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "esp.h"
#include "esp_int.h"
#include "espline.h"
#include "espnet.h"
#include "../libxpeccy/spectrum.h"
#include "../libxpeccy/hardware/hardware.h"
#include "../libxpeccy/xlog.h"

#define ESP_UART	0
#define ESP_FRAME	0x18		// 8N1, as uart_framing_r
#define ESP_LINE_MAX	255		// bytes after AT the receiver keeps (the 256th ends the line)
#define ESP_NAME_MAX	128

enum {RX_IDLE = 0, RX_LINE, RX_BUSY};

typedef struct {
	std::string b;
	double baud;
	int frame;
	long long at;			// not on the line before this edge (0: whenever)
} espMsg;

static struct {
	int st = ESP_OFF;
	bool loaded = false;
	std::map<std::string, std::string> fl;
	// the UART as it runs now
	long baud = 115200;
	int frame = ESP_FRAME;
	bool echo = true;
	// boot
	int bootStage = 0;
	long long bootAt = 0;
	// the receiver
	xEspLine line;
	int rx = RX_IDLE;
	int prev = -1;			// the byte before, for the AT head
	std::string buf;		// the line after AT
	size_t rstAt = 0;		// how much of AT+RST\r\n the line has matched (while busy)
	bool held = false;
	void (*hook)(Computer*, int, long long) = nullptr;
	// out
	std::deque<espMsg> out;
	size_t outBytes = 0;
	double farBaud = 0;
	int farFrame = -1;
	long long replyAt = 0;		// the reply to the line just ended starts here
	bool rst0 = false;		// NR 02 b7 as last seen
	// the pins: GPIO0, GPIO2
	bool ioOut[2] = {false, false};
	int ioLevel[2] = {1, 1};
} E;

static std::string flashFile;		// "" keeps nothing
static bool lanServer = false;		// a server listens on every interface, not only 127.0.0.1

void esp_set_lan(bool on) {lanServer = on;}
bool esp_lan() {return lanServer;}

// == time and rates ==

static double esp_clk(Computer* comp) {
	return nx_clk28_hz(comp);
}

long long esp_ms(Computer* comp, double ms) {
	return (long long)(esp_clk(comp) * ms / 1000.0);
}

// not nx_uart_sync(): that may hand the next byte in while one is being taken
long long esp_now(Computer* comp) {
	return nx_now(comp);
}

long long esp_host_ms() {
	return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// the ESP8266's UART runs from 80 MHz through a whole divider
double esp_actual(long baud) {
	if (baud < 1) baud = 1;
	long div = 80000000L / baud;
	if (div < 1) div = 1;
	return 80000000.0 / div;
}

long esp_baud() {return E.baud;}
int esp_frame_bits() {return E.frame;}

// what is queued keeps the rate it was put at
void esp_set_uart(long baud, int frame) {
	E.baud = baud;
	E.frame = frame & 0x1f;
	xlog(XLG_NET, XLL_INFO, "esp: rate %li (%.0f)", baud, esp_actual(baud));
}

// == flash ==

void esp_flash_file(const char* path) {
	flashFile = path ? path : "";
}

static void fl_load() {
	E.loaded = true;
	E.fl.clear();
	if (flashFile.empty()) return;
	FILE* f = fopen(flashFile.c_str(), "rb");
	if (!f) return;
	char buf[1024];
	while (fgets(buf, sizeof(buf), f)) {
		std::string s(buf);
		while (!s.empty() && ((s.back() == '\n') || (s.back() == '\r'))) s.pop_back();
		size_t eq = s.find(" = ");
		if (eq == std::string::npos) continue;
		E.fl[s.substr(0, eq)] = s.substr(eq + 3);
	}
	fclose(f);
}

static void fl_save() {
	if (flashFile.empty()) return;
	FILE* f = fopen(flashFile.c_str(), "wb");
	if (!f) return;
	for (auto& kv : E.fl) fprintf(f, "%s = %s\n", kv.first.c_str(), kv.second.c_str());
	fclose(f);
}

std::string esp_fl_get(const char* key, const std::string& def) {
	auto it = E.fl.find(key);
	return (it == E.fl.end()) ? def : it->second;
}

long esp_fl_num(const char* key, long def) {
	auto it = E.fl.find(key);
	if (it == E.fl.end() || it->second.empty()) return def;
	char* e = nullptr;
	long v = strtol(it->second.c_str(), &e, 10);
	return (e && !*e) ? v : def;
}

void esp_fl_set(const char* key, const std::string& val) {
	auto it = E.fl.find(key);
	if ((it != E.fl.end()) && (it->second == val)) return;	// written only when it changes
	E.fl[key] = val;
	fl_save();
}

void esp_fl_set_num(const char* key, long val) {
	esp_fl_set(key, std::to_string(val));
}

void esp_fl_clear() {
	E.fl.clear();
	E.fl["ssid"] = "";		// factory: no AP stored, which an absent key is not
	fl_save();
}

// == the line out ==

void esp_trace(const char* dir, const std::string& s) {
	if (!xlog_on(XLG_NET, XLL_DEBUG)) return;
	std::string t;
	for (size_t i = 0; (i < s.size()) && (t.size() < 160); i++) {
		unsigned char c = s[i];
		if (c == '\r') t += "\\r";
		else if (c == '\n') t += "\\n";
		else if ((c < 0x20) || (c > 0x7e)) {
			char h[8];
			snprintf(h, sizeof(h), "\\x%02X", c);
			t += h;
		} else t += (char)c;
	}
	if (t.size() >= 160) t += "...";
	xlog(XLG_NET, XLL_DEBUG, "esp %s %s", dir, t.c_str());
}

static void put_at(const std::string& s, long long at, double baud = 0, int frame = -1) {
	if (s.empty() || (E.st == ESP_OFF) || (E.st == ESP_HELD) || (E.st == ESP_LOADER) || (E.st == ESP_DEEPSLEEP)) return;
	espMsg m;
	m.b = s;
	m.baud = baud ? baud : esp_actual(E.baud);
	m.frame = (frame < 0) ? E.frame : frame;
	m.at = at;
	E.out.push_back(m);
	E.outBytes += s.size();
}

void esp_put(const std::string& s) {
	esp_trace(">", s);
	put_at(s, E.replyAt);
}

void esp_put_now(const std::string& s) {
	esp_trace(">", s);
	put_at(s, 0);
}

void esp_put_rate(const std::string& s, double baud, int frame) {
	put_at(s, 0, baud, frame);
}

void esp_ok() {esp_put("\r\nOK\r\n");}
void esp_err() {esp_put("\r\nERROR\r\n");}

size_t esp_out_queued() {
	return E.outBytes;
}

// whole messages onto the line, each at its own rate; a new rate waits for the line
static void esp_pump(Computer* comp) {
	while (!E.out.empty()) {
		espMsg& m = E.out.front();
		if ((m.baud != E.farBaud) || (m.frame != E.farFrame)) {
			if (nx_uart_feed_left(comp, ESP_UART)) break;
			E.farBaud = m.baud;
			E.farFrame = m.frame;
		}
		if (nx_uart_feed_room(comp, ESP_UART) < (int)m.b.size()) break;
		// asked again every time: a power-on of the machine clears it
		nx_uart_far(comp, ESP_UART, (long long)(esp_clk(comp) / E.farBaud * 65536.0), E.farFrame);
		nx_uart_feed_at(comp, ESP_UART, (const unsigned char*)m.b.data(), (int)m.b.size(), m.at);
		E.outBytes -= m.b.size();
		E.out.pop_front();
	}
}

// == pins ==

static void pins_update() {
	int v = 0;
	if (E.ioOut[0]) v |= 1 | (E.ioLevel[0] ? 2 : 0);
	if (E.ioOut[1]) v |= 4 | (E.ioLevel[1] ? 8 : 0);
	next_esp_pins = v;
}

void esp_gpio_drive(int pin, bool out, int level) {
	int i = (pin == 2) ? 1 : 0;
	E.ioOut[i] = out;
	E.ioLevel[i] = level ? 1 : 0;
	pins_update();
}

int esp_gpio_level(Computer* comp, int pin) {
	if (!comp || !comp->hw || (comp->hw->id != HW_ZXNEXT)) return 1;
	return (pin == 2) ? nx_esp_gpio2(comp) : nx_esp_gpio0(comp);
}

static void pins_float() {
	E.ioOut[0] = E.ioOut[1] = false;
	E.ioLevel[0] = E.ioLevel[1] = 1;
	pins_update();
}

// == the module ==

int esp_state() {return E.st;}
bool esp_echo() {return E.echo;}
void esp_set_echo(bool on) {E.echo = on;}

void esp_hold() {E.held = true;}
void esp_done() {
	E.held = false;
	if (E.rx == RX_BUSY) E.rx = RX_IDLE;
}

void esp_take_input(void (*hook)(Computer*, int, long long)) {E.hook = hook;}
void esp_release_input() {E.hook = nullptr;}

// a sample of the ROM's and the second loader's lines (the AT154 document's); no program reads them
static const char* esp_rom_log =
	"\r\n ets Jan  8 2013,rst cause:2, boot mode:(3,6)\r\n\r\nload 0x40100000, len 1856, room 16 \r\n"
	"tail 0\r\nchksum 0x63\r\nload 0x3ffe8000, len 776, room 8 \r\ntail 0\r\nchksum 0x02\r\n"
	"load 0x3ffe8310, len 552, room 8 \r\ntail 0\r\nchksum 0x79\r\ncsum 0x79\r\n\r\n"
	"2nd boot version : 1.5\r\n  SPI Speed      : 40MHz\r\n  SPI Mode       : DIO\r\n"
	"  SPI Flash Size & Map: 8Mbit(512KB+512KB)\r\njump to run user1 @ 1000\r\n\r\n";

// the parts back to what flash says, the receiver empty; what was queued still goes out (the
// OK of RST), a reset from the Next or power drops it first
static void esp_boot(Computer* comp, long long now) {
	esp_tcp_down();				// no CLOSED: the module is gone
	E.st = ESP_BOOT;
	E.bootStage = 0;
	E.bootAt = now + esp_ms(comp, 60);
	E.echo = true;				// ATE is never saved
	E.rx = RX_IDLE;
	E.prev = -1;
	E.buf.clear();
	E.rstAt = 0;
	E.held = false;
	E.hook = nullptr;
	E.replyAt = 0;
	pins_float();
	// UART0 from the UART_DEF record: out of 1..5000000 (erased) it is 115200 8N1
	long b = esp_fl_num("baud", 115200);
	int fr = (int)esp_fl_num("frame", ESP_FRAME) & 0x1f;
	if ((b < 1) || (b > 5000000)) {b = 115200; fr = ESP_FRAME;}
	E.baud = b;
	E.frame = fr;
	esp_basic_boot(comp);
	esp_wifi_boot(comp);
	esp_tcp_boot(comp);
	xlog(XLG_NET, XLL_INFO, "esp: boot at %li", E.baud);
}

void esp_reboot(Computer* comp, long long at) {
	esp_boot(comp, at);
}

void esp_deep_sleep(Computer* comp, long long ms) {
	(void)comp;
	(void)ms;		// the RTC wakes GPIO16, which goes nowhere on an ESP-01: asleep till a reset
	esp_tcp_down();
	E.st = ESP_DEEPSLEEP;
	E.hook = nullptr;
	E.held = false;
	E.rx = RX_IDLE;
	pins_float();
	xlog(XLG_NET, XLL_INFO, "esp: deep sleep, till a reset");
}

static void esp_boot_step(Computer* comp, long long now) {
	while ((E.st == ESP_BOOT) && (now >= E.bootAt)) {
		E.replyAt = 0;
		if (E.bootStage == 0) {		// the ROM's own log, at 74880: garbage at any other rate
			esp_put_rate(esp_rom_log, 74880.0, ESP_FRAME);
			E.bootAt = now + esp_ms(comp, 240);
			E.bootStage = 1;
		} else {
			esp_put_now("\r\nready\r\n");
			E.st = ESP_READY;
			esp_wifi_ready(comp, now);
		}
	}
}

// == the dispatcher ==

static const espCmd* esp_find(const std::string& name) {
	const espCmd* tabs[] = {esp_basic_cmds, esp_wifi_cmds, esp_tcp_cmds};
	for (const espCmd* t : tabs)
		for (const espCmd* c = t; c->name; c++)
			if (name == c->name) return c;
	return nullptr;
}

// the line after AT, \r\n included
static void esp_dispatch(Computer* comp, const std::string& line, long long end) {
	E.replyAt = end;
	esp_trace("<", (line.compare(0, 6, "+CWJAP") == 0) ? std::string("+CWJAP...") : line);
	// the name runs to the first = ? \r or digit
	size_t i = 0;
	while ((i < line.size()) && (line[i] != '=') && (line[i] != '?') && (line[i] != '\r') && !((line[i] >= '0') && (line[i] <= '9'))) i++;
	if ((i >= line.size()) || (i >= ESP_NAME_MAX)) {esp_err(); return;}
	std::string name = line.substr(0, i);
	char c = line[i];
	if (name.empty()) {			// AT has its execute form only
		if (c == '\r') esp_ok();
		else esp_err();
		return;
	}
	const espCmd* cmd = esp_find(name);
	if (!cmd) {esp_err(); return;}
	espForm f = nullptr;
	if (c == '\r') {
		f = cmd->exec;
	} else if ((c == '?') && (i + 1 < line.size()) && (line[i + 1] == '\r')) {
		f = cmd->query;
	} else if ((c == '=') && (i + 2 < line.size()) && (line[i + 1] == '?') && (line[i + 2] == '\r')) {
		f = cmd->test;
	} else if ((c == '=') || ((c >= '0') && (c <= '9'))) {
		f = cmd->set;
	}
	if (!f) {esp_err(); return;}
	espCmdCall call;
	call.comp = comp;
	call.name = name;
	call.p = line.substr(i);
	call.at = end;
	f(call);
}

// == the receiver ==

static void esp_input(Computer* comp, int c, long long end) {
	if (E.st != ESP_READY) return;			// booting, held, asleep or in its loader: lost
	E.replyAt = end;
	if (E.hook) {
		E.hook(comp, c, end);
		return;
	}
	if (E.echo && c) put_at(std::string(1, (char)c), end);	// every byte as it comes, before all else
	if (E.rx == RX_BUSY) {
		// a command is still running: a line is lost, but AT+RST\r\n forces a restart
		static const char rst[] = "AT+RST\r\n";
		if (c == rst[E.rstAt]) E.rstAt++;
		else E.rstAt = 0;			// not tried again on this byte
		if (c == '\n') {
			if (E.rstAt == 8) {
				esp_put("\r\nWill force to restart!!!\r\n");
				esp_ok();
				esp_reboot(comp, end + esp_ms(comp, 2));
				return;
			}
			esp_put("\r\nbusy p...\r\n");
			E.rstAt = 0;
		}
		return;
	}
	if (E.rx == RX_IDLE) {
		if (((E.prev == 'A') && (c == 'T')) || ((E.prev == 'a') && (c == 't'))) {
			E.rx = RX_LINE;
			E.buf.clear();
			E.prev = -1;
			return;
		}
		if (c == '\n') esp_put("\r\nERROR\r\n");
		E.prev = c;
		return;
	}
	// a line: kept to its LF, the 256th byte past AT ends it with no reply
	if (c != '\n') {
		if (E.buf.size() >= ESP_LINE_MAX) {
			E.rx = RX_IDLE;
			E.prev = -1;			// the head window never sees this byte
			return;
		}
		E.buf += (char)c;
		return;
	}
	E.buf += '\n';
	E.rx = RX_BUSY;
	E.rstAt = 0;
	E.held = false;
	std::string line = E.buf;
	E.buf.clear();
	esp_dispatch(comp, line, end);
	if (!E.held && (E.rx == RX_BUSY)) E.rx = RX_IDLE;
	E.prev = -1;
}

// == the outside ==

void esp_attach(Computer* comp) {
	if (!E.loaded) fl_load();
	E.line.reset();
	E.out.clear();
	E.outBytes = 0;
	E.farBaud = 0;
	E.farFrame = -1;
	E.rst0 = false;
	long long now = nx_uart_sync(comp);
	esp_boot(comp, now);
	next_esp_fitted = 1;
	xlog(XLG_NET, XLL_INFO, "esp: on UART 0");
}

void esp_detach(Computer* comp) {
	if (E.st == ESP_OFF) return;
	esp_tcp_down();
	E.out.clear();
	E.outBytes = 0;
	E.st = ESP_OFF;
	next_esp_fitted = 0;
	pins_float();
	if (comp && comp->hw && (comp->hw->id == HW_ZXNEXT)) nx_uart_far(comp, ESP_UART, 0, 0);
	xlog(XLG_NET, XLL_INFO, "esp: off");
}

static void esp_line_in(Computer* comp, long long now) {
	E.line.setRate(esp_clk(comp) / esp_actual(E.baud), E.frame);
	std::vector<int> got;
	std::vector<long long> ends;
	E.line.take(now, got, &ends);
	for (size_t k = 0; k < got.size(); k++) esp_input(comp, got[k], ends[k]);
}

void esp_byte(Computer* comp, int b, long long start) {
	if ((E.st == ESP_OFF) || comp->rzx.play) return;
	long presc;
	int frame, rtr;
	nx_uart_cfg(comp, ESP_UART, &presc, &frame, &rtr);
	E.line.put(b, start, presc, frame & 0x1f);
	esp_line_in(comp, 0);
	esp_pump(comp);
}

void esp_frame(Computer* comp) {
	if ((E.st == ESP_OFF) || comp->rzx.play) return;
	long long now = nx_uart_sync(comp);	// first: the bytes it hands in are taken here
	// NR 02 b7 holds the peripherals in reset; GPIO0 low as it lets go is the loader
	bool rst = nx_esp_reset(comp);
	if (rst && !E.rst0) {
		esp_tcp_down();
		E.out.clear();
		E.outBytes = 0;
		pins_float();
		E.st = ESP_HELD;
		xlog(XLG_NET, XLL_INFO, "esp: held in reset");
	} else if (!rst && E.rst0) {
		if (!nx_esp_gpio0(comp)) {
			E.st = ESP_LOADER;
			xlog(XLG_NET, XLL_INFO, "esp: GPIO0 low, its ROM loader (not emulated)");
		} else {
			esp_boot(comp, now);
		}
	}
	E.rst0 = rst;
	esp_line_in(comp, now);
	esp_boot_step(comp, now);
	if (E.st == ESP_READY) {
		E.replyAt = 0;
		esp_wifi_frame(comp, now);
		esp_tcp_frame(comp, now);
	}
	esp_pump(comp);
}
