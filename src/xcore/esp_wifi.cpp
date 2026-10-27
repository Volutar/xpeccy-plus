// The emulated ESP-01's Wi-Fi commands (NONOS AT 1.7.6), byte for byte, and its Wi-Fi status
// lines. The radio is synthetic: one AP, named as the station's SSID ("Xpeccy+" when it has
// none), that takes any password; nothing is ever seen of the host's own Wi-Fi. A SoftAP is
// kept as settings only - no station can join it - and smartconfig, WeChat discovery, WPS
// and mDNS start and never find anyone.
//
// Flash keys (nvram/esp01.conf), an absent key reads as the factory value after it:
//   ssid ("Xpeccy+": the old default, RESTORE writes ""), pass, pci (0), cwmode (1),
//   autoconn (1), dhcp (yes: station client), dhcps (yes: SoftAP server),
//   ip gw mask (the static station address; none when absent),
//   lease_ip lease_gw lease_mask (what the synthetic AP's DHCP hands out: 192.168.1.70/.1/24),
//   stamac (5c:cf:7f:58:50:2b), apmac (5e:cf:7f:58:50:2b),
//   apip apgw apmask (192.168.4.1/.1/24),
//   ap_ssid (ESP_ + the SoftAP MAC's last 3 bytes), ap_pass, ap_ch (1), ap_ecn (0),
//   ap_max (4), ap_hidden (0),
//   dhcps_lease dhcps_start dhcps_end (absent: the SDK's default range),
//   cc_policy (0), cc_code (CN), cc_start (1), cc_count (13).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "esp_int.h"

#define AP_BSSID	"5c:cf:7f:00:00:01"
#define AP_CH		6
#define AP_RSSI		-55

typedef unsigned long ip4;		// a.b.c.d as (a << 24) | ... | d

enum {J_NONE = 0, J_CONN, J_IP, J_OK, J_FAIL};

static struct {
	int mode = 1;
	bool autoconn = true;
	// the station's current config
	std::string ssid, pass;
	int pci = 0;
	// the station
	bool assoc = false;		// with the AP
	bool joined = false;		// and an IP
	int discFlag = 10;		// 1: WIFI DISCONNECT already printed since the last CONNECTED
	bool dhcpSta = true, dhcpAp = true;
	ip4 ip = 0, gw = 0, mask = 0;
	unsigned char staMac[6], apMac[6];
	std::string hostname;
	// a join on its way
	int jph = J_NONE;
	bool jcmd = false;		// CWJAP's: it ends in OK or FAIL
	long long jt0 = 0, jConn = 0, jIp = 0, jOk = 0, jDisc = 0, jFail = 0;
	bool jDiscDue = false;
	// a scan
	bool scanning = false;
	long long scanAt = 0;
	std::string scanSsid, scanMac;
	long scanCh = 0;
	long lapSort = 0, lapMask = 0x7ff;
	// the SoftAP
	std::string apSsid, apPass;
	int apCh = 1, apEcn = 0, apMax = 4, apHidden = 0;
	ip4 apIp = 0, apGw = 0, apMask = 0;
	bool dhcpsSet = false;
	long dhcpsLease = 120;
	ip4 dhcpsStart = 0, dhcpsEnd = 0;
	// the rest
	int ccPolicy = 0, ccStart = 1, ccCount = 13;
	std::string ccCode = "CN";
	bool smart = false;
	bool wps = false;
	long long wpsAt = 0;
	bool mdns = false;
} W;

// == the arguments, as the firmware takes them ==

// From the '=' (or the digit) of a set line: quoted strings with \ escaping, digits only,
// commas, and the CR right after the last one
struct espArgs {
	const std::string& s;
	size_t i;
	explicit espArgs(const espCmdCall& c) : s(c.p), i(0) {
		if (!s.empty() && (s[0] == '=')) i = 1;
	}
	bool at(char ch) const {return (i < s.size()) && (s[i] == ch);}
	bool end() const {return at('\r');}
	bool empty() const {return at(',') || end();}
	bool comma() {
		if (!at(',')) return false;
		i++;
		return true;
	}
	bool num(long* v) {
		size_t j = i;
		long n = 0;
		while ((j < s.size()) && (s[j] >= '0') && (s[j] <= '9') && (j - i < 9)) n = n * 10 + (s[j++] - '0');
		if ((j == i) || ((j < s.size()) && (s[j] >= '0') && (s[j] <= '9'))) return false;
		i = j;
		*v = n;
		return true;
	}
	bool str(std::string* out, size_t max) {
		if (!at('"')) return false;
		std::string r;
		size_t j = i + 1;
		while (true) {
			if (j >= s.size() || (s[j] == '\r')) return false;
			if ((s[j] == '\\') && (j + 1 < s.size()) && (s[j + 1] != '\r')) {
				r += s[j + 1];
				j += 2;
				continue;
			}
			if (s[j] == '"') break;
			r += s[j++];
		}
		if (r.size() > max) return false;
		i = j + 1;
		*out = r;
		return true;
	}
};

static bool ip_parse(const std::string& s, ip4* v) {
	ip4 r = 0;
	size_t i = 0;
	for (int k = 0; k < 4; k++) {
		if (k && ((i >= s.size()) || (s[i++] != '.'))) return false;
		size_t j = i;
		unsigned n = 0;
		while ((j < s.size()) && (s[j] >= '0') && (s[j] <= '9') && (j - i < 3)) n = n * 10 + (s[j++] - '0');
		if ((j == i) || (n > 255)) return false;
		r = (r << 8) | n;
		i = j;
	}
	if (i != s.size()) return false;
	*v = r;
	return true;
}

