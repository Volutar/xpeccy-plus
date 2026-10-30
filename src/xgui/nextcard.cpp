// The ZX Spectrum Next's own SD card: made from a System/Next release - an archive
// of it beside the program, else downloaded from GitLab (xcore/download) - as a
// folder or an image, or one the user has. A release known here is checked
// before it is used; one that is not says so.

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

#include "xgui.h"
#include "../xcore/xcore.h"
#include "../xcore/download.h"
#include "../xcore/sysnext.h"

// what the path is, and so what the window does with it
enum {NC_FOLDER = 0, NC_IMAGE, NC_HAVE, NC_NONE};

class xNextCard : public QDialog {
	public:
		xNextCard(QWidget* p, bool asked);
		QString path;		// the card, once there is one
		bool own;		// made here
		void reject() override;
	private:
		QLineEdit* lePath;
		QToolButton* btnPath;
		QLabel* labState;	// what the path is: a card found, or one to make
		QTimer* chk;		// the path looked at once typing pauses
		QComboBox* cbSize;
		QComboBox* cbRel;
		QLabel* labFrom;
		QLabel* lab;
		QLabel* labLine;	// what the program says, as it goes
		QProgressBar* bar;
		QDialogButtonBox* box;
		QPushButton* btnGo;
		QList<snRelease> rels;
		xDownload dl;		// the archive
		xDownload tags;		// the tag list, looked up as the window opens
		bool running;
		QString tmp;		// <config>/sysnext.part/: the download and what it unpacks to
		QString archive;	// the archive in use
		QString target;		// the card being made
		snRelease rel;
		bool treeKnown;		// the archive's own checksum matched: the tree is the known one
		bool dropArchive;	// a downloaded archive not checked yet: a failure takes it away
		int kind;		// NC_*, from the path
		int making;		// the kind being made, while it runs
		void check();
		void formEnabled(bool on);
		void fillReleases();
		void showFrom();
		void browse();
		void go();
		void verify();
		void unpack();
		void place();
		void fail(const QString& msg);
		bool tick();
};

// for a card on another disk than the config folder, where a rename cannot go
static bool nc_copy_tree(const QString& src, const QString& dst, std::function<bool()> tick) {
	if (!QDir().mkpath(dst)) return false;
	QDirIterator it(src, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		QString from = it.next();
		QString to = dst + "/" + QDir(src).relativeFilePath(from);
		if (it.fileInfo().isDir()) {
			if (!QDir().mkpath(to)) return false;
		} else if (!QFile::copy(from, to) || !tick()) {
			return false;
		}
	}
	return true;
}

static QString nc_size_name(qint64 bytes) {
	return QString("%0 MB").arg((bytes + (1 << 19)) >> 20);
}

