#include "filer.h"
#include "xcore/rzxrecord.h"
#include "xcore/xcore.h"
#include "xcore/autostart.h"
#include "xcore/vfat_scan.h"
#include "xcore/filemachine.h"
#include "xcore/cheats.h"
#include "xcore/sound.h"
#include "xcore/arcpath.h"
#include "xcore/saves.h"
#include "xcore/siblings.h"
#include "libxpeccy/cpu/Z80/z80.h"
#include "libxpeccy/filetypes/szx.h"
#include "libxpeccy/xstate.h"
#include "xgui/xgui.h"

#include <QAbstractItemDelegate>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileSystemModel>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QMutex>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyleOptionViewItem>
#include <QTreeView>
#include <QTreeWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static QFileDialog* fileDlg = nullptr;
static QWidget* fileDlgParent = nullptr;

// Type by suffix: Qt 6 reads a file of an unknown suffix on the GUI thread to name its MIME type
class xFileTypes : public QFileIconProvider {
public:
	QString type(const QFileInfo& fi) const override {
		if (!fi.isFile()) return QFileIconProvider::type(fi);
		QString ext = fi.suffix().toUpper();
		return ext.isEmpty() ? QString("File") : ext + " File";
	}
	// With an icon theme Qt picks a file's icon by its MIME type, read from the file: once per suffix
	QIcon icon(const QFileInfo& fi) const override {
		if (!fi.isFile() || QIcon::themeName().isEmpty()) return QFileIconProvider::icon(fi);
		QString ext = fi.suffix().toLower();
		QMutexLocker lock(&mutex);
		auto it = icons.constFind(ext);
		if (it == icons.constEnd()) it = icons.insert(ext, QFileIconProvider::icon(fi));
		return *it;
	}
	QIcon icon(IconType type) const override {return QFileIconProvider::icon(type);}
private:
	mutable QMutex mutex;
	mutable QHash<QString, QIcon> icons;
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
// Qt 5 runs a regex per pattern per file on the GUI thread: its model gets no patterns, this keeps files by suffix
class xSuffixFilter : public QSortFilterProxyModel {
public:
	explicit xSuffixFilter(QObject* parent) : QSortFilterProxyModel(parent) {}
	// takes the patterns the dialog has just given its model
	void take() {
		QFileSystemModel* fsm = static_cast<QFileSystemModel*>(sourceModel());
		const QStringList pat = fsm->nameFilters();
		if (pat.isEmpty()) return;
		fsm->setNameFilters(QStringList());
		exts.clear();
		rest.clear();
		all = false;
		for (const QString& p : pat) {
			const QString ext = p.mid(2);
			bool plain = p.startsWith("*.") && !ext.isEmpty();	// one suffix, compared with the last one of a name
			for (QChar c : ext)
				plain &= (c != '*') && (c != '?') && (c != '[') && (c != '.');
			if (p == "*")
				all = true;
			else if (plain)
				exts.insert(ext.toLower());
			else
				rest.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(p), QRegularExpression::CaseInsensitiveOption));
		}
		invalidateFilter();
	}
	// the model's own order: folders first, names in natural order
	void sort(int column, Qt::SortOrder order) override {sourceModel()->sort(column, order);}
protected:
	bool filterAcceptsRow(int row, const QModelIndex& parent) const override {
		const QFileSystemModel* fsm = static_cast<const QFileSystemModel*>(sourceModel());
		if (all || !fsm->nameFilters().isEmpty()) return true;	// a pattern typed as the name: Qt filters
		const QModelIndex idx = fsm->index(row, 0, parent);
		if (fsm->isDir(idx)) return true;
		const QString name = fsm->fileName(idx);
		const int dot = name.lastIndexOf('.');
		if ((dot >= 0) && exts.contains(name.mid(dot + 1).toLower())) return true;
		for (const QRegularExpression& re : rest)
			if (re.match(name).hasMatch()) return true;
		return false;
	}
private:
	QSet<QString> exts;
	QList<QRegularExpression> rest;
	bool all = true;
};
#endif

// After every setNameFilter / selectNameFilter of ours: Qt 5's patterns go to the suffix filter
static void file_dialog_filtered(QFileDialog* dlg) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	if (xSuffixFilter* sf = dynamic_cast<xSuffixFilter*>(dlg->proxyModel())) sf->take();
#else
	Q_UNUSED(dlg)
#endif
}

// Must run after the views exist (a provider set before the model is dropped) and before the first listing.
// Uniform rows stop the views measuring every row; List View gets them only while hidden, as one width would cut names when shown.
static void file_dialog_tune(QFileDialog* dlg) {
	QTreeView* tree = dlg->findChild<QTreeView*>("treeView");
	if (!tree || dlg->property("xTuned").toBool()) return;	// native, or done
	dlg->setProperty("xTuned", true);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	if (!dlg->proxyModel() && dlg->findChild<QFileSystemModel*>()) {
		xSuffixFilter* sf = new xSuffixFilter(dlg);
		dlg->setProxyModel(sf);		// first: the views get it as their model
		sf->take();
		QObject::connect(dlg, &QFileDialog::filterSelected, sf, &xSuffixFilter::take);	// after Qt sets the user's pick
	}
#endif
	static xFileTypes* types = new xFileTypes;	// never freed: a dialog may outlive any owner at exit
	dlg->setIconProvider(types);
	tree->setUniformRowHeights(true);
	// The user's sort and view, over the header state Qt restores
	tree->sortByColumn(conf.fdlg.sort, conf.fdlg.desc ? Qt::DescendingOrder : Qt::AscendingOrder);
	dlg->setViewMode(conf.fdlg.list ? QFileDialog::List : QFileDialog::Detail);
	QObject::connect(tree->header(), &QHeaderView::sortIndicatorChanged, tree, [](int col, Qt::SortOrder ord) {
		conf.fdlg.sort = qBound(0, col, 3);
		conf.fdlg.desc = (ord == Qt::DescendingOrder);
	});
	QListView* list = dlg->findChild<QListView*>("listView");
	QStackedWidget* views = dlg->findChild<QStackedWidget*>("stackedWidget");
	if (!list || !views) return;
	list->setLayoutMode(QListView::Batched);
	auto fit = [list, views]() {
		bool shown = (views->currentWidget() == list->parentWidget());
		list->setUniformItemSizes(!shown);
		conf.fdlg.list = shown;
	};
	QObject::connect(views, &QStackedWidget::currentChanged, list, fit);
	fit();
}

// On each show: the sort and view last picked in any file dialog; sorted again only if changed
static void file_dialog_recall(QFileDialog* dlg) {
	QTreeView* tree = dlg->findChild<QTreeView*>("treeView");
	if (!tree) return;
	const Qt::SortOrder ord = conf.fdlg.desc ? Qt::DescendingOrder : Qt::AscendingOrder;
	if ((tree->header()->sortIndicatorSection() != conf.fdlg.sort) || (tree->header()->sortIndicatorOrder() != ord))
		tree->sortByColumn(conf.fdlg.sort, ord);
	const QFileDialog::ViewMode mode = conf.fdlg.list ? QFileDialog::List : QFileDialog::Detail;
	if (dlg->viewMode() != mode) dlg->setViewMode(mode);
}

// Built on first use, in the folder it is about to show: a Qt dialog built
// without one lists and watches the cwd on the GUI thread, and started from a
// 37k-file folder the window froze for seconds
static QFileDialog* file_dialog() {
	if (fileDlg) return fileDlg;
	QString dir = QString::fromLocal8Bit(conf.lastDir.c_str());
	if (dir.isEmpty() || !QFileInfo(dir).isDir())
		dir = QString::fromLocal8Bit(conf.path.confDir.c_str());
	fileDlg = new QFileDialog(fileDlgParent, QString(), dir);
	fileDlg->setWindowIcon(QIcon(":/images/logo.png"));
	fileDlg->setWindowModality(Qt::ApplicationModal);
	fileDlg->setOption(QFileDialog::HideNameFilterDetails, false);
	fileDlg->setOption(QFileDialog::DontConfirmOverwrite, false);
	fileDlg->setOption(QFileDialog::DontUseNativeDialog, true);	// builds the views
	file_dialog_tune(fileDlg);
	fileDlg->resize(700, 480);
	return fileDlg;
}

// new

typedef struct {
	int id;
	int ch;
	const char* ext;
	const char* filt;
	int(*load)(Computer*, const char*, int);
	int(*save)(Computer*, const char*, int);
	const char* name;
} xFileTypeInfo;

typedef struct {
	int id;
	const char* defext;
	int drv;
	const char* name;
	xFileTypeInfo* fti;
	int child[32];
} xFileGroupInfo;

typedef struct {
	int id;
	int child[32];
} xFileHWInfo;

// Save takes the settings the export dialog holds, so the two roads to a wav
// do not write different files
static int save_wav(Computer* comp, const char* name, int drv) {
	return saveWAVopt(comp, name, &conf.tape.exp);
}

// An .szx links to the tape and the disks it was taken with, by their names on
// this host, the way Spectaculator writes them.
// a tape put in from a save: its bytes, as its file may not be there any more
static QString carriedTapeName;
static QString carriedTapeExt;
static QByteArray carriedTape;

// The tape's file as it is can be carried in a save: one recorded onto since is
// not that file any more, and a wav of minutes is no thing to copy into every
// save. local: the file to read, empty when the bytes are in memory already.
static bool tape_carriable(Computer* comp, QString* local) {
	Tape* tape = comp->tape;
	local->clear();
	if (!tape->path || !tape->blkCount || tape->changed) return false;
	QString path = QString::fromLocal8Bit(tape->path);
	if (!carriedTape.isEmpty() && (path == carriedTapeName)) return true;
	QString err;
	*local = arc_local(path, &err);
	QFileInfo inf(*local);
	return !local->isEmpty() && inf.isFile() && (inf.size() <= (4 << 20));
}

static QByteArray carry_tape(Computer* comp, QString* ext) {
	QString local;
	if (!tape_carriable(comp, &local)) return QByteArray();
	if (local.isEmpty()) {
		*ext = carriedTapeExt;
		return carriedTape;
	}
	QFile file(local);
	if (!file.open(QIODevice::ReadOnly)) return QByteArray();
	QString entry;
	QString path = QString::fromLocal8Bit(comp->tape->path);
	*ext = QFileInfo(arc_split(path, nullptr, &entry) ? entry : path).suffix().toLower();
	return file.readAll();
}

// what an .szx saved now would carry, in words: "tape and disks", "" for nothing
static QString carry_words(Computer* comp) {
	QString local;
	bool tape = tape_carriable(comp, &local);
	int disks = 0;
	int drives = !comp->dif ? 0 : (comp->dif->type == DIF_BDI) ? 4 : (comp->dif->type == DIF_P3DOS) ? 2 : 0;
	for (int i = 0; i < drives; i++)
		if (comp->dif->flp[i]->insert && comp->dif->flp[i]->path) disks++;
	QString d = (disks > 1) ? "disks" : "disk";
	if (tape && disks) return "tape and " + d;
	return tape ? QString("tape") : disks ? d : QString();
}

// the file in memory, with the images the machine has in linked by name; carry:
// the tape and the disks inside it too, the disks as the drives hold them
static int build_szx(Computer* comp, szxBuf* b, bool carry = false) {
	QByteArray tape;
	QByteArray disk[4];
	const char* dptr[4];
	if (comp->tape->path && comp->tape->blkCount)
		tape = QDir::toNativeSeparators(QString::fromLocal8Bit(comp->tape->path)).toLocal8Bit();
	for (int i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		if (flp->insert && flp->path)
			disk[i] = QDir::toNativeSeparators(QString::fromLocal8Bit(flp->path)).toLocal8Bit();
		dptr[i] = disk[i].isEmpty() ? NULL : disk[i].constData();
	}
	QByteArray tbytes;
	szxCarry tc;
	szxCarry dc[4];
	memset(&tc, 0, sizeof(tc));
	memset(dc, 0, sizeof(dc));
	if (carry) {
		QString ext;
		if (!tape.isEmpty()) tbytes = carry_tape(comp, &ext);
		if (!tbytes.isEmpty()) {
			tc.data = (const unsigned char*)tbytes.constData();
			tc.size = tbytes.size();
			strncpy(tc.ext, ext.toLatin1().constData(), sizeof(tc.ext) - 1);
		}
		int plus3 = comp->dif && (comp->dif->type == DIF_P3DOS);
		for (int i = 0; (i < 4) && comp->dif && ((comp->dif->type == DIF_BDI) || (plus3 && (i < 2))); i++) {
			if (!dptr[i]) continue;
			dc[i].data = plus3 ? dsk_build(comp->dif->flp[i], &dc[i].size) : udi_build(comp->dif->flp[i], &dc[i].size);
			strcpy(dc[i].ext, plus3 ? "dsk" : "udi");
		}
	}
	szx_set_carry(&tc, dc);
	szx_set_links(tape.isEmpty() ? NULL : tape.constData(), dptr);
	szx_set_machine(conf.macId.c_str());
	int res = szx_build(comp, b);
	szx_set_links(NULL, NULL);
	szx_set_carry(NULL, NULL);
	for (int i = 0; i < 4; i++)
		free((void*)dc[i].data);
	return res;
}

// whether a snapshot saved through the table carries its media: save_file decides
static bool saveCarry = false;

static int save_szx(Computer* comp, const char* name, int drv) {
	szxBuf b;
	int res = build_szx(comp, &b, saveCarry);
	if (res == ERR_OK)
		res = szx_write(&b, name);
	return res;
}

