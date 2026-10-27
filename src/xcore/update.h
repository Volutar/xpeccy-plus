#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

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

// Markdown of what theirs (the new build's CHANGELOG.md) has that this build has not:
// sections newer than self's version, less the entries mine (this build's own
// CHANGELOG.md, may be empty) already has. Empty when nothing is left.
QString up_whats_new(const QByteArray& theirs, const QByteArray& mine, const upBuild& self);