static std::string ip_str(ip4 v) {
	char b[20];
	snprintf(b, sizeof(b), "%lu.%lu.%lu.%lu", (v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
	return b;
}

static ip4 ip_fl(const char* key, const char* def) {
	ip4 v = 0;
	if (!ip_parse(esp_fl_get(key, def), &v)) ip_parse(def, &v);
	return v;
}

static int hexv(char c) {
	if ((c >= '0') && (c <= '9')) return c - '0';
	if ((c >= 'a') && (c <= 'f')) return c - 'a' + 10;
	if ((c >= 'A') && (c <= 'F')) return c - 'A' + 10;
	return -1;
}

static bool mac_parse(const std::string& s, unsigned char* m) {
	if (s.size() != 17) return false;
	for (int k = 0; k < 6; k++) {
		int h = hexv(s[k * 3]), l = hexv(s[k * 3 + 1]);
		if ((h < 0) || (l < 0) || ((k < 5) && (s[k * 3 + 2] != ':'))) return false;
		m[k] = (unsigned char)((h << 4) | l);
	}
	return true;
}

static std::string mac_str(const unsigned char* m) {
	char b[20];
	snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	return b;
}

static void mac_fl(const char* key, const char* def, unsigned char* m) {
	if (!mac_parse(esp_fl_get(key, def), m)) mac_parse(def, m);
}

static std::string yes(bool b) {return b ? "yes" : "no";}

// == the station ==

// the SDK's Wi-Fi event as the firmware prints it; passthrough mutes the whole callback,
// its flag included
static void wifi_event(int flag, const char* line) {
	if (esp_tcp_passthrough()) return;
	if ((flag == 1) && (W.discFlag == 1)) return;	// one WIFI DISCONNECT per connection
	W.discFlag = flag;
	esp_put_now(line);
}

static bool sta_on() {return W.mode != 2;}
static bool ap_on() {return W.mode != 1;}

static std::string ap_name() {
	return W.ssid.empty() ? std::string("Xpeccy+") : W.ssid;
}

// the station leaves the AP: the SDK's disconnect event, printed once per connection
static void sta_leave(Computer* comp) {
	W.jph = J_NONE;
	W.jDiscDue = false;
	if (!W.assoc) return;
	wifi_event(1, "WIFI DISCONNECT\r\n");
	W.assoc = false;
	W.joined = false;
	if (W.dhcpSta) W.ip = W.gw = W.mask = 0;
	esp_tcp_wifi_lost(comp);
}

// the stored AP, by itself: at boot, or when a mode with the station comes back
static void auto_join(Computer* comp, long long now) {
	if (!sta_on() || !W.autoconn || W.ssid.empty() || W.assoc) return;
	W.jph = J_CONN;
	W.jcmd = false;
	W.jt0 = now;
	W.jConn = now + esp_ms(comp, 1000);
	W.jIp = W.jConn + esp_ms(comp, 300);
}

static void got_ip() {
	W.joined = true;
	if (W.dhcpSta) {
		W.ip = ip_fl("lease_ip", "192.168.1.70");
		W.gw = ip_fl("lease_gw", "192.168.1.1");
		W.mask = ip_fl("lease_mask", "255.255.255.0");
	}
	wifi_event(3, "WIFI GOT IP\r\n");
}

static void join_step(Computer* comp, long long now) {
	bool more = true;
	while (more) {
		more = false;
		if ((W.jph == J_CONN) && (now >= W.jConn)) {
			W.assoc = true;
			wifi_event(0, "WIFI CONNECTED\r\n");
			W.jph = J_IP;
			more = true;
		} else if ((W.jph == J_IP) && (now >= W.jIp)) {
			got_ip();
			if (W.jcmd) {
				// CWJAP's own timer looks once a second
				long long sec = esp_ms(comp, 1000);
				W.jOk = W.jt0 + ((W.jIp - W.jt0 + sec - 1) / sec) * sec;
				W.jph = J_OK;
				more = true;
			} else {
				W.jph = J_NONE;
			}
		} else if ((W.jph == J_OK) && (now >= W.jOk)) {
			W.jph = J_NONE;
			esp_put_now("\r\nOK\r\n");
			esp_done();
		} else if (W.jph == J_FAIL) {
			if (W.jDiscDue && (now >= W.jDisc) && (now < W.jFail)) {
				W.jDiscDue = false;
				wifi_event(1, "WIFI DISCONNECT\r\n");
			}
			if (now >= W.jFail) {
				W.jph = J_NONE;
				W.jDiscDue = false;
				esp_put_now("+CWJAP:3\r\n\r\nFAIL\r\n");	// 3: no such AP
				esp_done();
			}
		}
	}
}

// == CWMODE ==

static void mode_test(espCmdCall& c) {
	esp_put(c.name + ":(1-3)\r\n");
	esp_ok();
}

static void mode_print(espCmdCall& c, int m) {
	esp_put(c.name + ":" + std::to_string(m) + "\r\n");
	esp_ok();
}

static void mode_query(espCmdCall& c) {mode_print(c, W.mode);}
static void mode_query_def(espCmdCall& c) {mode_print(c, (int)esp_fl_num("cwmode", 1));}

static void mode_setx(espCmdCall& c, bool def) {
	espArgs a(c);
	long m;
	if (!a.num(&m) || (m < 1) || (m > 3) || !a.end()) {esp_err(); return;}
	bool had = sta_on();
	W.mode = (int)m;
	if (def) esp_fl_set_num("cwmode", m);
	esp_ok();
	if (had && !sta_on()) sta_leave(c.comp);
	else if (!had && sta_on()) auto_join(c.comp, esp_now(c.comp));
}

static void mode_set(espCmdCall& c) {mode_setx(c, false);}
static void mode_set_def(espCmdCall& c) {mode_setx(c, true);}

// == CWJAP ==

static void jap_print(espCmdCall& c, const std::string& ssid, int pci) {
	if (!W.joined) {
		esp_put("No AP\r\n");
		esp_ok();
		return;
	}
	esp_put(c.name + ":\"" + ssid + "\",\"" AP_BSSID "\"," + std::to_string(AP_CH) + "," + std::to_string(AP_RSSI) + ","
		+ std::to_string(pci) + "\r\n");
	esp_ok();
}

static void jap_query(espCmdCall& c) {jap_print(c, W.ssid, W.pci);}
static void jap_query_def(espCmdCall& c) {jap_print(c, esp_fl_get("ssid", "Xpeccy+"), (int)esp_fl_num("pci", 0));}

static void jap_setx(espCmdCall& c, bool def) {
	if (!sta_on()) {esp_err(); return;}
	if (W.smart) {
		esp_put("Smart started");
		esp_err();
		return;
	}
	espArgs a(c);
	std::string ssid, pass, bssid;
	long pci = 0;
	if (!a.str(&ssid, 32) || ssid.empty() || !a.comma() || !a.str(&pass, 64)) {esp_err(); return;}
	if (a.comma()) {
		unsigned char m[6];
		if (!a.empty() && (!a.str(&bssid, 17) || !mac_parse(bssid, m))) {esp_err(); return;}
		if (!bssid.empty()) bssid = mac_str(m);		// hex in either case, as the firmware reads it
		if (a.comma() && (!a.num(&pci) || (pci > 1))) {esp_err(); return;}
	}
	if (!a.end()) {esp_err(); return;}
	sta_leave(c.comp);
	W.ssid = ssid;
	W.pass = pass;
	W.pci = (int)pci;
	if (def) {
		esp_fl_set("ssid", ssid);
		esp_fl_set("pass", pass);
		esp_fl_set_num("pci", pci);
	}
	esp_hold();
	long long now = esp_now(c.comp);
	W.jcmd = true;
	W.jt0 = now;
	if (!bssid.empty() && (bssid != AP_BSSID)) {
		// asked for an AP that is not there: it tries for 14 s
		W.jph = J_FAIL;
		W.jDiscDue = true;
		W.jDisc = now + esp_ms(c.comp, 2000);
		W.jFail = now + esp_ms(c.comp, 14000);
		return;
	}
	W.jph = J_CONN;
	W.jConn = now + esp_ms(c.comp, 1200);
	W.jIp = now + esp_ms(c.comp, 1500);
}

static void jap_set(espCmdCall& c) {jap_setx(c, false);}
static void jap_set_def(espCmdCall& c) {jap_setx(c, true);}

// == CWLAPOPT, CWLAP ==

static void lapopt_set(espCmdCall& c) {
	espArgs a(c);
	long sort, mask;
	if (!a.num(&sort)) {esp_err(); return;}
	if (sort > 1) {
		esp_put("flag error\r\n");
		esp_err();
		return;
	}
	if (!a.comma()) {esp_err(); return;}
	if (!a.num(&mask)) {
		esp_put("get mask error\r\n");
		esp_err();
		return;
	}
	if (!a.end()) {esp_err(); return;}
	W.lapSort = sort;
	W.lapMask = mask;
	esp_ok();
}

static void lap_start(espCmdCall& c, double ms) {
	W.scanning = true;
	W.scanAt = esp_now(c.comp) + esp_ms(c.comp, ms);
	esp_hold();
}

static void lap_exec(espCmdCall& c) {
	if (!sta_on()) {esp_err(); return;}
	W.scanSsid.clear();
	W.scanMac.clear();
	W.scanCh = 0;
	lap_start(c, 14 * 120 + 300);
}

static void lap_set(espCmdCall& c) {
	if (!sta_on()) {esp_err(); return;}
	espArgs a(c);
	std::string ssid, mac;
	long ch = 0, type = 0, tmin = 0, tmax = 0;
	unsigned char m[6];
	if (!a.empty() && !a.str(&ssid, 32)) {esp_err(); return;}
	if (a.comma()) {
		if (!a.empty() && (!a.str(&mac, 17) || !mac_parse(mac, m))) {esp_err(); return;}
		if (a.comma()) {
			if (!a.empty() && (!a.num(&ch) || (ch > 14))) {esp_err(); return;}
			if (a.comma()) {
				if (!a.empty() && (!a.num(&type) || (type > 1))) {esp_err(); return;}
				if (a.comma()) {
					if (!a.empty() && (!a.num(&tmin) || (tmin > 1500))) {esp_err(); return;}
					if (a.comma() && !a.empty() && (!a.num(&tmax) || (tmax > 1500))) {esp_err(); return;}
				}
			}
		}
	}
	if (!a.end()) {esp_err(); return;}
	W.scanSsid = ssid;
	W.scanMac = mac;
	W.scanCh = ch;
	double per = type ? (tmax ? tmax : 360) : (tmax ? tmax : 120);
	lap_start(c, (ch ? 1 : 14) * per + 300);
}

// one line per AP, the fields CWLAPOPT's mask keeps, commas only between them
static void lap_done() {
	W.scanning = false;
	std::string r;
	unsigned char m[6];
	bool match = (W.scanSsid.empty() || (W.scanSsid == ap_name()))
		&& (W.scanMac.empty() || (mac_parse(W.scanMac, m) && (mac_str(m) == AP_BSSID)))
		&& (!W.scanCh || (W.scanCh == AP_CH));
	if (match) {
		bool sec = !W.pass.empty();
		std::string f[11] = {sec ? "3" : "0", "\"" + ap_name().substr(0, 32) + "\"", std::to_string(AP_RSSI),
			"\"" AP_BSSID "\"", std::to_string(AP_CH), "-12", "0", sec ? "4" : "0", sec ? "4" : "0", "7", "0"};
		std::string line = "+CWLAP:(";
		bool first = true;
		for (int k = 0; k < 11; k++) {
			if (!((W.lapMask >> k) & 1)) continue;
			if (!first) line += ",";
			line += f[k];
			first = false;
		}
		r = line + ")\r\n";
	}
	esp_put_now(r + "\r\nOK\r\n");
	esp_done();
}

// == CWQAP ==

static void qap_test(espCmdCall& c) {
	(void)c;
	esp_ok();
}

static void qap_exec(espCmdCall& c) {
	esp_ok();
	sta_leave(c.comp);			// its event comes after the OK
}

// == CWSAP, CWLIF ==

static void sap_print(espCmdCall& c, const std::string& ssid, const std::string& pass, int ch, int ecn, int mx, int hid) {
	if (!ap_on()) {esp_err(); return;}
	char s[48];
	snprintf(s, sizeof(s), ",%i,%i,%i,%i\r\n", ch, ecn, mx, hid);
	esp_put(c.name + ":\"" + ssid + "\",\"" + pass + "\"" + s);
	esp_ok();
}

static std::string ap_ssid_def() {
	char b[16];
	snprintf(b, sizeof(b), "ESP_%02X%02X%02X", W.apMac[3], W.apMac[4], W.apMac[5]);
	return b;
}

static void sap_query(espCmdCall& c) {sap_print(c, W.apSsid, W.apPass, W.apCh, W.apEcn, W.apMax, W.apHidden);}
static void sap_query_def(espCmdCall& c) {
	sap_print(c, esp_fl_get("ap_ssid", ap_ssid_def()), esp_fl_get("ap_pass"), (int)esp_fl_num("ap_ch", 1),
		(int)esp_fl_num("ap_ecn", 0), (int)esp_fl_num("ap_max", 4), (int)esp_fl_num("ap_hidden", 0));
}

static void sap_setx(espCmdCall& c, bool def) {
	if (!ap_on()) {esp_err(); return;}
	espArgs a(c);
	std::string ssid, pass;
	long ch, ecn, mx = W.apMax, hid = W.apHidden;
	if (!a.str(&ssid, 32) || ssid.empty() || !a.comma() || !a.str(&pass, 64) || !a.comma()) {esp_err(); return;}
	if (!a.num(&ch) || (ch < 1) || (ch > 13) || !a.comma()) {esp_err(); return;}
	if (!a.num(&ecn) || (ecn > 4) || (ecn == 1) || (ecn && (pass.size() < 8))) {esp_err(); return;}
	if (a.comma()) {
		if (!a.num(&mx) || (mx < 1) || (mx > 8)) {esp_err(); return;}
		if (a.comma() && (!a.num(&hid) || (hid > 1))) {esp_err(); return;}
	}
	if (!a.end()) {esp_err(); return;}
	W.apSsid = ssid;
	W.apPass = pass;
	W.apCh = (int)ch;
	W.apEcn = (int)ecn;
	W.apMax = (int)mx;
	W.apHidden = (int)hid;
	if (def) {
		esp_fl_set("ap_ssid", ssid);
		esp_fl_set("ap_pass", pass);
		esp_fl_set_num("ap_ch", ch);
		esp_fl_set_num("ap_ecn", ecn);
		esp_fl_set_num("ap_max", mx);
		esp_fl_set_num("ap_hidden", hid);
	}
	esp_ok();
}

static void sap_set(espCmdCall& c) {sap_setx(c, false);}
static void sap_set_def(espCmdCall& c) {sap_setx(c, true);}

static void lif_exec(espCmdCall& c) {
	(void)c;
	if (!ap_on()) {esp_err(); return;}
	esp_ok();				// nobody ever joins the emulated SoftAP
}

// == CWDHCP, CWDHCPS ==

static int dhcp_bits_fl() {
	return ((esp_fl_get("dhcps", "yes") == "yes") ? 1 : 0) | ((esp_fl_get("dhcp", "yes") == "yes") ? 2 : 0);
}

// no line end of its own: the OK's CRLF follows at once
static void dhcp_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string((W.dhcpAp ? 1 : 0) | (W.dhcpSta ? 2 : 0)));
	esp_ok();
}