// A .nex or .snx is started as on the machine: NextZXOS boots from the card, and on its
// command line .cd goes into the file's folder and NEXLOAD or SPECTRUM starts it - the
// program finds its own files, and an open file handle, where it expects them. A file
// on the card itself starts where it is, so what it saves stays there; one from
// elsewhere has its folder put onto the card as a folder of the root - with the file
// alone in it for a snapshot or a file out of a collection (PIN_MAX).
// a node of a folder card by its name, without case as the guest sees it; -1 if none
static int vf_child(vFat* vf, int dir, const char* name) {
	for (int i = (dir >= 0) ? vf->node[dir].child : -1; i >= 0; i = vf->node[i].next) {
		if (!QString::compare(QString::fromUtf8(vf->node[i].name), QString::fromUtf8(name), Qt::CaseInsensitive)) return i;
	}
	return -1;
}

// started from dir (a node of the card, cd the path NextZXOS goes there by): by its
// name, else by the card's 8.3 name for it, which any name has and the keyboard can type
static int nex_arm(Computer* comp, int dir, const QByteArray& cd, const QFileInfo& inf, int welcome, int snx, int powered) {
	vFat* vf = comp->sdc->vfat;
	if (autostart_arm_nex(comp, cd.constData(), inf.fileName().toLocal8Bit().constData(), welcome, snx, powered)) return ERR_OK;
	QByteArray want = inf.fileName().toUtf8();
	for (int i = (dir >= 0) ? vf->node[dir].child : -1; i >= 0; i = vf->node[i].next) {
		if (vf->node[i].isdir || (want != vf->node[i].name)) continue;
		QString sn = QString::fromLatin1(vf->node[i].sname, 8).trimmed();
		QString ext = QString::fromLatin1(vf->node[i].sname + 8, 3).trimmed();
		if (!ext.isEmpty()) sn += "." + ext;
		return autostart_arm_nex(comp, cd.constData(), sn.toLatin1().constData(), welcome, snx, powered) ? ERR_OK : ERR_NEX_NAME;
	}
	return ERR_NEX_NAME;
}

// The folder goes with the program for what it reads beside it (res/, data/ and the like),
// but not a collection: one of more entries than this puts the program's file on alone
#define PIN_MAX		2000

// more than PIN_MAX entries down the folder's tree; the count stops there
static bool pin_huge(const QString& dir) {
	QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDirIterator::Subdirectories);
	for (int n = 0; it.hasNext(); n++) {
		it.next();
		if (n >= PIN_MAX) return true;
	}
	return false;
}

// dir put on the card as name, whole or with only that file of it (none if empty), and
// said in the log with why
static bool pin_put(Computer* comp, const QString& dir, const QString& name, const QString* only, const QString& why) {
	int n = 0;
	if (!sdc_mount_over(comp->sdc, dir, name, only, &n)) return false;
	QString what = !only ? QString("%0 put on the card as /%1, %2 entries").arg(dir, name, QString::number(n))
		: (only->isEmpty() || !n) ? QString("%0 put on the card as an empty /%1").arg(dir, name)
		: QString("%0 put on the card alone as /%1").arg(QDir(dir).filePath(*only), name);
	if (!why.isEmpty()) what += ": " + why;
	xlog(XLG_FILE, XLL_INFO, "next: %s", what.toUtf8().constData());
	return true;
}

