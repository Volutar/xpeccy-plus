#include "espline.h"
#include "esp_int.h"

void xEspLine::reset() {
	tr.clear();
	known = 0;
	inFrame = false;
	hunt = 0;
}

void xEspLine::snap(espSnap& s) {
	size_t n = tr.size();
	s.v("line.n", n);
	if (s.loading) tr.assign(n, Edge{0, 1});
	for (size_t i = 0; i < tr.size(); i++) {
		s.v("line.edge" + std::to_string(i), tr[i].t);
		s.v("line.level" + std::to_string(i), tr[i].lv);
	}
	s.v("line.known", known);
	s.v("line.bit", bit);
	s.v("line.frame", frame);
	s.v("line.inFrame", inFrame);
	s.v("line.t0", t0);
	s.v("line.hunt", hunt);
}

void xEspLine::setRate(double bitEdges, int fr) {
	bit = (bitEdges < 1.0) ? 1.0 : bitEdges;
	frame = fr & 0x1f;
}

static int frame_bits(int fr) {			// start, data, parity, stops
	return 1 + 5 + ((fr >> 3) & 3) + ((fr >> 2) & 1) + 1 + (fr & 1);
}

void xEspLine::put(int b, long long s, long gbit, int gfr) {
	if (gbit < 1) gbit = 1;
	int bits = 5 + ((gfr >> 3) & 3);
	int lv[12], n = 0;
	int parity = (gfr >> 1) & 1;
	lv[n++] = 0;
	for (int k = 0; k < bits; k++) {
		lv[n++] = (b >> k) & 1;
		parity ^= (b >> k) & 1;
	}
	if (gfr & 4) lv[n++] = parity;
	lv[n++] = 1;
	if (gfr & 1) lv[n++] = 1;
	int cur = tr.empty() ? 1 : tr.back().lv;
	for (int k = 0; k < n; k++) {
		if (lv[k] != cur) {
			tr.push_back({s + k * gbit, lv[k]});
			cur = lv[k];
		}
	}
	known = s + n * gbit;
}

int xEspLine::levelAt(double t) const {
	int lv = 1;
	for (const Edge& e : tr) {			// short: pruned as it is decoded
		if (e.t > t) break;
		lv = e.lv;
	}
	return lv;
}

long long xEspLine::fallAfter(long long t) const {
	int prev = 1;
	for (const Edge& e : tr) {
		if ((e.t >= t) && (e.lv == 0) && prev) return e.t;
		prev = e.lv;
	}
	return -1;
}

// A UART receiver: a falling edge starts a frame, the start bit is checked at its middle,
// every bit after is sampled at its middle, and the next start bit is looked for from the
// middle of the last stop bit. Beyond the last byte the guest sent the line is idle.
void xEspLine::take(long long now, std::vector<int>& out, std::vector<long long>* ends) {
	if (now < known) now = known;		// the guest's bytes are all there is up to now
	int nb = frame_bits(frame);
	int data = 5 + ((frame >> 3) & 3);
	for (;;) {
		if (!inFrame) {
			long long f = fallAfter(hunt);
			if (f < 0) break;
			t0 = (double)f;
			inFrame = true;
		}
		double last = t0 + (nb - 0.5) * bit;	// the middle of the last stop bit
		if (last > (double)now) break;
		if (levelAt(t0 + 0.5 * bit)) {		// a glitch, not a start bit
			inFrame = false;
			hunt = (long long)t0 + 1;
			drop_behind();			// bits too short for its rate: nothing may ever decode
			continue;
		}
		int v = 0;
		for (int k = 0; k < data; k++)
			v |= levelAt(t0 + (1.5 + k) * bit) << k;
		out.push_back(v);			// a framing or parity error still hands the byte on
		if (ends) ends->push_back((long long)(t0 + nb * bit));
		inFrame = false;
		hunt = (long long)last + 1;
		drop_behind();
	}
}

// what lies wholly behind the receiver is not needed again: every look is at hunt or after
void xEspLine::drop_behind() {
	while ((tr.size() > 1) && (tr[1].t <= hunt)) tr.pop_front();
}
