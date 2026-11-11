// The update check's logic over saved GitHub answers (data/: fetched 2026-10-27,
// nightly-20261110.json 2026-11-11) and changelogs: update-check <data dir> <CHANGELOG.md>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <cstring>

#include "update.h"

static int fails = 0;
static int checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static QByteArray load(const QString& path) {
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) {
		printf("cannot read %s\n", qPrintable(path));
		exit(2);
	}
	return file.readAll();
}

static upBuild rel(const char* base) {
	upBuild b;
	b.base = base;
	b.release = true;
	return b;
}

static upBuild dev(const char* base, int date, const char* commit = "") {
	upBuild b;
	b.base = base;
	b.date = date;
	b.commit = commit;
	return b;
}

static upRelease answer(const upBuild& b) {
	upRelease r;
	r.ok = true;
	r.build = b;
	r.nightly = !b.release;
	return r;
}

#define NIGHTLY_SHA "ce78b130062e63b07be3d6c61b7a2ed46c9abf04"
#define OTHER_SHA "cebd9903aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static void test_parse(const QString& dir) {
	QByteArray latest = load(dir + "/latest-2026.8.2.json");
	QByteArray nightly = load(dir + "/nightly-20261027.json");
	static const struct {const char* pkg; const char* rel; const char* nig;} tab[] = {
		{"win-x64", "0d853c632a229a27", "2f19fad5ce279015"},
		{"win-x64-qt6", "26a9e8108cb84cba", "31955ca6ac680493"},
		{"win-x86", "873adee14715b6c7", "a0a65203458d9422"},
		{"macos-arm64-qt6", "6cc07f15f1696f1b", "db834ea914d80748"},
		{"linux-x86_64", "33e9f7685bab91c8", "22bfdf42bd3f4e14"},
	};
	for (const auto& row : tab) {
		upRelease r = up_parse(latest, row.pkg);
		CHECK(r.ok);
		CHECK(!r.nightly);
		CHECK(r.tag == "v2026.8.2");
		CHECK(r.build.base == "2026.8.2" && r.build.release);
		CHECK(r.asset.sha256.startsWith(row.rel));
		CHECK(r.asset.url.startsWith("https://github.com/dotkoval/xpeccy-plus/releases/download/v2026.8.2/"));
		CHECK(r.asset.url.endsWith(r.asset.name));
		CHECK(r.asset.size > 20000000);
		CHECK(r.page == "https://github.com/dotkoval/xpeccy-plus/releases/tag/v2026.8.2");
		CHECK(r.body.contains("Xpeccy+ 2026.8.2"));

		upRelease n = up_parse(nightly, row.pkg);
		CHECK(n.ok);
		CHECK(n.nightly);
		CHECK(n.build.base == "2026.9" && !n.build.release);
		CHECK(n.build.date == 20261027);
		CHECK(n.build.commit == NIGHTLY_SHA);
		CHECK(n.asset.sha256.startsWith(row.nig));
		CHECK(n.asset.name.startsWith("xpeccy-plus-2026.9-dev+20261027-"));
	}
	// a build no package was made for, or one that is not there
	CHECK(up_parse(latest, "").ok && up_parse(latest, "").asset.url.isEmpty());
	CHECK(up_parse(latest, "win-arm64").asset.url.isEmpty());
	CHECK(up_parse(latest, "x64").asset.url.isEmpty());

	// broken and error answers
	CHECK(!up_parse("", "win-x64").ok);
	CHECK(!up_parse("<html>", "win-x64").ok);
	upRelease lim = up_parse("{\"message\":\"API rate limit exceeded for 1.2.3.4.\"}", "win-x64");
	CHECK(!lim.ok && lim.error.startsWith("API rate limit exceeded"));
	CHECK(!up_parse("{\"tag_name\":\"latest-build\",\"assets\":[]}", "win-x64").ok);
	CHECK(!up_parse("{\"tag_name\":\"nightly\",\"assets\":[]}", "win-x64").ok);

	// compare/<self>...<nightly>: the nightly ahead of a build, and behind a newer one
	CHECK(up_compare_ahead(load(dir + "/compare-ahead.json")));
	CHECK(!up_compare_ahead(load(dir + "/compare-behind.json")));
	CHECK(!up_compare_ahead("{\"status\":\"diverged\"}"));
	CHECK(!up_compare_ahead("{\"message\":\"Not Found\"}"));
	CHECK(!up_compare_ahead(""));

	// a release with one package missing
	QJsonObject obj = QJsonDocument::fromJson(latest).object();
	obj["assets"] = QJsonArray();
	upRelease bare = up_parse(QJsonDocument(obj).toJson(), "win-x64");
	CHECK(bare.ok && bare.asset.url.isEmpty());

	// the packages as named since 2026.9 find nothing in a release from before
	CHECK(up_parse(latest, "win-x64-legacy").asset.url.isEmpty());
	CHECK(up_parse(latest, "win-x86-legacy").asset.url.isEmpty());
	CHECK(up_parse(latest, "macos-arm64").asset.url.isEmpty());
}

