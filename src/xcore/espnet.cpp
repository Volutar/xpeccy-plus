#include <chrono>
#include <mutex>
#include <thread>
#include <cstring>

#include "espnet.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static bool sock_wouldblock() {
	int e = WSAGetLastError();
	return (e == WSAEWOULDBLOCK) || (e == WSAEINPROGRESS);
}
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close ::close
static bool sock_wouldblock() {
	return (errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINPROGRESS) || (errno == EINTR);
}
#endif

// a send to a socket the far end has reset raises SIGPIPE on POSIX, which ends the program:
// Linux is told per call, macOS per socket (SO_NOSIGPIPE, set when it is made)
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

bool esp_net_init() {
#ifdef _WIN32
	static bool done = false;
	if (!done) {
		WSADATA wd;
		done = (WSAStartup(MAKEWORD(2, 2), &wd) == 0);
	}
	return done;
#else
	return true;
#endif
}

static void sock_nonblock(sock_t s) {
#ifdef _WIN32
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
	int nosig = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
#endif
}

static std::string ip_text(const sockaddr_in& sa) {
	const unsigned char* a = (const unsigned char*)&sa.sin_addr;
	return std::to_string(a[0]) + "." + std::to_string(a[1]) + "." + std::to_string(a[2]) + "." + std::to_string(a[3]);
}

// one lookup, shared by its owner and the thread doing it: either may be gone first
struct xEspLookup {
	std::mutex lock;
	bool done = false;
	bool ok = false;
	sockaddr_storage addr;
	int len = 0;
};

static bool lookup_now(const std::string& host, bool udp, sockaddr_in* out) {
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;		// the ESP8266 AT firmware is IPv4 only
	hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
	addrinfo* res = nullptr;
	bool ok = (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) && res;
	if (ok) memcpy(out, res->ai_addr, sizeof(*out));
	if (res) freeaddrinfo(res);
	return ok;
}

static void lookup_run(std::shared_ptr<xEspLookup> lk, std::string host, bool udp) {
	sockaddr_in sa;
	bool ok = lookup_now(host, udp, &sa);
	std::lock_guard<std::mutex> g(lk->lock);
	if (ok) {
		memcpy(&lk->addr, &sa, sizeof(sa));
		lk->len = (int)sizeof(sa);
	}
	lk->ok = ok;
	lk->done = true;
}

// == a connection ==

xEspSock::~xEspSock() {
	close();
}

void xEspSock::open(bool u, const std::string& host, int p, int localPort, bool onLan) {
	close();
	esp_net_init();
	udp = u;
	port = p;
	lport = localPort;
	lan = onLan;
	ip.clear();
	why = F_NONE;
	look = std::make_shared<xEspLookup>();
	st = S_RESOLVING;
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = inet_addr(host.c_str());
	if ((sa.sin_addr.s_addr != INADDR_NONE) || (host == "255.255.255.255")) {
		// a dotted address needs no lookup: the socket is there at once
		memcpy(&look->addr, &sa, sizeof(sa));
		look->len = (int)sizeof(sa);
		look->ok = look->done = true;
		poll();
		return;
	}
	std::thread(lookup_run, look, host, u).detach();
}

void xEspSock::adopt(long long f, const std::string& from, int p) {
	close();
	udp = false;
	fd = f;
	ip = from;
	port = p;
	lport = 0;
	why = F_NONE;
	st = S_OPEN;
}

void xEspSock::connectTo() {
	sockaddr_in sa;
	{
		std::lock_guard<std::mutex> g(look->lock);
		memcpy(&sa, &look->addr, sizeof(sa));
	}
	sa.sin_port = htons((unsigned short)port);
	ip = ip_text(sa);
	sock_t s = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, udp ? IPPROTO_UDP : IPPROTO_TCP);
	if (s == SOCK_BAD) {
		st = S_FAILED;
		return;
	}
	sock_nonblock(s);
	if (!udp) {
		int one = 1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
	} else {			// not connected: it sends to the peer set and hears anyone
		// a fixed port on a network address is what makes Windows' firewall ask (measured;
		// an ephemeral one never does)
		sockaddr_in la;
		memset(&la, 0, sizeof(la));
		la.sin_family = AF_INET;
		bool local = (((const unsigned char*)&sa.sin_addr)[0] == 127);
		if (lport && local) {
			la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			la.sin_port = htons((unsigned short)lport);
		} else if (lport && lan) {
			la.sin_port = htons((unsigned short)lport);
		}
		if (::bind(s, (sockaddr*)&la, sizeof(la))) {	// the port is taken on the host: any other
			la.sin_port = 0;
			::bind(s, (sockaddr*)&la, sizeof(la));
		}
		fd = (long long)s;
		st = S_OPEN;
		return;
	}
	fd = (long long)s;
	if (::connect(s, (sockaddr*)&sa, sizeof(sa)) == 0) {
		st = S_OPEN;
	} else if (sock_wouldblock()) {
		st = udp ? S_OPEN : S_CONNECTING;
	} else {
		why = F_REFUSED;
		st = S_FAILED;
	}
}

