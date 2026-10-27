// The ESP's TCP/IP commands: links (CIPSTART, CIPSEND, CIPCLOSE, +IPD), the server, passive
// receive, the send buffer, passthrough, DNS, ping and SNTP. Replies are NONOS AT 1.7.6's, byte
// for byte (the lab's at176-ch5.md). The sockets are the host's (espnet).

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "esp_int.h"
#include "espnet.h"
#include "../libxpeccy/xlog.h"

#define TCP_LINKS	5
#define TCP_SEND_MAX	2048		// one CIPSEND
#define TCP_IPD_MAX	1460		// a segment: the first +IPD of a reply carries its header
#define TCP_OUT_MAX	16384		// queued for the line before the sockets are left to wait
#define TCP_RX_MAX	65536		// read ahead of the line (active receive)
#define TCP_HOLD_MAX	2920		// passive receive: the module's buffer, then TCP waits
#define TCP_PASS_MAX	2920		// passthrough: a packet goes when its buffer is full
#define TCP_CONNECT_MS	10000		// a connect gives up
#define DNS_DEFAULT	"208.67.222.222"

enum {K_TCP = 0, K_UDP, K_SSL};
enum {P_NONE = 0, P_START, P_DOMAIN, P_PING_LOOK, P_PING, P_SEND_LOOK};

struct tcpLink {
	xEspSock sock;
	bool used = false;		// open: CIPSTATUS lists it, CIPSEND takes it
	bool starting = false;		// a connect is under way
	bool silent = false;		// passthrough's own (re)connect: no CONNECT, no OK, retried
	bool counted = false;		// in the module's link count
	bool failNow = false;		// no route out: the start fails at the next frame
	int kind = K_TCP;
	bool server = false;		// a client of our server (tetype 1)
	std::string host;
	int port = 0;
	int lport = 0;			// UDP local port asked for
	int udpMode = 0;
	int keep = 0;			// TCP keep alive, s
	bool abort = false;		// CIPCLOSEMODE: a reset, not a FIN
	std::string rx;			// received: to frame (active) or held for CIPRECVDATA (passive)
	size_t rxSeen = 0;		// how much of rx the last frame end saw: the quiet test
	bool peerGone = false;
	std::string tx;			// to the socket
	int sendOk = 0;			// a CIPSEND of this many bytes waits for its SEND OK
	int mode8 = 0;			// what is sending: 0 none, 1 CIPSEND, 2 CIPSENDEX
	long long lastAct = 0;		// for CIPSTO
	long long deadline = 0;
	long long retryAt = 0;
};

static struct {
	tcpLink link[TCP_LINKS];
	int mux = 0;
	int cipmode = 0;
	bool dinfo = false;
	int recvmode = 0;
	int sslSize = 2048;
	int sslConf = 0;
	int maxConn = 5;
	int sto = 180;
	int status = 5;			// what CIPSTATUS reports: 2..5
	int count = 0;			// links the module counts
	bool server = false;
	int serverPort = 333;
	xEspListen listen;
	xEspSock reject;		// a client over the limit, taken to be closed
	// a command that goes on
	int pend = P_NONE;
	int pendLink = 0;
	xEspResolve res;
	xEspPing ping;
	// CIPSEND's payload
	int sendLink = 0;
	int sendKind = 0;
	size_t sendLen = 0;
	int sendPort = 0;		// a UDP target being looked up
	std::string sendBuf;
	bool sendEsc = false;
	long long waitLast = -1;
	size_t rstAt = 0;
	// passthrough
	bool inPass = false;
	std::string passTx;
	int plus = 0;
	long long passLast = -1;
	// DNS servers (lwIP's slots)
	std::string dns[2];
	bool dnsUser = false;		// CIPDNS set them; else DHCP's

	// SNTP
	std::string ntp[3];
	int tz = 0;
	xEspSock ntpSock;
	bool ntpBusy = false;
	bool ntpSent = false;
	int ntpSrv = 0;
	long long ntpStart = 0;
	long long ntpNext = 0;
	bool synced = false;
	long long unixAt = 0;		// the time an answer gave, s
	long long hostAt = 0;		// the host's ms when it came
} T;

// == helpers ==

// the firmware's parameter readers: a number is decimal digits (10 at most, no sign); a string
// must be "quoted", \ escaping the next byte
struct espArg {
	const std::string& s;
	size_t i = 1;			// the '=' (or the digit after the name) is skipped, as the firmware does
	explicit espArg(const std::string& p) : s(p) {}
	int at() const {return (i < s.size()) ? (unsigned char)s[i] : -1;}
	bool eat(char c) {
		if (at() != c) return false;
		i++;
		return true;
	}
	bool end() const {return at() == '\r';}
	bool num(long long* v) {
		long long x = 0;
		int k = 0;
		while ((k < 10) && (at() >= '0') && (at() <= '9')) {
			x = x * 10 + (at() - '0');
			i++;
			k++;
		}
		if (!k || ((at() >= '0') && (at() <= '9'))) return false;
		*v = x;
		return true;
	}
	// the length taken; 0 and nothing taken for an empty field; -1 not a string or too long
	int str(std::string* o, size_t max) {
		o->clear();
		if ((at() == ',') || (at() == '\r')) return 0;
		if (at() != '"') return -1;
		bool esc = false;
		for (size_t k = i + 1; k < s.size(); k++) {
			char c = s[k];
			if (c == '\r') return -1;
			if (!esc && (c == '\\')) {
				esc = true;
				continue;
			}
			if (!esc && (c == '"')) {
				i = k + 1;
				return (int)o->size();
			}
			if (o->size() >= max) return -1;
			*o += c;
			esc = false;
		}
		return -1;
	}
};

// lwIP's ipaddr_addr: four decimal parts, else "none" (255.255.255.255, as the firmware reads it)
static unsigned ip_val(const std::string& s) {
	unsigned v = 0;
	int parts = 0;
	size_t i = 0;
	while (parts < 4) {
		if ((i >= s.size()) || (s[i] < '0') || (s[i] > '9')) return 0xffffffffu;
		unsigned p = 0;
		while ((i < s.size()) && (s[i] >= '0') && (s[i] <= '9')) {
			p = p * 10 + (s[i] - '0');
			if (p > 255) return 0xffffffffu;
			i++;
		}
		v = (v << 8) | p;
		parts++;
		if (parts < 4) {
			if ((i >= s.size()) || (s[i] != '.')) return 0xffffffffu;
			i++;
		}
	}
	return (i == s.size()) ? v : 0xffffffffu;
}