// The nightly of 2026-10-27 with its files named the 2026.9 way, plus the copies under the
// old names nightly.yml adds for the builds from before (win-x64-qt6, win-x86)
static QByteArray renamed_nightly(const QByteArray& json) {
	static const struct {const char* from; const char* to; bool copy;} ren[] = {
		{"-win-x64.zip", "-win-x64-legacy.zip", false},
		{"-win-x64-qt6.zip", "-win-x64.zip", false},
		{"-win-x86.zip", "-win-x86-legacy.zip", false},
		{"-macos-arm64-qt6.dmg", "-macos-arm64.dmg", false},
		{"-win-x64.zip", "-win-x64-qt6.zip", true},		// of the renamed: Qt 6
		{"-win-x86-legacy.zip", "-win-x86.zip", true},
	};
	QJsonObject obj = QJsonDocument::fromJson(json).object();
	QJsonArray assets = obj["assets"].toArray();
	for (const auto& r : ren) {
		for (int i = 0; i < assets.size(); i++) {
			QJsonObject a = assets[i].toObject();
			QString name = a["name"].toString();
			if (!name.endsWith(r.from)) continue;
			name = name.left(name.size() - (int)strlen(r.from)) + r.to;
			a["name"] = name;
			QString url = a["browser_download_url"].toString();
			a["browser_download_url"] = url.left(url.lastIndexOf('/') + 1) + QString(name).replace("+", "%2B");
			if (r.copy) {
				assets.append(a);
			} else {
				assets[i] = a;
			}
			break;
		}
	}
	obj["assets"] = assets;
	return QJsonDocument(obj).toJson();
}

static void test_renamed(const QString& dir) {
	QByteArray nightly = renamed_nightly(load(dir + "/nightly-20261027.json"));
	// the sha256 heads of the 2026-10-27 files: Qt 6 x64, Qt 5 x64, Qt 5 x86, macOS
	static const char* qt6 = "31955ca6ac680493";
	static const char* qt5x64 = "2f19fad5ce279015";
	static const char* qt5x86 = "a0a65203458d9422";
	static const struct {const char* pkg; const char* sha; const char* file;} tab[] = {
		// win-x64 is also the old Qt 5 x64 build's name: that one takes Qt 6 now,
		// as nightly.yml says (builds since never change Qt by an update: updatewin.cpp)
		{"win-x64", qt6, "-win-x64.zip"},
		{"win-x64-legacy", qt5x64, "-win-x64-legacy.zip"},
		{"win-x86-legacy", qt5x86, "-win-x86-legacy.zip"},
		{"macos-arm64", "db834ea914d80748", "-macos-arm64.dmg"},
		{"linux-x86_64", "22bfdf42bd3f4e14", "-linux-x86_64.AppImage"},
		// builds from before: Qt 6 and x86 by the copies, macOS by the page
		{"win-x64-qt6", qt6, "-win-x64-qt6.zip"},
		{"win-x86", qt5x86, "-win-x86.zip"},
		{"macos-arm64-qt6", NULL, NULL},
		// an ARM64 build takes no x64 file from a nightly with none of its own
		{"win-arm64", NULL, NULL},
	};
	for (const auto& row : tab) {
		upRelease n = up_parse(nightly, row.pkg);
		CHECK(n.ok && n.nightly);
		CHECK(n.build.base == "2026.9" && n.build.date == 20261027);
		if (!row.sha) {
			CHECK(n.asset.url.isEmpty());
			continue;
		}
		CHECK(n.asset.sha256.startsWith(row.sha));
		CHECK(n.asset.name == QString("xpeccy-plus-2026.9-dev+20261027") + row.file);
		CHECK(n.asset.url.endsWith(QString(n.asset.name).replace("+", "%2B")));
	}
}