int xEspSock::poll() {
	if (st == S_RESOLVING) {
		bool done, ok;
		{
			std::lock_guard<std::mutex> g(look->lock);
			done = look->done;
			ok = look->ok;
		}
		if (done) {
			if (ok) {
				connectTo();
			} else {
				why = F_DNS;
				st = S_FAILED;
			}
		}
	}
	if (st == S_CONNECTING) {
		sock_t s = (sock_t)fd;
		fd_set wr, ex;
		FD_ZERO(&wr);
		FD_ZERO(&ex);
		FD_SET(s, &wr);
		FD_SET(s, &ex);
		timeval tv = {0, 0};
		if (select((int)s + 1, nullptr, &wr, &ex, &tv) > 0) {
			int err = 0;
			socklen_t len = sizeof(err);
			getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
			if (FD_ISSET(s, &ex) || err) {
				why = F_REFUSED;
				st = S_FAILED;
			} else {
				st = S_OPEN;
			}
		}
	}
	return st;
}

int xEspSock::localPort() const {
	if ((fd < 0) || (udp && lport)) return lport;
	sockaddr_in sa;
	socklen_t len = sizeof(sa);
	if (getsockname((sock_t)fd, (sockaddr*)&sa, &len)) return lport;
	return ntohs(sa.sin_port);
}

bool xEspSock::setPeer(const std::string& to, int p) {
	unsigned long a = inet_addr(to.c_str());
	if ((a == INADDR_NONE) && (to != "255.255.255.255")) return false;
	ip = to;
	port = p;
	return true;
}

int xEspSock::send(const unsigned char* b, int n) {
	if (st != S_OPEN) return -1;
	int r;
	if (udp) {
		sockaddr_in sa;
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = inet_addr(ip.c_str());
		sa.sin_port = htons((unsigned short)port);
		r = (int)::sendto((sock_t)fd, (const char*)b, n, 0, (sockaddr*)&sa, sizeof(sa));
	} else {
		r = (int)::send((sock_t)fd, (const char*)b, n, SEND_FLAGS);
	}
	if (r >= 0) return r;
	return sock_wouldblock() ? 0 : -1;
}

int xEspSock::recv(unsigned char* b, int n) {
	if (st != S_OPEN) return -1;
	int r;
	if (udp) {
		sockaddr_in sa;
		socklen_t len = sizeof(sa);
		r = (int)::recvfrom((sock_t)fd, (char*)b, n, 0, (sockaddr*)&sa, &len);
		if (r > 0) {
			fip = ip_text(sa);
			fport = ntohs(sa.sin_port);
		}
	} else {
		r = (int)::recv((sock_t)fd, (char*)b, n, 0);
	}
	if (r > 0) return r;
	if (r == 0) {
		if (udp) return 0;
		st = S_CLOSED;
		return -1;
	}
	if (sock_wouldblock()) return 0;
#ifdef _WIN32
	if (udp && (WSAGetLastError() == WSAECONNRESET)) return 0;	// an ICMP port unreachable
#endif
	st = S_CLOSED;
	return -1;
}

void xEspSock::close(bool abort) {
	if (fd >= 0) {
		if (abort && !udp) {
			linger lg;
			lg.l_onoff = 1;
			lg.l_linger = 0;
			setsockopt((sock_t)fd, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
		}
		sock_close((sock_t)fd);
	}
	fd = -1;
	look.reset();
	if (st != S_NONE) st = S_CLOSED;
}

// == a server ==

xEspListen::~xEspListen() {
	close();
}

bool xEspListen::open(int port, bool any) {
	close();
	esp_net_init();
	sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == SOCK_BAD) return false;
	sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(any ? INADDR_ANY : INADDR_LOOPBACK);
	sa.sin_port = htons((unsigned short)port);
	if (::bind(s, (sockaddr*)&sa, sizeof(sa)) || ::listen(s, 5)) {
		sock_close(s);
		return false;
	}
	sock_nonblock(s);
	fd = (long long)s;
	return true;
}

bool xEspListen::accept(xEspSock& out) {
	if (fd < 0) return false;
	sockaddr_in sa;
	socklen_t len = sizeof(sa);
	sock_t c = ::accept((sock_t)fd, (sockaddr*)&sa, &len);
	if (c == SOCK_BAD) return false;
	sock_nonblock(c);
	int one = 1;
	setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
	out.adopt((long long)c, ip_text(sa), ntohs(sa.sin_port));
	return true;
}