static std::string ip_text(unsigned v) {
	char b[20];
	snprintf(b, sizeof(b), "%u.%u.%u.%u", (v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255);
	return b;
}

static bool ip_numeric(const std::string& s) {
	return (ip_val(s) != 0xffffffffu) || (s == "255.255.255.255");
}

static std::string lid(int id) {
	return T.mux ? (std::to_string(id) + ",") : std::string();
}

static const char* kind_name(int k) {
	return (k == K_UDP) ? "UDP" : (k == K_SSL) ? "SSL" : "TCP";
}

static std::string peer_ip(tcpLink& l) {
	std::string ip = l.sock.peerIp();
	return ip.empty() ? std::string("0.0.0.0") : ip;
}

static void out_put(const std::string& s) {esp_put(s);}
static void out_now(const std::string& s) {esp_put_now(s);}
typedef void (*tcpOut)(const std::string&);

// CONNECT, or CONNECT FAIL
static std::string conn_msg(int id, bool fail) {
	return lid(id) + (fail ? "CONNECT FAIL\r\n" : "CONNECT\r\n");
}

static void count_up(tcpLink& l) {
	if (l.counted) return;
	l.counted = true;
	T.count++;
}

static void count_down(tcpLink& l) {
	if (!l.counted) return;
	l.counted = false;
	if (T.count > 0) T.count--;
	if (!T.count) T.status = 4;
}

// the slot empty again; passive data stays readable after a close (till the id is used again)
static void link_reset(tcpLink& l, bool keepRx) {
	l.sock.close(l.abort);
	l.used = false;
	l.starting = false;
	l.silent = false;
	l.failNow = false;
	l.server = false;
	l.abort = false;
	if (!keepRx) l.rx.clear();
	l.rxSeen = 0;
	l.peerGone = false;
	l.tx.clear();
	l.sendOk = 0;
	l.mode8 = 0;
	l.keep = 0;
	l.udpMode = 0;
	l.lport = 0;
}

static bool passive(tcpLink& l) {
	return T.recvmode && (l.kind == K_TCP) && !T.cipmode;
}

// what is still held goes out as +IPD (or raw in CIPMODE 1), cut at a segment
static void rx_flush(int id, tcpOut out) {
	tcpLink& l = T.link[id];
	if (passive(l)) return;
	while (!l.rx.empty()) {
		size_t n = (l.rx.size() > TCP_IPD_MAX) ? TCP_IPD_MAX : l.rx.size();
		if (T.cipmode && (id == 0)) {
			out(l.rx.substr(0, n));
		} else {
			std::string h = "\r\n+IPD," + lid(id) + std::to_string(n);
			if (T.dinfo) h += "," + peer_ip(l) + "," + std::to_string(l.sock.peerPort());
			esp_trace(">", h + ":");
			out(h + ":" + l.rx.substr(0, n));
		}
		l.rx.erase(0, n);
	}
	l.rxSeen = 0;
}

static void send_wait_end();

// a link ends: its CLOSED (after everything it held), and SEND FAIL for a send that was waiting
static void link_close(int id, bool print, tcpOut out) {
	tcpLink& l = T.link[id];
	rx_flush(id, out);
	if (print) out(lid(id) + "CLOSED\r\n");
	if (l.sendOk) {
		out("SEND FAIL\r\n");
		send_wait_end();
	}
	count_down(l);
	link_reset(l, passive(l));
}

// == CIPSTART and the connect ==

static void start_open(tcpLink& l) {
	l.sock.open(l.kind == K_UDP, l.host, l.port, (l.kind == K_UDP) ? l.lport : 0, esp_lan());
}

// a dotted address needs no lookup: espnet opens the socket at once, so it is there before the reply
static bool udp_settle(tcpLink& l) {
	return l.sock.poll() == xEspSock::S_OPEN;
}

// UDP mode 1: the peer becomes the first sender heard; mode 2: every sender
static void udp_follow(tcpLink& l) {
	if (!l.udpMode) return;
	l.sock.setPeer(l.sock.fromIp(), l.sock.fromPort());
	if (l.udpMode == 1) l.udpMode = 0;
}

// passthrough's link is kept up after the far end went
static void pass_reconnect(Computer* comp, int id, long long now) {
	tcpLink& l = T.link[id];
	l.sock.close();
	l.used = false;
	l.peerGone = false;
	l.rxSeen = 0;
	l.tx.clear();
	l.starting = true;
	l.silent = true;
	l.retryAt = now + esp_ms(comp, 1000);
	l.deadline = 0;
}

static void start_fail_async(int id) {
	tcpLink& l = T.link[id];
	esp_put_now("\r\nERROR\r\n");
	esp_put_now(lid(id) + "CLOSED\r\n");
	count_down(l);
	link_reset(l, false);
	T.pend = P_NONE;
	esp_done();
}

static void start_poll(Computer* comp, int id, long long now) {
	tcpLink& l = T.link[id];
	if (l.silent) {
		if (!esp_wifi_joined()) return;
		if (l.sock.state() == xEspSock::S_NONE || l.sock.state() == xEspSock::S_CLOSED) {
			if (now < l.retryAt) return;
			start_open(l);
		}
		int st = l.sock.poll();
		if ((st == xEspSock::S_OPEN) && (l.kind != K_SSL)) {
			l.starting = false;
			l.silent = false;
			l.used = true;
			l.lastAct = now;
			count_up(l);
			if (l.kind != K_UDP) T.status = 3;
		} else if ((st == xEspSock::S_FAILED) || (st == xEspSock::S_OPEN)) {
			l.sock.close();
			l.retryAt = now + esp_ms(comp, 1000);
		}
		return;
	}
	if (l.failNow) {			// no way out: a name is not found, an address not reached
		if (ip_numeric(l.host)) {
			if (l.kind != K_UDP) count_up(l);
			start_fail_async(id);
		} else {
			esp_put_now("DNS Fail\r\n\r\nERROR\r\n");
			link_reset(l, false);
			T.pend = P_NONE;
			esp_done();
		}
		return;
	}
	int st = l.sock.poll();
	if ((l.kind != K_UDP) && ((st == xEspSock::S_CONNECTING) || (st == xEspSock::S_OPEN))) count_up(l);
	if (st == xEspSock::S_OPEN) {
		if (l.kind == K_SSL) {
			// no TLS here: the handshake fails, as the module's does against a peer it cannot talk to
			xlog(XLG_NET, XLL_INFO, "esp: link %i SSL to %s:%i, no TLS: the handshake fails", id, l.host.c_str(), l.port);
			start_fail_async(id);
			return;
		}
		l.starting = false;
		l.used = true;
		l.lastAct = now;
		if (l.kind == K_UDP) count_up(l);
		else T.status = 3;
		esp_put_now(conn_msg(id, false));
		esp_put_now("\r\nOK\r\n");
		T.pend = P_NONE;
		esp_done();
		xlog(XLG_NET, XLL_INFO, "esp: link %i %s to %s:%i open", id, kind_name(l.kind), l.host.c_str(), l.port);
	} else if ((st == xEspSock::S_FAILED) || (now >= l.deadline)) {
		// a name still being looked up at the deadline is the module's DNS timeout
		bool dns = (st == xEspSock::S_RESOLVING) || ((st == xEspSock::S_FAILED) && (l.sock.failure() == xEspSock::F_DNS));
		xlog(XLG_NET, XLL_INFO, "esp: %s:%i %s", l.host.c_str(), l.port, dns ? "not found" : "refused");
		if (dns) {
			esp_put_now("DNS Fail\r\n\r\nERROR\r\n");
			link_reset(l, false);
			T.pend = P_NONE;
			esp_done();
		} else if (l.kind == K_UDP) {
			esp_put_now(conn_msg(id, true));
			esp_put_now("\r\nERROR\r\n");
			link_reset(l, false);
			T.pend = P_NONE;
			esp_done();
		} else {
			count_up(l);
			start_fail_async(id);
		}
	}
}

static void cipstart_set(espCmdCall& c) {
	espArg a(c.p);
	if ((esp_wifi_mode() == 1) && !esp_wifi_joined()) {esp_put("no ip\r\n"); esp_err(); return;}
	long long v;
	int id = 0;
	if (T.mux) {
		if (!a.num(&v) || !a.eat(',')) {esp_err(); return;}
		if (v >= TCP_LINKS) {esp_put("ID ERROR\r\n"); esp_err(); return;}
		id = (int)v;
	}
	std::string type, host;
	if (a.str(&type, 6) < 1) {esp_put("Link type ERROR\r\n"); esp_err(); return;}
	int kind;
	if (type == "TCP") kind = K_TCP;
	else if (type == "UDP") kind = K_UDP;
	else if (type == "SSL") kind = K_SSL;
	else {esp_put("Link type ERROR\r\n"); esp_err(); return;}
	if (!a.eat(',')) {esp_err(); return;}
	if (a.str(&host, 64) < 1) {esp_put("IP ERROR\r\n"); esp_err(); return;}
	if (!a.eat(',')) {esp_put("ENTRY ERROR\r\n"); esp_err(); return;}
	long long port = 0, lport = 0, mode = 0, keep = 0;
	if (!a.num(&port) || (port > 65535)) {esp_err(); return;}
	if (kind == K_UDP) {
		if (a.eat(',')) {
			if (!a.num(&lport)) lport = 0;		// empty: any port
			if ((a.at() != ',') && !a.end()) {esp_err(); return;}
			if (lport > 65535) {esp_err(); return;}
			if (a.eat(',')) {
				if (!a.num(&mode) || !a.end() || (mode > 2)) {esp_err(); return;}
			}
		} else {
			if (!a.end()) {esp_err(); return;}
			if (!port) {esp_put("Miss param\r\n"); esp_err(); return;}
		}
	} else if (a.eat(',')) {
		if (!a.num(&keep) || (keep > 7200)) {esp_err(); return;}
	}
	if (!a.end()) {esp_err(); return;}
	tcpLink& l = T.link[id];
	if (l.used || l.starting) {esp_put("ALREADY CONNECTED\r\n"); esp_err(); return;}
	if (kind == K_SSL) {
		for (int i = 0; i < TCP_LINKS; i++)
			if ((T.link[i].used || T.link[i].starting) && (T.link[i].kind == K_SSL)) {esp_put("ssl reach max\r\n"); esp_err(); return;}
	}
	if ((kind == K_UDP) && T.cipmode && mode) {esp_err(); return;}
	link_reset(l, false);
	l.kind = kind;
	l.host = host;
	l.port = (int)port;
	l.lport = (int)lport;
	l.udpMode = (int)mode;
	l.keep = (int)keep;
	l.starting = true;
	l.deadline = esp_now(c.comp) + esp_ms(c.comp, TCP_CONNECT_MS);
	// in the SoftAP modes the module tries without a station IP; it gets nowhere
	if (!esp_wifi_joined()) l.failNow = true;
	else start_open(l);
	xlog(XLG_NET, XLL_INFO, "esp: link %i %s to %s:%i", id, type.c_str(), host.c_str(), (int)port);
	// UDP to an address is only a socket made: the module answers at once, within the line's
	// first byte time (penguin reads it with an 8 ms limit), not at the next frame
	if ((kind == K_UDP) && !l.failNow && ip_numeric(host) && udp_settle(l)) {
		l.starting = false;
		l.used = true;
		l.lastAct = esp_now(c.comp);
		count_up(l);
		esp_put(conn_msg(id, false));
		esp_ok();
		return;
	}
	T.pend = P_START;
	T.pendLink = id;
	esp_hold();
}

// == CIPSTATUS, CIFSR ==

static void ok_form(espCmdCall& c) {
	(void)c;
	esp_ok();
}

static void cipstatus_exec(espCmdCall& c) {
	(void)c;
	if (!esp_wifi_joined()) T.status = 5;
	else if (T.status == 5) T.status = 2;
	std::string r = "STATUS:" + std::to_string(T.status) + "\r\n";
	for (int i = 0; i < TCP_LINKS; i++) {
		tcpLink& l = T.link[i];
		if (!l.used) continue;
		char b[128];
		snprintf(b, sizeof(b), "+CIPSTATUS:%i,\"%s\",\"%s\",%i,%i,%i\r\n", i, kind_name(l.kind), peer_ip(l).c_str(),
			l.sock.peerPort(), l.sock.localPort(), l.server ? 1 : 0);
		r += b;
	}
	esp_put(r);
	esp_ok();
}

static void cifsr_exec(espCmdCall& c) {
	(void)c;
	int m = esp_wifi_mode();
	std::string r;
	if ((m == 2) || (m == 3)) r += "+CIFSR:APIP,\"" + esp_wifi_ap_ip() + "\"\r\n+CIFSR:APMAC,\"" + esp_wifi_ap_mac() + "\"\r\n";
	if ((m == 1) || (m == 3)) {
		r += "+CIFSR:STAIP,\"" + (esp_wifi_joined() ? esp_wifi_sta_ip() : std::string("0.0.0.0")) + "\"\r\n";
		r += "+CIFSR:STAMAC,\"" + esp_wifi_sta_mac() + "\"\r\n";
	}
	esp_put(r);
	esp_ok();
}

// == CIPDOMAIN ==

static void cipdomain_set(espCmdCall& c) {
	espArg a(c.p);
	std::string h;
	if (a.str(&h, 63) < 1) {esp_put("IP ERROR\r\n"); esp_err(); return;}
	if (!a.end()) {esp_err(); return;}
	if (ip_numeric(h)) {
		esp_put("+CIPDOMAIN:" + ip_text(ip_val(h)) + "\r\n");
		esp_ok();
		return;
	}
	if (!esp_wifi_joined()) {esp_put("DNS Fail\r\n"); esp_err(); return;}
	T.res.start(h);
	T.pend = P_DOMAIN;
	esp_hold();
}

// == CIPSSLSIZE, CIPSSLCCONF ==

static bool ssl_link_open() {
	for (int i = 0; i < TCP_LINKS; i++)
		if ((T.link[i].used || T.link[i].starting) && (T.link[i].kind == K_SSL)) return true;
	return false;
}

static void sslsize_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(T.sslSize) + "\r\n");
	esp_ok();
}

