// The update check's logic over saved GitHub answers (data/, fetched 2026-10-27) and
// changelogs: update-check <data dir> <CHANGELOG.md>

#include <QFile>
#include <QJsonArray>
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

int main(int argc, char** argv) {
	if (argc < 3) {
		printf("usage: update-check <data dir> <CHANGELOG.md>\n");
		return 2;
	}
	test_parse(argv[1]);
	test_compare();
	test_pick();
	test_whats_new(argv[2]);
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