xNextCard::xNextCard(QWidget* p, bool asked):QDialog(p) {
	setWindowTitle("ZX Spectrum Next SD Card");
	own = false;
	running = false;
	treeKnown = false;
	dropArchive = false;
	kind = NC_NONE;
	making = NC_NONE;
	QVBoxLayout* lay = new QVBoxLayout(this);
	// the card in the config, shown as it is; opened on its own, the window says what is wrong with it
	QString card = QString::fromLocal8Bit(conf.next.card.c_str());
	QString start = card;
	// asked with a card in the slot, that card is the user's (the Next's own is unusable): it goes first
	QString slot = conf.zx->sdc->image ? QString::fromLocal8Bit(conf.zx->sdc->image) : QString();
	if (asked) {
		if (!slot.isEmpty()) start = slot;
		QString why = !slot.isEmpty() ? QString("There is no System/Next on the card in the slot, %0, so the Next cannot start from it. Pick another card, or make one.")
				.arg(QDir::toNativeSeparators(slot).toHtmlEscaped())
			: card.isEmpty() ? QString("The Next has no card to start from yet.")
			: !QFileInfo::exists(card) ? QString("The Next's card is not at %0 any more. Pick it where it is now, or make another.")
				.arg(QDir::toNativeSeparators(card).toHtmlEscaped())
			: QString("There is no System/Next on the Next's card, %0, so the Next cannot start from it. Pick another card, or make one.")
				.arg(QDir::toNativeSeparators(card).toHtmlEscaped());
		QLabel* labWhy = new QLabel("<b>" + why + "</b>");
		labWhy->setWordWrap(true);
		labWhy->setMinimumWidth(460);
		lay->addWidget(labWhy);
	} else if (!QFileInfo::exists(card)) {
		start.clear();
	}
	if (start.isEmpty()) start = sn_default_path(false);
	QLabel* intro = new QLabel(QString("The Next starts from an SD card with System/Next on it: its ROMs, NextZXOS and "
		"the programs that come with it. It is not part of Xpeccy+. A card is made here from the official "
		"release, unchanged - an archive of it beside the program, or downloaded from "
		"<a href=\"%0\">%1</a>.").arg(sn_repo_url()).arg(QString(sn_repo_url()).remove("https://")));
	intro->setWordWrap(true);
	intro->setTextInteractionFlags(Qt::TextBrowserInteraction);
	intro->setOpenExternalLinks(true);
	intro->setMinimumWidth(460);
	lay->addWidget(intro);
	xOptSheet sheet;
	QWidget* pw = new QWidget;
	QVBoxLayout* pv = new QVBoxLayout(pw);
	pv->setContentsMargins(0, 0, 0, 0);
	QHBoxLayout* pl = new QHBoxLayout;
	lePath = new QLineEdit;
	lePath->setMinimumWidth(280);
	lePath->setPlaceholderText("A card with System/Next, or where to make one");
	btnPath = new QToolButton;
	btnPath->setIcon(QIcon(":/images/fileopen.png"));
	btnPath->setToolTip("Pick a folder or an image");
	pl->addWidget(lePath, 1);
	pl->addWidget(btnPath);
	pv->addLayout(pl);
	labState = new QLabel;
	labState->setWordWrap(true);
	labState->setTextFormat(Qt::RichText);
	labState->setMinimumHeight(labState->fontMetrics().lineSpacing() * 2);	// the longest says it in two
	labState->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	pv->addWidget(labState);
	sheet.field("Path", pw);
	cbSize = new QComboBox;
	for (int gb : {1, 2, 4, 8, 16})
		cbSize->addItem(QString("%0 GB").arg(gb), (qint64)gb << 30);
	cbSize->setCurrentIndex(2);
	sheet.field("Size", cbSize, "On the disk it takes only what is written to it");
	cbRel = new QComboBox;
	sheet.field("Release", cbRel);
	labFrom = new QLabel;
	labFrom->setWordWrap(true);
	sheet.field("From", labFrom);
	lay->addWidget(sheet.body);
	lab = new QLabel;
	lab->setWordWrap(true);
	lab->hide();
	lay->addWidget(lab);
	bar = new QProgressBar;
	bar->setTextVisible(false);
	bar->hide();
	lay->addWidget(bar);
	labLine = new QLabel;
	QFont fnt = labLine->font();
	fnt.setItalic(true);
	labLine->setFont(fnt);
	labLine->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	labLine->hide();
	lay->addWidget(labLine);
	box = new QDialogButtonBox(QDialogButtonBox::Cancel);
	btnGo = box->addButton("Create Card", QDialogButtonBox::AcceptRole);
	btnGo->setDefault(true);
	btnGo->setIcon(QIcon(":/images/ok-apply.png"));
	box->button(QDialogButtonBox::Cancel)->setIcon(QIcon(":/images/cancel.png"));
	lay->addWidget(box);
	// named for the control port's set-value and click-widget
	lePath->setObjectName("nextCardPath");
	labState->setObjectName("nextCardState");
	cbSize->setObjectName("nextCardSize");
	cbRel->setObjectName("nextCardRelease");
	btnGo->setObjectName("nextCardGo");
	connect(box, &QDialogButtonBox::rejected, this, &xNextCard::reject);
	connect(btnGo, &QPushButton::clicked, this, &xNextCard::go);
	connect(btnPath, &QPushButton::clicked, this, &xNextCard::browse);
	connect(cbRel, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {showFrom();});
	// an image is opened to be looked at, so not at every key typed
	chk = new QTimer(this);
	chk->setSingleShot(true);
	chk->setInterval(250);
	connect(chk, &QTimer::timeout, this, &xNextCard::check);
	connect(lePath, &QLineEdit::textChanged, this, [this]() {
		btnGo->setEnabled(false);	// until the new path is looked at
		chk->start();
	});
	// the folder with other files: the card made in a new folder inside it
	connect(labState, &QLabel::linkActivated, this, [this]() {
		lePath->setText(QDir::toNativeSeparators(QDir(QDir::fromNativeSeparators(lePath->text().trimmed())).filePath("next-sd")));
		check();
	});
	rels = sn_releases();
	fillReleases();
	lePath->setText(start);
	check();
	// releases newer than the ones known here, if GitLab can be reached; quietly not otherwise
	tags.toMemory(sn_tags_url(), 15);
	dl_follow(this, &tags, nullptr, nullptr, [this](const dlStatus& st) {
		if (st.state != DL_DONE) return;
		QString was = cbRel->currentData().toString();
		sn_add_tags(&rels, tags.data());
		fillReleases();
		int idx = cbRel->findData(was);
		if (idx >= 0) cbRel->setCurrentIndex(idx);
	});
}