static void dhcp_query_def(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(dhcp_bits_fl()));
	esp_ok();
}

static void dhcp_setx(espCmdCall& c, bool def) {
	espArgs a(c);
	long m, en;
	if (!a.num(&m) || (m > 2) || !a.comma() || !a.num(&en) || (en > 1) || !a.end()) {esp_err(); return;}
	if (m != 1) W.dhcpAp = (en != 0);			// 0: the SoftAP's server, 1: the station's client, 2: both
	if (m != 0) {
		bool was = W.dhcpSta;
		W.dhcpSta = (en != 0);
		if (W.dhcpSta && !was && W.joined) {
			W.ip = ip_fl("lease_ip", "192.168.1.70");
			W.gw = ip_fl("lease_gw", "192.168.1.1");
			W.mask = ip_fl("lease_mask", "255.255.255.0");
		} else if (!W.dhcpSta && was && !esp_fl_get("ip").empty()) {
			W.ip = ip_fl("ip", "0.0.0.0");		// a stored static address comes back
			W.gw = ip_fl("gw", "0.0.0.0");
			W.mask = ip_fl("mask", "255.255.255.0");
		}
	}
	if (def) {
		esp_fl_set("dhcps", yes(W.dhcpAp));
		esp_fl_set("dhcp", yes(W.dhcpSta));
	}
	esp_ok();
}

