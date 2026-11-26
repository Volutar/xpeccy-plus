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
	FL_PCIMG,
	FL_SZX,
	FL_POK,
	FL_NEX,
	FL_SNX,
	FL_SCR		// a ZX screen: into the screen the machine shows
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
	FG_IF2_ROM,
	FG_POK,
	FG_DISKS,	// Open's one disk group: the same as Disk A
	FG_SCREEN
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
void file_errors(int);
bool file_is_pok(const QString&);	// cheats: the Cheats window opens them, load_file does not
bool file_is_medium(Computer*, const QString& path, int id);	// a tape or a disk, not a machine state
bool file_is_screen(Computer*, const QString& path, int id);	// a ZX screen: neither, and not the game
// the save dialog alone, for a file type the tables do not carry
// suggest: a path to start from, folder and name; empty for the last folder
QString file_ask_save(const char* title, const char* filter, const char* ext, const QString& suggest = QString(), const QString& own = QString());
// the open dialog alone: the path, with id and drv set to what was picked in it
QString file_ask_open(Computer*, int* id, int* drv);
QString file_ask_load(const char* title, const char* filter);
int load_file(Computer* comp, const char* name, int id, int drv);
// A tape or a disk put into the running machine, read on a thread of its own into a Tape
// or a Floppy nobody else sees: begin (gui thread; asks to save the one it replaces), read
// (any thread), end (gui thread: in under the emulation lock, if the deck or the drive
// still holds what it did at begin), free. load_file's way for anything else.
struct xMount {
	Computer* comp = nullptr;
	QString path;		// canonical; an archive's entry as an archive path
	int ftype = 0;		// FL_*
	bool disk = false;
	int drv = 0;
	QByteArray boot;	// the boot file a TR-DOS disk gets, when one does
	QByteArray stamp;	// what the deck or the drive held at begin
	bool game = false;	// it becomes the game: its hash is worked out on the way (saves.h)
	Tape* tape = nullptr;
	Floppy* flp = nullptr;
	int err = 0;
	QString why;		// the archive's word when the bytes could not be had
};
bool media_mount_can(Computer*, const QString& path, int id, int drv);
bool media_mount_asks(Computer*, const QString& path, int id, int drv);	// begin will ask about a change
// ask false: a changed medium in the way is not asked about, begin refuses instead
int media_mount_begin(Computer*, const QString& path, int id, int drv, xMount*, bool ask = true);
void media_mount_read(xMount*);
int media_mount_end(xMount*);		// ERR_CANCEL: something else went in meanwhile
// end's parts, for a caller that holds the emulation lock itself: fits (read well, and
// nothing else went in), put (fits, then in; under the lock), note (after a put)
bool media_mount_fits(xMount*);
bool media_mount_put(xMount*);
void media_mount_note(xMount*);
void media_mount_fail(xMount*);		// says why read failed
void media_mount_free(xMount*);
// An archive's images the request can take (id and drv as load_file has them), in
// natural order; empty, err set, when none
QStringList media_candidates(Computer*, const QString& archive, int id, int drv, QString* err);
// The set the image at path belongs to (xcore/siblings.h), from its folder: names and
// paths to open (an archive path for an archive beside it), in natural order, *cur set to
// the one at path; empty when it has none. For an archive path, the archive's set.
typedef struct {
	QString name;	// the file's name in the folder
	QString path;
} xSibling;
QList<xSibling> media_siblings(Computer*, const QString& path, int id, int drv, int* cur);
// The image step places on from the one at path in its set (id and drv the slot's),
// round past either end: the set by name, or with whole (a step by hand) an archive's
// own list when its names make none. Of a side's several files the one in seen (the
// images that were in), else the one with the same [flags], else one with none; one in
// skip (a disk in another drive) never. place gets its place, "side B". Empty: none.
QString media_next_side(Computer*, const QString& path, int id, int drv, const QStringList& seen, int step,
	const QStringList& skip = QStringList(), QString* place = nullptr, bool whole = false);
// the image to open from an archive, as an archive path: the only one, the one picked
// when ask, the first otherwise; empty when none or the user cancelled (said why)
QString media_from_archive(Computer*, const QString& archive, int id, int drv, bool ask);
// the last open the user made, which Reload makes again
typedef struct {
	QString path;
	int id;
	int drv;
	int run;
	bool pinned;	// on the running machine, whatever the file wants
} xMediaOpen;
// a file the user opened: kept for Reload, and its labels come with it; one
// into drive A, a tape or a snapshot is the game the quick saves are of
void media_opened(const QString& path, int id, int drv, int run, bool pinned, bool newGame = true);
xMediaOpen media_last_open();
// a snapshot loaded since the last call, empty when none
QString file_take_snapshot();
// the last recording opened for playback
QString rzx_current();
// the image in use, as the window title names it; empty when none
QString media_current();
QString media_image_name();
void media_set_current(const QString& path);
// the machine that file should be opened on, before it is (xcore/filemachine.h):
// *mac comes back empty to keep the running one, false means do not open it
bool media_machine(Computer*, const QString& path, int id, int drv, int run, std::string* mac);
bool media_machine_known(Computer*, const QString& path, int id, int drv, int run);	// ...without asking
// a tape, or a disk going into drive A: there is a choice between running it and mounting it
bool media_runnable(Computer*, const QString& path, int id, int drv);
bool media_to_drive(Computer*, const QString& path, int id);	// a disk image, or a file put on one
// AS_* the last loaded file would need to start, see xcore/autostart.h
int file_autostart_kind();
// reset the machine and start what was just opened, if run says so
void media_autorun(Computer*, int run);
// a run with no window: nothing is asked or shown, the log says what was refused
void media_autorun_headless(int on);
// drop that record: media a profile puts back was not opened by the user
void media_autorun_forget();
// live: the machine was running, so a snapshot may run it on to a better moment;
// suggest: where the save dialog starts, folder and name, when name is empty
int save_file(Computer* comp, const char* name, int id, int drv, int live = 0, const QString& suggest = QString());
// write back to path, or ask where when its format cannot be written
int save_back(Computer*, const char* path, int id, int drv);
// Quick saves, where xcore/saves.h says. Save: the machine into path, an .szx
// carrying its media unless the settings say links only; a machine that cannot
// be an .szx is held in memory until exit. 0 failed, 1 memory only, 2 the file.
int quick_save(Computer*, const QString& path);
// A quick load: the machine as it is kept for quick_undo first, then - the
// caller having switched to the machine the save names - the save put back.
// load: 1 done
void quick_keep_undo(Computer*);
int quick_load(Computer*, const QString& path);
// the one held in memory, 0 when there is none of this machine
int quick_load_held(Computer*);
// Back to before the last quick load; again, and that load is back. The same
// two steps: prepare keeps the machine as it is, then the caller switches to
// quick_undo_machine(). 0 when there is nothing to go back to.
std::string quick_undo_machine();
void quick_undo_prepare(Computer*);
int quick_undo(Computer*);
// a state of the machine taken in this process put back: the hits it left dropped, a
// recording being made goes on (a join)
int quick_xstate_load(struct xState*, Computer*);

// a changed disk or tape asked about: ERR_OK when it is safe to lose
int saveChangedDisk(Computer*,int);
int saveChangedTape(Computer*);
// the Drives menu and the Disk manager name what is in a drive alike
QString drive_media(const char* path, bool in);
int drive_count(Computer*);