static void test_compare() {
	CHECK(up_calver_cmp("2026.9", "2026.10") < 0);
	CHECK(up_calver_cmp("2026.8.2", "2026.8") > 0);
	CHECK(up_calver_cmp("2026.8.0", "2026.8") == 0);
	CHECK(up_calver_cmp("2027.1", "2026.12") > 0);

	upBuild latest = rel("2026.8.2");
	CHECK(up_newer(latest, rel("2026.8.1")));
	CHECK(up_newer(latest, rel("2026.8")));
	CHECK(!up_newer(latest, rel("2026.8.2")));
	CHECK(!up_newer(latest, rel("2026.9")));
	// a dev build is older than the release of its base, newer than the one before
	CHECK(up_newer(latest, dev("2026.8.2", 20261026)));
	CHECK(!up_newer(latest, dev("2026.9", 20261027, NIGHTLY_SHA)));

	upBuild night = dev("2026.9", 20261027, NIGHTLY_SHA);
	CHECK(up_newer(night, dev("2026.9", 20261026, OTHER_SHA)));
	CHECK(!up_newer(night, dev("2026.9", 20261027, NIGHTLY_SHA)));
	CHECK(up_newer(night, dev("2026.9", 20261027, OTHER_SHA)));	// forced rebuild the same day
	CHECK(!up_newer(night, dev("2026.9", 20261027, "")));	// no commit known: not offered
	CHECK(!up_newer(night, dev("2026.9", 20261028, OTHER_SHA)));
	CHECK(up_newer(night, rel("2026.8.2")));
	CHECK(!up_newer(night, rel("2026.9")));
	CHECK(up_newer(dev("2026.10", 20261108), rel("2026.9")));

	// the same day from another commit: GitHub's compare decides
	CHECK(up_same_day(night, dev("2026.9", 20261027, OTHER_SHA)));
	CHECK(!up_same_day(night, dev("2026.9", 20261026, OTHER_SHA)));
	CHECK(!up_same_day(night, dev("2026.9", 20261027, NIGHTLY_SHA)));
	CHECK(!up_same_day(latest, rel("2026.8.1")));

	CHECK(up_default_channel(rel("2026.8.2")) == UP_RELEASES);
	CHECK(up_default_channel(night) == UP_DEV);
}

static void test_pick() {
	// Nightly asks both: a release can be fresher than the last nightly
	QList<upRelease> both;
	both << answer(rel("2026.9")) << answer(dev("2026.9", 20261106, NIGHTLY_SHA));
	CHECK(up_pick(both, dev("2026.9", 20261101, OTHER_SHA)) == 0);
	CHECK(up_pick(both, rel("2026.9")) == -1);
	CHECK(up_pick(both, rel("2026.8.2")) == 0);

	QList<upRelease> later;
	later << answer(rel("2026.9")) << answer(dev("2026.10", 20261108, NIGHTLY_SHA));
	CHECK(up_pick(later, rel("2026.9")) == 1);
	CHECK(up_pick(later, dev("2026.9", 20261101, OTHER_SHA)) == 1);
	CHECK(up_pick(later, dev("2026.10", 20261108, NIGHTLY_SHA)) == -1);

	// a failed answer is skipped, the other still counts
	QList<upRelease> half;
	half << upRelease() << answer(dev("2026.10", 20261108, NIGHTLY_SHA));
	CHECK(up_pick(half, rel("2026.9")) == 1);
	CHECK(up_pick(QList<upRelease>(), rel("2026.9")) == -1);
}