static void dhcp_set(espCmdCall& c) {dhcp_setx(c, false);}
static void dhcp_set_def(espCmdCall& c) {dhcp_setx(c, true);}

// the SDK's own range: the 100 addresses after the SoftAP's
static void dhcps_default(ip4 apip, ip4* s, ip4* e) {
	ip4 net = apip & 0xffffff00UL;
	ip4 h = apip & 255;
	ip4 st = (h >= 0x80) ? h - 100 : h + 1;
	*s = net | st;
	*e = net | (st + 99);
}

static void dhcps_print(espCmdCall& c, long lease, ip4 s, ip4 e) {
	if (!ap_on()) {esp_err(); return;}
	esp_put(c.name + ":" + std::to_string(lease) + "," + ip_str(s) + "," + ip_str(e));
	esp_ok();
}

static void dhcps_query(espCmdCall& c) {
	ip4 s = W.dhcpsStart, e = W.dhcpsEnd;
	if (!W.dhcpsSet) dhcps_default(W.apIp, &s, &e);
	dhcps_print(c, W.dhcpsSet ? W.dhcpsLease : 120, s, e);
}

static void dhcps_query_def(espCmdCall& c) {
	ip4 s, e;
	dhcps_default(ip_fl("apip", "192.168.4.1"), &s, &e);
	if (!esp_fl_get("dhcps_start").empty()) {
		ip_parse(esp_fl_get("dhcps_start"), &s);
		ip_parse(esp_fl_get("dhcps_end"), &e);
	}
	dhcps_print(c, esp_fl_num("dhcps_lease", 120), s, e);
}