void xNextCard::fillReleases() {
	QSignalBlocker blk(cbRel);
	cbRel->clear();
	for (const snRelease& r : rels)
		cbRel->addItem(QString("System/Next %0%1").arg(r.release).arg(r.checked() ? "" : " (not checked)"), r.release);
	// the newest one known here, unless an archive of another is beside the program
	int pick = -1;
	for (int i = 0; i < rels.size(); i++) {
		if (!rels[i].local.isEmpty() && (pick < 0)) pick = i;
	}
	for (int i = 0; (pick < 0) && (i < rels.size()); i++) {
		if (rels[i].checked()) pick = i;
	}
	cbRel->setCurrentIndex(qMax(pick, 0));
	showFrom();
}

void xNextCard::showFrom() {
	int idx = cbRel->currentIndex();
	if ((idx < 0) || (idx >= rels.size())) {
		labFrom->clear();
		return;
	}
	const snRelease& r = rels[idx];
	QString from = !r.local.isEmpty()
		? QString(r.cached ? "%0, downloaded before" : "%0, beside the program").arg(QFileInfo(r.local).fileName())
		: QString("gitlab.com%0").arg(r.bytes ? QString(", about %0").arg(nc_size_name(r.bytes)) : QString());
	if (!r.checked()) from += "<br><i>No checksum of this release is known here: it is taken as it comes.</i>";
	labFrom->setText(from);
}

// The path decides: System/Next on it is a card to use; a new or empty folder, or a new
// .img, is where one is made; anything else is left alone and the line says why.
void xNextCard::check() {
	chk->stop();
	QString at = QDir::cleanPath(QDir::fromNativeSeparators(lePath->text().trimmed()));
	QFileInfo inf(at);
	QString say;
	kind = NC_NONE;
	if (at.isEmpty()) {
		say = "Say where the card is, or where to make one.";
	} else if (inf.exists() && sn_card_ok(at)) {
		kind = NC_HAVE;
		say = inf.isDir() ? "<b>Found</b> a card with System/Next: the Next starts from it."
			: "<b>Found</b> an image with System/Next: the Next starts from it.";
	} else if (inf.isDir()) {
		if (QDir(at).isEmpty()) {
			kind = NC_FOLDER;
			say = "<b>New card</b>: made in this empty folder, with the release below.";
		} else {
			say = "This folder has other files and no System/Next. Pick an empty one, "
				"or make the card in <a href=\"next-sd\">next-sd inside it</a>.";
		}
	} else if (inf.exists()) {
		say = "This file has no System/Next on it: the Next cannot start from it. "
			"Pick another, or type a new name ending in .img.";
	} else if (at.endsWith(".img", Qt::CaseInsensitive)) {
		kind = NC_IMAGE;
		say = "<b>New card</b>: an image is made here, with the release and size below.";
	} else {
		kind = NC_FOLDER;
		say = "<b>New card</b>: a folder is made here, with the release below.";
	}
	labState->setText(say);
	// a line longer than the room kept for it: the window grows, not the text cut
	int more = labState->heightForWidth(labState->width()) - labState->height();
	if (isVisible() && (more > 0)) resize(width(), height() + more);
	bool make = (kind == NC_FOLDER) || (kind == NC_IMAGE);
	cbSize->setEnabled(kind == NC_IMAGE);
	cbRel->setEnabled(make);
	labFrom->setEnabled(make);
	btnGo->setText((kind == NC_HAVE) ? "Use This Card" : "Create Card");
	btnGo->setEnabled(kind != NC_NONE);
}

