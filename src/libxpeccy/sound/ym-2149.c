#include "ayym.h"

// extern int ayDACvol[32];

int ymDACvol[32] = {0x0000,0x0000,0x003B,0x0074,0x00A4,0x00CA,0x00FB,0x0134,
                    0x0184,0x01E0,0x0244,0x028D,0x030C,0x03AD,0x044C,0x04E8,
                    0x05D4,0x06FD,0x0838,0x0965,0x0B28,0x0D5F,0x0F91,0x11D7,
                    0x1540,0x1988,0x1DCC,0x2211,0x2874,0x3040,0x3828,0x3FFF};

// ym_reset = ay_reset

// ym_rd: in ay-3-8910.c, beside ay_rd

// ym_sync = ay_sync (with 5-bit volumes)

// Only the table is the YM's: the gate, the mix and the debugger's Lev column all
// come from the AY code. Written out a second time here they drifted apart, and a
// muted channel went on playing.
sndPair ym_vol(aymChip* chip) {
	ay_flush(chip);					// a YM2203's fm half with it
	return ay_mix_tab(chip, ymDACvol);		// YM:5-bit DAC volume
}