static void dhcps_setx(espCmdCall& c, bool def) {
	if (!ap_on()) {esp_err(); return;}
	espArgs a(c);
	long en, lease = 120;
	std::string ss, es;
	ip4 s = 0, e = 0;
	if (!a.num(&en) || (en > 1)) {esp_err(); return;}
	if (en) {
		if (!a.comma() || !a.num(&lease) || (lease < 1) || (lease > 2880) || !a.comma()) {esp_err(); return;}
		if (!a.str(&ss, 32) || !ip_parse(ss, &s) || !a.comma() || !a.str(&es, 32) || !ip_parse(es, &e)) {esp_err(); return;}
		// the SDK wants them in the SoftAP's /24, in order
		ip4 net = W.apIp & 0xffffff00UL;
		if (((s & 0xffffff00UL) != net) || ((e & 0xffffff00UL) != net) || (e < s)) {esp_err(); return;}
	}
	if (!a.end()) {esp_err(); return;}
	W.dhcpsSet = (en != 0);
	W.dhcpsLease = lease;
	W.dhcpsStart = s;
	W.dhcpsEnd = e;
	if (def) {
		esp_fl_set_num("dhcps_lease", lease);
		esp_fl_set("dhcps_start", en ? ip_str(s) : std::string());
		esp_fl_set("dhcps_end", en ? ip_str(e) : std::string());
	}
	esp_ok();
}

static void dhcps_set(espCmdCall& c) {dhcps_setx(c, false);}
static void dhcps_set_def(espCmdCall& c) {dhcps_setx(c, true);}

// == CWAUTOCONN ==

static void auto_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(W.autoconn ? 1 : 0));
	esp_ok();
}

static void auto_set(espCmdCall& c) {
	espArgs a(c);
	long n;
	if (!a.num(&n) || (n > 1) || !a.end()) {esp_err(); return;}
	W.autoconn = (n != 0);
	esp_fl_set_num("autoconn", n);		// always saved
	esp_ok();
}

// == CIPSTAMAC, CIPAPMAC ==

static void mac_print(espCmdCall& c, const unsigned char* m) {
	esp_put(c.name + ":\"" + mac_str(m) + "\"\r\n");
	esp_ok();
}

static void stamac_query(espCmdCall& c) {mac_print(c, W.staMac);}
static void apmac_query(espCmdCall& c) {mac_print(c, W.apMac);}

static void stamac_query_def(espCmdCall& c) {
	unsigned char m[6];
	if (!mac_parse(esp_fl_get("stamac"), m)) {mac_print(c, W.staMac); return;}
	mac_print(c, m);
}

static void apmac_query_def(espCmdCall& c) {
	unsigned char m[6];
	if (!mac_parse(esp_fl_get("apmac"), m)) {mac_print(c, W.apMac); return;}
	mac_print(c, m);
}

// all-FF, all-00 and multicast refused; the interface must be on
static bool mac_arg(espCmdCall& c, bool on, unsigned char* m) {
	espArgs a(c);
	std::string s;
	if (!on || !a.str(&s, 17) || !mac_parse(s, m) || !a.end()) return false;
	bool ff = true, zero = true;
	for (int k = 0; k < 6; k++) {
		if (m[k] != 0xff) ff = false;
		if (m[k] != 0) zero = false;
	}
	return !ff && !zero && !(m[0] & 1);
}

static void stamac_setx(espCmdCall& c, bool def) {
	unsigned char m[6];
	if (!mac_arg(c, sta_on(), m)) {esp_err(); return;}
	for (int k = 0; k < 6; k++) W.staMac[k] = m[k];
	if (def) esp_fl_set("stamac", mac_str(m));
	esp_ok();
}

static void apmac_setx(espCmdCall& c, bool def) {
	unsigned char m[6];
	if (!mac_arg(c, ap_on(), m)) {esp_err(); return;}
	for (int k = 0; k < 6; k++) W.apMac[k] = m[k];
	if (def) esp_fl_set("apmac", mac_str(m));
	esp_ok();
}

