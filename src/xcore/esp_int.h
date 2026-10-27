#pragma once

// The emulated ESP-01's insides, shared by its parts: esp.cpp (the line, the dispatcher, boot,
// flash, pins), esp_basic.cpp (the basic commands), esp_wifi.cpp (Wi-Fi), esp_tcp.cpp (TCP/IP).
// What it answers is NONOS AT 1.7.6's (SDK 3.0.6), byte for byte; the lab's at176-ch3/4/5.md
// say where each reply comes from.

#include <string>

struct Computer;

// == a command ==

// The firmware's own record: a name and four forms, each NULL when the command lacks it (that
// form then answers ERROR). The name has its '+' ("+GMR"); "" is the bare AT, "E" is ATE.
struct espCmdCall {
	Computer* comp;
	std::string name;		// as in the table, with the '+'
	std::string p;			// the line after the name, from the '=' (or digit) on, '\r' kept
	long long at;			// the edge the line ended on
};

typedef void (*espForm)(espCmdCall& c);

struct espCmd {
	const char* name;
	espForm test;			// AT+X=?
	espForm query;			// AT+X?
	espForm set;			// AT+X=... (also AT+X<digit>: the firmware takes it as a set)
	espForm exec;			// AT+X
};

// each part's table, ended by a {NULL} record; searched in this order
extern const espCmd esp_basic_cmds[];
extern const espCmd esp_wifi_cmds[];
extern const espCmd esp_tcp_cmds[];

// == the line out ==

void esp_put(const std::string& s);			// a reply to the line just ended, after its echo
void esp_put_now(const std::string& s);			// unsolicited: whenever the line is free
void esp_put_rate(const std::string& s, double baud, int frame);	// at another rate (the ROM log)
void esp_ok();						// \r\nOK\r\n
void esp_err();						// \r\nERROR\r\n
void esp_trace(const char* dir, const std::string& s);	// the event log, NET at DEBUG
size_t esp_out_queued();				// bytes waiting for the line

// == time ==

long long esp_now(Computer* comp);			// CLK_28 edges
long long esp_ms(Computer* comp, double ms);		// that many ms in edges
long long esp_host_ms();				// the host's clock (SNTP, ping)

// == the receiver ==

// A command that goes on after its handler returns (a join, a connect) keeps the module busy:
// lines meanwhile get busy p...; esp_done() lets it go
void esp_hold();
void esp_done();

// Bytes the guest sends go to a part instead of the command line (CIPSEND's payload,
// passthrough) while a hook is set; the hook gets each byte and the edge its stop bit ended on
void esp_take_input(void (*hook)(Computer*, int, long long));
void esp_release_input();

// == the module ==

enum {ESP_OFF = 0, ESP_HELD, ESP_LOADER, ESP_BOOT, ESP_READY, ESP_DEEPSLEEP};
int esp_state();
void esp_reboot(Computer* comp, long long at);		// RST, RESTORE, the end of GSLP
void esp_deep_sleep(Computer* comp, long long ms);	// GSLP: GPIO16 is not wired on an ESP-01
bool esp_echo();
void esp_set_echo(bool on);

// the UART as the module runs it
long esp_baud();					// asked for; the line runs at esp_actual() of it
int esp_frame_bits();					// uart_framing_r form
double esp_actual(long baud);
void esp_set_uart(long baud, int frame);		// for what is put from now on (an OK put before goes at the old rate)

// == flash ==

// The user and system parameter areas as key = value text (nvram/esp01.conf); a key absent
// reads as its default. Every set writes the file.
std::string esp_fl_get(const char* key, const std::string& def = "");
long esp_fl_num(const char* key, long def);
void esp_fl_set(const char* key, const std::string& val);
void esp_fl_set_num(const char* key, long val);
void esp_fl_clear();					// RESTORE: the factory state

bool esp_lan();						// a server takes clients from the network too

// == pins: GPIO0 and GPIO2 reach the Next (NR A8/A9) ==

enum {ESP_IO_IN = 0, ESP_IO_OUT};
void esp_gpio_drive(int pin, bool out, int level);	// pin 0 or 2
int esp_gpio_level(Computer* comp, int pin);		// what is on the pin now (the Next's drive too)

// == the parts ==

// boot: after RST, RESTORE, a reset from the Next or power: the part's run-time state back to
// what flash says; ready: the module has printed "ready" (auto-connect); frame: each frame end
// while ready; down: the module stops (no output from here on)
void esp_basic_boot(Computer* comp);
void esp_wifi_boot(Computer* comp);
void esp_tcp_boot(Computer* comp);
void esp_wifi_ready(Computer* comp, long long now);
void esp_wifi_frame(Computer* comp, long long now);
void esp_tcp_frame(Computer* comp, long long now);
void esp_tcp_down();					// sockets closed, no CLOSED printed

// what the parts ask of each other
bool esp_wifi_joined();					// the station has an IP
int esp_wifi_mode();					// CWMODE_CUR 1..3
std::string esp_wifi_sta_ip();
std::string esp_wifi_sta_mac();
std::string esp_wifi_ap_ip();
std::string esp_wifi_ap_mac();
std::string esp_wifi_dhcp_dns();			// the DNS server the AP's DHCP handed out, "" if none
void esp_tcp_wifi_lost(Computer* comp);			// the station left its AP: links close
bool esp_tcp_passthrough();				// passthrough is sending: the Wi-Fi status lines keep quiet