static void sslsize_set(espCmdCall& c) {
	if (ssl_link_open()) {esp_put("must close ssl link\r\n"); esp_err(); return;}
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || !a.end() || (v < 1) || (v > 8192)) {esp_err(); return;}
	T.sslSize = (int)v;
	esp_ok();
}

static void sslcconf_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(T.sslConf) + "\r\n");
	esp_ok();
}

static void sslcconf_set(espCmdCall& c) {
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || !a.end() || (v & ~3LL)) {esp_err(); return;}
	T.sslConf = (int)v;		// RAM only: 1.7.6 does not keep it
	esp_ok();
}

// == CIPSEND and its kin ==

static void wait_hook(Computer* comp, int c, long long end);

static void send_wait_end() {
	esp_release_input();
}

// the payload is in: CIPSEND's Recv, then the bytes go to the link
static void send_done(Computer* comp, long long end) {
	esp_release_input();
	tcpLink& l = T.link[T.sendLink];
	int n = (int)T.sendBuf.size();
	esp_put("\r\nRecv " + std::to_string(n) + " bytes\r\n");
	if (!l.used || (l.sock.state() != xEspSock::S_OPEN)) {
		esp_put("SEND FAIL\r\n");
		l.mode8 = 0;
		T.sendBuf.clear();
		return;
	}
	l.tx += T.sendBuf;
	l.lastAct = end;
	T.sendBuf.clear();
	// a datagram is out as soon as the socket takes it: SEND OK at once, as the module's sent
	// callback (TCP's waits for the far end, which the frame stands for)
	if ((l.kind == K_UDP) && (l.sock.state() == xEspSock::S_OPEN)) {
		int k = l.sock.send((const unsigned char*)l.tx.data(), (int)l.tx.size());
		if (k == (int)l.tx.size()) {
			l.tx.clear();
			l.mode8 = 0;
			esp_put("\r\nSEND OK\r\n");
			return;
		}
		if (k > 0) l.tx.erase(0, k);
	}
	l.sendOk = n;
	T.waitLast = -1;
	T.rstAt = 0;
	esp_take_input(wait_hook);
	(void)comp;
}