// the form, while a card is being made: what the path allows is looked at again after
void xNextCard::formEnabled(bool on) {
	QWidget* form[] = {lePath, btnPath, cbSize, cbRel, btnGo};
	for (QWidget* w : form) w->setEnabled(on);
	if (on) check();
}

// a folder or an image, one there or a new one: the path field then says which
void xNextCard::browse() {
	QMenu menu(this);
	QAction* dir = menu.addAction(QIcon(":/images/fileopen.png"), "Folder...");
	QAction* img = menu.addAction(QIcon(":/images/sdcard.png"), "Image...");
	QAction* act = menu.exec(btnPath->mapToGlobal(QPoint(0, btnPath->height())));
	QString at = QDir::fromNativeSeparators(lePath->text().trimmed());
	QString got;
	if (act == dir) {
		QString from = QFileInfo(at).isDir() ? at : QFileInfo(at).path();
		got = QFileDialog::getExistingDirectory(this, "The card's folder, or one to make it in", from,
			QFileDialog::DontUseNativeDialog | QFileDialog::ShowDirsOnly);
	} else if (act == img) {
		QFileDialog dlg(this, "The card's image, or a name for a new one");
		dlg.setOption(QFileDialog::DontUseNativeDialog);
		dlg.setOption(QFileDialog::DontConfirmOverwrite);	// one there is used, never written over
		dlg.setAcceptMode(QFileDialog::AcceptSave);
		dlg.setNameFilters(QStringList() << "Images (*.img)" << "All files (*)");
		dlg.setDefaultSuffix("img");
		dlg.setLabelText(QFileDialog::Accept, "Choose");
		if (QFileInfo(at).isDir()) {
			dlg.setDirectory(at);
		} else if (!at.isEmpty()) {
			dlg.setDirectory(QFileInfo(at).path());
			if (at.endsWith(".img", Qt::CaseInsensitive)) dlg.selectFile(QFileInfo(at).fileName());
		}
		if (dlg.exec() && !dlg.selectedFiles().isEmpty()) got = dlg.selectedFiles().first();
	}
	if (got.isEmpty()) return;
	lePath->setText(QDir::toNativeSeparators(got));
	check();
}

bool xNextCard::tick() {
	QApplication::processEvents();
	return running;
}

void xNextCard::go() {
	check();		// as it is now, not as it was when last typed
	if (kind == NC_NONE) return;
	target = QDir::cleanPath(QDir::fromNativeSeparators(lePath->text().trimmed()));
	if (kind == NC_HAVE) {
		path = QDir::toNativeSeparators(target);
		own = false;
		accept();
		return;
	}
	making = kind;
	lab->show();
	QFileInfo inf(target);
	if (!QDir().mkpath(inf.path())) {
		lab->setText("Cannot make the folder this goes into.");
		return;
	}
	int idx = cbRel->currentIndex();
	if ((idx < 0) || (idx >= rels.size())) return;
	rel = rels[idx];
	running = true;
	treeKnown = false;
	formEnabled(false);
	bar->setRange(0, 0);
	bar->show();
	labLine->clear();
	labLine->show();
	tmp = QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/sysnext.part/";
	QDir(tmp).removeRecursively();
	if (!QDir().mkpath(tmp + "x")) {
		fail("Cannot write into the config folder");
		return;
	}
	if (!rel.local.isEmpty()) {
		archive = rel.local;
		dropArchive = rel.cached;
		verify();
		return;
	}
	// kept in the config folder for the next card; under another name until it is whole,
	// which a download cut short leaves for the next try to continue
	QDir().mkpath(sn_cache_dir());
	archive = sn_cache_dir() + sn_archive_name(rel);
	QString part = archive + ".part";
	lab->setText(QString("Downloading System/Next %0").arg(rel.release));
	dl.toFile(sn_archive_url(rel), part);
	dl_follow(this, &dl, bar, labLine, [this, part](const dlStatus& st) {
		if (!running) return;		// cancelled
		if (st.state == DL_FAIL) fail(st.error);
		if (st.state != DL_DONE) return;
		QFile::remove(archive);
		if (!QFile::rename(part, archive)) {
			QFile::remove(part);
			fail("Cannot write into the config folder");
			return;
		}
		labLine->clear();
		dropArchive = true;
		verify();
	}, rel.bytes);	// GitLab makes the archive as it sends it and gives no length
}

