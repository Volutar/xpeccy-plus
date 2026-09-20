#pragma once

#include <QWidget>
#include <string>

#include "libxpeccy/filetypes/filetypes.h"
#include "libxpeccy/spectrum.h"

enum {
	FL_NONE = 0,
	FL_TAP,
	FL_TZX,
	FL_WAV,
	FL_SCL,
	FL_TRD,
	FL_FDI,
	FL_UDI,
	FL_DSK,
	FL_TD0,
	FL_SNA,
	FL_Z80,
	FL_SPG,
	FL_RZX,
	FL_HOBETA,
	FL_RAW,
	FL_SLT_BIN,
	FL_SLT_ROM,
	FL_IMA,
	FL_PCIMG
};

enum {
	FG_DISK = -1,
	FG_ALL = 0,
	FG_TAPE = (1 << 10),
	FG_DISK_A,
	FG_DISK_B,
	FG_DISK_C,
	FG_DISK_D,
	FG_SNAPSHOT,
	FG_RZX,
	FG_HOBETA,
	FG_RAW,
	FG_IF2_ROM
};

enum {
	FH_SPECTRUM = (1 << 12),
	FH_ALF,
	FH_SLOTS,
	FH_DRIVE_A,
	FH_DRIVE_B,
	FH_DRIVE_C,
	FH_DRIVE_D
};

void initFileDialog(QWidget*);
void fitFileDialog(QWidget*);
// the save dialog alone, for a file type the tables do not carry
QString file_ask_save(const char* title, const char* filter, const char* ext);
// the open dialog alone: the path, with id and drv set to what was picked in it
QString file_ask_open(Computer*, int* id, int* drv);
int load_file(Computer* comp, const char* name, int id, int drv);
// load the last snapshot and labels file again; returns which of them it did
#define RELOAD_SNAPSHOT	1
#define RELOAD_LABELS	2
int media_reload(Computer*);
// a snapshot loaded since the last call, empty when none
QString file_take_snapshot();
// the image in use, as the window title names it; empty when none
QString media_current();
void media_set_current(const QString& path);
// the machine that file should be opened on, before it is (xcore/filemachine.h):
// *mac comes back empty to keep the running one, false means do not open it
bool media_machine(Computer*, const QString& path, int id, int drv, int run, std::string* mac);
// AS_* the last loaded file would need to start, see xcore/autostart.h
int file_autostart_kind();
// reset the machine and start what was just opened, if run says so
void media_autorun(Computer*, int run);
// drop that record: media a profile puts back was not opened by the user
void media_autorun_forget();
int save_file(Computer* comp, const char* name, int id, int drv);

int saveChangedDisk(Computer*,int);