static void send_hook(Computer* comp, int c, long long end) {
	std::string& b = T.sendBuf;
	bool done = false;
	if ((T.sendKind == 2) && (c == '\\')) {		// CIPSENDEX: \\ is one backslash, \0 ends the data
		if (T.sendEsc) {
			T.sendEsc = false;
		} else {
			b += '\\';
			T.sendEsc = true;
		}
	} else if ((T.sendKind == 2) && (c == '0') && T.sendEsc) {
		b.erase(b.size() - 1);
		T.sendEsc = false;
		done = true;
	} else {
		b += (char)c;
		T.sendEsc = false;
	}
	if (b.size() >= T.sendLen) done = true;
	if (done) send_done(comp, end);
}

// between the payload and SEND OK: bytes are echoed, a burst gets busy s..., AT+RST still restarts
static void wait_hook(Computer* comp, int c, long long end) {
	if (esp_echo()) esp_put(std::string(1, (char)c));
	static const char rst[] = "AT+RST\r\n";
	if (c == rst[T.rstAt]) T.rstAt++;
	else T.rstAt = (c == 'A') ? 1 : 0;
	if (T.rstAt == 8) {
		esp_put("\r\nWill force to restart!!!\r\n");
		esp_ok();
		esp_reboot(comp, end + esp_ms(comp, 2));
		return;
	}
	if ((T.waitLast < 0) || (end - T.waitLast > esp_ms(comp, 1))) esp_put("\r\nbusy s...\r\n");
	T.waitLast = end;
}

// CIPSEND=, CIPSENDEX=: one handler, as in the firmware (kind 0, 2)
static void send_set(espCmdCall& c, int kind) {
	if (T.cipmode) {esp_put("IPMODE=1\r\n"); esp_err(); return;}
	if (c.p.empty() || (c.p[0] != '=')) {esp_err(); return;}
	espArg a(c.p);
	long long v;
	int id = 0;
	if (T.mux) {
		if (!a.num(&v) || (v >= TCP_LINKS) || !a.eat(',')) {esp_err(); return;}
		id = (int)v;
	}
	tcpLink& l = T.link[id];
	if (!l.used) {esp_put("link is not valid\r\n"); esp_err(); return;}
	if ((l.mode8 != kind) && l.mode8) {esp_put("send mode error\r\n"); esp_err(); return;}
	long long n = 0;
	if (!a.num(&n)) n = 0;			// not a number: caught by no tail
	if (n > TCP_SEND_MAX) {esp_put("too long\r\n"); esp_err(); return;}
	std::string tip;
	long long tport = 0;
	bool target = false;
	if ((l.kind == K_UDP) && (a.at() == ',')) {
		a.i++;
		if (a.str(&tip, 64) < 1) {esp_put("ip error\r\n"); esp_err(); return;}
		if (!a.eat(',')) {esp_put("no port\r\n"); esp_err(); return;}
		if (!a.num(&tport)) tport = 0;
		if (a.at() == ',') {esp_put("port error\r\n"); esp_err(); return;}
		if ((tport < 1) || (tport > 65535)) {esp_put("port range error\r\n"); esp_err(); return;}
		target = true;
	}
	if (!a.end()) {esp_put("no tail\r\n"); esp_err(); return;}
	if (n < 1) {esp_err(); return;}
	T.sendLink = id;
	T.sendKind = kind;
	T.sendLen = (size_t)n;
	T.sendBuf.clear();
	T.sendEsc = false;
	l.mode8 = kind ? kind : 1;
	// a datagram elsewhere: the link's peer becomes that one, for later sends too; a name is
	// looked up first, and the prompt comes when it is found
	if (target && !l.sock.setPeer(tip, (int)tport)) {
		T.sendPort = (int)tport;
		T.res.start(tip);
		T.pend = P_SEND_LOOK;
		esp_hold();
		return;
	}
	esp_ok();
	esp_put("> ");
	esp_take_input(send_hook);		// from here, before the > is out: NXtel sends at once
}

static void cipsend_set(espCmdCall& c) {send_set(c, 0);}
static void cipsendex_set(espCmdCall& c) {send_set(c, 2);}

// == passthrough ==

static void pass_hook(Computer* comp, int c, long long end) {
	long long guard = esp_ms(comp, 20);
	long long gap = (T.passLast < 0) ? guard : (end - T.passLast);
	T.passLast = end;
	if ((c == '+') && (T.plus < 3) && ((T.plus > 0) || (gap >= guard))) {
		T.plus++;			// perhaps the escape: held till the guard after it
		return;
	}
	if (T.plus) {
		T.passTx.append(T.plus, '+');
		T.plus = 0;
	}
	T.passTx += (char)c;
}

static void pass_enter() {
	T.inPass = true;
	T.passTx.clear();
	T.plus = 0;
	T.passLast = -1;
	esp_take_input(pass_hook);
}

bool esp_tcp_passthrough() {
	return T.inPass;
}

