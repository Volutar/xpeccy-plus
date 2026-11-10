#pragma once

// Saves of a game: the quick saves and the autosaves of the image the user
// opened, each an .szx with a picture beside it, in a folder of the game's own
// under <saves>/ - named after the game and the hash of its bytes, so the same
// game bare, in an archive or moved finds the same folder. With no game, or
// with per-game saves off, they go to the machine's folder instead.
//
// The folder holds game.json (the game's name, its hash, the paths it was
// opened from) and kind-YYYYMMDD-HHMMSS-zzz.szx + .png: the name is the order,
// so a ring needs no counters.

#include <QDateTime>
#include <QList>
#include <QString>

#include <string>

#include "../libxpeccy/spectrum.h"

enum {
	SAVE_QUICK = 0,
	SAVE_EXIT,		// the autosave taken on exit
	SAVE_TIMED,		// the periodic one
	SAVE_KINDS
};

typedef struct {
	QString path;		// the .szx
	QString pic;		// its picture, empty when there is none
	QDateTime time;
	int kind;
	qint64 size;		// bytes, the picture's too
} xSave;

// The game: what runs - opened into drive A, as a tape or as a snapshot, or
// started from the tape or drive A since. A user reset or a machine switch
// ends it. The bytes as opened are hashed here (an entry's for an archive path).
void game_set(const QString& image);
// the hash game_set will want, worked out on another thread: an image read in the
// background (xMount) does not hash megabytes on the gui thread
void game_hash_ahead(const QString& image);
// the tape or drive A started with this image: the game, unless it is already
void game_follow(const QString& image);
bool same_image(const QString&, const QString&);	// one file, however its path was written
// the game of a save, as its folder's json has it (or the json beside it):
// that save is going in
void game_set_save(const QString& save);
void game_forget();
// as it is, to put back (undoing a quick load)
typedef struct {
	QString image;
	QString hash;
	QString name;
	int src;
	int resets;		// xUserResets as it was set
	std::string mac;	// ...and the machine
} xGame;
xGame game_now();
void game_put(const xGame&);
// forgets it once the tape or the disk it was opened as is out
void game_check(Computer*);
// a user reset or another machine since it was set: what ran has ended
bool game_stale();
// the reset an open makes to start its image is part of the start
void game_restamp();
// the game's tape gave way to the next one of its set (Rewind at end): still the game
void game_tape_next(const QString& from, const QString& to);
// another side of its set took the place of the game's tape or disk (picked from a
// menu): still the game. False when the image that went out was not the game's
bool game_side_next(const QString& from, const QString& to, bool disk);
QString game_image();			// empty: none
QString game_name();			// what the folder and the menus call it

// where all of them are: the setting, or <config>/saves
QString saves_root();
// The folder a new save goes to: the game's, or the machine's when there is no
// game (or per-game saves are off). make: create it with its json; empty when
// it is not there and make is false.
QString saves_dir(bool make);
// the game's own folder, empty when it has none yet (no save taken)
QString saves_game_dir();
// the common last-session file older versions wrote: still listed and loaded
QString saves_session_path();
// the newest autosave (on exit or timed) of any folder, empty when none
QString saves_newest_auto();
// what Resume Last Session loads: the last autosave, that one gone the newest
// autosave left; path empty when none. name: its game's or machine's, if known
xSave saves_resume(QString* name = nullptr);
// a folder's saves, newest first; kind -1 for all
QList<xSave> saves_list(const QString& dir, int kind = -1);
// the name of a new save of that kind in dir, by the time now
QString saves_new_path(const QString& dir, int kind);
// the oldest of that kind past keep, gone with their pictures
void saves_trim(const QString& dir, int kind, int keep);
void saves_remove(const xSave& sv);
// "Today 21:05:13", "Yesterday ...", the date otherwise
QString saves_time_text(const QDateTime& t);
QString saves_kind_name(int kind);
// the image a folder's game was opened from last, empty for a machine's folder
QString saves_folder_image(const QString& dir);

// Every folder with saves in it, newest save first, and the last session's
// common file as a folder of one (dir empty). For the Save History window
// and Recent.
typedef struct {
	QString dir;
	QString name;		// the game's, or the machine's
	bool machine;		// a machine's folder: no game
	QStringList images;	// the paths the game was opened from
	QList<xSave> saves;
	qint64 size;
} xSaveFolder;
QList<xSaveFolder> saves_folders();
// the folder of the game opened from this path, empty when it has none
QString saves_folder_of(const QString& image);
// a folder with no save left goes, its json with it
void saves_drop_empty(const QString& dir);
// the saves of a folder older than days, the newest kept when keepOne
QList<xSave> saves_old(const xSaveFolder& f, int days, bool keepOne);
// gone with its picture, the json beside it and the config's pointer at it
void saves_delete(const QList<xSave>& list);
// at start, when the settings say: the old ones of every folder
void saves_clean_old();