static void stamac_set(espCmdCall& c) {stamac_setx(c, false);}
static void stamac_set_def(espCmdCall& c) {stamac_setx(c, true);}
static void apmac_set(espCmdCall& c) {apmac_setx(c, false);}
static void apmac_set_def(espCmdCall& c) {apmac_setx(c, true);}

// == CIPSTA, CIPAP ==

static void ip_print(espCmdCall& c, ip4 ip, ip4 gw, ip4 mask) {
	esp_put(c.name + ":ip:\"" + ip_str(ip) + "\"\r\n" + c.name + ":gateway:\"" + ip_str(gw) + "\"\r\n" + c.name
		+ ":netmask:\"" + ip_str(mask) + "\"\r\n");
	esp_ok();
}

static void sta_query(espCmdCall& c) {ip_print(c, W.ip, W.gw, W.mask);}

static void sta_query_def(espCmdCall& c) {
	if (esp_fl_get("ip").empty()) {ip_print(c, 0, 0, 0); return;}
	ip_print(c, ip_fl("ip", "0.0.0.0"), ip_fl("gw", "0.0.0.0"), ip_fl("mask", "0.0.0.0"));
}

// "<ip>"[,"<gateway>"[,"<netmask>"]], an empty field allowed; *gw and *mask come in as the
// values an omitted one takes
static bool ip_args(espCmdCall& c, ip4* ip, ip4* gw, ip4* mask, bool gwFromIp) {
	espArgs a(c);
	std::string s;
	if (!a.str(&s, 32) || !ip_parse(s, ip)) return false;
	if (gwFromIp) *gw = (*ip & 0xffffff00UL) | 1;
	if (a.comma()) {
		if (!a.empty() && (!a.str(&s, 32) || !ip_parse(s, gw))) return false;
		if (a.comma() && !a.empty() && (!a.str(&s, 32) || !ip_parse(s, mask))) return false;
	}
	return a.end();
}

static void sta_setx(espCmdCall& c, bool def) {
	ip4 ip, gw = 0, mask = 0xffffff00UL;
	if (!ip_args(c, &ip, &gw, &mask, true)) {esp_err(); return;}
	W.dhcpSta = false;			// a static address ends the DHCP client
	W.ip = ip;
	W.gw = gw;
	W.mask = mask;
	if (def) {
		esp_fl_set("dhcp", "no");
		esp_fl_set("ip", ip_str(ip));
		esp_fl_set("gw", ip_str(gw));
		esp_fl_set("mask", ip_str(mask));
	}
	esp_ok();
}

static void sta_set(espCmdCall& c) {sta_setx(c, false);}
static void sta_set_def(espCmdCall& c) {sta_setx(c, true);}

static void ap_query(espCmdCall& c) {ip_print(c, W.apIp, W.apGw, W.apMask);}

static void ap_query_def(espCmdCall& c) {
	ip_print(c, ip_fl("apip", "192.168.4.1"), ip_fl("apgw", "192.168.4.1"), ip_fl("apmask", "255.255.255.0"));
}

static void ap_setx(espCmdCall& c, bool def) {
	ip4 ip, gw = W.apGw, mask = 0xffffff00UL;
	if (!ap_on() || !ip_args(c, &ip, &gw, &mask, false)) {esp_err(); return;}
	W.apIp = ip;
	W.apGw = gw;
	W.apMask = mask;
	if (def) {
		esp_fl_set("apip", ip_str(ip));
		esp_fl_set("apgw", ip_str(gw));
		esp_fl_set("apmask", ip_str(mask));
	}
	esp_ok();
}

static void ap_set(espCmdCall& c) {ap_setx(c, false);}
static void ap_set_def(espCmdCall& c) {ap_setx(c, true);}

// == smartconfig, WeChat discovery ==

// no phone sends the credentials: started, it waits till CWSTOPSMART
static void smart_start(espCmdCall& c) {
	if ((W.mode != 1) || W.smart) {esp_err(); return;}
	(void)c;
	W.smart = true;
	esp_ok();
}

static void smart_set(espCmdCall& c) {
	espArgs a(c);
	long t;
	if (!a.num(&t) || (t < 1) || (t > 3) || !a.end()) {esp_err(); return;}
	smart_start(c);
}

static void smart_stop(espCmdCall& c) {
	(void)c;
	W.smart = false;
	esp_ok();
}

static void disc_start(espCmdCall& c) {
	espArgs a(c);
	std::string id, type;
	long t;
	if (!a.str(&id, 64) || !a.comma() || !a.str(&type, 64) || !a.comma() || !a.num(&t) || (t > 24 * 3600) || !a.end()) {
		esp_err();
		return;
	}
	esp_ok();
}

static void disc_stop(espCmdCall& c) {
	(void)c;
	esp_ok();
}

// == WPS ==

// no WPS AP answers: it gives up after the 2 minutes WPS allows
static void wps_on(espCmdCall& c) {
	W.wps = true;
	W.wpsAt = esp_now(c.comp) + esp_ms(c.comp, 120000);
	esp_put("wps started\r\n");
	esp_ok();
}

static void wps_exec(espCmdCall& c) {
	if (W.mode != 1) {
		esp_put("opmode mismatch when wps\r\n");
		esp_err();
		return;
	}
	wps_on(c);
}

static void wps_set(espCmdCall& c) {
	if (W.mode != 1) {
		esp_put("opmode mismatch when wps\r\n");
		esp_err();
		return;
	}
	espArgs a(c);
	long n;
	if (!a.num(&n) || (n > 1)) {
		esp_put("wps command error\r\n");
		esp_err();
		return;
	}
	if (n) {
		wps_on(c);
		return;
	}
	W.wps = false;
	esp_put("wps stoped\r\n");
	esp_ok();
}