static void cipsend_exec(espCmdCall& c) {
	(void)c;
	if (T.server || !T.cipmode || !T.link[0].used) {esp_err(); return;}
	esp_ok();
	esp_put("\r\n>");
	pass_enter();
}

static void pass_frame(Computer* comp, long long now) {
	if (!T.inPass) return;
	long long guard = esp_ms(comp, 20);
	bool quiet = (T.passLast < 0) || ((now - T.passLast) >= guard);
	if ((T.plus == 3) && quiet) {
		T.plus = 0;
		T.inPass = false;
		esp_release_input();
		xlog(XLG_NET, XLL_DEBUG, "esp: passthrough ends");
	} else if (T.plus && quiet) {
		T.passTx.append(T.plus, '+');
		T.plus = 0;
	}
	if (!T.passTx.empty() && (quiet || (T.passTx.size() >= TCP_PASS_MAX))) {
		tcpLink& l = T.link[0];
		if (l.used) l.tx += T.passTx;	// while it reconnects the bytes are lost
		T.passTx.clear();
	}
}

// == CIPCLOSE, CIPCLOSEMODE ==

static void cipclose_set(espCmdCall& c) {
	if (!T.mux) {esp_put("MUX=0\r\n"); esp_err(); return;}
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || (a.at() == ',') || (v > TCP_LINKS)) {esp_err(); return;}
	if (v == TCP_LINKS) {
		if (!T.count) {esp_ok(); return;}
		for (int i = 0; i < TCP_LINKS; i++)
			if (T.link[i].used) link_close(i, true, out_put);
		esp_ok();
		return;
	}
	int id = (int)v;
	if (!T.link[id].used) {esp_put("UNLINK\r\n"); esp_err(); return;}
	link_close(id, true, out_put);
	esp_ok();
}

static void cipclose_exec(espCmdCall& c) {
	(void)c;
	if (T.mux) {esp_put("MUX=1\r\n"); esp_err(); return;}
	if (!T.link[0].used) {esp_err(); return;}
	if (T.server) {esp_put("we must restart\r\n"); esp_ok(); return;}
	link_close(0, true, out_put);
	esp_ok();
}

static void cipclosemode_set(espCmdCall& c) {
	espArg a(c.p);
	long long v, ab;
	int id = 0;
	if (T.mux) {
		if (!a.num(&v) || !a.eat(',') || (v >= TCP_LINKS)) {esp_err(); return;}
		id = (int)v;
	}
	if (!a.num(&ab) || !a.end() || (ab > 1)) {esp_err(); return;}
	tcpLink& l = T.link[id];
	if (!l.used || (l.kind != K_TCP)) {esp_err(); return;}
	l.abort = (ab == 1);
	esp_ok();
}

// == CIPMUX, CIPMODE ==

static void cipmux_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(T.mux) + "\r\n");
	esp_ok();
}

static void cipmux_set(espCmdCall& c) {
	if (T.status == 3) {esp_put("link is builded\r\n"); esp_err(); return;}
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || !a.end()) {esp_err(); return;}
	if (v == 1) {
		if (T.cipmode) {esp_put("IPMODE must be 0\r\n"); esp_err(); return;}
		T.mux = 1;
	} else if (v == 0) {
		if (T.server) {esp_put("CIPSERVER must be 0\r\n"); esp_err(); return;}
		if (T.mux && T.count) {esp_put("Connection exists\r\n"); esp_err(); return;}
		T.mux = 0;
	} else {
		esp_err();
		return;
	}
	esp_ok();
}

static void cipmode_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(T.cipmode) + "\r\n");
	esp_ok();
}

static void cipmode_set(espCmdCall& c) {
	if (T.mux || T.server) {esp_put("CIPMUX and CIPSERVER must be 0\r\n"); esp_err(); return;}
	tcpLink& l = T.link[0];
	if (l.used && l.mode8) {esp_err(); return;}
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || !a.end() || (v > 1)) {esp_err(); return;}
	if (l.used && (l.kind == K_UDP) && l.udpMode && v) {esp_err(); return;}
	T.cipmode = (int)v;
	esp_ok();
}

// == the server ==

static void server_drop_clients(tcpOut out) {
	for (int i = 0; i < TCP_LINKS; i++)
		if (T.link[i].used && T.link[i].server) link_close(i, true, out);
}

static void cipserver_set(espCmdCall& c) {
	if (!T.mux) {esp_err(); return;}
	espArg a(c.p);
	long long mode, port = 333;
	if (!a.num(&mode)) {esp_err(); return;}
	if (mode == 0) {
		if (!a.end()) {esp_err(); return;}
	} else if (mode == 1) {
		if (a.eat(',')) {
			if (!a.num(&port) || !a.end() || (port < 1) || (port > 65535)) {esp_err(); return;}
		} else if (!a.end()) {
			esp_err();
			return;
		}
	} else {
		esp_err();
		return;
	}
	if ((mode == 1) == T.server) {esp_put("no change\r\n"); esp_ok(); return;}
	if (mode == 1) {
		T.server = true;
		T.serverPort = (int)port;
		const char* where = esp_lan() ? "every interface" : "127.0.0.1";
		if (!T.listen.open((int)port, esp_lan()))
			xlog(XLG_NET, XLL_WARN, "esp: server port %i cannot be had on %s; the module still answers OK", (int)port, where);
		else
			xlog(XLG_NET, XLL_INFO, "esp: server on %s, port %i", where, (int)port);
		esp_ok();
	} else {
		T.server = false;
		T.listen.close();
		esp_ok();
		server_drop_clients(out_put);	// their CLOSED come after the OK, from the disconnects
	}
}

static void maxconn_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(T.maxConn));	// no line end of its own
	esp_ok();
}

static void maxconn_set(espCmdCall& c) {
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || (v < 1) || (v > TCP_LINKS) || !a.end() || T.server) {esp_err(); return;}
	T.maxConn = (int)v;
	esp_ok();
}

static void cipsto_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(T.sto) + "\r\n");
	esp_ok();
}

static void cipsto_set(espCmdCall& c) {
	if (!T.server) {esp_err(); return;}
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || !a.end() || (v > 7200)) {esp_err(); return;}
	T.sto = (int)v;
	esp_ok();
}

static void server_frame(Computer* comp, long long now) {
	if (!T.server || !T.listen.isOpen()) return;
	for (int guard = 0; guard < 8; guard++) {
		int clients = 0, id = -1;
		for (int i = 0; i < TCP_LINKS; i++) {
			if (T.link[i].used && T.link[i].server) clients++;
			if ((id < 0) && !T.link[i].used && !T.link[i].starting) id = i;
		}
		if ((id < 0) || (clients >= T.maxConn)) {
			if (!T.listen.accept(T.reject)) return;
			T.reject.close();		// over the limit: dropped, nothing said
			continue;
		}
		tcpLink& l = T.link[id];
		link_reset(l, false);
		if (!T.listen.accept(l.sock)) return;
		l.used = true;
		l.server = true;
		l.kind = K_TCP;
		l.host = l.sock.peerIp();
		l.port = l.sock.peerPort();
		l.lastAct = now;
		count_up(l);
		T.status = 3;
		esp_put_now(conn_msg(id, false));
		xlog(XLG_NET, XLL_INFO, "esp: server: client %i from %s:%i", id, l.host.c_str(), l.port);
	}
	(void)comp;
}