void xEspListen::close() {
	if (fd >= 0) sock_close((sock_t)fd);
	fd = -1;
}

// == a name ==

void xEspResolve::start(const std::string& host) {
	esp_net_init();
	res.clear();
	look = std::make_shared<xEspLookup>();
	std::thread(lookup_run, look, host, false).detach();
}

int xEspResolve::poll() {
	if (!look) return -1;
	std::shared_ptr<xEspLookup> keep = look;	// outlives the lock below
	std::lock_guard<std::mutex> g(keep->lock);
	if (!keep->done) return 0;
	int r = -1;
	if (keep->ok) {
		sockaddr_in sa;
		memcpy(&sa, &keep->addr, sizeof(sa));
		res = ip_text(sa);
		r = 1;
	}
	look.reset();
	return r;
}

// == a ping ==

struct xEspPingJob {
	std::mutex lock;
	bool done = false;
	int ms = -1;			// -1 no answer
};

static int ping_now(const std::string& host, int timeoutMs) {
	sockaddr_in sa;
	if (!lookup_now(host, false, &sa)) return -1;
	auto t0 = std::chrono::steady_clock::now();
#ifdef _WIN32
	HANDLE h = IcmpCreateFile();
	if (h == INVALID_HANDLE_VALUE) return -1;
	char data[32];
	for (int i = 0; i < (int)sizeof(data); i++) data[i] = (char)('a' + i % 23);
	unsigned char reply[sizeof(ICMP_ECHO_REPLY) + sizeof(data) + 8 + 64];
	DWORD n = IcmpSendEcho(h, sa.sin_addr.s_addr, data, sizeof(data), NULL, reply, sizeof(reply), (DWORD)timeoutMs);
	IcmpCloseHandle(h);
	if (!n || (((ICMP_ECHO_REPLY*)reply)->Status != IP_SUCCESS)) return -1;
#else
	sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);	// no rights needed where allowed
	if (s == SOCK_BAD) return -1;
	unsigned char pk[40];
	memset(pk, 0, sizeof(pk));
	pk[0] = 8;				// echo request; the id is the kernel's
	pk[7] = 1;				// sequence 1
	for (int i = 8; i < (int)sizeof(pk); i++) pk[i] = (unsigned char)('a' + (i - 8) % 23);
	unsigned long sum = 0;
	for (int i = 0; i < (int)sizeof(pk); i += 2) sum += (pk[i] << 8) | pk[i + 1];
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	sum = ~sum & 0xffff;
	pk[2] = (unsigned char)(sum >> 8);
	pk[3] = (unsigned char)sum;
	if (sendto(s, (const char*)pk, sizeof(pk), 0, (sockaddr*)&sa, sizeof(sa)) < 0) {
		sock_close(s);
		return -1;
	}
	bool got = false;
	for (;;) {
		long left = timeoutMs - (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		if (left <= 0) break;
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		timeval tv;
		tv.tv_sec = left / 1000;
		tv.tv_usec = (left % 1000) * 1000;
		if (select(s + 1, &rd, nullptr, nullptr, &tv) <= 0) break;
		unsigned char in[256];
		int n = (int)recv(s, (char*)in, sizeof(in), 0);
		if ((n >= 8) && (in[0] == 0)) {		// echo reply (the header is left out on these sockets)
			got = true;
			break;
		}
		if ((n >= 28) && ((in[0] >> 4) == 4) && (in[(in[0] & 15) * 4] == 0)) {	// with the IP header (macOS)
			got = true;
			break;
		}
	}
	sock_close(s);
	if (!got) return -1;
#endif
	return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

static void ping_run(std::shared_ptr<xEspPingJob> job, std::string host, int timeoutMs) {
	int ms = ping_now(host, timeoutMs);
	std::lock_guard<std::mutex> g(job->lock);
	job->ms = ms;
	job->done = true;
}

void xEspPing::start(const std::string& host, int timeoutMs) {
	esp_net_init();
	time = 0;
	job = std::make_shared<xEspPingJob>();
	std::thread(ping_run, job, host, timeoutMs).detach();
}

int xEspPing::poll() {
	if (!job) return -1;
	std::shared_ptr<xEspPingJob> keep = job;	// outlives the lock below
	std::lock_guard<std::mutex> g(keep->lock);
	if (!keep->done) return 0;
	int r = (keep->ms >= 0) ? 1 : -1;
	time = (keep->ms >= 0) ? keep->ms : 0;
	job.reset();
	return r;
}