// == MDNS ==

// kept as state: nothing is announced on the host's network
static void mdns_set(espCmdCall& c) {
	espArgs a(c);
	long en;
	if (!a.num(&en) || (en > 1)) {
		esp_put("para error when mdns\r\n");
		esp_err();
		return;
	}
	if (!en) {
		if (!a.end()) {
			esp_put("para error when disable mdns\r\n");
			esp_err();
			return;
		}
		W.mdns = false;
		esp_ok();
		return;
	}
	const char* bad = nullptr;
	std::string host, server;
	long port;
	if ((W.mode != 1) && (W.mode != 3)) bad = "opmode mismatch when mdns\r\n";
	else if (!W.ip) bad = "ip invalid when mdns\r\n";
	else if (!a.comma() || !a.str(&host, 32) || host.empty()) bad = "get hostname fail when mdns\r\n";
	else if (!a.comma()) bad = "hostname error when mdns\r\n";
	else if (!a.str(&server, 32) || server.empty()) bad = "get servername fail when mdns\r\n";
	else if (!a.comma()) bad = "servername error when mdns\r\n";
	else if (!a.num(&port) || (port > 65534) || !a.end()) bad = "servername error when mdns\r\n";
	if (bad) {
		esp_put(bad);
		esp_err();
		return;
	}
	W.mdns = true;
	esp_ok();
}

// == CWHOSTNAME ==

static void host_query(espCmdCall& c) {
	esp_put(c.name + ":" + (sta_on() ? W.hostname : std::string("(null)")) + "\r\n");
	esp_ok();
}

static void host_set(espCmdCall& c) {
	espArgs a(c);
	std::string h;
	if (!a.str(&h, 32) || h.empty() || !a.end() || !sta_on()) {esp_err(); return;}
	W.hostname = h;				// never saved
	esp_ok();
}

// == CWCOUNTRY ==

static void cc_print(espCmdCall& c, int pol, const std::string& code, int st, int n) {
	esp_put(c.name + ":" + std::to_string(pol) + ",\"" + code + "\"," + std::to_string(st) + "," + std::to_string(n) + "\r\n");
	esp_ok();
}

static void cc_query(espCmdCall& c) {cc_print(c, W.ccPolicy, W.ccCode, W.ccStart, W.ccCount);}
static void cc_query_def(espCmdCall& c) {
	cc_print(c, (int)esp_fl_num("cc_policy", 0), esp_fl_get("cc_code", "CN"), (int)esp_fl_num("cc_start", 1),
		(int)esp_fl_num("cc_count", 13));
}

static void cc_setx(espCmdCall& c, bool def) {
	espArgs a(c);
	long pol, st, n;
	std::string code;
	if (!a.num(&pol) || (pol > 1) || !a.comma() || !a.str(&code, 3) || code.empty() || !a.comma()) {esp_err(); return;}
	if (!a.num(&st) || (st < 1) || !a.comma() || !a.num(&n) || (n < 1) || !a.end()) {esp_err(); return;}
	if (st + n - 1 > 14) {esp_err(); return;}	// the SDK's channel range
	W.ccPolicy = (int)pol;
	W.ccCode = code;
	W.ccStart = (int)st;
	W.ccCount = (int)n;
	if (def) {
		esp_fl_set_num("cc_policy", pol);
		esp_fl_set("cc_code", code);
		esp_fl_set_num("cc_start", st);
		esp_fl_set_num("cc_count", n);
	}
	esp_ok();
}

static void cc_set(espCmdCall& c) {cc_setx(c, false);}
static void cc_set_def(espCmdCall& c) {cc_setx(c, true);}

// == the table: the firmware's rows; a bare query is _CUR's, a bare set _DEF's ==

const espCmd esp_wifi_cmds[] = {
	{"+CWMODE", mode_test, mode_query, mode_set_def, NULL},
	{"+CWMODE_DEF", mode_test, mode_query_def, mode_set_def, NULL},
	{"+CWMODE_CUR", mode_test, mode_query, mode_set, NULL},
	{"+CWJAP", NULL, jap_query, jap_set_def, NULL},
	{"+CWJAP_DEF", NULL, jap_query_def, jap_set_def, NULL},
	{"+CWJAP_CUR", NULL, jap_query, jap_set, NULL},
	{"+CWLAP", NULL, NULL, lap_set, lap_exec},
	{"+CWLAPOPT", NULL, NULL, lapopt_set, NULL},
	{"+CWQAP", qap_test, NULL, NULL, qap_exec},
	{"+CWSAP", NULL, sap_query, sap_set_def, NULL},
	{"+CWSAP_DEF", NULL, sap_query_def, sap_set_def, NULL},
	{"+CWSAP_CUR", NULL, sap_query, sap_set, NULL},
	{"+CWLIF", NULL, NULL, NULL, lif_exec},
	{"+CWDHCP", NULL, dhcp_query, dhcp_set_def, NULL},
	{"+CWDHCP_DEF", NULL, dhcp_query_def, dhcp_set_def, NULL},
	{"+CWDHCP_CUR", NULL, dhcp_query, dhcp_set, NULL},
	{"+CWDHCPS_DEF", NULL, dhcps_query_def, dhcps_set_def, NULL},
	{"+CWDHCPS_CUR", NULL, dhcps_query, dhcps_set, NULL},
	{"+CWAUTOCONN", NULL, auto_query, auto_set, NULL},
	{"+CWHOSTNAME", NULL, host_query, host_set, NULL},
	{"+CWCOUNTRY_CUR", NULL, cc_query, cc_set, NULL},
	{"+CWCOUNTRY_DEF", NULL, cc_query_def, cc_set_def, NULL},
	{"+WPS", NULL, NULL, wps_set, wps_exec},
	{"+CIPSTAMAC", NULL, stamac_query, stamac_set_def, NULL},
	{"+CIPSTAMAC_DEF", NULL, stamac_query_def, stamac_set_def, NULL},
	{"+CIPSTAMAC_CUR", NULL, stamac_query, stamac_set, NULL},
	{"+CIPAPMAC", NULL, apmac_query, apmac_set_def, NULL},
	{"+CIPAPMAC_DEF", NULL, apmac_query_def, apmac_set_def, NULL},
	{"+CIPAPMAC_CUR", NULL, apmac_query, apmac_set, NULL},
	{"+CIPSTA", NULL, sta_query, sta_set_def, NULL},
	{"+CIPSTA_DEF", NULL, sta_query_def, sta_set_def, NULL},
	{"+CIPSTA_CUR", NULL, sta_query, sta_set, NULL},
	{"+CIPAP", NULL, ap_query, ap_set_def, NULL},
	{"+CIPAP_DEF", NULL, ap_query_def, ap_set_def, NULL},
	{"+CIPAP_CUR", NULL, ap_query, ap_set, NULL},
	{"+MDNS", NULL, NULL, mdns_set, NULL},
	// the firmware's second table
	{"+CWSTARTSMART", NULL, NULL, smart_set, smart_start},
	{"+CWSTOPSMART", NULL, NULL, NULL, smart_stop},
	{"+CWSTARTDISCOVER", NULL, NULL, disc_start, NULL},
	{"+CWSTOPDISCOVER", NULL, NULL, NULL, disc_stop},
	{NULL, NULL, NULL, NULL, NULL}
};

