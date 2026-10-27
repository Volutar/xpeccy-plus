#pragma once

// The host's network for the emulated ESP: non-blocking sockets polled from the emulation
// thread, and name lookups on a thread of their own so a slow DNS never holds a frame.

#include <memory>
#include <string>
#include <vector>

struct xEspLookup;
struct xEspPingJob;

class xEspSock {
	public:
		enum {S_NONE = 0, S_RESOLVING, S_CONNECTING, S_OPEN, S_FAILED, S_CLOSED};
		enum {F_NONE = 0, F_DNS, F_REFUSED};
		~xEspSock();
		// localPort: UDP only, the port to send from and hear on (0 any). A fixed port is taken
		// on 127.0.0.1 for a far end there, on every interface only when lan (Windows' firewall
		// asks about that), else an ephemeral one: the far end's replies come all the same
		void open(bool udp, const std::string& host, int port, int localPort = 0, bool lan = false);
		void adopt(long long fd, const std::string& ip, int port);	// a connection a listener took
		int poll();			// moves the state on; returns it
		int state() const {return st;}
		int failure() const {return why;}
		std::string peerIp() const {return ip;}		// the far end, once it is looked up
		int peerPort() const {return port;}
		int localPort() const;		// UDP: the port asked for, whichever the host gave
		int send(const unsigned char* b, int n);	// bytes taken, -1 the link is gone
		int recv(unsigned char* b, int n);		// bytes read, 0 none yet, -1 closed
		void close(bool abort = false);			// abort: a reset, not a FIN
		// UDP is not tied to one far end: it sends to the peer set, hears anyone
		bool setPeer(const std::string& ip, int port);	// a dotted IP; false if it is not one
		std::string fromIp() const {return fip;}	// who sent the datagram recv() gave last
		int fromPort() const {return fport;}
		bool udp = false;
	private:
		int st = S_NONE;
		int why = F_NONE;
		long long fd = -1;
		int port = 0;
		int lport = 0;
		bool lan = false;
		std::string ip;
		std::string fip;
		int fport = 0;
		std::shared_ptr<xEspLookup> look;
		void connectTo();
};

// a TCP server socket: on 127.0.0.1, which the host's own programs reach and the LAN does not,
// or on every interface when the user lets it
class xEspListen {
	public:
		~xEspListen();
		bool open(int port, bool any = false);	// false: the port is taken or not allowed
		bool accept(xEspSock& s);	// a client was waiting: now it is s
		void close();
		bool isOpen() const {return fd >= 0;}
	private:
		long long fd = -1;
};

// one name looked up, on a thread of its own
class xEspResolve {
	public:
		void start(const std::string& host);
		int poll();			// 0 still looking, 1 found, -1 not found
		std::string ip() const {return res;}
		bool busy() const {return look != nullptr;}
	private:
		std::shared_ptr<xEspLookup> look;
		std::string res;
};

// one ICMP echo, on a thread of its own, where the host lets a program without rights send
// one (Windows' ICMP API, a ping socket elsewhere); never a made-up answer
class xEspPing {
	public:
		void start(const std::string& host, int timeoutMs);
		int poll();			// 0 waiting, 1 answered, -1 no answer, not found or not allowed
		int ms() const {return time;}
		bool busy() const {return job != nullptr;}
	private:
		std::shared_ptr<xEspPingJob> job;
		int time = 0;
};

bool esp_net_init();