// == PING ==

static void ping_set(espCmdCall& c) {
	espArg a(c.p);
	std::string h;
	if ((a.str(&h, 128) < 1) || !a.end()) {esp_err(); return;}
	if (!esp_wifi_joined()) {esp_err(); return;}
	T.res.start(h);
	T.pend = P_PING_LOOK;
	esp_hold();
}

// == CIPDINFO, passive receive ==

static void cipdinfo_query(espCmdCall& c) {
	esp_put(c.name + ":" + (T.dinfo ? "TRUE" : "FALSE"));	// no line end of its own
	esp_ok();
}

static void cipdinfo_set(espCmdCall& c) {
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || !a.end() || (v > 1)) {esp_err(); return;}
	T.dinfo = (v == 1);
	esp_ok();
}

static void recvmode_query(espCmdCall& c) {
	esp_put(c.name + ":" + std::to_string(T.recvmode) + "\r\n");
	esp_ok();
}

static void recvmode_set(espCmdCall& c) {
	espArg a(c.p);
	long long v;
	if (!a.num(&v) || !a.end() || (v > 1)) {esp_err(); return;}
	T.recvmode = (int)v;
	esp_ok();
}

static void recvdata_set(espCmdCall& c) {
	espArg a(c.p);
	long long v, n;
	int id = 0;
	if (T.mux) {
		if (!a.num(&v) || (v >= TCP_LINKS)) {esp_err(); return;}
		id = (int)v;
		a.i++;				// the firmware steps over the next byte, whatever it is
	}
	if (!a.num(&n) || !a.end() || (n < 1) || (n > TCP_SEND_MAX)) {esp_err(); return;}
	tcpLink& l = T.link[id];
	if (l.used && (l.kind != K_TCP)) {esp_err(); return;}
	if (!l.used && l.rx.empty()) {esp_err(); return;}
	size_t k = ((size_t)n < l.rx.size()) ? (size_t)n : l.rx.size();
	if (k) {
		esp_put("\r\n" + c.name + "," + std::to_string(k) + ":" + l.rx.substr(0, k));
		l.rx.erase(0, k);
		l.rxSeen = 0;
	}
	esp_ok();
}

static void recvlen_query(espCmdCall& c) {
	std::string r = c.name + ":";
	for (int i = 0; i < TCP_LINKS; i++) {
		tcpLink& l = T.link[i];
		size_t n = (l.used && (l.kind != K_TCP)) ? 0 : l.rx.size();
		if (!passive(l) && l.used) n = 0;
		if (i) r += ",";
		r += std::to_string(n);
	}
	esp_put(r + "\r\n");
	esp_ok();
}

// == SNTP ==

