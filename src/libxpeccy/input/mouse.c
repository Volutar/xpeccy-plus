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

// the buttons down, as the port has them: bit 0 is the left one unless swapped
int mouse_buttons(Mouse* mou) {
	int res = mou->mmb ? 4 : 0;
	if (mou->swapButtons) {
		res |= (mou->rmb ? 1 : 0) | (mou->lmb ? 2 : 0);
	} else {
		res |= (mou->lmb ? 1 : 0) | (mou->rmb ? 2 : 0);
	}
	return res;
}