static const char* LOG_NEW =
	"# Changelog\n\nPreamble.\n\n"
	"## Unreleased\n\n### Added\n\n- **Gamma**: third,\n  two lines.\n\n"
	"## 2026.9 - 2026-11-07\n\n### Added\n\n- **Alpha**: first.\n\n- **Beta**: second.\n\n### Fixed\n\n- **Delta**: fixed.\n\n"
	"## 2026.8.2 - 2026-10-27\n\n### Fixed\n\n- **Old**: old one.\n";

static void test_whats_new(const QString& changelog) {
	// a release build sees the versions above its own
	QString out = up_whats_new(LOG_NEW, "", rel("2026.8.2"));
	CHECK(out.contains("## Unreleased") && out.contains("**Gamma**") && out.contains("  two lines."));
	CHECK(out.contains("## 2026.9 - 2026-11-07") && out.contains("**Alpha**") && out.contains("### Fixed"));
	CHECK(!out.contains("2026.8.2") && !out.contains("**Old**") && !out.contains("Preamble"));
	CHECK(up_whats_new(LOG_NEW, "", rel("2026.9")).startsWith("## Unreleased"));
	CHECK(!up_whats_new(LOG_NEW, "", rel("2026.9")).contains("Alpha"));

	// a dev build of 2026.9 had Alpha already (worded with other line breaks) and Delta
	QByteArray mine = "## Unreleased\n\n### Added\n\n- **Alpha**:\n  first.\n\n### Fixed\n\n- **Delta**: fixed.\n";
	out = up_whats_new(LOG_NEW, mine, dev("2026.9", 20261101));
	CHECK(out.contains("**Beta**") && out.contains("**Gamma**"));
	CHECK(!out.contains("**Alpha**") && !out.contains("**Delta**"));
	CHECK(!out.contains("### Fixed"));	// a group left empty goes
	CHECK(!out.contains("**Old**"));

	// everything known: nothing
	CHECK(up_whats_new(LOG_NEW, LOG_NEW, dev("2026.9", 20261101)).isEmpty());

	// the real one, CRLF or not: 2026.8.1 sees 2026.8.2 and what is unreleased
	QByteArray real = load(changelog);
	out = up_whats_new(real, "", rel("2026.8.1"));
	CHECK(out.startsWith("## Unreleased"));
	CHECK(out.contains("## 2026.8.2 - 2026-10-27"));
	CHECK(!out.contains("## 2026.8.1"));
	CHECK(!out.contains("Keep a Changelog"));
	QByteArray crlf = real;
	crlf.replace("\r\n", "\n").replace("\n", "\r\n");
	CHECK(up_whats_new(crlf, "", rel("2026.8.1")) == out);
}

// --- putting a new build in place ---

static void put(const QString& path, const QByteArray& data) {
	QDir().mkpath(QFileInfo(path).path());
	QFile file(path);
	if (file.open(QFile::WriteOnly | QFile::Truncate)) file.write(data);
}

static QByteArray get(const QString& path) {
	QFile file(path);
	return file.open(QFile::ReadOnly) ? file.readAll() : QByteArray("<none>");
}

