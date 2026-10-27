#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

// The update check's logic, Qt Core only: what a GitHub release answer says, whether it is
// newer than this build, and which changelog entries this build lacks. No network and no
// version.h here - the caller passes this build in, so tests/update can pass any.

#define UP_REPO		"dotkoval/xpeccy-plus"
#define UP_API		"https://api.github.com/repos/" UP_REPO "/releases/"
#define UP_RAW		"https://raw.githubusercontent.com/" UP_REPO "/"
#define UP_COMPARE	"https://api.github.com/repos/" UP_REPO "/compare/"

enum {UP_RELEASES = 0, UP_NIGHTLY};

// a build: this one, or one on GitHub
struct upBuild {
	QString base;		// CalVer, "2026.9"
	bool release = false;	// false: a dev build of base
	int date = 0;		// YYYYMMDD; dev builds only, 0 unknown
	QString commit;		// 40 hex digits, or empty
};

struct upAsset {
	QString name;
	QString url;
	QString sha256;		// 64 hex digits, lower case, or empty
	qint64 size = 0;
};

struct upRelease {
	bool ok = false;
	QString error;		// !ok: why, short
	bool nightly = false;
	upBuild build;
	QString tag;		// "v2026.9" or "nightly"
	QString body;		// Markdown
	QString page;		// the release's page
	upAsset asset;		// this build's package; url empty when it has none
};

// "2026.9" against "2026.10": <0, 0, >0
int up_calver_cmp(const QString& a, const QString& b);
// a against b: higher base, then a release above a dev build of its base, then the later
// date; <0, 0, >0. Commits are not compared.
int up_build_cmp(const upBuild& a, const upBuild& b);
// the answer offers something this build is not: newer, or a dev build of the same date
// made from another commit - which up_same_day() then asks GitHub about
bool up_newer(const upBuild& cand, const upBuild& self);
// cand is a dev build of self's date from another commit: whether it is newer can only be
// told by the commits, through compare/<self>...<cand>
bool up_same_day(const upBuild& cand, const upBuild& self);
// that compare answer: true when cand has all of self's commits and more
bool up_compare_ahead(const QByteArray& json);
// a releases/latest or releases/tags/nightly answer; package is this build's (XPACKAGE),
// empty for none
upRelease up_parse(const QByteArray& json, const QString& package);
// of the answers, the newest that is newer than self; -1 for none
int up_pick(const QList<upRelease>& list, const upBuild& self);
// the default channel of a build
int up_default_channel(const upBuild& self);

// --- putting a new build in place of this one (update_apply.cpp) ---
//
// The new build is unpacked into a folder inside the install (the same volume: every move
// is a rename), then each of its files takes the place of the old one, which is renamed
// aside to <name>.old - a running exe and its DLLs can be renamed, not overwritten. Every
// rename is written to update.journal first, so an update cut short is undone at the next
// start. The install folder itself is never renamed or moved.
//
// config/ holds the user's files beside the ones that ship. A shipped file the user has
// not changed (its sha256 is the one the old install's manifest.sha256 lists) is
// replaced; one changed, or one no manifest vouches for, is kept and the new one goes to
// config/update-<version>/ instead. Files the user wrote ship in no release, so are never
// touched.

#define UP_MANIFEST	"manifest.sha256"
#define UP_JOURNAL	"update.journal"
#define UP_LEFT		"update.left"		// .old files still in use when cleaned up
#define UP_STAGE	"update.new"		// where the new build is unpacked

enum {UPS_ADD = 0, UPS_REPLACE, UPS_KEEP, UPS_DROP};

struct upStep {
	int kind;		// UPS_*: KEEP puts the new file aside, DROP an old one no release has
	QString rel;		// "Qt6Core.dll", "config/keymaps/x.map"
};

// sha256 of a file, lower case hex; empty when it cannot be read
QString up_file_sha(const QString& path);
// manifest.sha256: path -> sha256
QMap<QString, QString> up_manifest(const QString& file);
// what turns the install app into the build unpacked at stage
QList<upStep> up_plan(const QString& app, const QString& stage);
// does it; false with err after undoing what was done. stopAfter >= 0 stops after that
// many renames as a power cut would, journal left behind (for tests).
bool up_apply(const QString& app, const QString& stage, const QString& version,
	const QList<upStep>& plan, QString* err, int stopAfter = -1);

enum {UPR_NONE = 0, UPR_DONE, UPR_UNDONE};
struct upResume {
	int state = UPR_NONE;
	QString version;	// DONE: what it is now
	QStringList kept;	// DONE: config files of the user's kept, the new ones put aside
	QStringList moved;	// UNDONE: the files put back
	QStringList failed;	// what could not be put back or removed
};
// at a start: an update finished is cleaned up, one cut short undone
upResume up_resume(const QString& app);

// Markdown of what theirs (the new build's CHANGELOG.md) has that this build has not:
// sections newer than self's version, less the entries mine (this build's own
// CHANGELOG.md, may be empty) already has. Empty when nothing is left.
QString up_whats_new(const QByteArray& theirs, const QByteArray& mine, const upBuild& self);