// the archive's own checksum, when one is known: a match settles it. GitLab builds the
// archive on request, so a mismatch is only settled by the tree after unpacking.
void xNextCard::verify() {
	if (!rel.archiveSha.isEmpty() && archive.endsWith(".tar.gz")) {
		lab->setText("Checking the archive");
		bar->setRange(0, 0);
		QString sum = sn_file_hash(archive, [this]() {return tick();});
		if (!running) {
			QDir(tmp).removeRecursively();
			return;
		}
		treeKnown = (sum == rel.archiveSha);
	}
	unpack();
}

void xNextCard::unpack() {
	lab->setText("Unpacking");
	bar->setRange(0, 1000);
	QString err;
	bool ok = sn_unpack_archive(archive, tmp + "x", &err, [this](qint64 done, qint64 total) {
		bar->setValue(total ? int(1000.0 * done / total) : 0);
		return tick();
	});
	if (!running) {
		QDir(tmp).removeRecursively();
		return;
	}
	if (!ok) {
		fail(err);
		return;
	}
	place();
}

void xNextCard::place() {
	QString root = sn_unpacked_root(tmp + "x");
	if (!QFileInfo(root + "/machines/next/enNextZX.rom").isFile()) {
		fail("This archive is not a System/Next release");
		return;
	}
	if (treeKnown || !rel.checked()) dropArchive = false;
	if (!treeKnown && rel.checked()) {
		lab->setText("Checking the files");
		bar->setRange(0, 0);
		QString sum = sn_tree_hash(root, [this]() {return tick();});
		if (!running) {
			QDir(tmp).removeRecursively();
			return;
		}
		if (sum != rel.treeSha) {
			fail(QString("The files are not those of System/Next %0: the archive is damaged or changed").arg(rel.release));
			return;
		}
		dropArchive = false;
	}
	if (making == NC_FOLDER) {
		lab->setText("Putting the card in place");
		if (QFileInfo(target).isDir()) QDir().rmdir(target);	// checked empty
		if (!QDir().rename(root, target) && !nc_copy_tree(root, target, [this]() {return tick();})) {
			if (running) QDir(target).removeRecursively();		// the half copied; a cancel has it already
			if (running) fail("Cannot put the files in " + QDir::toNativeSeparators(target));
			return;
		}
	} else {
		lab->setText("Making the image");
		bar->setRange(0, 100);
		QString part = target + ".part~";
		QString err;
		bool ok = sn_make_image(root, part, cbSize->currentData().toLongLong(), &err, [this](int pct) {
			bar->setValue(pct);
			return tick();
		});
		if (!running) {
			QDir(tmp).removeRecursively();
			return;
		}
		if (!ok || !QFile::rename(part, target)) {
			QFile::remove(part);
			fail(ok ? QString("Cannot write ") + QDir::toNativeSeparators(target) : err);
			return;
		}
	}
	QDir(tmp).removeRecursively();
	running = false;
	path = QDir::toNativeSeparators(target);
	own = true;
	accept();
}

void xNextCard::fail(const QString& msg) {
	running = false;
	if (!tmp.isEmpty()) QDir(tmp).removeRecursively();
	QString more;
	if (dropArchive) {		// downloaded and bad: the next try downloads it again
		QFile::remove(archive);
		dropArchive = false;
		more = "<br><br>The downloaded archive was taken away: Create Card downloads it again.";
		rels = sn_releases();
		fillReleases();
	}
	bar->hide();
	labLine->hide();
	lab->setText("No card was made.<br><br>" + msg.toHtmlEscaped() + more);
	formEnabled(true);
}

// cancel: whatever runs is stopped and what it left is taken away
void xNextCard::reject() {
	bool was = running;
	running = false;
	dl.cancel();
	tags.cancel();
	if (was && !tmp.isEmpty()) QDir(tmp).removeRecursively();
	QDialog::reject();
}

bool next_card_dialog(QWidget* parent, QString* path, bool* own, bool asked) {
	xNextCard dlg(parent, asked);
	if (dlg.exec() != QDialog::Accepted) return false;
	*path = dlg.path;
	*own = dlg.own;
	return true;
}