static QString sha(const QByteArray& data) {
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

#define STAGE "update.new/xpeccy-plus-2.0-win-x64"

// an install of 1.0 with the user's own files, and 2.0 unpacked beside it
static void make_trees(const QString& app, bool manifest) {
	QDir(app).removeRecursively();
	put(app + "/xpeccy-plus.exe", "exe 1");
	put(app + "/Qt.dll", "qt 1");
	put(app + "/gone.dll", "gone 1");			// 2.0 has it no more
	put(app + "/mine.dll", "mine");				// no release's
	put(app + "/config/keymaps/a.map", "a 1");		// as shipped
	put(app + "/config/keymaps/b.map", "b user");		// shipped as "b 1", changed
	put(app + "/config/styles/c.qss", "c 1");		// the same in 2.0
	put(app + "/config/styles/d.qss", "d user");		// shipped as "d 1", changed; 2.0 has "d 1"
	put(app + "/config/config.conf", "settings");
	put(app + "/config/nvram/x.cmos", "cmos");
	if (manifest) {
		QByteArray man;
		const char* rows[][2] = {{"xpeccy-plus.exe", "exe 1"}, {"Qt.dll", "qt 1"}, {"gone.dll", "gone 1"},
			{"config/keymaps/a.map", "a 1"}, {"config/keymaps/b.map", "b 1"}, {"config/styles/c.qss", "c 1"},
			{"config/styles/d.qss", "d 1"}};
		for (auto& row : rows) man += sha(row[1]).toLatin1() + "  " + row[0] + "\n";
		put(app + "/manifest.sha256", man);
	}
	QString st = app + "/" STAGE;
	put(st + "/xpeccy-plus.exe", "exe 2");
	put(st + "/Qt.dll", "qt 2");
	put(st + "/config/keymaps/a.map", "a 2");
	put(st + "/config/keymaps/b.map", "b 2");
	put(st + "/config/styles/c.qss", "c 1");
	put(st + "/config/styles/d.qss", "d 1");
	put(st + "/config/roms/new.rom", "rom");
	put(st + "/manifest.sha256", "the new one");
}

static QString plan_text(const QList<upStep>& plan) {
	static const char* kind[] = {"add", "replace", "keep", "drop"};
	QStringList out;
	foreach(const upStep& st, plan) out.append(QString("%0 %1").arg(kind[st.kind], st.rel));
	return out.join(", ");
}

// the 1.0 install as it was
static bool tree_is_old(const QString& app) {
	return (get(app + "/xpeccy-plus.exe") == "exe 1") && (get(app + "/Qt.dll") == "qt 1")
		&& (get(app + "/gone.dll") == "gone 1") && (get(app + "/config/keymaps/a.map") == "a 1")
		&& (get(app + "/config/keymaps/b.map") == "b user") && !QFileInfo::exists(app + "/config/roms/new.rom")
		&& !QFileInfo::exists(app + "/xpeccy-plus.exe.old") && !QFileInfo::exists(app + "/config/update-2.0/keymaps/b.map");
}

static void test_apply() {
	QTemporaryDir tmp;
	QString app = tmp.path() + "/Xpeccy";

	make_trees(app, true);
	QList<upStep> plan = up_plan(app, app + "/" STAGE);
	CHECK(plan_text(plan) == "replace Qt.dll, replace config/keymaps/a.map, keep config/keymaps/b.map, "
		"add config/roms/new.rom, replace manifest.sha256, replace xpeccy-plus.exe, drop gone.dll");
	QString err;
	CHECK(up_apply(app, app + "/" STAGE, "2.0", plan, &err));
	CHECK(get(app + "/xpeccy-plus.exe") == "exe 2");
	CHECK(get(app + "/xpeccy-plus.exe.old") == "exe 1");
	CHECK(get(app + "/Qt.dll") == "qt 2");
	CHECK(get(app + "/config/keymaps/a.map") == "a 2");
	CHECK(get(app + "/config/keymaps/b.map") == "b user");
	CHECK(get(app + "/config/update-2.0/keymaps/b.map") == "b 2");
	CHECK(get(app + "/config/styles/c.qss") == "c 1");
	CHECK(get(app + "/config/styles/d.qss") == "d user");	// the release has nothing new for it
	CHECK(!QFileInfo::exists(app + "/config/update-2.0/styles/d.qss"));
	CHECK(get(app + "/config/roms/new.rom") == "rom");
	CHECK(get(app + "/config/config.conf") == "settings");
	CHECK(get(app + "/config/nvram/x.cmos") == "cmos");
	CHECK(get(app + "/mine.dll") == "mine");
	CHECK(!QFileInfo::exists(app + "/gone.dll"));
	CHECK(get(app + "/manifest.sha256") == "the new one");
	CHECK(QFileInfo::exists(app + "/" UP_JOURNAL));
	upResume res = up_resume(app);
	CHECK(res.state == UPR_DONE);
	CHECK(res.version == "2.0");
	CHECK(res.kept == QStringList() << "config/keymaps/b.map");
	CHECK(res.failed.isEmpty());
	CHECK(!QFileInfo::exists(app + "/xpeccy-plus.exe.old") && !QFileInfo::exists(app + "/gone.dll.old"));
	CHECK(!QFileInfo::exists(app + "/" UP_JOURNAL) && !QFileInfo::exists(app + "/" UP_STAGE));
	CHECK(up_resume(app).state == UPR_NONE);
	CHECK(QFileInfo(app).isDir() && QFileInfo(app).fileName() == "Xpeccy");

	// no manifest (an install from before them): every changed config file is the user's
	make_trees(app, false);
	plan = up_plan(app, app + "/" STAGE);
	CHECK(plan_text(plan) == "replace Qt.dll, keep config/keymaps/a.map, keep config/keymaps/b.map, "
		"add config/roms/new.rom, keep config/styles/d.qss, add manifest.sha256, replace xpeccy-plus.exe");

	// the unpacked build gone, or no program in it: nothing to do, nothing dropped
	make_trees(app, true);
	QDir(app + "/" STAGE).removeRecursively();
	CHECK(up_plan(app, app + "/" STAGE).isEmpty());
	put(app + "/" STAGE "/config/roms/new.rom", "rom");
	CHECK(up_plan(app, app + "/" STAGE).isEmpty());

	// a power cut after three renames: the next start puts everything back
	make_trees(app, true);
	plan = up_plan(app, app + "/" STAGE);
	CHECK(!up_apply(app, app + "/" STAGE, "2.0", plan, &err, 3));
	CHECK(err == "stopped");
	CHECK(QFileInfo::exists(app + "/" UP_JOURNAL));
	CHECK(!tree_is_old(app));
	res = up_resume(app);
	CHECK(res.state == UPR_UNDONE);
	CHECK(res.moved.size() == 3);
	CHECK(res.failed.isEmpty());
	CHECK(tree_is_old(app));
	CHECK(!QFileInfo::exists(app + "/" UP_JOURNAL) && !QFileInfo::exists(app + "/" UP_STAGE));

	// a rename that fails part way (a file stands where a folder must go): undone at once
	make_trees(app, true);
	put(app + "/sub", "a file");
	put(app + "/" STAGE "/sub/x.dll", "x");
	plan = up_plan(app, app + "/" STAGE);
	CHECK(!up_apply(app, app + "/" STAGE, "2.0", plan, &err));
	CHECK(err.startsWith("Cannot move"));
	CHECK(tree_is_old(app));
	CHECK(get(app + "/" STAGE "/xpeccy-plus.exe") == "exe 2");
	CHECK(!QFileInfo::exists(app + "/" UP_JOURNAL));

	// a .old file still held: left for the next start
	make_trees(app, true);
	plan = up_plan(app, app + "/" STAGE);
	CHECK(up_apply(app, app + "/" STAGE, "2.0", plan, &err));
	{
		QFile held(app + "/Qt.dll.old");
		held.open(QFile::ReadOnly);
		res = up_resume(app);
#ifdef _WIN32
		CHECK(res.failed == QStringList() << "Qt.dll.old");
		CHECK(QFileInfo::exists(app + "/" UP_LEFT));
#endif
	}
	CHECK(up_resume(app).state == UPR_NONE);
	CHECK(!QFileInfo::exists(app + "/Qt.dll.old") && !QFileInfo::exists(app + "/" UP_LEFT));
}

// --- an x64 build on an ARM64 machine: the native build wanted ---

#define NIGHTLY_1110_SHA "0d02dcdb4f21831890fa018dfdfe9f55ed975de3"

static void test_native(const QString& dir) {
	CHECK(up_native_package("win-x64", true) == "win-arm64");
	CHECK(up_native_package("win-x64-legacy", true) == "win-arm64");
	CHECK(up_native_package("win-x86-legacy", true) == "win-arm64");
	CHECK(up_native_package("win-arm64", true) == "win-arm64");
	CHECK(up_native_package("win-x64", false) == "win-x64");
	CHECK(up_native_package("win-x86-legacy", false) == "win-x86-legacy");
	CHECK(up_native_package("macos-arm64", true) == "macos-arm64");
	CHECK(up_native_package("linux-x86_64", true) == "linux-x86_64");
	CHECK(up_native_package("", true).isEmpty());

	// the nightly of 2026-11-10 has win-arm64; 2026.8.2 is from before it
	QByteArray nightly = load(dir + "/nightly-20261110.json");
	QByteArray latest = load(dir + "/latest-2026.8.2.json");
	upRelease n = up_parse_native(nightly, "win-x64", "win-arm64");
	CHECK(n.ok && n.native && n.nightly);
	CHECK(n.asset.name == "xpeccy-plus-2026.10-dev+20261110-win-arm64.zip");
	CHECK(n.asset.sha256.size() == 64);
	upRelease o = up_parse_native(nightly, "win-x64", "win-x64");
	CHECK(o.ok && !o.native && (o.asset.name == "xpeccy-plus-2026.10-dev+20261110-win-x64.zip"));
	upRelease x86 = up_parse_native(nightly, "win-x86-legacy", "win-arm64");
	CHECK(x86.ok && x86.native && (x86.asset.name == n.asset.name));
	// no native file in it: the build's own, as before
	upRelease l = up_parse_native(latest, "win-x64", "win-arm64");
	CHECK(l.ok && !l.native && l.asset.name.endsWith("-win-x64.zip"));
	CHECK(!up_parse_native("", "win-x64", "win-arm64").ok);
	CHECK(!up_parse_native("{\"message\":\"API rate limit exceeded\"}", "win-x64", "win-arm64").native);

	// this very nightly as the native build: a switch, no update
	upBuild self = dev("2026.10", 20261110, NIGHTLY_1110_SHA);
	QList<upRelease> list = QList<upRelease>() << l << n;
	CHECK(up_pick(list, self) == -1);
	CHECK(up_pick_same(list, self) == 1);
	// its own file only: nothing to switch to
	CHECK(up_pick_same(QList<upRelease>() << l << o, self) == -1);
	// the same day from another commit, maybe an earlier one: no switch
	CHECK(up_pick_same(list, dev("2026.10", 20261110, OTHER_SHA)) == -1);
	// an older build: an update (which brings the native file), not a switch
	upBuild older = dev("2026.10", 20261101, OTHER_SHA);
	CHECK(up_pick(list, older) == 1);
	CHECK(up_pick_same(list, older) == -1);
	// a release, the same as this one
	upRelease r = answer(rel("2026.10"));
	r.native = true;
	r.asset.url = "https://example.invalid/x.zip";
	CHECK(up_pick_same(QList<upRelease>() << r, rel("2026.10")) == 0);
	CHECK(up_pick_same(QList<upRelease>() << r, rel("2026.10.1")) == -1);
	CHECK(up_pick_same(QList<upRelease>() << r, dev("2026.10", 20261110)) == -1);
	r.asset.url.clear();
	CHECK(up_pick_same(QList<upRelease>() << r, rel("2026.10")) == -1);
}

#define ARM_STAGE "update.new/xpeccy-plus-2.0-win-arm64"

// an x64 install of 2.0, and the arm64 build of 2.0 unpacked beside it: other runtime
// DLLs, other plugins, licenses
static void make_native_trees(const QString& app) {
	QDir(app).removeRecursively();
	const char* rows[][2] = {{"xpeccy-plus.exe", "exe x64"}, {"Qt6Core.dll", "qt x64"},
		{"libstdc++-6.dll", "stdc++"}, {"libgcc_s_seh-1.dll", "gcc"}, {"styles/qwindowsvistastyle.dll", "vista"},
		{"config/keymaps/a.map", "a 1"}, {"config/keymaps/b.map", "b 1"}};
	QByteArray man;
	for (auto& row : rows) {
		put(app + "/" + row[0], row[1]);
		man += sha(row[1]).toLatin1() + "  " + row[0] + "\n";
	}
	put(app + "/manifest.sha256", man);
	put(app + "/config/keymaps/b.map", "b user");
	put(app + "/config/config.conf", "settings");
	QString st = app + "/" ARM_STAGE;
	put(st + "/xpeccy-plus.exe", "exe arm64");
	put(st + "/Qt6Core.dll", "qt arm64");
	put(st + "/libc++.dll", "c++");
	put(st + "/styles/qmodernwindowsstyle.dll", "modern");
	put(st + "/licenses/packages.txt", "list");
	put(st + "/config/keymaps/a.map", "a 1");
	put(st + "/config/keymaps/b.map", "b 1");
	put(st + "/manifest.sha256", "arm64's");
}

static void test_apply_native() {
	QTemporaryDir tmp;
	QString app = tmp.path() + "/Xpeccy";
	make_native_trees(app);
	QList<upStep> plan = up_plan(app, app + "/" ARM_STAGE);
	CHECK(plan_text(plan) == "replace Qt6Core.dll, add libc++.dll, add licenses/packages.txt, replace manifest.sha256, "
		"add styles/qmodernwindowsstyle.dll, replace xpeccy-plus.exe, drop libgcc_s_seh-1.dll, drop libstdc++-6.dll, "
		"drop styles/qwindowsvistastyle.dll");
	QString err;
	CHECK(up_apply(app, app + "/" ARM_STAGE, "2.0", plan, &err));
	CHECK(up_resume(app).state == UPR_DONE);
	QStringList files;
	QDirIterator it(app, QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext()) files.append(QDir(app).relativeFilePath(it.next()));
	files.sort();
	CHECK(files.join(", ") == "Qt6Core.dll, config/config.conf, config/keymaps/a.map, config/keymaps/b.map, "
		"libc++.dll, licenses/packages.txt, manifest.sha256, styles/qmodernwindowsstyle.dll, xpeccy-plus.exe");
	CHECK(get(app + "/xpeccy-plus.exe") == "exe arm64");
	CHECK(get(app + "/config/keymaps/b.map") == "b user");
	CHECK(get(app + "/config/config.conf") == "settings");

	// cut short half way: the x64 install back as it was. For a switch this tests up_resume()
	// alone: an x64 exe beside ARM64 Qt DLLs fails to load (0xC000007B) before main(), so the
	// rollback never runs there - the window is milliseconds. A same-kind update cut short
	// is rolled back by this same code
	make_native_trees(app);
	plan = up_plan(app, app + "/" ARM_STAGE);
	CHECK(!up_apply(app, app + "/" ARM_STAGE, "2.0", plan, &err, 6));
	upResume res = up_resume(app);
	CHECK(res.state == UPR_UNDONE && res.failed.isEmpty());
	CHECK(get(app + "/xpeccy-plus.exe") == "exe x64" && get(app + "/libstdc++-6.dll") == "stdc++");
	CHECK(get(app + "/styles/qwindowsvistastyle.dll") == "vista" && !QFileInfo::exists(app + "/libc++.dll"));
	CHECK(!QFileInfo::exists(app + "/licenses/packages.txt"));
}

int main(int argc, char** argv) {
	if (argc < 3) {
		printf("usage: update-check <data dir> <CHANGELOG.md>\n");
		return 2;
	}
	test_parse(argv[1]);
	test_renamed(argv[1]);
	test_compare();
	test_pick();
	test_whats_new(argv[2]);
	test_apply();
	test_native(argv[1]);
	test_apply_native();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
