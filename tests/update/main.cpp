// The update check's logic over saved GitHub answers (data/, fetched 2026-10-27) and
// changelogs: update-check <data dir> <CHANGELOG.md>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

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
	CHECK(up_default_channel(night) == UP_NIGHTLY);
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

int main(int argc, char** argv) {
	if (argc < 3) {
		printf("usage: update-check <data dir> <CHANGELOG.md>\n");
		return 2;
	}
	test_parse(argv[1]);
	test_compare();
	test_pick();
	test_whats_new(argv[2]);
	test_apply();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
