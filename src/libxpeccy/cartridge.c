#include "cartridge.h"
#include <stdlib.h>
#include <string.h>

xCartridge* sltCreate() {
	xCartridge* slt = (xCartridge*)malloc(sizeof(xCartridge));
	memset(slt, 0x00, sizeof(xCartridge));
	return slt;
}

void sltDestroy(xCartridge* slot) {
	if (slot == NULL) return;
	sltEject(slot);
	free(slot);
}

void sltSetPath(xCartridge* slot, const char* p) {
	slot->path = (char*)realloc(slot->path, strlen(p) + 1);
	strcpy(slot->path, p);
}

void sltEject(xCartridge* slot) {
	if (slot->data == NULL) return;
	free(slot->data);
	slot->data = NULL;
	if (slot->path) {
		free(slot->path);
		slot->path = NULL;
	}
	if (slot->brkMap) {
		free(slot->brkMap);
		slot->brkMap = NULL;
	}
}
