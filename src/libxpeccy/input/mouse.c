#include <stdlib.h>
#include <string.h>

#include "input.h"

// mouse

Mouse* mouseCreate(cbirq cb, void* p) {
	Mouse* mou = (Mouse*)malloc(sizeof(Mouse));
	memset(mou,0x00,sizeof(Mouse));
	mou->sensitivity = 1.0f;
	mou->xirq = cb;
	mou->xptr = p;
	return mou;
}

void mouseDestroy(Mouse* mou) {
	free(mou);
}

void mouseReleaseAll(Mouse* mou) {
	mou->lmb = 0;
	mou->rmb = 0;
	mou->mmb = 0;
	mou->autox = 0;
	mou->autoy = 0;
}

void mousePress(Mouse* mou, int wut, int val) {
	x_input_bump();
	switch(wut) {
		case XM_LMB: mou->lmb = 1; break;
		case XM_RMB: mou->rmb = 1; break;
		case XM_MMB: mou->mmb = 1; break;
		case XM_WHEELDN: mou->wheel++; break;
		case XM_WHEELUP: mou->wheel--; break;
		case XM_UP: mou->autoy = val; break;
		case XM_DOWN: mou->autoy = -val; break;
		case XM_LEFT: mou->autox = -val; break;
		case XM_RIGHT: mou->autox = val; break;
	}
}

void mouseRelease(Mouse* mou, int wut) {
	x_input_bump();
	switch(wut) {
		case XM_LMB: mou->lmb = 0; break;
		case XM_RMB: mou->rmb = 0; break;
		case XM_MMB: mou->mmb = 0; break;
		case XM_UP:
		case XM_DOWN: mou->autoy = 0; break;
		case XM_LEFT:
		case XM_RIGHT: mou->autox = 0; break;
	}
}

int mouseGetX(Mouse* mou) {return mou->xpos * mou->sensitivity;}
int mouseGetY(Mouse* mou) {return mou->ypos * mou->sensitivity;}

// the buttons down, as the port has them (a set bit is down): the Kempston mouse
// interface has the right one on D0 and the left on D1, the ZX Evo's AVR the other
// way round; D2 is the middle one. Swap trades the host's two
int mouse_buttons(Mouse* mou) {
	int l = mou->swapButtons ? mou->rmb : mou->lmb;
	int r = mou->swapButtons ? mou->lmb : mou->rmb;
	int res = mou->mmb ? 4 : 0;
	return res | (mou->leftD0 ? (l | (r << 1)) : (r | (l << 1)));
}

// the port's bits as left 1, right 2, middle 4: the buttons the program got
int mouse_lrm(Mouse* mou, int bits) {
	if (mou->leftD0) return bits & 7;
	return (bits & 4) | ((bits & 1) << 1) | ((bits >> 1) & 1);
}
