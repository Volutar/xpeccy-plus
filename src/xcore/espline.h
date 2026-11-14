#pragma once

// The ESP's side of the Next's UART line: what its own receiver makes of the bits the
// guest sends, at the ESP's rate. Equal rates give the bytes back; a mismatch gives what a
// real receiver would - wrong bytes, extra ones, none - which is what baud detection reads.

#include <deque>
#include <vector>

class espSnap;

class xEspLine {
	public:
		void reset();
		void snap(espSnap& s);			// in a snapshot of the module (esp_int.h)
		void setRate(double bitEdges, int frame);	// the ESP's bit in CLK_28 edges, its frame
		// a byte the guest sends: its start bit at edge s, a bit of guestBit edges, its frame
		void put(int byte, long long s, long guestBit, int guestFrame);
		// what the receiver has decoded up to edge now; the edge each byte's stop bit ends on
		void take(long long now, std::vector<int>& out, std::vector<long long>* ends = nullptr);
	private:
		struct Edge {long long t; int lv;};
		std::deque<Edge> tr;		// the line's level changes, oldest first
		long long known = 0;		// the line is known up to here (idle high after it)
		double bit = 243.0;
		int frame = 0x18;
		bool inFrame = false;
		double t0 = 0;			// the start bit's falling edge being decoded
		long long hunt = 0;		// look for a start bit from here
		int levelAt(double t) const;
		long long fallAfter(long long t) const;
		void drop_behind();
};