static std::string asctime_of(long long t) {
	static const char* wd[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
	static const char* mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	if (t < 0) t = 0;
	long long days = t / 86400, s = t % 86400;
	// days since 1970 to a date (the proleptic Gregorian calendar)
	long long z = days + 719468;
	long long era = z / 146097;
	long long doe = z - era * 146097;
	long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long long y = yoe + era * 400;
	long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long long mp = (5 * doy + 2) / 153;
	long long d = doy - (153 * mp + 2) / 5 + 1;
	long long m = mp + ((mp < 10) ? 3 : -9);
	if (m <= 2) y++;
	char b[64];
	snprintf(b, sizeof(b), "%s %s %02i %02i:%02i:%02i %02i", wd[days % 7], mn[m - 1], (int)d, (int)(s / 3600),
		(int)(s / 60 % 60), (int)(s % 60), (int)y);
	return b;
}

static bool ntp_on() {
	return !T.ntp[0].empty() || !T.ntp[1].empty() || !T.ntp[2].empty();
}

static void sntpcfg_query(espCmdCall& c) {
	std::string r;
	if (!ntp_on()) {
		r = c.name + ":0\r\n";
	} else {
		r = c.name + ":1," + std::to_string(T.tz);
		for (int i = 0; i < 3; i++)
			if (!T.ntp[i].empty()) r += ",\"" + T.ntp[i] + "\"";
		r += "\r\n";
	}
	esp_put(r);
	esp_ok();
}

static void sntpcfg_set(espCmdCall& c) {
	espArg a(c.p);
	long long en, tz = 0;
	if (!a.num(&en)) {esp_err(); return;}
	std::string s[3];
	if (en == 1) {
		if (!a.eat(',')) {esp_err(); return;}
		bool neg = a.eat('-');
		if (!a.num(&tz)) {esp_err(); return;}
		if (neg) tz = -tz;
		if ((tz < -11) || (tz > 13)) {esp_err(); return;}
		if (a.eat(',')) {
			for (int i = 0; i < 3; i++) {
				if (a.str(&s[i], 32) < 1) {esp_err(); return;}
				if (!a.eat(',')) break;
			}
		} else {
			s[0] = "cn.ntp.org.cn";
			s[1] = "ntp.sjtu.edu.cn";
			s[2] = "us.pool.ntp.org";
		}
	} else if (en != 0) {
		esp_err();
		return;
	}
	if (!a.end()) {esp_err(); return;}
	for (int i = 0; i < 3; i++) T.ntp[i] = s[i];
	T.tz = (int)tz;
	T.synced = false;
	T.ntpSock.close();
	T.ntpBusy = false;
	T.ntpNext = 0;
	esp_ok();
}

static void sntptime_query(espCmdCall& c) {
	long long t = 0;
	if (T.synced) t = T.unixAt + (esp_host_ms() - T.hostAt) / 1000 + (long long)T.tz * 3600;
	esp_put(c.name + ":" + asctime_of(t));	// its \n is cut: OK's own \r\n ends the line
	esp_ok();
}

// one query at a time, the servers in turn; once answered, again in an hour
static void sntp_frame() {
	if (!ntp_on() || !esp_wifi_joined()) {
		if (T.ntpBusy) T.ntpSock.close();
		T.ntpBusy = false;
		return;
	}
	long long ms = esp_host_ms();
	if (!T.ntpBusy) {
		if (ms < T.ntpNext) return;
		for (int k = 0; (k < 3) && T.ntp[T.ntpSrv % 3].empty(); k++) T.ntpSrv++;
		T.ntpSock.open(true, T.ntp[T.ntpSrv % 3], 123);
		T.ntpBusy = true;
		T.ntpSent = false;
		T.ntpStart = ms;
		return;
	}
	int st = T.ntpSock.poll();
	bool fail = (st == xEspSock::S_FAILED) || (ms - T.ntpStart > 3000);
	if ((st == xEspSock::S_OPEN) && !fail) {
		if (!T.ntpSent) {
			unsigned char q[48];
			memset(q, 0, sizeof(q));
			q[0] = 0x1b;		// version 3, client
			T.ntpSent = (T.ntpSock.send(q, sizeof(q)) == (int)sizeof(q));
			return;
		}
		unsigned char r[64];
		int n = T.ntpSock.recv(r, sizeof(r));
		if (n >= 48) {
			unsigned long long secs = ((unsigned long long)r[40] << 24) | (r[41] << 16) | (r[42] << 8) | r[43];
			T.unixAt = (long long)secs - 2208988800LL;
			T.hostAt = ms;
			T.synced = true;
			T.ntpSock.close();
			T.ntpBusy = false;
			T.ntpNext = ms + 3600000;
			xlog(XLG_NET, XLL_INFO, "esp: SNTP time from %s", T.ntp[T.ntpSrv % 3].c_str());
		}
		return;
	}
	if (fail) {
		T.ntpSock.close();
		T.ntpBusy = false;
		T.ntpSrv++;
		T.ntpNext = ms + 1000;
	}
}

// == CIPDNS_CUR, CIPDNS_DEF ==

static void dns_lines(const std::string& name, const std::string& a, const std::string& b) {
	std::string r;
	if (!a.empty() && (a != "0.0.0.0")) r += name + ":" + a + "\r\n";
	if (!b.empty() && (b != "0.0.0.0") && (b != a)) r += name + ":" + b + "\r\n";
	esp_put(r);
	esp_ok();
}

// lwIP's slot 0 is the server DHCP handed out (OpenDNS before any), unless CIPDNS set its own
static void dnscur_query(espCmdCall& c) {
	if (T.dnsUser) {
		dns_lines(c.name, T.dns[0], T.dns[1]);
		return;
	}
	std::string d = esp_wifi_dhcp_dns();
	dns_lines(c.name, d.empty() ? std::string(DNS_DEFAULT) : d, "");
}

static void dnsdef_query(espCmdCall& c) {
	if (esp_fl_get("dns") != "yes") dns_lines(c.name, "", "");
	else dns_lines(c.name, esp_fl_get("dns1"), esp_fl_get("dns2"));
}

static void dns_set(espCmdCall& c, bool def) {
	espArg a(c.p);
	long long en;
	if (!a.num(&en) || (en > 1)) {esp_err(); return;}
	std::string d1 = DNS_DEFAULT, d2;
	if (en == 1) {
		if (a.eat(',')) {
			std::string s;
			if (a.str(&s, 64) < 1) {esp_put("IP1 ERROR\r\n"); esp_err(); return;}
			unsigned v1 = ip_val(s);
			if (!v1) {esp_put("IP1 invalid\r\n"); esp_err(); return;}
			d1 = d2 = ip_text(v1);
			if (a.eat(',')) {
				if (a.str(&s, 64) < 1) {esp_put("IP2 ERROR\r\n"); esp_err(); return;}
				unsigned v2 = ip_val(s);
				if (!v2) {esp_put("IP2 invalid\r\n"); esp_err(); return;}
				if (v2 == v1) {esp_err(); return;}
				d2 = ip_text(v2);
			}
		} else {
			d2 = d1;
		}
	}
	T.dns[0] = d1;
	T.dns[1] = d2;
	T.dnsUser = (en == 1);
	if (!a.end()) {esp_err(); return;}
	if (def) {
		esp_fl_set("dns", en ? "yes" : "no");
		esp_fl_set("dns1", en ? d1 : std::string());
		esp_fl_set("dns2", en ? d2 : std::string());
	}
	esp_ok();
}

static void dnscur_set(espCmdCall& c) {dns_set(c, false);}
static void dnsdef_set(espCmdCall& c) {dns_set(c, true);}

// == the frame ==

static void pend_frame() {
	if (T.pend == P_DOMAIN) {
		int r = T.res.poll();
		if (!r) return;
		if (r > 0) esp_put_now("+CIPDOMAIN:" + T.res.ip() + "\r\n\r\nOK\r\n");
		else esp_put_now("DNS Fail\r\n\r\nERROR\r\n");
		T.pend = P_NONE;
		esp_done();
	} else if (T.pend == P_SEND_LOOK) {
		int r = T.res.poll();
		if (!r) return;
		tcpLink& l = T.link[T.sendLink];
		T.pend = P_NONE;
		esp_done();
		if ((r < 0) || !l.used || !l.sock.setPeer(T.res.ip(), T.sendPort)) {
			l.mode8 = 0;
			esp_put_now("\r\nERROR\r\n");
			return;
		}
		esp_put_now("\r\nOK\r\n> ");
		esp_take_input(send_hook);
	} else if (T.pend == P_PING_LOOK) {
		int r = T.res.poll();
		if (!r) return;
		if (r < 0) {
			esp_put_now("\r\nERROR\r\n");
			T.pend = P_NONE;
			esp_done();
			return;
		}
		T.ping.start(T.res.ip(), 1000);
		T.pend = P_PING;
	} else if (T.pend == P_PING) {
		int r = T.ping.poll();
		if (!r) return;
		if (r > 0) esp_put_now("+" + std::to_string(T.ping.ms()) + "\r\n\r\nOK\r\n");
		else esp_put_now("+timeout\r\n\r\nERROR\r\n");
		T.pend = P_NONE;
		esp_done();
	}
}

static void link_frame(Computer* comp, int i, long long now) {
	tcpLink& l = T.link[i];
	if (l.starting) {
		start_poll(comp, i, now);
		return;
	}
	if (!l.used) return;
	unsigned char buf[4096];
	l.sock.poll();
	bool raw = T.cipmode && (i == 0);
	// what the guest sent
	if (!l.tx.empty() && (l.sock.state() == xEspSock::S_OPEN)) {
		int n = l.sock.send((const unsigned char*)l.tx.data(), (int)l.tx.size());
		if (n > 0) {
			l.tx.erase(0, n);
			l.lastAct = now;
		}
		if (n < 0) l.tx.clear();
	}
	if (l.sendOk && l.tx.empty() && !raw) {
		l.sendOk = 0;
		l.mode8 = 0;
		esp_put_now("\r\nSEND OK\r\n");	// before any +IPD it brings
		send_wait_end();
	}
	// what came back, none of it before its SEND OK
	if (!l.sendOk && !l.peerGone && (esp_out_queued() < TCP_OUT_MAX)) {
		if (passive(l)) {
			while (l.rx.size() < TCP_HOLD_MAX) {
				size_t room = TCP_HOLD_MAX - l.rx.size();
				int n = l.sock.recv(buf, (int)((room < TCP_IPD_MAX) ? room : TCP_IPD_MAX));
				if (n < 0) {l.peerGone = true; break;}
				if (!n) break;
				l.rx.append((const char*)buf, n);
				l.lastAct = now;
				esp_put_now("\r\n+IPD," + lid(i) + std::to_string(n) + "\r\n");
			}
		} else {
			while (l.rx.size() < TCP_RX_MAX) {
				int n = l.sock.recv(buf, sizeof(buf));
				if (n < 0) {l.peerGone = true; break;}
				if (!n) break;
				l.lastAct = now;
				if (l.kind == K_UDP) udp_follow(l);
				if ((l.kind == K_UDP) && !raw) {	// a datagram is a packet of its own; CIPDINFO names its sender
					std::string h = "\r\n+IPD," + lid(i) + std::to_string(n);
					if (T.dinfo) h += "," + l.sock.fromIp() + "," + std::to_string(l.sock.fromPort());
					esp_put_now(h + ":" + std::string((const char*)buf, n));
					continue;
				}
				l.rx.append((const char*)buf, n);
			}
		}
	}
	// a packet when the wire is quiet or a segment is full
	if (!passive(l)) {
		while (!l.rx.empty() && (esp_out_queued() < TCP_OUT_MAX) && (raw || (l.rx.size() >= TCP_IPD_MAX) || (l.rx.size() == l.rxSeen) || l.peerGone)) {
			size_t n = (l.rx.size() > TCP_IPD_MAX) ? TCP_IPD_MAX : l.rx.size();
			if (raw) {
				esp_put_now(l.rx.substr(0, n));
			} else {
				std::string h = "\r\n+IPD," + lid(i) + std::to_string(n);
				if (T.dinfo) h += "," + peer_ip(l) + "," + std::to_string(l.sock.peerPort());
				esp_trace(">", h + ":");
				esp_put_now(h + ":" + l.rx.substr(0, n));
			}
			l.rx.erase(0, n);
		}
	}
	l.rxSeen = l.rx.size();
	// the far end went: CLOSED once everything is framed; passthrough calls again instead
	if (l.peerGone && (passive(l) || l.rx.empty())) {
		if (T.inPass && (i == 0)) {
			pass_reconnect(comp, i, now);
			return;
		}
		xlog(XLG_NET, XLL_INFO, "esp: link %i to %s:%i closed by the far end", i, l.host.c_str(), l.port);
		link_close(i, true, out_now);
		return;
	}
	// the server drops a client quiet for CIPSTO seconds
	if (l.server && T.sto && (now - l.lastAct > esp_ms(comp, T.sto * 1000.0))) {
		link_close(i, true, out_now);
	}
}

void esp_tcp_frame(Computer* comp, long long now) {
	pend_frame();
	for (int i = 0; i < TCP_LINKS; i++) link_frame(comp, i, now);
	server_frame(comp, now);
	pass_frame(comp, now);
	sntp_frame();
}

// == the module's life ==

void esp_tcp_down() {
	for (int i = 0; i < TCP_LINKS; i++) {
		T.link[i].counted = false;
		link_reset(T.link[i], false);
	}
	T.listen.close();
	T.reject.close();
	T.ntpSock.close();
	T.ntpBusy = false;
	T.pend = P_NONE;
	T.inPass = false;
	T.passTx.clear();
	T.plus = 0;
	T.count = 0;
	T.server = false;
}

void esp_tcp_boot(Computer* comp) {
	(void)comp;
	esp_tcp_down();
	T.mux = 0;
	T.cipmode = 0;
	T.dinfo = false;
	T.recvmode = 0;
	T.sslSize = 2048;
	T.sslConf = 0;
	T.maxConn = 5;
	T.sto = 180;
	T.status = 5;
	T.serverPort = 333;
	T.sendBuf.clear();
	for (int i = 0; i < 3; i++) T.ntp[i].clear();
	T.tz = 0;
	T.synced = false;
	T.ntpNext = 0;
	T.ntpSrv = 0;
	// lwIP's DNS slots: OpenDNS unless CIPDNS_DEF set its own
	T.dnsUser = (esp_fl_get("dns") == "yes");
	if (T.dnsUser) {
		T.dns[0] = esp_fl_get("dns1");
		T.dns[1] = esp_fl_get("dns2");
	} else {
		T.dns[0] = DNS_DEFAULT;
		T.dns[1].clear();
	}
}

// the station left its AP: every link ends with its CLOSED, a connect under way fails
void esp_tcp_wifi_lost(Computer* comp) {
	for (int i = 0; i < TCP_LINKS; i++) {
		tcpLink& l = T.link[i];
		if (T.inPass && (i == 0) && (l.used || l.starting)) {
			pass_reconnect(comp, i, esp_now(comp));
			continue;
		}
		if (l.starting) {
			if (l.kind != K_UDP) count_up(l);
			start_fail_async(i);
		} else if (l.used) {
			link_close(i, true, out_now);
		}
	}
}

// == the table ==

const espCmd esp_tcp_cmds[] = {
	{"+CIFSR", ok_form, nullptr, nullptr, cifsr_exec},
	{"+CIPSTATUS", ok_form, nullptr, nullptr, cipstatus_exec},
	{"+CIPSTART", nullptr, nullptr, cipstart_set, nullptr},
	{"+CIPSSLSIZE", nullptr, sslsize_query, sslsize_set, nullptr},
	{"+CIPSSLCCONF", nullptr, sslcconf_query, sslcconf_set, nullptr},
	{"+CIPCLOSE", ok_form, nullptr, cipclose_set, cipclose_exec},
	{"+CIPCLOSEMODE", nullptr, nullptr, cipclosemode_set, nullptr},
	{"+CIPSEND", ok_form, nullptr, cipsend_set, cipsend_exec},
	{"+CIPSENDEX", ok_form, nullptr, cipsendex_set, nullptr},
	{"+CIPDINFO", nullptr, cipdinfo_query, cipdinfo_set, nullptr},
	{"+CIPMUX", nullptr, cipmux_query, cipmux_set, nullptr},
	{"+CIPSERVER", nullptr, nullptr, cipserver_set, nullptr},
	{"+CIPSERVERMAXCONN", nullptr, maxconn_query, maxconn_set, nullptr},
	{"+CIPMODE", nullptr, cipmode_query, cipmode_set, nullptr},
	{"+CIPRECVMODE", nullptr, recvmode_query, recvmode_set, nullptr},
	{"+CIPRECVDATA", nullptr, nullptr, recvdata_set, nullptr},
	{"+CIPRECVLEN", nullptr, recvlen_query, nullptr, nullptr},
	{"+CIPSTO", nullptr, cipsto_query, cipsto_set, nullptr},
	{"+CIPDOMAIN", nullptr, nullptr, cipdomain_set, nullptr},
	{"+CIPSNTPCFG", nullptr, sntpcfg_query, sntpcfg_set, nullptr},
	{"+CIPSNTPTIME", nullptr, sntptime_query, nullptr, nullptr},
	{"+PING", nullptr, nullptr, ping_set, nullptr},
	{"+CIPDNS_CUR", nullptr, dnscur_query, dnscur_set, nullptr},
	{"+CIPDNS_DEF", nullptr, dnsdef_query, dnsdef_set, nullptr},
	{nullptr, nullptr, nullptr, nullptr, nullptr}
};