// == the hooks ==

void esp_wifi_boot(Computer* comp) {
	(void)comp;
	W.mode = (int)esp_fl_num("cwmode", 1);
	if ((W.mode < 1) || (W.mode > 3)) W.mode = 1;
	W.autoconn = (esp_fl_num("autoconn", 1) != 0);
	W.ssid = esp_fl_get("ssid", "Xpeccy+");
	W.pass = esp_fl_get("pass");
	W.pci = (int)esp_fl_num("pci", 0);
	W.assoc = W.joined = false;
	W.discFlag = 10;			// not 1: the first disconnect after boot is printed
	W.dhcpSta = (esp_fl_get("dhcp", "yes") == "yes");
	W.dhcpAp = (esp_fl_get("dhcps", "yes") == "yes");
	if (W.dhcpSta) {
		W.ip = W.gw = W.mask = 0;
	} else {
		W.ip = ip_fl("ip", "192.168.1.70");
		W.gw = ip_fl("gw", "192.168.1.1");
		W.mask = ip_fl("mask", "255.255.255.0");
	}
	mac_fl("stamac", "5c:cf:7f:58:50:2b", W.staMac);
	mac_fl("apmac", "5e:cf:7f:58:50:2b", W.apMac);
	char h[16];
	snprintf(h, sizeof(h), "ESP-%02X%02X%02X", W.staMac[3], W.staMac[4], W.staMac[5]);
	W.hostname = h;
	W.jph = J_NONE;
	W.jDiscDue = false;
	W.scanning = false;
	W.lapSort = 0;
	W.lapMask = 0x7ff;
	W.apSsid = esp_fl_get("ap_ssid", ap_ssid_def());
	W.apPass = esp_fl_get("ap_pass");
	W.apCh = (int)esp_fl_num("ap_ch", 1);
	W.apEcn = (int)esp_fl_num("ap_ecn", 0);
	W.apMax = (int)esp_fl_num("ap_max", 4);
	W.apHidden = (int)esp_fl_num("ap_hidden", 0);
	W.apIp = ip_fl("apip", "192.168.4.1");
	W.apGw = ip_fl("apgw", "192.168.4.1");
	W.apMask = ip_fl("apmask", "255.255.255.0");
	W.dhcpsSet = !esp_fl_get("dhcps_start").empty();
	W.dhcpsLease = esp_fl_num("dhcps_lease", 120);
	W.dhcpsStart = ip_fl("dhcps_start", "0.0.0.0");
	W.dhcpsEnd = ip_fl("dhcps_end", "0.0.0.0");
	W.ccPolicy = (int)esp_fl_num("cc_policy", 0);
	W.ccCode = esp_fl_get("cc_code", "CN");
	W.ccStart = (int)esp_fl_num("cc_start", 1);
	W.ccCount = (int)esp_fl_num("cc_count", 13);
	W.smart = false;
	W.wps = false;
	W.mdns = false;
}

void esp_wifi_ready(Computer* comp, long long now) {
	auto_join(comp, now);
}

void esp_wifi_frame(Computer* comp, long long now) {
	join_step(comp, now);
	if (W.scanning && (now >= W.scanAt)) lap_done();
	if (W.wps && (now >= W.wpsAt)) {
		W.wps = false;
		esp_put_now("wps fail\r\n");
	}
}

bool esp_wifi_joined() {return W.joined;}
int esp_wifi_mode() {return W.mode;}
std::string esp_wifi_sta_ip() {return ip_str(W.ip);}
std::string esp_wifi_sta_mac() {return mac_str(W.staMac);}
std::string esp_wifi_ap_ip() {return ip_str(W.apIp);}
std::string esp_wifi_ap_mac() {return mac_str(W.apMac);}

// the DNS server the DHCP lease named: the emulated router hands out itself, its gateway
std::string esp_wifi_dhcp_dns() {
	if (!W.joined || !W.dhcpSta) return "";
	return ip_str(ip_fl("lease_gw", "192.168.1.1"));
}