static int load_next(Computer* comp, const char* name, int snx) {
	if (comp->hw->id != HW_ZXNEXT) return ERR_NEX_HW;
	if (!comp->sdc->vfat) return ERR_NEX_CARD;
	QFileInfo inf(QString::fromLocal8Bit(name));
	// a card nobody has booted yet runs its welcome pages until they are turned off
	// with D, which a card served read only cannot keep
	auto welcome = [comp]() {
		vFat* vf = comp->sdc->vfat;
		return (vf_child(vf, vf_child(vf, 0, "nextzxos"), "autoexec.1st") >= 0) ? 1 : 0;
	};
	// on the card: its folder there, found by name down from the root
	QString card = QDir(QString::fromLocal8Bit(comp->sdc->image)).canonicalPath();
	QString folder = inf.absoluteDir().canonicalPath();
	if (!card.isEmpty() && ((folder == card) || folder.startsWith(card + "/"))) {
		QString rel = QDir(card).relativeFilePath(folder);
		if (rel == ".") rel.clear();
		int node = 0;
		foreach(QString part, rel.split('/', X_SkipEmptyParts))
			node = vf_child(comp->sdc->vfat, node, part.toUtf8().constData());
		if ((node >= 0) && (rel.toLatin1() == rel.toUtf8())) {
			int res = nex_arm(comp, node, ("/" + rel).toLatin1(), inf, welcome(), snx, 0);
			if (res == ERR_OK) return res;
		}
		// a path the keyboard cannot type: as a file from elsewhere
	}
	// the folder's name, as the keyboard types it, and one the card's root has not got
	QString base;
	foreach(QChar c, inf.absoluteDir().dirName().toLower()) {
		if (((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-')) base += c;
	}
	base = base.left(20);
	if (base.isEmpty()) base = "nex";
	QStringList taken = QDir(QString::fromLocal8Bit(comp->sdc->image)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
	QString dir = base;
	for (int i = 2; taken.contains(dir, Qt::CaseInsensitive); i++)
		dir = base + QString::number(i);
	// The card changes under the machine, which a soft reset does not tell: NextZXOS's
	// disk layer keeps in memory what it read of the old one, and the folder put on is
	// not found ("No such file or dir"). So off and on first, the card read again (as
	// Read-only or not, as the user said), then the folder put on.
	xm_power_cycle();
	// a .sna or .z80 is loaded whole and reads nothing beside it
	QString ext = inf.suffix().toLower();
	QString file = inf.fileName();
	QString why = ((ext == "sna") || (ext == "z80")) ? QString("a snapshot reads nothing beside it")
		: pin_huge(inf.absolutePath()) ? QString("its folder holds over %0 entries").arg(PIN_MAX) : QString();
	if (!pin_put(comp, inf.absolutePath(), dir, why.isEmpty() ? NULL : &file, why)) return ERR_NEX_CARD;
	return nex_arm(comp, vf_child(comp->sdc->vfat, 0, dir.toUtf8().constData()), dir.toLatin1(), inf, welcome(), snx, 1);
}

static int load_nex(Computer* comp, const char* name, int drv) {
	return load_next(comp, name, 0);
}

// a .snx is a .sna that NextZXOS loads with the Next left as it is (no 48K/128K
// modes) and the file left open for the program
static int load_snx(Computer* comp, const char* name, int drv) {
	return load_next(comp, name, 1);
}

// a .sna or .z80 on the Next goes the same way: its power-on boots the firmware over
// anything put into memory, and SPECTRUM is how NextZXOS opens one
static int load_sna(Computer* comp, const char* name, int drv) {
	return (comp->hw->id == HW_ZXNEXT) ? load_next(comp, name, 1) : loadSNA(comp, name, drv);
}

static int load_z80(Computer* comp, const char* name, int drv) {
	return (comp->hw->id == HW_ZXNEXT) ? load_next(comp, name, 1) : loadZ80(comp, name, drv);
}

static xFileTypeInfo ft_tab[] = {
	{FL_SNA, 0, ".sna", "*.sna", load_sna, saveSNA, "SNA snapshot"},
	{FL_Z80, 0, ".z80", "*.z80", load_z80, saveZ80, "Z80 snapshot"},
	{FL_SZX, 0, ".szx", "*.szx", loadSZX, save_szx, "SZX snapshot"},
	{FL_SPG, 0, ".spg", "*.spg", loadSPG, NULL, "SPG snapshot"},
	{FL_NEX, 0, ".nex", "*.nex", load_nex, NULL, "NEX program"},
	{FL_SNX, 0, ".snx", "*.snx", load_snx, NULL, "SNX snapshot"},
	{FL_TAP, 0, ".tap", "*.tap", loadTAP, saveTAP, "TAP tape image"},
	{FL_TZX, 0, ".tzx", "*.tzx", loadTZX, NULL, "TZX tape image"},
	{FL_WAV, 0, ".wav", "*.wav", loadWAV, save_wav, "WAV recording"},
	{FL_SCL, 1, ".scl", "*.scl", loadSCL, saveSCL, "SCL disk image"},
	{FL_TRD, 1, ".trd", "*.trd", loadTRD, saveTRD, "TRD disk image"},
	{FL_TD0, 1, ".td0", "*.td0", loadTD0, NULL, "TD0 disk image"},
	{FL_FDI, 1, ".fdi", "*.fdi", loadFDI, NULL, "FDI disk image"},
	{FL_UDI, 1, ".udi", "*.udi", loadUDI, saveUDI, "UDI disk image"},
	{FL_DSK, 1, ".dsk", "*.dsk", loadDSK, saveDSK, "DSK disk image"},
	{FL_HOBETA, 0, ".$", "*.$?", loadHobeta, NULL, "Hobeta file"},
	{FL_SLT_ROM, 0, ".rom", "*.rom", loadSlot, NULL, "Cartridge image"},
	{FL_SLT_BIN, 0, ".bin", "*.bin", loadSlot, NULL, "Cartridge image"},
	{FL_IMA, 1, ".ima", "*.ima", load_ima, NULL, "1.44 FDD image"},
	{FL_PCIMG, 1, ".img", "*.img", load_ima, NULL, "1.44 FDD image"},
	{FL_RZX, 0, ".rzx", "*.rzx", loadRZX, NULL, "RZX playback"},
	{FL_POK, 0, ".pok", "*.pok", NULL, NULL, "Cheats"},
	{FL_RAW, 0, NULL, "*", loadRaw, NULL, "RAW file to TR-DOS disk"},			// * for all files; *.* for all files that have extension
	{0, 0, NULL, NULL, NULL, NULL, NULL}
};

static xFileTypeInfo ft_sltraw = {FL_SLT_BIN, 0, NULL, NULL, loadSlot, NULL, "RAW cartridge image"};
static xFileTypeInfo ft_raw = {FL_RAW, 0, NULL, NULL, loadRaw, NULL, "RAW file to disk A"};
static xFileTypeInfo ft_dum = {FL_NONE, 0, NULL, NULL, NULL, NULL, "Dummy entry"};

#define DISK_TYPES FL_SCL, FL_TRD, FL_TD0, FL_FDI, FL_UDI, FL_DSK, FL_IMA, FL_PCIMG, FL_HOBETA, 0

// 3rd parameter
// < 0 : allways on
// 0..3 : disk (must be inserted for save)
// 4 : tape (block count > 0)
static xFileGroupInfo fg_tab[] = {
	{FG_SNAPSHOT, ".szx", -1, "Snapshot", NULL, {FL_SZX, FL_SNA, FL_Z80, FL_SPG, FL_NEX, FL_SNX, 0}},
	{FG_TAPE, ".tap", 4, "Tape", NULL, {FL_TAP, FL_TZX, FL_WAV, 0}},
	// before the drives: a filter's group is the last name it starts with
	{FG_DISKS, ".trd", 0, "Disk", NULL, {DISK_TYPES}},
	{FG_DISK_A, ".trd", 0, "Disk A", NULL, {DISK_TYPES}},
	{FG_DISK_B, ".trd", 1, "Disk B", NULL, {DISK_TYPES}},
	{FG_DISK_C, ".trd", 2, "Disk C", NULL, {DISK_TYPES}},
	{FG_DISK_D, ".trd", 3, "Disk D", NULL, {DISK_TYPES}},
	{FG_RAW, "", 0, "RAW file to disk", &ft_raw, {FL_RAW, 0}},
	{FG_IF2_ROM, "", -1, "Cartridge image", &ft_sltraw, {FL_SLT_ROM, FL_SLT_BIN, 0}},
	{FG_RZX, "", -1, "RZX playback", NULL, {FL_RZX, 0}},
	{FG_POK, "", -1, "Cheats", NULL, {FL_POK, 0}},
	{0, "", -1, NULL, NULL, {0}}
};

static xFileGroupInfo fg_dum = {0, "", -1, NULL, NULL, {0}};

// a wav is a recording of the tape, not its image: saved only as an export
static bool ft_export(xFileTypeInfo* inf) {
	return inf && (inf->id == FL_WAV);
}

static xFileHWInfo fh_tab[] = {
	{FH_SPECTRUM, {FG_SNAPSHOT, FG_TAPE, FG_DISKS, FG_DISK_A, FG_DISK_B, FG_DISK_C, FG_DISK_D, FG_RAW, FG_RZX, FG_IF2_ROM, FG_POK, 0}},
	{FH_ALF, {FG_IF2_ROM, FG_SNAPSHOT, 0}},
	{FH_SLOTS, {FG_IF2_ROM, 0}},
	{FH_DRIVE_A, {FG_DISK_A, FG_RAW, 0}},
	{FH_DRIVE_B, {FG_DISK_B, FG_RAW, 0}},
	{FH_DRIVE_C, {FG_DISK_C, FG_RAW, 0}},
	{FH_DRIVE_D, {FG_DISK_D, FG_RAW, 0}},
	{0, {0}}
};

static xFileHWInfo hw_tab[] = {
	{FH_SPECTRUM, {HW_ATM1, HW_ATM2, HW_P1024, HW_PENT, HW_PENTEVO, HW_PHOENIX, HW_PLUS2A, HW_PLUS3, HW_PROFI, HW_PROFI3, HW_SCORP, HW_SCORPTP, HW_TSLAB, HW_ZX48, HW_ZX128, HW_ZXNEXT, 0}},
	{FH_ALF, {HW_ALF, 0}},
	{0, {0}}
};

int detect_hw_id(int hwid) {
	int i = 0;
	int x;
	int id = 0;
	while (hw_tab[i].id > 0) {
		x = 0;
		while (hw_tab[i].child[x] > 0) {
			if (hw_tab[i].child[x] == hwid)
				id = hw_tab[i].id;
			x++;
		}
		i++;
	}
	return id;
}

xFileHWInfo* file_find_hw(int id) {
	int i = 0;
	while ((fh_tab[i].id > 0) && (fh_tab[i].id != id))
		i++;
	return &fh_tab[i];
}

xFileGroupInfo* file_find_group(int id) {
	int i = 0;
	while ((fg_tab[i].id > 0) && (fg_tab[i].id != id))
		i++;
	return &fg_tab[i];
}

xFileGroupInfo* file_detect_grp(QString flt) {
	int i = 0;
	xFileGroupInfo* inf = &fg_dum;
	while (fg_tab[i].id > 0) {
		if (flt.startsWith(fg_tab[i].name, Qt::CaseInsensitive))
			inf = &fg_tab[i];
		i++;
	}
	return inf;
}

xFileTypeInfo* file_find_type(int id) {
	int i = 0;
	while ((ft_tab[i].id > 0) && (ft_tab[i].id != id))
		i++;
	return &ft_tab[i];
}

xFileTypeInfo* file_detect_type(QString flt) {
	int i = 0;
	xFileTypeInfo* inf = &ft_dum;
	while (ft_tab[i].id > 0) {
		if (flt.startsWith(ft_tab[i].name, Qt::CaseInsensitive)) {
			inf = &ft_tab[i];
		}
		i++;
	}
	return inf;
}

xFileTypeInfo* file_find_hw_ext(int hw, QString path) {
	xFileTypeInfo* inf = NULL;
	xFileHWInfo* hwinf;
	xFileGroupInfo* fginf;
	xFileTypeInfo* ftinf;
	QString ext = path.split(".").last();
	int ig;
	int iz;
	hw = detect_hw_id(hw);
	if (hw != 0) {
		hwinf = file_find_hw(hw);
		ig = 0;
		while ((hwinf->child[ig] != 0) && (inf == NULL)) {
			fginf = file_find_group(hwinf->child[ig]);
			iz = 0;
			while ((fginf->child[iz] != 0) && (inf == NULL)) {
				ftinf = file_find_type(fginf->child[iz]);
				if (ftinf->id != 0) {
					if (ftinf->id == FL_HOBETA) {
						if (ext.startsWith("$"))
							inf = ftinf;
					} else if (ftinf->ext == NULL) {

					} else if (path.endsWith(ftinf->ext, Qt::CaseInsensitive)) {
						inf = ftinf;
					}
				}
				iz++;
			}
			ig++;
		}
	}
	return inf;
}

// what a file opened as id is: the group's own type when it has one (a raw
// file, a cartridge), otherwise whatever the extension says
static xFileTypeInfo* file_type_of(Computer* comp, int id, const QString& path) {
	xFileGroupInfo* grp = file_find_group(id);	// not a group: the empty one
	return grp->fti ? grp->fti : file_find_hw_ext(comp->hw->id, path);
}

static QString allfilt;

QString file_get_type_filter(int id, int sv) {
	QString flt;
	xFileTypeInfo* inf = file_find_type(id);
	if (inf->filt && ((!sv && inf->load) || (sv && inf->save))) {
		flt = QString(inf->filt);
		if (!allfilt.contains(inf->filt) && inf->ext)
			allfilt.append(inf->filt).append(" ");
	}
	return flt;
}

QString file_get_group_filter(Computer* comp, int id, int sv) {
	QStringList flt;
	QStringList exf;
	QString ftf;
	QString ofl;
	int i;
	xFileGroupInfo* inf = file_find_group(id);
	if (inf->id > 0) {
		if (!sv || (inf->drv < 0) || (sv && !(inf->drv & ~3) && comp->dif->flp[inf->drv & 3]->insert) || ((inf->drv == 4) && comp->tape->blkCount > 0)) {
			i = 0;
			while(inf->child[i] > 0) {
				ofl = file_get_type_filter(inf->child[i], sv);
				xFileTypeInfo* tin = file_find_type(inf->child[i]);
				if (!ofl.isEmpty() && sv && ft_export(tin))
					exf.append(QString("%0, export (%1)").arg(tin->name, ofl));
				else if (!ofl.isEmpty())
					ftf.append(ofl).append(" ");
				i++;
			}
			ftf = ftf.trimmed();
			if (!sv && !ftf.isEmpty() && (inf->id != FG_RAW) && (inf->id != FG_POK))
				ftf.append(" " ARC_FILTER);
			if (!ftf.isEmpty())
				flt.append(QString("%0 (%1)").arg(inf->name, ftf));
			flt.append(exf);
		}
	}
	return flt.join(";;");
}

QString file_get_hw_filter(Computer* comp, int id, int sv) {
	QStringList flt;
	QString gfl;
	allfilt.clear();
	int i;
	xFileHWInfo* inf = file_find_hw(id);
	if (inf->id > 0) {
		bool disks = false;		// Open has one Disk group, Save one per drive
		for (i = 0; inf->child[i] > 0; i++)
			disks |= (inf->child[i] == FG_DISKS);
		for (i = 0; inf->child[i] > 0; i++) {
			int g = inf->child[i];
			if (sv ? (g == FG_DISKS) : (disks && (g >= FG_DISK_A) && (g <= FG_DISK_D))) continue;
			gfl = file_get_group_filter(comp, g, sv);
			if (!gfl.isEmpty()) flt.append(gfl);
		}
	}
	if ((flt.size() > 1) && !sv)
		flt.prepend(QString("All files (%0)").arg(allfilt + ARC_FILTER));
	return flt.join(";;");
}

typedef struct {
	int err;
	const char* text;
} xFilerError;

static xFilerError err_tab[] = {
	{ERR_CANT_OPEN, "Can't open file"},
	{ERR_NO_DRIVE, "The machine has no such drive"},
	{ERR_RZX_SIGN, "Wrong RZX signature"},
	{ERR_RZX_CRYPT, "Encrypted RZX is not supported"},
	{ERR_RZX_UNPACK, "RZX unpack error"},
	{ERR_RZX_REC, "This machine cannot be recorded"},
	{ERR_TZX_SIGN, "Wrong TZX signature"},
	{ERR_TZX_UNKNOWN, "Unknown TZX block"},
	{ERR_TRD_LEN, "Incorrect TRD size"},
	{ERR_TRD_SIGN, "Not a TR-DOS disk"},
	{ERR_UDI_SIGN, "Wrong UDI signature"},
	{ERR_FDI_SIGN, "Wrong FDI signature"},
	{ERR_FDI_HEAD, "Wrong FDI heads count"},
	{ERR_HOB_CANT, "Can't create file at disk"},
	{ERR_SCL_SIGN, "Wrong SCL signature"},
	{ERR_SCL_MANY, "Too many files in SCL"},
	{ERR_RAW_LONG, "File is too big"},
	{ERR_DSK_SIGN, "Wrong DSK signature"},
	{ERR_DSK_WIDE, "DSK holds 86 cylinders, this disk has more: save it as TRD or UDI"},
	{ERR_TD0_SIGN, "Wrong TD0 signature"},
	{ERR_TD0_TYPE, "Unsupported TD0"},
	{ERR_TD0_VERSION, "Unsupported TD0 version"},
	{ERR_WAV_HEAD, "Wrong WAV header"},
	{ERR_WAV_FORMAT, "Unsupported WAV format"},
	{ERR_Z80_HW, "Z80 snapshot: unsupported hardware"},
	{ERR_TRD_SNF, "Wrong disk structure for TRD file"},
	{ERR_SPG_SIGN, "Wrong SPG signature"},
	{ERR_SPG_VERSION, "Unsupported SPG version"},
	{ERR_SZX_SIGN, "Wrong SZX signature"},
	{ERR_SZX_HW, "SZX snapshot: a machine this emulator does not have"},
	{ERR_SZX_DATA, "SZX snapshot: no CPU or memory in it"},
	{ERR_TAP_DATA, "This tape has blocks TAP cannot hold: export it to WAV"},
	{ERR_TAP_EMPTY, "The tape is empty"},
	{ERR_NOTRD, "Not a TR-DOS disk"},
	{ERR_CANT_SAVE, "Can't save in this format"},
	{ERR_FILE_TYPE, "Unknown file type"},
	{ERR_SNAP_SHORT, "The snapshot is cut short"},
	{ERR_NEX_HW, "NEX and SNX files need the ZX Spectrum Next"},
	{ERR_NEX_CARD, "NEX and SNX files start from NextZXOS: put the System/Next folder in the SD card slot"},
	{ERR_NEX_NAME, "The file's name cannot be typed in NextZXOS"},
	{ERR_SNAP_NEXT, "The ZX Spectrum Next's state saves only as .szx"},
	{ERR_OK, ""}
};

// a cancel was the user's own answer, so it is not reported
void file_errors(int err) {
	int i = 0;
	if ((err == ERR_OK) || (err == ERR_CANCEL)) return;
	while ((err_tab[i].err != ERR_OK) && (err_tab[i].err != err))
		i++;
	if (err_tab[i].err != ERR_OK) {
		shitHappens(err_tab[i].text);
	} else {
		shitHappens(QString("Error #%0").arg(err).toLocal8Bit().constData());
	}
}

static int disk_id[] = {FG_DISK_A, FG_DISK_B, FG_DISK_C, FG_DISK_D};
static int boot_ft[] = {FL_SCL, FL_TRD, FL_TD0, FL_FDI, FL_UDI, FL_HOBETA, 0};

// what the last loaded file would need to start on its own, and the drive it
// went to. The caller decides whether to act on it
static int last_as_kind = AS_NONE;
static int last_as_drv = 0;
// and a snapshot loaded since the window last asked
static QString last_snapshot;
// the recording the RZX player shows
static QString rzx_path;

QString rzx_current() {
	return rzx_path;
}

int file_autostart_kind() {
	return last_as_kind;
}

QString file_take_snapshot() {
	QString res = last_snapshot;
	last_snapshot.clear();
	return res;
}

// the image named in the window title, which is what Favorites take. A
// screenshot breakpoint reads it from the emulation thread.
static QString media_now;
static QMutex media_mx;

QString media_current() {
	QMutexLocker lock(&media_mx);
	return media_now;
}

// what a video of the machine is named after: the image in use, else what is
// in the deck or drive A - a tape about to be started - else the machine
// the image in use, as the title has it, without its folder and extension;
// empty when nothing is: an image left mounted is not what the machine runs
QString media_image_name() {
	return QFileInfo(media_current()).completeBaseName();
}

void media_set_current(const QString& path) {
	QMutexLocker lock(&media_mx);
	media_now = path;
}

void media_autorun_forget() {
	last_as_kind = AS_NONE;
	last_as_drv = 0;
}

static char as_no_tape[] = "This machine cannot start a tape";
static char as_no_disk[] = "This machine cannot start that disk";
static char as_not_a[] = "Only drive A can be started";

// a run with no window (--bench) has nowhere to put a question or a notice
static int as_headless = 0;

void media_autorun_headless(int on) {
	as_headless = on;
}

// A disk cannot be started while a divMMC is on: its drives are out. Said in a
// window with the machine paused: the message line is gone before it can be read.
// The gui asks to switch the board off before it gets here (drivesBack); this is
// the command line's.
static void as_board_refused() {
	if (as_headless) return;		// the log has the refusal
	pause_add(PR_EXTRA);
	QMessageBox::information(nullptr, "Cannot start the disk",
		"The floppy drives are unavailable while divMMC is enabled: the disk is in its drive, "
		"but cannot be started.\n\n"
		"Switch the divMMC off with Media > divMMC, or start a disk image from the "
		"card in the esxDOS browser (Machine > NMI).");
	pause_del(PR_EXTRA);
}

// Media just opened from the gui: mounting is not enough when the user wants it
// started, so do what the command line does - reset and press the keys. A
// machine that cannot take this kind of media keeps running, the image just
// sits in its drive.
// booting is drive A business
static bool as_from_drive(int kind, int drv) {
	return (kind == AS_TAPE) || (drv <= 0);
}

// a tape, or a disk going into drive A
static bool as_runnable(int kind, int drv) {
	return (kind != AS_NONE) && as_from_drive(kind, drv);
}

void media_autorun(Computer* comp, int run) {
	int kind = file_autostart_kind();
	if (!comp || !run || (kind == AS_NONE)) return;
	if (!as_from_drive(kind, last_as_drv)) {
		comp->msg = as_not_a;
		xlog(XLG_FILE, XLL_INFO, "autostart: only drive A can be started");
		return;
	}
	emu_lock();		// arming resets the machine
	int ok = autostart_arm(comp, kind);
	emu_unlock();
	xlog(XLG_FILE, XLL_INFO, "autostart %s: %s", ok ? "armed" : "refused",
		comp->hw->name);
	if (ok) return;
	if (autostart_board_blocks(comp, kind) || ((kind != AS_TAPE) && comp->dif->out))
		as_board_refused();
	else
		comp->msg = (kind == AS_TAPE) ? as_no_tape : as_no_disk;
}

static int as_kind_of(int ftype) {
	switch (ftype) {
		case FL_TAP:
		case FL_TZX:
		case FL_WAV:
			return AS_TAPE;
		case FL_SCL:
		case FL_TRD:
		case FL_TD0:
		case FL_FDI:
		case FL_UDI:
			return AS_DISK;
		case FL_DSK:
			return AS_DISK3;
	}
	return AS_NONE;
}

// ERR_OK once the disk or tape the file would replace is safe to lose
static int replace_ok(Computer* comp, xFileTypeInfo* inf, int drv) {
	if (inf->ch) return saveChangedDisk(comp, drv);
	if (as_kind_of(inf->id) == AS_TAPE) return saveChangedTape(comp);
	return ERR_OK;
}

// Ask: the other machines that can take the file, the one it names first, and
// Keep for the running one. false is Cancel - the file is not opened at all
static bool media_ask_machine(const QString& path, const xFileMacPick& pick, std::string* mac) {
	const xMachine* cur = xm_find(conf.macId);
	QString curName = QString::fromLocal8Bit(cur ? cur->name.c_str() : conf.macId.c_str());
	QString name = arc_name(path).toHtmlEscaped();
	QDialog dlg;
	dlg.setWindowTitle("Choose a Machine");
	QVBoxLayout* lay = new QVBoxLayout(&dlg);
	QString text = pick.runs ? QString("Run <b>%1</b> on:").arg(name)
		: QString("<b>%1</b> does not run on %2.<br>Run it on:").arg(name, curName.toHtmlEscaped());
	if (pick.unsure)
		text = QString("The snapshot needs the ROM of the model it was taken on, and the file does not say which.<br>") + text;
	QLabel* lab = new QLabel(text);
	lab->setWordWrap(true);
	lay->addWidget(lab);
	QListWidget* list = new QListWidget;
	for (const std::string& id : pick.ask) {
		const xMachine* m = xm_find(id);
		QListWidgetItem* it = new QListWidgetItem(QString::fromLocal8Bit(m ? m->name.c_str() : id.c_str()));
		it->setData(Qt::UserRole, QString::fromLocal8Bit(id.c_str()));
		list->addItem(it);
	}
	list->setCurrentRow(0);
	// the whole list in view: it is never longer than the machines there are
	list->setMinimumHeight(list->sizeHintForRow(0) * list->count() + 2 * list->frameWidth());
	lay->addWidget(list);
	QHBoxLayout* btns = new QHBoxLayout;
	QPushButton* run = new QPushButton("Switch");
	QPushButton* keep = new QPushButton(QString("Keep %1").arg(curName));
	QPushButton* cancel = new QPushButton("Cancel");
	(pick.keep ? keep : run)->setDefault(true);
	btns->addStretch();
	btns->addWidget(run);
	btns->addWidget(keep);
	btns->addWidget(cancel);
	lay->addLayout(btns);
	enum {ASK_CANCEL = 0, ASK_SWITCH, ASK_KEEP};
	QObject::connect(run, &QPushButton::clicked, &dlg, [&dlg]() {dlg.done(ASK_SWITCH);});
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, [&dlg]() {dlg.done(ASK_SWITCH);});
	QObject::connect(keep, &QPushButton::clicked, &dlg, [&dlg]() {dlg.done(ASK_KEEP);});
	QObject::connect(cancel, &QPushButton::clicked, &dlg, [&dlg]() {dlg.done(ASK_CANCEL);});
	int res = dlg.exec();
	if ((res == ASK_SWITCH) && list->currentItem())
		*mac = list->currentItem()->data(Qt::UserRole).toString().toLocal8Bit().data();
	return res != ASK_CANCEL;
}

// The machine a file is to be opened on, asked before it is opened: *mac comes
// back empty to keep the running one. Only what is about to run gets a say - a
// snapshot always, a tape or a disk only when it is started, and a disk only
// from drive A, the one that boots. false: the user backed out of the question.
// a tape or a disk that is not started stays on the running machine: nothing to look at
bool media_machine_known(Computer* comp, const QString& path, int id, int drv, int run) {
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (!inf) return true;
	int kind = as_kind_of(inf->id);		// AS_NONE: a snapshot, or nothing that runs
	return (kind != AS_NONE) && (!run || !as_runnable(kind, drv));
}

bool media_machine(Computer* comp, const QString& path, int id, int drv, int run, std::string* mac) {
	mac->clear();
	if (media_machine_known(comp, path, id, drv, run)) return true;
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	QString err;
	QString local = arc_local(path, &err);		// its bytes say which machine
	if (local.isEmpty()) return true;			// opening it says why not
	xFileMacPick pick = fm_pick(inf->id, local.toLocal8Bit().data());
	if (!pick.ask.empty() && as_headless) {		// no one to ask: the answer offered
		if (!pick.keep) *mac = pick.ask.front();
		return true;
	}
	if (!pick.ask.empty())
		return media_ask_machine(path, pick, mac);
	*mac = pick.target;
	return true;
}

bool media_runnable(Computer* comp, const QString& path, int id, int drv) {
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	return inf && as_runnable(as_kind_of(inf->id), drv);
}

// a file that goes into a floppy drive
bool media_to_drive(Computer* comp, const QString& path, int id) {
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (!inf) return false;
	switch (inf->id) {
		case FL_SCL: case FL_TRD: case FL_TD0: case FL_FDI: case FL_UDI: case FL_DSK:
		case FL_IMA: case FL_PCIMG: case FL_HOBETA: case FL_RAW:
			return true;
	}
	return false;
}

void disk_boot(Computer* comp, int drv, int id) {
	if (!conf.boot) return;
	int idx = 0;
	while (boot_ft[idx] && (boot_ft[idx] != id))
		idx++;
	if (boot_ft[idx])
		loadBoot(comp, conf.path.boot.c_str(), drv);
}

// The dialog is shared with the saves: the name a save left in it goes, and the
// mode is set before the filter, which in save mode puts its own suffix on that
// name - an RZX just recorded came back offered as an .szx.
static QFileDialog* file_dialog_open_mode(const QString& title, const QString& flt) {
	QFileDialog* filer = file_dialog();
	filer->setWindowTitle(title);
	filer->setAcceptMode(QFileDialog::AcceptOpen);
	filer->selectFile(QString());
	// Set only when changed: Qt 5 matches every file against every pattern on each set, and a set picks the first filter
	QStringList want;
	for (const QString& f : flt.split(flt.contains(";;") ? QString(";;") : QString("\n")))
		want << f.simplified();
	if ((filer->nameFilters() != want) || (filer->selectedNameFilter() != want.value(0))) {
		filer->setNameFilter(flt);
		file_dialog_filtered(filer);
	}
	filer->setDirectory(conf.lastDir.c_str());
	filer->setHistory(QStringList());
	return filer;
}

// The open dialog, for the kind of file id asks for. What was picked in it
// comes back through id and drv: the filter chosen is the group the file is
// opened as, and a drive of its own when drv did not name one.
QString file_ask_open(Computer* comp, int* id, int* drv) {
	int fid = *id;
	if (fid == FG_DISK)
		fid = disk_id[*drv & 3];
	if (fid == FG_ALL)
		fid = detect_hw_id(comp->hw->id);
	QString path;
	QString flt = file_get_hw_filter(comp, fid, 0);
	if (flt.isEmpty()) {
		flt = file_get_group_filter(comp, fid, 0);
		if (flt.isEmpty())
			flt = file_get_type_filter(fid, 0);
	}
	if (flt.isEmpty()) return path;
	QFileDialog* filer = file_dialog_open_mode("Open File", flt);
	if (filer->exec()) {
		path = filer->selectedFiles().first();
		xFileGroupInfo* grp = file_detect_grp(filer->selectedNameFilter());
		if (grp->id > 0) *id = (grp->id == FG_DISKS) ? FG_DISK_A : grp->id;
		if (*drv < 0) *drv = grp->drv;
		conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
	}
	return path;
}

// A save of ours being put back: what it carries goes in as it was - a disk
// the game wrote to since too, without asking - and an image already in with
// the same bytes stays. Off, an image in its place is left in, so a disk
// written to since is not thrown away.
static bool szx_exact = false;

static void media_rename(Computer* comp, const QString& local, const QString& path);

// the drive already holds the carried image: the same name, the same bytes
static bool szx_holds(Computer* comp, const szxMedia* m, const QString& link) {
	QByteArray raw = link.toLocal8Bit();
	if (m->kind == SZX_MED_TAPE)
		return comp->tape->path && (raw == comp->tape->path) && !comp->tape->changed;
	Floppy* flp = comp->dif->flp[m->drive & 3];
	if (!flp->insert || !flp->path || (raw != flp->path)) return false;
	int dsk = !strcmp(m->ext, "dsk") && (m->kind == SZX_MED_PLUS3);
	if (strcmp(m->ext, "udi") && !dsk) return true;		// someone else's: by name only
	size_t len = 0;
	unsigned char* now = dsk ? dsk_build(flp, &len) : udi_build(flp, &len);
	bool same = now && (len == m->size) && !memcmp(now, m->data, len);
	free(now);
	return same;
}

// The Next's SD card the snapshot ran from: in, as the file names it, with the folder of a
// program from elsewhere put back on it under its name; then whether the card still reads
// as it did. NextZXOS keeps what it read of the card in memory - open files, where they
// are - so on another card, or one written since, the program may read the wrong things.
// Told on the screen, not refused and not in a box: the user may know better.
static bool szxCardChanged = false;

static void szx_card_mount(Computer* comp, bool load) {
	const szxCard* c = szx_card();
	szxCardChanged = false;
	if (!c->on || !comp->hw || (comp->hw->id != HW_ZXNEXT)) return;
	QString path = QString::fromLocal8Bit(c->path);
	auto same = [comp, &path]() {
		return comp->sdc->image && (QFileInfo(QString::fromLocal8Bit(comp->sdc->image)).canonicalFilePath() == QFileInfo(path).canonicalFilePath());
	};
	if (load && !same()) {
		if (QFileInfo::exists(path)) {
			xm_next_put_card(path);
			xlog(XLG_FILE, XLL_INFO, "szx: SD card %s", c->path);
		} else {
			xlog(XLG_FILE, XLL_WARN, "szx: SD card %s not found", c->path);
		}
	}
	// the folder a program from elsewhere ran in, put on the card again by the same rule
	// (one at a time, as opening one does); one named by a file holds that file alone
	if (load && same() && comp->sdc->vfat && c->pins) {
		QFileInfo hinf(QString::fromUtf8(c->pin[0].host));
		QString name = QString::fromUtf8(c->pin[0].name);
		bool alone = hinf.isFile();
		QString dir = alone ? hinf.absolutePath() : hinf.absoluteFilePath();
		QString file = alone ? hinf.fileName() : QString();
		vFat* vf = comp->sdc->vfat;
		int node = vf_child(vf, 0, c->pin[0].name);
		bool there = (node >= 0) && vf->node[node].pinned && vf->node[node].hroot
			&& (QFileInfo(QString::fromUtf8(vf->node[node].hroot)).canonicalFilePath() == QFileInfo(dir).canonicalFilePath());
		// a folder is there as the rule put it, whole or empty; a file, with that file in it
		if (there) {
			int f = vfat_pin_file(vf, node);
			there = alone ? ((f >= 0) && (file == QString::fromUtf8(vf->node[f].name)))
				: (!vf->node[node].cut || (vf->node[node].child < 0));
		}
		if (!there && !alone && !hinf.isDir()) {
			xlog(XLG_FILE, XLL_WARN, "szx: %s, the program's folder or file, not found", c->pin[0].host);
		} else if (!there) {
			QString why = (!alone && pin_huge(dir)) ? QString("it holds over %0 entries").arg(PIN_MAX) : QString();
			bool cut = alone || !why.isEmpty();
			if (!pin_put(comp, dir, name, cut ? &file : NULL, why))
				xlog(XLG_FILE, XLL_WARN, "szx: the program's folder %s could not go on the card", c->pin[0].host);
		}
	}
	szxCardChanged = c->print && (sdc_dir_print(comp->sdc) != c->print);
	if (szxCardChanged) xlog(XLG_FILE, XLL_WARN, "szx: the SD card is not as it was when the snapshot was saved");
}

// The tape and the disks an .szx links to or carries. A link that does not
// resolve is looked for beside the snapshot, as Spectaculator writes plain names
// too; a carried image is written out to a temporary file first, and goes in
// under the name it had when the file says it. See szx_exact for what is left in.
// A carried disk is unsaved if it was when saved, or if its file is gone: the
// question on exit is where it gets back to a file. Nothing is taken out for
// what the snapshot did not have; the user can keep its media out altogether,
// but for a quick load.
static void szx_mount(Computer* comp, const QString& snap) {
	const szxMedia* med;
	bool load = szx_exact || conf.saves.snapLoad;
	int count = szx_media(&med);
	if (!load) {
		if (count) xlog(XLG_FILE, XLL_INFO, "szx: the tape and the drives left as they are (Options)");
		count = 0;
	}
	int carried = 0;	// drives whose disk came out of the snapshot
	int gone = 0;		// ...and has no file left under its name
	for (int i = 0; i < count; i++) {
		const szxMedia* m = &med[i];
		QString path;		// the file read
		QString name;		// the name it goes in under, when not that one
		QString link = QDir::fromNativeSeparators(QString::fromLocal8Bit(m->path));
		if (m->data && !link.isEmpty()) {
			const char* in = (m->kind == SZX_MED_TAPE) ? comp->tape->path : comp->dif->flp[m->drive & 3]->path;
			bool there = in && *in && (arc_canonical(QString::fromLocal8Bit(in)) == arc_canonical(link));
			if (szx_exact ? szx_holds(comp, m, link) : there) {
				path = name = link;		// in its place already
			} else {
				QString err;
				QString entry;
				QString base = QFileInfo(arc_split(link, nullptr, &entry) ? entry : link).completeBaseName();
				path = arc_temp_file(base + "." + QString::fromLocal8Bit(m->ext).toLower(), &err);
				name = link;
			}
		} else if (m->data) {
			QString base = QFileInfo(snap).completeBaseName();
			if (m->kind != SZX_MED_TAPE) base += QString("-%1").arg(QChar('a' + m->drive));
			path = QDir::temp().filePath(base + "." + QString::fromLocal8Bit(m->ext).toLower());
		} else {
			path = link;
			if (!arc_exists(path))
				path = QFileInfo(snap).dir().filePath(QFileInfo(link).fileName());
			if (!arc_exists(path)) {
				xlog(XLG_FILE, XLL_WARN, "szx: %s not found", m->path);
				continue;
			}
		}
		if (path.isEmpty()) {
			xlog(XLG_FILE, XLL_WARN, "szx: no place for the carried image of %s", m->path);
			continue;
		}
		bool fresh = m->data && (path != name);
		if (fresh) {
			QFile file(path);
			if (!file.open(QIODevice::WriteOnly) || (file.write((const char*)m->data, m->size) != (qint64)m->size)) {
				xlog(XLG_FILE, XLL_WARN, "szx: cannot write the carried image to %s", path.toLocal8Bit().constData());
				continue;
			}
		}
		if (!arc_split(path)) path = QFileInfo(path).canonicalFilePath();
		QString err;
		QString local = arc_local(path, &err);	// the bytes, unpacked when in an archive
		if (local.isEmpty()) {
			xlog(XLG_FILE, XLL_WARN, "szx: %s: %s", path.toLocal8Bit().constData(), err.toLocal8Bit().constData());
			continue;
		}
		QString as = name.isEmpty() ? path : name;	// what the drive is to say
		xFileTypeInfo* inf = file_find_hw_ext(comp->hw->id, local);
		int kind = inf ? as_kind_of(inf->id) : AS_NONE;
		if ((m->kind == SZX_MED_TAPE) ? (kind != AS_TAPE) : ((kind != AS_DISK) && (kind != AS_DISK3))) {
			xlog(XLG_FILE, XLL_WARN, "szx: %s is no image of the kind it is linked as", path.toLocal8Bit().constData());
			continue;
		}
		QByteArray asRaw = as.toLocal8Bit();
		if (m->kind == SZX_MED_TAPE) {
			const char* now = comp->tape->path;
			if (fresh || !now || (arc_canonical(QString::fromLocal8Bit(now)) != arc_canonical(as))) {
				if (!szx_exact && (replace_ok(comp, inf, 0) != ERR_OK)) continue;
				if (inf->load(comp, local.toLocal8Bit().data(), 0) != ERR_OK) continue;
				if (local != as) media_rename(comp, local, as);
				if (fresh && !name.isEmpty()) {
					carriedTapeName = as;
					carriedTapeExt = QString::fromLocal8Bit(m->ext).toLower();
					carriedTape = QByteArray((const char*)m->data, (int)m->size);
				}
			}
			if (m->block < comp->tape->blkCount) {
				tapRewind(comp->tape, m->block);
			} else if ((m->block == comp->tape->blkCount) && (m->block > 0)) {
				// taken with the tape run out: it stays run out, for
				// "Rewind at end" to wind back to the block a load asks for
				tapRewind(comp->tape, m->block - 1);
				tapNextBlock(comp->tape);
			}
		} else {
			int drv = m->drive & 3;
			Floppy* flp = comp->dif->flp[drv];
			if (!flp->fitted) continue;
			if (fresh || !flp->insert || !flp->path || (arc_canonical(QString::fromLocal8Bit(flp->path)) != arc_canonical(as))) {
				if (!szx_exact && (replace_ok(comp, inf, drv) != ERR_OK)) continue;
				if (inf->load(comp, local.toLocal8Bit().data(), drv) != ERR_OK) continue;
				if (local != as) media_rename(comp, local, as);
				disk_boot(comp, drv, inf->id);		// as opening it does
				if (fresh) {
					carried |= 1 << drv;
					if (!name.isEmpty() && !arc_exists(name)) gone |= 1 << drv;
				}
			}
			flp->protect = m->protect ? 1 : 0;
			flp->trk = (m->cylinder > 127) ? 127 : m->cylinder;	// 256 tracks: a damaged file stays in them
		}
		if (fresh && !name.isEmpty()) QFile::remove(path);	// read whole, and the drive has the name
		xlog(XLG_FILE, XLL_INFO, "szx: %s %s%s", (m->kind == SZX_MED_TAPE) ? "tape" : "disk",
			asRaw.constData(), fresh ? " (carried)" : "");
	}
	szx_ext_media(comp, carried, load);
	for (int i = 0; i < 4; i++)
		if (gone & (1 << i)) comp->dif->flp[i]->changed = 1;
	szx_card_mount(comp, load);
	if (szxCardChanged)
		comp->msg = (char*)" the SD card has changed since this save ";
}

// What the snapshot was taken with and this machine does not have. Nothing of
// the machine is changed for it: the user is told, and decides.
static void szx_warn() {
	static const struct {
		int note;
		const char* text;
	} tab[] = {
		{SZN_RELATIVE, "a model this emulator does not have, loaded on its nearest relative"},
		{SZN_TIMINGS, "the other ULA timings (early/late)"},
		{SZN_ISSUE, "another board issue (2/3), so the EAR bit reads otherwise"},
		{SZN_ULAPLUS, "ULA+ in use"},
		{SZN_AY, "an AY"},
		{SZN_BETA, "a Beta 128 disk interface"},
		{SZN_COVOX, "a Covox"},
		{SZN_DEVICE, "a device not emulated here (Interface 1, Multiface, SpecDrum...)"},
		{SZN_ZC, "a Z-Controller (Options > Machine, SD card)"},
		{0, NULL}
	};
	int notes = szx_notes();
	if (!notes || !conf.running) return;
	QString text = QObject::tr("The snapshot was taken with something this machine does not have:");
	text += "\n";
	for (int i = 0; tab[i].text; i++) {
		if (notes & tab[i].note)
			text += QString("\n- ") + tab[i].text;
	}
	text += "\n\n";
	text += QObject::tr("It may not run as it did. The machine's settings are left as they are.");
	QMessageBox mbx(QMessageBox::Warning, "SZX snapshot", text, QMessageBox::Ok);
	mbx.exec();
}

/* Images in archives
 *
 * The images of an archive a request can take are the entries whose extension a
 * loader of that request knows: Disk A's Open takes disks, the tape player's tapes.
 * Left out: folders, archives in it, cheats, files that are only raw bytes, and what
 * the World of Spectrum layout puts beside an image (SCRSHOT/, INFO/, POKES/). A
 * cartridge only when one is asked for: a release's .bin is rarely one.
 */

// the file types a request may be opened as
static void id_types(Computer* comp, int id, int drv, QSet<int>* types) {
	if (id == FG_DISK) id = disk_id[(drv < 0 ? 0 : drv) & 3];
	if (id == FG_ALL) id = detect_hw_id(comp->hw->id);
	QVector<int> groups;
	xFileHWInfo* hw = file_find_hw(id);
	if (hw->id > 0) {
		for (int i = 0; hw->child[i]; i++) groups.append(hw->child[i]);
	} else if (file_find_group(id)->id > 0) {
		groups.append(id);
	} else {
		types->insert(id);
	}
	for (int grp : groups) {
		xFileGroupInfo* inf = file_find_group(grp);
		for (int i = 0; inf->child[i]; i++) types->insert(inf->child[i]);
	}
	types->remove(FL_POK);
	types->remove(FL_RAW);
	bool cart = (id == FH_SLOTS) || (id == FG_IF2_ROM) || (id == FH_ALF);
	if (!cart) {
		types->remove(FL_SLT_BIN);
		types->remove(FL_SLT_ROM);
	}
}

static bool arc_beside_image(const QString& entry) {
	const QStringList parts = entry.split('/');
	for (int i = 0; i < parts.size() - 1; i++) {
		for (const char* dir : {"SCRSHOT", "INFO", "POKES"})
			if (!parts[i].compare(dir, Qt::CaseInsensitive)) return true;
	}
	return false;
}

// numbers as numbers, the rest as text whatever its case: Side 2 before Side 10
static bool natural_less(const QString& a, const QString& b) {
	int i = 0, j = 0;
	while ((i < a.size()) && (j < b.size())) {
		if (a[i].isDigit() && b[j].isDigit()) {
			int ei = i, ej = j;
			while ((ei < a.size()) && a[ei].isDigit()) ei++;
			while ((ej < b.size()) && b[ej].isDigit()) ej++;
			QString na = a.mid(i, ei - i), nb = b.mid(j, ej - j);
			while ((na.size() > 1) && (na[0] == '0')) na.remove(0, 1);
			while ((nb.size() > 1) && (nb[0] == '0')) nb.remove(0, 1);
			if (na.size() != nb.size()) return na.size() < nb.size();
			if (na != nb) return na < nb;
			i = ei;
			j = ej;
			continue;
		}
		const QChar ca = a[i].toCaseFolded(), cb = b[j].toCaseFolded();
		if (ca != cb) return ca < cb;
		i++;
		j++;
	}
	if ((a.size() - i) != (b.size() - j)) return (a.size() - i) < (b.size() - j);
	return a < b;
}

QStringList media_candidates(Computer* comp, const QString& archive, int id, int drv, QString* err) {
	QStringList res;
	const QVector<arcEntry>* list = arc_entries(archive, err);
	if (!list) {
		*err = QFileInfo(archive).fileName() + ": " + *err;
		return res;
	}
	QSet<int> types;
	id_types(comp, id, drv, &types);
	for (const arcEntry& entry : *list) {
		if (entry.folder || arc_beside_image(entry.name) || (arc_type(entry.name) != ARC_NONE)) continue;
		xFileTypeInfo* inf = file_find_hw_ext(comp->hw->id, entry.name);
		if (inf && types.contains(inf->id)) res.append(entry.name);
	}
	std::stable_sort(res.begin(), res.end(), natural_less);
	if (res.isEmpty())
		*err = QString("%1 holds no image this machine can open here").arg(QFileInfo(archive).fileName());
	return res;
}

/* Sibling images
 *
 * The other sides, tapes or disks of a set, beside the image in use in its folder, by
 * their names (xcore/siblings.h). The folder is read when the list is asked for and
 * nothing is kept. An archive beside it counts when it holds one image the slot takes,
 * and that entry is what opens.
 */

// The folder's files whose names start with prefix, any case. On Windows the file system
// is given the prefix: a TOSEC folder of 37 000 files takes 0.1 ms that way and 75 ms
// through every name. Its pattern also matches short 8.3 names, so the names are
// checked again.
static QStringList folder_names(const QString& dir, const QString& prefix) {
	QStringList res;
#ifdef _WIN32
	const QString pat = QDir::toNativeSeparators(dir) + "\\" + prefix + "*";
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileExW(reinterpret_cast<LPCWSTR>(pat.utf16()), FindExInfoBasic, &fd,
		FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
	if (h == INVALID_HANDLE_VALUE) return res;
	do {
		const QString name = QString::fromWCharArray(fd.cFileName);
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name.startsWith(prefix, Qt::CaseInsensitive))
			res.append(name);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
#else
	QDirIterator it(dir, QDir::Files);
	while (it.hasNext()) {
		it.next();
		const QString name = it.fileName();
		if (name.startsWith(prefix, Qt::CaseInsensitive)) res.append(name);
	}
#endif
	return res;
}

static QString sib_value_of(const QString& name) {
	return QString::fromStdString(sib_value(name.toUtf8().constData()));
}

// A set in its order: by the place each name marks (Side A, B; the game, then its
// levels), the names in natural order within one
static void sib_sort(QList<xSibling>* set) {
	std::stable_sort(set->begin(), set->end(), [](const xSibling& a, const xSibling& b) {
		const QString va = sib_value_of(a.name), vb = sib_value_of(b.name);
		if (va != vb) return natural_less(va, vb);
		return natural_less(a.name, b.name);
	});
}

QList<xSibling> media_siblings(Computer* comp, const QString& path, int id, int drv, int* cur) {
	QList<xSibling> res;
	QString file = path;
	arc_split(path, &file);
	const QFileInfo inf(file);
	const QString me = inf.fileName();
	std::string prefix;
	if (!sib_prefix(me.toUtf8().constData(), &prefix)) return res;
	QSet<int> types;
	id_types(comp, id, drv, &types);
#ifdef _WIN32
	const Qt::CaseSensitivity cs = Qt::CaseInsensitive;	// a path from Recent may differ in case
#else
	const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
	// what the slot takes, an archive by the name under its own extension
	std::vector<std::string> names;
	QStringList found;
	int at = -1;
	for (const QString& name : folder_names(inf.absolutePath(), QString::fromUtf8(prefix.c_str()))) {
		QString inner = name;
		if (arc_type(name) != ARC_NONE) inner.truncate(name.lastIndexOf('.'));
		xFileTypeInfo* ft = file_find_hw_ext(comp->hw->id, inner);
		if (!ft || !types.contains(ft->id)) continue;
		if (!name.compare(me, cs)) at = int(found.size());
		found.append(name);
		names.push_back(name.toUtf8().constData());
	}
	if (at < 0) return res;
	for (int i : sib_group(names, at)) {
		const QString name = found[i];
		const QString full = inf.dir().filePath(name);
		if (i == at) {
			res.append({name, path});
		} else if (arc_type(name) == ARC_NONE) {
			res.append({name, full});
		} else {
			QString err;
			const QStringList entries = media_candidates(comp, full, id, drv, &err);
			if (entries.size() == 1) res.append({name, arc_join(full, entries.first())});
		}
	}
	if (res.size() < 2) return QList<xSibling>();
	sib_sort(&res);
	for (int i = 0; i < res.size(); i++)
		if (res[i].path == path) *cur = i;
	return res;
}

// An archive's images that are a set by name: the archive's own list is not one, it
// holds versions as often as sides (Gryzor - 48k / - 128k)
static QList<xSibling> arc_siblings(Computer* comp, const QString& archive, const QString& entry, int id, int drv, int* cur) {
	QList<xSibling> res;
	QString err;
	const QStringList names = media_candidates(comp, archive, id, drv, &err);
	const int at = int(names.indexOf(entry));
	if ((names.size() < 2) || (at < 0)) return res;
	std::vector<std::string> raw;
	for (const QString& name : names) raw.push_back(name.toUtf8().constData());
	for (int i : sib_group(raw, at)) res.append({names[i], arc_join(archive, names[i])});
	sib_sort(&res);
	for (int i = 0; i < res.size(); i++)
		if (res[i].name == entry) *cur = i;
	return res;
}

// the [flags] of a name, as they stand
static QString name_flags(const QString& name) {
	static const QRegularExpression rx("\\[[^\\]]*\\]");
	QString res;
	QRegularExpressionMatchIterator it = rx.globalMatch(name);
	while (it.hasNext()) res += it.next().captured(0);
	return res;
}

#ifdef _WIN32
static const Qt::CaseSensitivity path_case = Qt::CaseInsensitive;
#else
static const Qt::CaseSensitivity path_case = Qt::CaseSensitive;
#endif

// the set by name only: in an archive its members, else the folder
static QList<xSibling> side_set(Computer* comp, const QString& path, int id, int drv, int* cur) {
	QString archive, entry;
	QList<xSibling> set;
	*cur = -1;
	if (arc_split(path, &archive, &entry)) set = arc_siblings(comp, archive, entry, id, drv, cur);
	if (set.isEmpty()) set = media_siblings(comp, path, id, drv, cur);
	return set;
}

static bool same_file(const QString& a, const QString& b) {
	return !QDir::fromNativeSeparators(a).compare(QDir::fromNativeSeparators(b), path_case);
}

static bool path_in(const QStringList& list, const QString& path) {
	for (const QString& one : list)
		if (same_file(one, path)) return true;
	return false;
}

// a place as the OSD shows it: a side's letter in capitals, "side B", "tape 1 side A"
static QString place_text(const QString& place) {
	static const QRegularExpression rx("\\b[a-d]\\b");
	QString res = place;
	QRegularExpressionMatchIterator it = rx.globalMatch(place);
	while (it.hasNext()) {
		const int at = int(it.next().capturedStart());
		res[at] = res[at].toUpper();
	}
	return res;
}

// Of the files at place next in the set (cur the image's own), the one to put in; empty
// when every one is in skip
static QString side_pick(Computer* comp, const QList<xSibling>& set, int cur, const QString& next, int id, int drv,
		const QString& path, const QStringList& seen, const QStringList& skip) {
	// An image that was in and has this one in its set: the user's own side A[a], which
	// side B's set leaves out, its names matching with their [flags] first
	for (const QString& was : seen) {
		if (same_file(was, path) || path_in(skip, was)) continue;
		int at;
		const QList<xSibling> its = side_set(comp, was, id, drv, &at);
		if ((at < 0) || (sib_value_of(its[at].name) != next)) continue;
		for (const xSibling& sib : its)
			if (same_file(sib.path, path)) return was;
	}
	QList<xSibling> take;
	for (const xSibling& sib : set)
		if ((sib_value_of(sib.name) == next) && !path_in(skip, sib.path)) take.append(sib);
	if (take.isEmpty()) return QString();
	const QString flags = name_flags(set[cur].name);
	for (const xSibling& sib : take)
		if (name_flags(sib.name) == flags) return sib.path;
	for (const xSibling& sib : take)
		if (name_flags(sib.name).isEmpty()) return sib.path;
	return take.first().path;
}

// the place k steps on from me among n, round past either end
static int ring(int me, int k, int n) {
	return (((me + k) % n) + n) % n;
}

// An archive's images in its own order, an image a place, as the menus list them: the
// one step places on from the one at path, empty when the archive holds one
static QString arc_next(Computer* comp, const QString& path, int id, int drv, int step, const QStringList& skip,
		QString* place) {
	QString archive, entry, err;
	if (!arc_split(path, &archive, &entry)) return QString();
	const QStringList names = media_candidates(comp, archive, id, drv, &err);
	const int n = int(names.size());
	const int me = int(names.indexOf(entry));
	if ((n < 2) || (me < 0)) return QString();
	for (int k = 1; k < n; k++) {
		const int i = ring(me, k * step, n);
		const QString res = arc_join(archive, names[i]);
		if (path_in(skip, res)) continue;
		if (place) {
			// the OSD letters are ASCII, a name may not be: then its number
			const QString name = QFileInfo(names[i]).fileName();
			bool ascii = true;
			for (const QChar c : name) ascii = ascii && (c.unicode() >= 0x20) && (c.unicode() < 0x7f);
			*place = ascii ? name : QString("%1 of %2").arg(i + 1).arg(n);
		}
		return res;
	}
	return QString();
}

QString media_next_side(Computer* comp, const QString& path, int id, int drv, const QStringList& seen, int step,
		const QStringList& skip, QString* place, bool whole) {
	int cur;
	const QList<xSibling> set = side_set(comp, path, id, drv, &cur);
	// the places in the set, in its order
	QStringList values;
	if (cur >= 0) {
		for (const xSibling& sib : set) {
			const QString val = sib_value_of(sib.name);
			if (!values.contains(val)) values.append(val);
		}
	}
	const int n = int(values.size());
	if (n < 2) return whole ? arc_next(comp, path, id, drv, step, skip, place) : QString();
	const int me = int(values.indexOf(sib_value_of(set[cur].name)));
	// step places on, round past either end; a place with every file skipped is passed
	for (int k = 1; k < n; k++) {
		const QString next = values[ring(me, k * step, n)];
		const QString res = side_pick(comp, set, cur, next, id, drv, path, seen, skip);
		if (res.isEmpty()) continue;
		if (place) *place = place_text(next);
		return res;
	}
	return QString();
}

static QString arc_size_text(qint64 size) {
	if (size < 0) return QString();
	if (size < 1024) return QString("%1 bytes").arg(size);
	return QString("%1 KB").arg((size + 1023) / 1024);
}

// Several images: the user picks one, the first in natural order ready
static QString media_pick(Computer* comp, const QString& archive, const QStringList& names) {
	QMap<QString, qint64> sizes;
	QString err;
	if (const QVector<arcEntry>* list = arc_entries(archive, &err)) {
		for (const arcEntry& entry : *list) sizes.insert(entry.name, entry.size);
	}
	QDialog dlg;
	dlg.setWindowTitle("Open from Archive");
	QVBoxLayout* lay = new QVBoxLayout(&dlg);
	QLabel* lab = new QLabel(QString("<b>%1</b> holds %2 images. Open:")
		.arg(QFileInfo(archive).fileName().toHtmlEscaped()).arg(names.size()));
	lab->setWordWrap(true);
	lay->addWidget(lab);
	QTreeWidget* tree = new QTreeWidget;
	tree->setColumnCount(3);
	tree->setHeaderLabels({"Name", "Kind", "Size"});
	tree->setRootIsDecorated(false);
	tree->setUniformRowHeights(true);
	tree->setAllColumnsShowFocus(true);
	for (const QString& name : names) {
		xFileTypeInfo* inf = file_find_hw_ext(comp->hw->id, name);
		QTreeWidgetItem* it = new QTreeWidgetItem({name, inf ? QString(inf->name) : QString(),
			arc_size_text(sizes.value(name, -1))});
		it->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
		tree->addTopLevelItem(it);
	}
	tree->setCurrentItem(tree->topLevelItem(0));
	// as tall as its rows, up to 12 of them; more scroll
	QHeaderView* hdr = tree->header();
	hdr->setStretchLastSection(false);
	const int rows = qMin(int(names.size()), 12);
	const bool more = names.size() > rows;
	tree->setFixedHeight(hdr->sizeHint().height() + 2 * tree->frameWidth()
		+ tree->sizeHintForRow(0) * rows + 2);
	// Each column as wide as what it holds, every row of it, with the style's margins:
	// the delegate's measure, as the view itself only measures the rows in sight. Done
	// once the window is up - showing it puts every column back to the default width -
	// and the window then fits them; the name takes what is left over
	QTimer::singleShot(0, &dlg, [&dlg, tree, hdr, more]() {
		QStyleOptionViewItem opt;
		opt.initFrom(tree->viewport());
		opt.widget = tree;
		int width = 2 * tree->frameWidth() + (more ? tree->verticalScrollBar()->sizeHint().width() : 0);
		for (int i = 0; i < 3; i++) {
			int w = hdr->sectionSizeHint(i);
			for (int r = 0; r < tree->topLevelItemCount(); r++)
				w = qMax(w, tree->itemDelegate()->sizeHint(opt, tree->model()->index(r, i)).width());
			hdr->resizeSection(i, w);
			width += w;
		}
		hdr->setSectionResizeMode(0, QHeaderView::Stretch);
		tree->setMinimumWidth(qMin(width, 720));
		const QPoint mid = dlg.geometry().center();
		dlg.adjustSize();
		dlg.move(dlg.pos() + mid - dlg.geometry().center());
	});
	lay->addWidget(tree);
	QDialogButtonBox* btns = new QDialogButtonBox;
	QPushButton* open = btns->addButton("Open", QDialogButtonBox::AcceptRole);
	btns->addButton(QDialogButtonBox::Cancel);
	open->setDefault(true);
	lay->addWidget(btns);
	QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	QObject::connect(tree, &QTreeWidget::itemDoubleClicked, &dlg, &QDialog::accept);
	if ((dlg.exec() != QDialog::Accepted) || !tree->currentItem()) return QString();
	return tree->currentItem()->text(0);
}

QString media_from_archive(Computer* comp, const QString& archive, int id, int drv, bool ask) {
	QString err;
	const QStringList names = media_candidates(comp, archive, id, drv, &err);
	if (names.isEmpty()) {
		xlog(XLG_FILE, XLL_WARN, "%s: %s", archive.toLocal8Bit().constData(), err.toLocal8Bit().constData());
		shitHappens(err.toLocal8Bit().constData());
		return QString();
	}
	QString name = names.first();
	if (ask && (names.size() > 1)) {
		name = media_pick(comp, archive, names);
		if (name.isEmpty()) return QString();
	}
	return arc_join(archive, name);
}

// what the loader named after the copy it read is named after the archive path
static void media_rename(Computer* comp, const QString& local, const QString& path) {
	QByteArray from = local.toLocal8Bit();
	QByteArray to = path.toLocal8Bit();
	if (comp->tape->path && (from == comp->tape->path)) {
		int changed = comp->tape->changed;
		tape_set_path(comp->tape, to.constData());
		comp->tape->changed = changed;
	}
	if (comp->slot->path && (from == comp->slot->path)) sltSetPath(comp->slot, to.constData());
	for (int i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		if (flp->path && (from == flp->path)) flp_set_path(flp, to.constData());
	}
}

// what the last load opened, an archive's entry named: the path the drive has
static QString last_loaded;

int load_file(Computer* comp, const char* name, int id, int drv) {
	last_as_kind = AS_NONE;
	xspeed_modes_off();
	QString path = arc_canonical(QString::fromLocal8Bit(name));
	if (path.isEmpty()) return ERR_CANT_OPEN;
	// an archive given as it is: its first image, nothing asked (the gui asked already)
	if (arc_bare(path) && (id != FG_RAW)) {
		path = media_from_archive(comp, path, id, drv, false);
		if (path.isEmpty()) return ERR_CANCEL;		// said why
	}
	QString why;
	QString local = arc_local(path, &why);	// the bytes, unpacked when in an archive
	if (local.isEmpty()) {
		xlog(XLG_FILE, XLL_WARN, "open %s: %s", path.toLocal8Bit().constData(), why.toLocal8Bit().constData());
		shitHappens(why.toLocal8Bit().constData());
		return ERR_CANCEL;
	}
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (inf && (inf->id == FL_POK)) inf = NULL;	// not a medium: see file_is_pok
	int err = inf ? ERR_OK : ERR_FILE_TYPE;
	if (drv < 0) drv = 0;
	if (inf && inf->ch && !comp->dif->flp[drv & 3]->fitted) {
		err = ERR_NO_DRIVE;
	} else if (inf && inf->load) {
		if (replace_ok(comp, inf, drv) != ERR_OK) {		// a failed save has been shown
			err = ERR_CANCEL;
		} else {
			if (inf->id == FL_RZX)
				rzxr_stop(comp);		// playing one ends the one being made
			else if (comp->rzx.play)
				rzxStop(comp);			// the recording would go on feeding what was opened
			err = inf->load(comp, local.toLocal8Bit().data(), drv);
			if (local != path) media_rename(comp, local, path);
			if ((err == ERR_OK) && (inf->id == FL_SZX)) {
				szx_mount(comp, path);
				szx_warn();
			}
			disk_boot(comp, drv, inf->id);
			if (err == ERR_OK) {
				last_as_kind = as_kind_of(inf->id);
				last_as_drv = drv;
				switch (inf->id) {
					case FL_SNA: case FL_Z80: case FL_SZX: case FL_SPG: case FL_NEX: case FL_SNX:
						rzx_rec_touch();
						last_snapshot = path;
						labels_rom_scan(comp);		// an SZX may hold a ROM of its own
						break;
					case FL_RZX:			// its snapshots load as it plays
						last_snapshot = path;
						break;
				}
				if (inf->id == FL_RZX)
					rzx_path = path;
			}
		}
	}
	if (err == ERR_OK) {
		last_loaded = path;
		// the drive only means anything for a disk (inf->ch marks one)
		if (inf && inf->ch) {
			xlog(XLG_FILE, XLL_INFO, "open %s into drive %c: %s", inf->name,
				'A' + (drv & 3), path.toLocal8Bit().constData());
		} else {
			xlog(XLG_FILE, XLL_INFO, "open %s: %s", inf ? inf->name : "?",
				path.toLocal8Bit().constData());
		}
	} else {
		xlog(XLG_FILE, XLL_WARN, "open %s failed, error %i",
			path.toLocal8Bit().constData(), err);
	}
	// whatever was loaded, the sound devices may stand elsewhere now
	if (err == ERR_OK) comp->sndSeed = 1;
	file_errors(err);
	return err;
}

bool file_is_pok(const QString& path) {
	return path.endsWith(".pok", Qt::CaseInsensitive);
}

// a tape or a disk, or a file into the disk in a drive: what it opens into leaves the
// machine's state as it is
bool file_is_medium(Computer* comp, const QString& path, int id) {
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	return inf && (inf->ch || (as_kind_of(inf->id) == AS_TAPE) || (inf->id == FL_HOBETA));
}

// MOUNTING AWAY FROM THE EMULATION (xMount, filer.h)

typedef int (*xTapeReader)(Tape*, const char*);
typedef int (*xDiskReader)(Floppy*, const char*);

static xTapeReader mount_tape_reader(int ftype) {
	switch (ftype) {
		case FL_TAP: return tap_read;
		case FL_TZX: return tzx_read;
		case FL_WAV: return wav_read;
	}
	return nullptr;
}

// hobeta is not here: it writes into the disk already in the drive
static xDiskReader mount_disk_reader(int ftype) {
	switch (ftype) {
		case FL_TRD: return trd_read;
		case FL_SCL: return scl_read;
		case FL_FDI: return fdi_read;
		case FL_UDI: return udi_read;
		case FL_TD0: return td0_read;
		case FL_DSK: return dsk_read;
		case FL_IMA: case FL_PCIMG: return ima_read;
	}
	return nullptr;
}

static void mount_no_irq(int, void*) {}

// what the deck or the drive holds: another image put in meanwhile changes it
static QByteArray mount_stamp(Computer* comp, const xMount* m) {
	if (m->disk) {
		Floppy* flp = comp->dif->flp[m->drv];
		return QByteArray(flp->path ? flp->path : "") + '|' + QByteArray::number(flp->insert)
			+ '|' + QByteArray::number(flp->changed);
	}
	Tape* tap = comp->tape;
	return QByteArray(tap->path ? tap->path : "") + '|' + QByteArray::number(tap->blkCount)
		+ '|' + QByteArray::number(tap->changed);
}

// No RZX playing, which a load stops; an archive given whole is picked from first, and a
// disk goes only into a drive that is there for it - load_file says why not
bool media_mount_can(Computer* comp, const QString& path, int id, int drv) {
	if (comp->rzx.play || arc_bare(path)) return false;
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (!inf || !inf->load) return false;
	if (!inf->ch) return mount_tape_reader(inf->id) != nullptr;
	Floppy* flp = comp->dif->flp[((drv < 0) ? 0 : drv) & 3];
	return mount_disk_reader(inf->id) && flp->fitted && !comp->dif->out;
}

bool media_mount_asks(Computer* comp, const QString& path, int id, int drv) {
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (inf && inf->ch) return comp->dif->flp[((drv < 0) ? 0 : drv) & 3]->changed;
	return comp->tape->changed && (comp->tape->blkCount > 0);
}

int media_mount_begin(Computer* comp, const QString& path, int id, int drv, xMount* m, bool ask) {
	m->comp = comp;
	m->path = arc_canonical(path);
	if (m->path.isEmpty()) return ERR_CANT_OPEN;
	xFileTypeInfo* inf = file_type_of(comp, id, m->path);
	m->ftype = inf->id;
	m->disk = inf->ch;
	m->drv = (drv < 0) ? 0 : (drv & 3);
	if (!ask && media_mount_asks(comp, m->path, id, drv)) return ERR_CANCEL;
	if (replace_ok(comp, inf, m->drv) != ERR_OK) return ERR_CANCEL;	// a failed save has been shown
	if (m->disk) {
		Floppy* flp = comp->dif->flp[m->drv];
		m->flp = flpCreate(flp->id, mount_no_irq, nullptr);
		// what the drive is, and the protect tab a disk keeps unless its file says
		m->flp->trk80 = flp->trk80;
		m->flp->doubleSide = flp->doubleSide;
		m->flp->trklen = flp->trklen;
		m->flp->protect = flp->protect;
		int i = 0;
		while (boot_ft[i] && (boot_ft[i] != m->ftype))
			i++;
		if (conf.boot && boot_ft[i])
			m->boot = QByteArray(conf.path.boot.c_str());
	} else {
		m->tape = tape_create(nullptr, nullptr);
	}
	m->stamp = mount_stamp(comp, m);
	return ERR_OK;
}

void media_mount_read(xMount* m) {
	QString local = arc_local(m->path, &m->why);	// the bytes, unpacked when in an archive
	if (local.isEmpty()) {
		m->err = ERR_CANCEL;
		return;
	}
	QByteArray loc = local.toLocal8Bit();
	QByteArray name = m->path.toLocal8Bit();
	if (m->disk) {
		m->err = mount_disk_reader(m->ftype)(m->flp, loc.constData());
		if ((m->err == ERR_OK) && !m->boot.isEmpty())
			boot_read(m->flp, m->boot.constData());
		if (local != m->path)
			flp_set_path(m->flp, name.constData());
	} else {
		m->err = mount_tape_reader(m->ftype)(m->tape, loc.constData());
		if (local != m->path)
			tape_set_path(m->tape, name.constData());
	}
	if ((m->err == ERR_OK) && m->game)
		game_hash_ahead(m->path);
}

bool media_mount_fits(xMount* m) {
	Computer* comp = m->comp;
	return (m->err == ERR_OK) && (comp == conf.zx) && (mount_stamp(comp, m) == m->stamp)
		&& (!m->disk || (comp->dif->flp[m->drv]->fitted && !comp->dif->out));
}

bool media_mount_put(xMount* m) {
	if (!media_mount_fits(m)) return false;
	Computer* comp = m->comp;
	xspeed_modes_off();		// as load_file
	if (m->disk)
		flp_take_disk(comp->dif->flp[m->drv], m->flp);
	else
		tap_take_image(comp->tape, m->tape);
	return true;
}

int media_mount_end(xMount* m) {
	emu_lock();
	bool put = media_mount_put(m);
	if (put) x_media_replaced();
	emu_unlock();
	if (!put) {
		xlog(XLG_FILE, XLL_INFO, "open %s: something else went in meanwhile", m->path.toLocal8Bit().constData());
		return ERR_CANCEL;
	}
	media_mount_note(m);
	return ERR_OK;
}

void media_mount_note(xMount* m) {
	QByteArray name = m->path.toLocal8Bit();
	last_as_kind = as_kind_of(m->ftype);
	last_as_drv = m->drv;
	last_loaded = m->path;
	xFileTypeInfo* inf = file_find_type(m->ftype);
	if (m->disk) {
		xlog(XLG_FILE, XLL_INFO, "open %s into drive %c: %s", inf->name, 'A' + m->drv, name.constData());
	} else {
		xlog(XLG_FILE, XLL_INFO, "open %s: %s", inf->name, name.constData());
	}
}

void media_mount_fail(xMount* m) {
	last_as_kind = AS_NONE;
	QByteArray name = m->path.toLocal8Bit();
	if (!m->why.isEmpty()) {
		xlog(XLG_FILE, XLL_WARN, "open %s: %s", name.constData(), m->why.toLocal8Bit().constData());
		shitHappens(m->why.toLocal8Bit().constData());
		return;
	}
	xlog(XLG_FILE, XLL_WARN, "open %s failed, error %i", name.constData(), m->err);
	file_errors(m->err);
}

// after end the Tape or the Floppy holds what went out
void media_mount_free(xMount* m) {
	if (m->tape) tape_destroy(m->tape);
	if (m->flp) flpDestroy(m->flp);
	m->tape = nullptr;
	m->flp = nullptr;
}

static xMediaOpen media_last = {QString(), FG_ALL, 0, 0, false};

void media_opened(const QString& path, int id, int drv, int run, bool pinned, bool newGame) {
	media_last = {path, id, drv, run, pinned};
	if (drv < 1) {				// a disk in drive B is not the program
		labels_image(path);
		if (newGame) game_set(last_loaded.isEmpty() ? path : last_loaded);
	}
}

xMediaOpen media_last_open() {
	return media_last;
}

// Run the machine on to the end of the next INT acknowledge, where loading puts
// a .sna with interrupts off back, a frame and a bit at most. Returns 0 if it
// never got there: interrupts kept off, or a breakpoint.
static int snap_align(Computer* comp) {
	CPU* cpu = comp->cpu;
	int flen = comp_frame_ticks(comp);
	int t0 = comp->tickCount;
	int done = 0;
	emu_lock();
	while ((comp->tickCount - t0 < flen + flen / 4) && !done && !comp->flgBRK) {
		int iff = cpu->flgIFF1;
		int pc = cpu->regPC;
		int sp = cpu->regSP;
		snd_scope_step(comp, compExec(comp));
		// taken: interrupts went off and the pc was pushed - past a HALT if the
		// cpu sat in one - where a DI pushes nothing
		if (iff && !cpu->flgIFF1 && (cpu->regSP == ((sp - 2) & 0xffff))) {
			int ret = memRd(comp->mem, cpu->regSP) | (memRd(comp->mem, (cpu->regSP + 1) & 0xffff) << 8);
			done = (ret == pc) || (ret == ((pc + 1) & 0xffff));
		}
	}
	emu_unlock();
	return done;
}

// Whether a snapshot saved by hand carries its media: 1 yes, 0 no, -1 cancelled.
// The Save dialog's line picked says it and is remembered; on the first save the
// line left as it was is asked about, once. No dialog (the command line): the
// setting, embedded until answered.
static int snap_carry(const QString& words, const QString& picked, const QString& carryLine, const QString& preLine) {
	int& mode = conf.saves.snapMedia;
	if (carryLine.isEmpty()) return mode != SNAP_MEDIA_LINKS;
	if ((mode == SNAP_MEDIA_ASK) && (picked == preLine)) {
		QMessageBox box(QMessageBox::Question, "Save Snapshot", QString("<b>Put the %0 into the snapshot?</b>").arg(words),
			QMessageBox::NoButton, fileDlgParent);
		box.setInformativeText("Inside, it loads whole even if the files are moved or changed. "
			"Linked, it is smaller but needs the files where they are.\n\n"
			"Asked once: the file type in the Save dialog changes it.");
		QPushButton* embed = box.addButton("Embed", QMessageBox::YesRole);
		QPushButton* link = box.addButton("Link Only", QMessageBox::NoRole);
		box.addButton(QMessageBox::Cancel);
		box.setDefaultButton(embed);
		box.exec();
		if (box.clickedButton() == embed) mode = SNAP_MEDIA_CARRY;
		else if (box.clickedButton() == link) mode = SNAP_MEDIA_LINKS;
		else return -1;
	} else {
		mode = (picked == carryLine) ? SNAP_MEDIA_CARRY : SNAP_MEDIA_LINKS;
	}
	return mode == SNAP_MEDIA_CARRY;
}

int save_file(Computer* comp, const char* name, int id, int drv, int live, const QString& suggest) {
	QString path = QString::fromLocal8Bit(name);
	QString flt;
	QString ext;
	QString words;		// what an .szx can carry; the dialog's lines for it
	QString carryLine;
	QString preLine;
	xFileTypeInfo* inf = NULL;
	xFileTypeInfo* tin;
	xFileGroupInfo* grp;
	int i;
	int flg;
	if (id == FG_DISK)
		id = disk_id[drv & 3];
	if (id == FG_ALL)
		id = detect_hw_id(comp->hw->id);
	int err = ERR_OK;
	if (path.isEmpty()) {
		flt = file_get_hw_filter(comp, id, 1);
		if (flt.isEmpty()) {
			flt = file_get_group_filter(comp, id, 1);
			if (flt.isEmpty())
				flt = file_get_type_filter(id, 1);
		}
		if (!flt.isEmpty()) {
			// the snapshot line twice when there is media to carry: with it, or linked
			QStringList lines = flt.split(";;");
			QString group = file_find_group(FG_SNAPSHOT)->name;
			int snap = lines.size() - 1;
			while ((snap >= 0) && !lines[snap].startsWith(group + " (")) snap--;
			words = (snap >= 0) ? carry_words(comp) : QString();
			if (!words.isEmpty()) {
				carryLine = QString("%0 with %1 (*.szx)").arg(group, words);
				lines.insert(snap, carryLine);
				flt = lines.join(";;");
				preLine = (conf.saves.snapMedia == SNAP_MEDIA_LINKS) ? lines[snap + 1] : carryLine;
			}
			QFileDialog* filer = file_dialog();
			filer->setWindowTitle("Save File");
			filer->setNameFilter(flt);
			if (!preLine.isEmpty()) filer->selectNameFilter(preLine);
			filer->setAcceptMode(QFileDialog::AcceptSave);
			file_dialog_filtered(filer);
			if (suggest.isEmpty()) {
				filer->setDirectory(conf.lastDir.c_str());
			} else {
				// a folder gone with the file: the last one used
				QString dir = QFileInfo(suggest).absolutePath();
				filer->setDirectory(QDir(dir).exists() ? dir : QString(conf.lastDir.c_str()));
				filer->selectFile(QFileInfo(suggest).fileName());
			}
			filer->setHistory(QStringList());
			if (filer->exec()) {
				path = filer->selectedFiles().first();
				flt = filer->selectedNameFilter();
				grp = file_detect_grp(flt);
				if (grp->id != FL_NONE) {
					drv = grp->drv;
					i = 0;
					flg = 1;
					// scan group file types and check if path extension is the same
					while ((grp->child[i] != FL_NONE) && flg) {
						tin = file_find_type(grp->child[i]);
						if (tin) {
							if (path.endsWith(tin->ext, Qt::CaseInsensitive)) {
								flg = 0;
								inf = tin;
							}
						}
						i++;
					}
					// if no filetypes found, add default extension
					if (flg)
						path.append(grp->defext);
				} else {
					tin = file_detect_type(flt);
					if (tin->id != FL_NONE) {
						if (tin->ext && !path.endsWith(tin->ext, Qt::CaseInsensitive))
							path.append(tin->ext);
						inf = tin;
					}
				}
				conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
			}
		}
	}
	if (path.isEmpty()) {
		// no dialog: nothing of this kind can be written
		err = flt.isEmpty() ? ERR_CANT_SAVE : ERR_CANCEL;
		file_errors(err);
		return err;
	}
	if (drv < 0)
		drv = 0;
	if (!inf)
		// inf = file_ext_type(path);
		inf = file_find_hw_ext(comp->hw->id, path);
	if (inf) {
		xlog(XLG_FILE, XLL_INFO, "filetype: %s", inf->name);
		if (inf->save) {
			// a .sna has no frame position: a running machine is saved where
			// loading puts it back, a held one stays put and is told so
			if (inf->id == FL_SNA) {
				int aligned = live && !comp->rzx.play && snap_align(comp);
				if (!aligned)
					comp->msg = (char*)" SNA: no frame position, use SZX ";
			}
			int carry = (inf->id == FL_SZX) ? snap_carry(words, flt, carryLine, preLine) : 0;
			if (carry < 0) return ERR_CANCEL;	// nothing to tell
			saveCarry = (carry > 0);
			err = inf->save(comp, path.toLocal8Bit().data(), drv);
			saveCarry = false;
		} else {
			err = ERR_CANT_SAVE;
		}
	} else {
		err = ERR_FILE_TYPE;
	}
	if (err != ERR_OK)
		xlog(XLG_FILE, XLL_WARN, "save %s failed, error %i", path.toLocal8Bit().constData(), err);
	file_errors(err);
	return err;
}

// never into an archive: an image from one is saved as a file of its own
static bool file_can_save(Computer* comp, const char* path) {
	if (!path || !*path || arc_split(QString::fromLocal8Bit(path))) return false;
	xFileTypeInfo* inf = file_find_hw_ext(comp->hw->id, QString::fromLocal8Bit(path));
	return inf && inf->save && !ft_export(inf);
}

// an image from an archive is offered beside the archive, under the entry's name;
// one whose file is gone (moved since a snapshot brought it back) under its name
int save_back(Computer* comp, const char* path, int id, int drv) {
	if (file_can_save(comp, path)) {
		QString file = QString::fromLocal8Bit(path);
		if (QFileInfo(file).isFile()) return save_file(comp, path, id, drv);
		return save_file(comp, "", id, drv, 0, file);
	}
	QString img = path ? QString::fromLocal8Bit(path) : QString();
	return save_file(comp, "", id, drv, 0, arc_split(img) ? arc_beside(img, QString()) : QString());
}


// Quick saves. A save is an .szx in the game's folder (xcore/saves.h); a
// machine that cannot be one is held in memory instead, until exit.
static xState* quickState = NULL;
static std::string quickMac;		// the machine the one in memory is of

// The file is only built while the machine is held, and packed after: packing
// several megabytes of an Evo's memory would hold it for frames.
int quick_save(Computer* comp, const QString& path) {
	szxBuf b;
	int res = 0;
	emu_lock();
	int can = szxCanSave(comp);
	if (!can) {
		if (!quickState) quickState = xstate_create();
		res = (quickState && xstate_save(quickState, comp)) ? 1 : 0;
		quickMac = res ? conf.macId : std::string();
	}
	int built = can && !path.isEmpty() && (build_szx(comp, &b, conf.saves.carry) == ERR_OK);
	emu_unlock();
	if (built && (szx_write(&b, path.toLocal8Bit().data()) == ERR_OK))
		res = 2;
	return res;
}

// what a quick load replaced, to take it back with: an .szx in memory carrying
// its media, or the machine's state when it cannot be one
typedef struct {
	QByteArray szx;
	xState* st;
	std::string mac;
	xGame game;
} quickKept;

static quickKept undoKept = {QByteArray(), NULL, std::string(), xGame()};

// A quick save may be from before what a recording has logged since, so the
// recording joins there instead of going back with the machine. A breakpoint
// hit it holds is from then, not for now.
int quick_xstate_load(xState* st, Computer* comp) {
	rzxRecPos rec = comp->rzx.rec;
	int res = xstate_load(st, comp);
	if (res) comp_brk_clear(comp);
	if (res && rec.on) {
		comp->rzx.rec = rec;
		rzx_rec_touch();
	}
	return res;
}

// the machine as it is now, into k (held)
static bool quick_keep(Computer* comp, quickKept* k) {
	k->szx.clear();
	k->mac.clear();
	k->game = game_now();
	if (szxCanSave(comp)) {
		szxBuf b;
		if (build_szx(comp, &b, true) != ERR_OK) return false;
		k->szx = QByteArray((const char*)b.data, (int)b.len);
		sb_free(&b);
	} else {
		if (!k->st) k->st = xstate_create();
		if (!k->st || !xstate_save(k->st, comp)) return false;
	}
	k->mac = conf.macId;
	return true;
}

// ...and back (held)
static int quick_put(Computer* comp, quickKept* k) {
	int res = 0;
	if (!k->szx.isEmpty()) {
		if (comp->rzx.play) rzxStop(comp);
		res = (loadSZX_buf(comp, (const unsigned char*)k->szx.constData(), k->szx.size()) == ERR_OK);
		if (res) {
			szx_exact = true;
			szx_mount(comp, QString());
			szx_exact = false;
			rzx_rec_touch();
		}
	} else if (k->st) {
		res = quick_xstate_load(k->st, comp);
	}
	if (res) {
		game_put(k->game);
		last_snapshot = k->game.image;
		labels_rom_scan(comp);		// an SZX may hold a ROM of its own
	}
	return res;
}

// the machine as it is, for quick_undo: taken before a quick load switches
// the machine, which resets it
static quickKept redoKept = {QByteArray(), NULL, std::string(), xGame()};

void quick_keep_undo(Computer* comp) {
	emu_lock();
	if (!quick_keep(comp, &undoKept)) undoKept.mac.clear();
	emu_unlock();
}

int quick_load(Computer* comp, const QString& path) {
	emu_lock();
	game_set_save(path);
	szx_exact = true;
	int res = (load_file(comp, path.toLocal8Bit().data(), FG_ALL, 0) == ERR_OK);
	szx_exact = false;
	if (res) {
		cheats_forget();
		last_snapshot = game_image();	// the title names the game, not the save
	}
	emu_unlock();
	return res;
}

int quick_load_held(Computer* comp) {
	if (!quickState || (quickMac != conf.macId)) return 0;
	emu_lock();
	int res = quick_xstate_load(quickState, comp);
	if (res) cheats_forget();
	emu_unlock();
	return res;
}

std::string quick_undo_machine() {
	return undoKept.mac;
}

void quick_undo_prepare(Computer* comp) {
	emu_lock();
	if (!quick_keep(comp, &redoKept)) redoKept.mac.clear();
	emu_unlock();
}

// back to before the last quick load; again, and that load is back
int quick_undo(Computer* comp) {
	if (undoKept.mac.empty() || (undoKept.mac != conf.macId)) return 0;
	emu_lock();
	int res = quick_put(comp, &undoKept);
	if (res && !redoKept.mac.empty())
		std::swap(undoKept, redoKept);
	redoKept.mac.clear();
	redoKept.szx.clear();
	if (res) cheats_forget();
	emu_unlock();
	return res;
}

// The save dialog on its own, for an export that is not one of the file types
// the tables above know. Same dialog as every other open and save in the app.
// own: a file that is written over without asking - the one a recording goes on in
QString file_ask_save(const char* title, const char* filter, const char* ext, const QString& suggest, const QString& own) {
	QFileDialog* filer = file_dialog();
	filer->setWindowTitle(title);
	filer->setNameFilter(filter);
	filer->setAcceptMode(QFileDialog::AcceptSave);
	file_dialog_filtered(filer);
	if (suggest.isEmpty()) {
		filer->setDirectory(conf.lastDir.c_str());
	} else {
		filer->setDirectory(QFileInfo(suggest).absolutePath());
		filer->selectFile(QFileInfo(suggest).fileName());
	}
	filer->setHistory(QStringList());
	filer->setOption(QFileDialog::DontConfirmOverwrite, !own.isEmpty());
	int ok = filer->exec();
	filer->setOption(QFileDialog::DontConfirmOverwrite, false);
	if (!ok) return QString();
	QString path = filer->selectedFiles().first();
	if (!path.endsWith(ext, Qt::CaseInsensitive))
		path.append(ext);
	if (!own.isEmpty() && QFileInfo::exists(path) && (QFileInfo(path) != QFileInfo(own))) {
		QString q = QString("%1 already exists.\nDo you want to replace it?").arg(QFileInfo(path).fileName());
		if (QMessageBox::warning(filer->parentWidget(), title, q, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return QString();
	}
	conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
	return path;
}

// one file of one kind, to open
QString file_ask_load(const char* title, const char* filter) {
	QFileDialog* filer = file_dialog_open_mode(title, filter);
	if (!filer->exec()) return QString();
	QString path = filer->selectedFiles().first();
	conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
	return path;
}

// old

void initFileDialog(QWidget* par) {
	fileDlgParent = par;
}

// Every Qt file dialog restores the column widths any Qt program last left in
// the shared QtProject settings, and a long image name gets cut in them. Give
// the name all the room the other columns do not need, whatever was stored.
// Called on Show, after that restore; a native dialog has no tree and is skipped.
// Also tunes the QFileDialog::get*() dialogs, which may open in a big folder.
void fitFileDialog(QWidget* w) {
	QFileDialog* dlg = qobject_cast<QFileDialog*>(w);
	QTreeView* tree = dlg ? dlg->findChild<QTreeView*>("treeView") : nullptr;
	if (!tree) return;
	file_dialog_tune(dlg);
	file_dialog_recall(dlg);
	file_dialog_filtered(dlg);	// whatever patterns Qt put on the model since
	QHeaderView* hdr = tree->header();
	hdr->setSectionResizeMode(QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(0, QHeaderView::Stretch);
	hdr->setStretchLastSection(false);
	// the places list on the left: same shared settings, stored narrower than its names
	QSplitter* split = dlg->findChild<QSplitter*>("splitter");
	QListView* side = dlg->findChild<QListView*>("sidebar");
	if (!split || !side || (split->indexOf(side) != 0) || (split->count() != 2)) return;
	int need = side->sizeHintForColumn(0) + 2 * side->frameWidth() + side->verticalScrollBar()->sizeHint().width();
	QList<int> sz = split->sizes();
	if (sz[0] >= need) return;
	split->setSizes({need, qMax(sz[0] + sz[1] - need, 0)});
}

// ERR_OK once the changed medium is safe to lose: saved, or the user said No
static int ask_save_changed(Computer* comp, QString what, const char* path, int id, int drv) {
	if (path && *path)
		what += ": " + arc_name(QString::fromLocal8Bit(path));
	QMessageBox mbox;
	mbox.setText(QString("<b>%0 has been changed</b>").arg(what.toHtmlEscaped()));
	mbox.setInformativeText("Do you want to save it?");
	mbox.setStandardButtons(QMessageBox::Yes|QMessageBox::No|QMessageBox::Cancel);
	mbox.setIcon(QMessageBox::Warning);
	switch (mbox.exec()) {
		case QMessageBox::Yes: return save_back(comp, path, id, drv);
		case QMessageBox::No: return ERR_OK;
	}
	return ERR_CANCEL;
}

int saveChangedDisk(Computer* comp,int id) {
	id &= 3;
	Floppy* flp = comp->dif->flp[id];
	if (!flp->changed) return ERR_OK;
	return ask_save_changed(comp, QString("Disk %0").arg(QChar('A' + id)), flp->path, FG_DISK, id);
}

int saveChangedTape(Computer* comp) {
	Tape* tape = comp->tape;
	if (!tape->changed || (tape->blkCount < 1)) return ERR_OK;
	return ask_save_changed(comp, "Tape", tape->path, FG_TAPE, -1);
}

int testSlotOn(Computer* comp) {
	int res = 0;
	for (int i = 0; i < 256; i++) {
		if (comp->mem->map[i].type == MEM_SLOT) {
			res = 1;
			i = 256;
		}
	}
	return res;
}
