#pragma once

#include <stdio.h>
#include <string>
#include <vector>

// Which machine a file is opened on. One row per format, each set to one of
// the words below or to the id of a machine that always takes it. When this
// is asked at all is media_machine()'s business, in filer.cpp.

#define FM_AUTO	"auto"		// switch only when the running machine cannot take it
#define FM_ASK	"ask"		// the same, but the user picks the machine
#define FM_KEEP	"keep"		// never switch

// what a file needs of a machine
enum {
	FMN_ANY = 0,
	FMN_TSCONF,
	FMN_TRDOS,		// to start a TR-DOS disk
	FMN_PLUS3DOS,		// to start a +3 disk
	FMN_SNAPSHOT,		// what the file was taken on, see probe
	FMN_RZX			// the same, but it has to be that machine exactly
};

typedef struct {
	int ftype;		// FL_*
	const char* key;	// in the config
	const char* name;
	int need;		// FMN_*
	const char* target;	// the machine Auto switches to
	int (*probe)(const char*);	// FMN_SNAPSHOT/FMN_RZX: SNAP_HW_* of the file
} xFileMac;

// the rows, ending with a NULL key
const xFileMac* fm_rows();
const char* fm_need_text(int need);
// can that machine take a file that needs this (snap: SNAP_HW_*, for a snapshot)
bool fm_runs(const std::string& id, int need, int snap = 0);

// what the user set; a row left on Auto is not kept
void fm_clear();
std::string fm_pref(const std::string& key);
void fm_load(const std::string& key, const std::string& val);
void fm_save(FILE*);

// what opening a file of that type should do to the running machine
typedef struct {
	std::string target;			// switch to it; empty stays
	std::vector<std::string> ask;		// or let the user pick one, the automatic pick first
} xFileMacPick;

xFileMacPick fm_pick(int ftype, const char* path);
