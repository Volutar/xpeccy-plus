#pragma once

#include "../libxpeccy/spectrum.h"

// autostart kinds
enum {
	AS_NONE = 0,
	AS_TAPE,
	AS_DISK,	// tr-dos image
	AS_DISK3	// +3 disk
};

// arm() once, when the media is opened; frame() ticks it until it is done.
// arm() answers 0 when this machine cannot start that kind of media at all
int autostart_arm(Computer*, int kind);
// could a machine of that core and disk interface (DIF_*) start it
int autostart_can(int hwid, int dif, int kind);
// a divMMC that maps takes the disk entries: that kind cannot be started now
int autostart_board_blocks(Computer*, int kind);
void autostart_frame(Computer*);
// the machine it was typing into is gone: switched, reset, another file in
void autostart_cancel();
// the machine is being driven to the media: nothing of it is to be shown
int autostart_busy();
// the ZX Spectrum Next: NextZXOS boots off the SD card, and on its command line
// .cd goes into a folder of the card's root and NEXLOAD (snx: SPECTRUM) starts a file
// there; 0 if a name cannot be typed. welcome: the card starts NextZXOS's welcome pages first
int autostart_arm_nex(Computer*, const char* dir, const char* name, int welcome, int snx, int powered);
// those lines can be typed, asked before anything changes; a word (a folder's name for .cd,
// a file's for NEXLOAD) the keyboard types as it is, with no space
int autostart_nex_can(const char* dir, const char* name, int snx);
int autostart_nex_word(const char* word);
// press or let go of keys named as a keymap names them ("E", "Sp"...)
void autostart_key(Computer*, const char* keys, int press);
