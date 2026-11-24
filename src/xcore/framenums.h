#pragma once

// The machine's numbers of one frame, stamped on the picture where the emulation
// hands it over: what a shader needs to place a dot on its line, the line in the
// frame and the color carrier against both.
struct xFrameNums {
	int frame = 0;			// the machine's frame number
	int fullX = 0;			// dots a line
	int fullY = 0;			// lines a frame
	int cutX = 0;			// the first shown dot and line
	int cutY = 0;
	double carrier = 0;		// color carrier, cycles a dot
	bool locked = false;		// to the dot clock: the pattern stands still
	double blend = 0;		// antiflicker left to the look (frames came unmixed): the frame before's part, 0 .. 0.5
};
