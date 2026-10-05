#include "opt_hotkeytab.h"
#include "../../xcore/xcore.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFont>
#include <QMenu>
#include <QMessageBox>
#include <QStyledItemDelegate>

// PortableText, not NativeText: NativeText follows Qt's own Ctrl/Meta convention,
// which is inverted from ours on macOS and swaps the symbols.
static QString shortcutText(const QKeySequence& seq) {
	QString str = seq.toString();
#ifdef __APPLE__
	str.replace("Meta", "Cmd");
#endif
	return str;
}

static const char* presetTitles[HKP_COUNT] = {"Modern", "Classic", "Custom"};

// A key typed into the search in another layout is the Latin key in its place:
// F5 typed with a Cyrillic layout on comes out as its own letter and a 5.
static QString latinKeys(const QString& txt) {
	QString res;
	for (QChar ch : txt) {
		if (ch.unicode() >= 0x400) {
			int lat = key2qid(qKey2id(ch.toUpper().unicode()));
			if ((lat > 0x20) && (lat < 0x7f)) ch = QChar(lat);
		}
		res += ch;
	}
	return res;
}

// model

// The sections are the menus', so a key is found where its command is. The
// debugger's own keys go by the part of it they work in.
enum {HK_FILE = 0, HK_MACHINE, HK_TIME, HK_MEDIA, HK_VIEW, HK_INPUT, HK_DEBUG,
	HK_DEBUGGER, HK_DISASM, HK_DUMP, HK_OTHER, HK_COUNT};
static const char* hk_names[HK_COUNT] = {"File", "Machine", "Time", "Media", "View", "Input", "Debug",
	"Debugger", "Disassembler", "Memory dump", "Other"};

static int hk_section(const xShortcut& cut) {
	if (!(cut.grp & SCG_MAIN)) {
		if (cut.grp & SCG_DISASM) return HK_DISASM;
		if (cut.grp & SCG_DUMP) return HK_DUMP;
		return HK_DEBUGGER;
	}
	switch (cut.id) {
		case XCUT_LOAD: case XCUT_RELOAD: case XCUT_SAVE: case XCUT_FASTSAVE: case XCUT_FAVORITE:
		case XCUT_SCRSHOT: case XCUT_COMBOSHOT: case XCUT_VIDREC: case XCUT_WAV_OUT: case XCUT_OPTIONS:
		case XCUT_QUICKSAVE: case XCUT_QUICKLOAD: case XCUT_HOTKEYS:
			return HK_FILE;
		case XCUT_RESET: case XCUT_RES_48: case XCUT_RES_128: case XCUT_RES_DOS: case XCUT_RES_SERVICE:
		case XCUT_NMI: case XCUT_TURBO: case XCUT_SPEED_UP: case XCUT_SPEED_DOWN: case XCUT_MUTE:
			return HK_MACHINE;
		case XCUT_PAUSE: case XCUT_FAST: case XCUT_REWIND: case XCUT_FFWD: case XCUT_SLOWMO:
			return HK_TIME;
		case XCUT_TAPWIN: case XCUT_TAPLAY: case XCUT_TAPREC: case XCUT_RZXWIN:
			return HK_MEDIA;
		case XCUT_MOUSE: case XCUT_GRABKBD:
			return HK_INPUT;
		case XCUT_DEBUG: case XCUT_SCRWIN: case XCUT_SNDWIN:
			return HK_DEBUG;
		case XCUT_SIZEX1: case XCUT_SIZEX2: case XCUT_SIZEX3: case XCUT_SIZEX4: case XCUT_SIZEX5:
		case XCUT_SIZEX6: case XCUT_FULLSCR: case XCUT_RATIO: case XCUT_NOFLICK:
		case XCUT_RELOAD_SHD: case XCUT_KEYBOARD:
			return HK_VIEW;
	}
	return HK_OTHER;		// a key nobody placed yet: seen, not lost under View
}

xHotkeyModel::xHotkeyModel(const xHotkeySet* s, QObject* p):xTableModel(p) {
	set = s;
	QVector<xRow> sec[HK_COUNT];
	xShortcut* tab = shortcut_tab();
	for (int i = 0; tab[i].text; i++) {
		QString txt(tab[i].text);
		int col = txt.indexOf(": ");		// "Debugger: Step in" under Debugger is "Step in"
		if (!(tab[i].grp & SCG_MAIN) && (col > 0)) txt = txt.mid(col + 2);
		sec[hk_section(tab[i])].append({i, txt});
	}
	for (int s = 0; s < HK_COUNT; s++) {
		if (sec[s].isEmpty()) continue;
		list.append({-1, QString(hk_names[s])});
		list += sec[s];
	}
}

// a heading is a label, not a row to pick
Qt::ItemFlags xHotkeyModel::flags(const QModelIndex& idx) const {
	return (cut(idx.row()) < 0) ? Qt::ItemIsEnabled : xTableModel::flags(idx);
}

// and the pointer passing over it lights nothing
class xHotkeyItem : public QStyledItemDelegate {
	public:
		xHotkeyItem(QObject* p) : QStyledItemDelegate(p) {}
	protected:
		void initStyleOption(QStyleOptionViewItem* opt, const QModelIndex& idx) const override {
			QStyledItemDelegate::initStyleOption(opt, idx);
			if (!(idx.flags() & Qt::ItemIsSelectable))
				opt->state &= ~(QStyle::State_MouseOver | QStyle::State_HasFocus);
		}
};

int xHotkeyModel::cut(int row) const {
	return ((row < 0) || (row >= list.size())) ? -1 : list[row].tab;
}

QString xHotkeyModel::rowText(int row) const {
	return ((row < 0) || (row >= list.size())) ? QString() : list[row].text;
}

int xHotkeyModel::columnCount(const QModelIndex&) const {
	return 3;
}

int xHotkeyModel::rowCount(const QModelIndex&) const {
	return list.size();
}

QVariant xHotkeyModel::headerData(int sec, Qt::Orientation ori, int role) const {
	if ((ori != Qt::Horizontal) || (role != Qt::DisplayRole)) return QVariant();
	static const char* names[3] = {"Action", "Key", "Alternate"};
	return ((sec >= 0) && (sec < 3)) ? QString(names[sec]) : QVariant();
}

QVariant xHotkeyModel::data(const QModelIndex& idx, int role) const {
	QVariant var;
	if (!idx.isValid()) return var;
	int row = idx.row();
	int col = idx.column();
	if ((row < 0) || (row >= list.size())) return var;
	xShortcut* tab = shortcut_tab();
	int t = list[row].tab;
	QKeySequence keys[2];
	if ((t >= 0) && (col > 0)) hotkeys_resolve(*set, tab[t].id, keys);
	// a key Custom has of its own, unlike its base
	bool own = (t >= 0) && (set->preset == HKP_CUSTOM) && set->over.count(tab[t].id);
	switch (role) {
		case Qt::DisplayRole:
			if (col == 0) {
				var = list[row].text;
			} else if (t >= 0) {
				var = shortcutText(keys[col - 1]);
			}
			break;
		case Qt::FontRole:
			if ((t < 0) || (own && (col > 0))) {
				QFont fnt;
				fnt.setBold(true);
				var = fnt;
			}
			break;
		case Qt::ToolTipRole:
			if (own && (col > 0)) {
				QKeySequence base[2];
				hotkeys_default(set->base, tab[t].id, base);
				var = QString("%0: %1").arg(presetTitles[set->base])
					.arg(base[col - 1].isEmpty() ? QString("none") : shortcutText(base[col - 1]));
			}
			break;
		// a heading in the colors of the debugger's panel titles, which every style sets
		case Qt::BackgroundRole:
			if (t < 0) var = QColor(conf.pal.value("dbg.header.bg"));
			break;
		case Qt::ForegroundRole:
			if (t < 0) var = QColor(conf.pal.value("dbg.header.txt"));
			break;
	}
	return var;
}

// table

xHotkeyTable::xHotkeyTable(QWidget* p):QTableView(p) {
	work = hotkeys_get();
	model = new xHotkeyModel(&work, this);
	edt = new xKeyEditor();		// a window of its own: as a child it never got the keys
	setModel(model);
	setItemDelegate(new xHotkeyItem(this));
	for (int r = 0; r < model->rowCount(); r++)
		if (model->cut(r) < 0) setSpan(r, 0, 1, 3);
	setContextMenuPolicy(Qt::CustomContextMenu);

	preset = new QComboBox;
	for (int i = 0; i < HKP_COUNT; i++)
		preset->addItem(presetTitles[i]);
	filter = new QLineEdit;
	filter->setPlaceholderText("Search an action or a key");
	filter->setClearButtonEnabled(true);
	reset = new QPushButton("Reset");
	reset->setToolTip("Custom back to the preset it was made from");
	findKey = new QPushButton("Find key...");
	findKey->setToolTip("Press a key to see what it does");

	connect(this, &QTableView::doubleClicked, this, &xHotkeyTable::dbl_click);
	connect(this, &QWidget::customContextMenuRequested, this, &xHotkeyTable::rowMenu);
	connect(edt, &xKeyEditor::s_done, this, &xHotkeyTable::set_seq);
	connect(preset, QOverload<int>::of(&QComboBox::activated), this, &xHotkeyTable::presetChanged);
	connect(filter, &QLineEdit::textChanged, this, &xHotkeyTable::filterChanged);
	connect(reset, &QPushButton::clicked, this, &xHotkeyTable::resetCustom);
	connect(findKey, &QPushButton::clicked, this, [this]() {edt->edit(-1, 0, "Find a key", QKeySequence());});
}

QWidget* xHotkeyTable::controls() {
	QWidget* wid = new QWidget;
	QHBoxLayout* lay = new QHBoxLayout(wid);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->addWidget(new QLabel("Layout"));
	lay->addWidget(preset);
	lay->addWidget(reset);
	lay->addSpacing(12);
	lay->addWidget(filter, 1);
	lay->addWidget(findKey);
	return wid;
}

void xHotkeyTable::load() {
	// the .ui sets the header up after the constructor, so it is put right here
	horizontalHeader()->setVisible(true);
	horizontalHeader()->setStretchLastSection(false);
	horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	// the names column fits every name, so it keeps its width while the search hides rows
	horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
	if (!filter->text().isEmpty()) filter->clear();
	resizeColumnToContents(0);
	work = hotkeys_get();
	updateControls();
	model->update();
}

void xHotkeyTable::commit() {
	hotkeys_set(work);
}

void xHotkeyTable::focusFilter() {
	filter->setFocus();
	filter->selectAll();
}

void xHotkeyTable::updateControls() {
	preset->setCurrentIndex(work.preset);
	reset->setEnabled((work.preset == HKP_CUSTOM) && !work.over.empty());
	preset->setItemText(HKP_CUSTOM, work.over.empty() ? QString(presetTitles[HKP_CUSTOM])
		: QString("%0 (from %1)").arg(presetTitles[HKP_CUSTOM]).arg(presetTitles[work.base]));
}

// Custom is kept while a preset is picked, so picking one back loses nothing
void xHotkeyTable::presetChanged(int idx) {
	work.preset = idx;
	updateControls();
	model->update();
}

void xHotkeyTable::resetCustom() {
	if (QMessageBox::question(this, "Hotkeys", QString("Drop your own keys and go back to %0?")
			.arg(presetTitles[work.base])) != QMessageBox::Yes) return;
	work.over.clear();
	updateControls();
	model->update();
}

// A row shows when its action, its section or one of its keys fits. A key is
// matched whole or as the last key of a combination, so F1 is not F10.
void xHotkeyTable::filterChanged(const QString& txt) {
	QString flt = txt.trimmed();
	QString kflt = latinKeys(flt);
	xShortcut* tab = shortcut_tab();
	int head = -1;
	bool headFits = false;
	bool any = false;
	for (int r = 0; r <= model->rowCount(); r++) {
		int t = model->cut(r);
		if ((t < 0) || (r == model->rowCount())) {		// a heading, or the end
			if (head >= 0) setRowHidden(head, !any);
			if (r == model->rowCount()) break;
			head = r;
			headFits = model->rowText(r).contains(flt, Qt::CaseInsensitive);
			any = false;
			continue;
		}
		bool fit = flt.isEmpty() || headFits || model->rowText(r).contains(flt, Qt::CaseInsensitive);
		QKeySequence keys[2];
		hotkeys_resolve(work, tab[t].id, keys);
		for (int s = 0; (s < 2) && !fit; s++) {
			QString key = shortcutText(keys[s]);
			if (key.isEmpty()) continue;
			fit = !key.compare(kflt, Qt::CaseInsensitive)
				|| key.endsWith("+" + kflt, Qt::CaseInsensitive)
				|| (kflt.contains('+') && key.contains(kflt, Qt::CaseInsensitive));
		}
		setRowHidden(r, !fit);
		any |= fit;
	}
}

// the key column edits the key, the alternate's the alternate
static int slotOf(const QModelIndex& idx) {
	return (idx.column() == 2) ? 1 : 0;
}

void xHotkeyTable::dbl_click(QModelIndex idx) {
	if (idx.isValid()) editSlot(idx.row(), slotOf(idx));
}

void xHotkeyTable::editSlot(int row, int slot) {
	int t = model->cut(row);
	if (t < 0) return;			// a heading
	xShortcut* tab = shortcut_tab();
	QKeySequence keys[2];
	hotkeys_resolve(work, tab[t].id, keys);
	edt->edit(tab[t].id, slot, model->rowText(row), keys[slot]);
}

void xHotkeyTable::keyPressEvent(QKeyEvent* ev) {
	QModelIndex idx = currentIndex();
	switch (ev->key()) {
		case Qt::Key_Return:
		case Qt::Key_Enter:
			if (idx.isValid()) editSlot(idx.row(), slotOf(idx));
			break;
		case Qt::Key_Delete:
		case Qt::Key_Backspace:
			if (idx.isValid() && (model->cut(idx.row()) >= 0))
				set_seq(shortcut_tab()[model->cut(idx.row())].id, slotOf(idx), QKeySequence());
			break;
		default:
			QTableView::keyPressEvent(ev);
			break;
	}
}

void xHotkeyTable::rowMenu(const QPoint& pos) {
	QModelIndex idx = indexAt(pos);
	int t = model->cut(idx.row());
	if (t < 0) return;
	int id = shortcut_tab()[t].id;
	int row = idx.row();
	QMenu menu(this);
	menu.addAction("Change key...", this, [this, row]() {editSlot(row, 0);});
	menu.addAction("Change alternate...", this, [this, row]() {editSlot(row, 1);});
	menu.addAction("No keys", this, [this, id]() {
		QKeySequence none[2];
		if (toCustom()) putKeys(id, none);
	});
	if ((work.preset == HKP_CUSTOM) && work.over.count(id)) {
		menu.addSeparator();
		menu.addAction(QString("As in %0").arg(presetTitles[work.base]), this, [this, id]() {
			work.over.erase(id);
			updateControls();
			model->update();
		});
	}
	menu.exec(viewport()->mapToGlobal(pos));
}

// A change is made in Custom. From a preset that Custom is not based on, with
// changes of its own, starting over loses them, so it asks.
bool xHotkeyTable::toCustom() {
	if (work.preset == HKP_CUSTOM) return true;
	if (!work.over.empty() && (work.base != work.preset)) {
		int ans = QMessageBox::question(this, "Hotkeys",
			QString("Your Custom keys are changed from %0.\nStart Custom over from %1? Your changes there are lost.")
			.arg(presetTitles[work.base]).arg(presetTitles[work.preset]));
		if (ans != QMessageBox::Yes) return false;
		work.over.clear();
	}
	work.base = work.preset;
	work.preset = HKP_CUSTOM;
	return true;
}

void xHotkeyTable::putKeys(int id, const QKeySequence* keys) {
	hotkeys_put(work, id, keys);
	updateControls();
	model->update();
}

void xHotkeyTable::set_seq(int id, int slot, QKeySequence seq) {
	if (id < 0) {			// Find key: the key goes into the search
		filter->setText(shortcutText(seq));
		return;
	}
	QStringList names;
	for (int other : hotkeys_holders(work, id, seq))
		names.append(QString(find_shortcut_id(other)->text));
	if (!names.isEmpty()) {
		int ans = QMessageBox::question(this, "Hotkeys",
			QString("%0 is already %1.\nMove it here?").arg(shortcutText(seq)).arg(names.join(", ")));
		if (ans != QMessageBox::Yes) return;
	}
	if (!toCustom()) return;
	hotkeys_assign(work, id, slot, seq);
	updateControls();
	model->update();
	filterChanged(filter->text());
}

// editor

xKeyEditor::xKeyEditor(QWidget* p):QDialog(p) {
	QVBoxLayout* lay = new QVBoxLayout;
	QHBoxLayout* hbx = new QHBoxLayout;
	lab.clear();
	but.setIcon(QIcon(":/images/ok-apply.png"));
	but.setText("Confirm");
	clr.setIcon(QIcon(":/images/cancel.png"));
	clr.setText("Clear");
	lab.setAlignment(Qt::AlignCenter);
	lab.setMinimumWidth(240);
	lay->addWidget(&lab);
	hbx->addWidget(&clr);
	hbx->addWidget(&but);
	lay->addLayout(hbx);
	setLayout(lay);
	setModal(true);
	connect(&clr, SIGNAL(released()), this, SLOT(clear()));
	connect(&but, SIGNAL(released()), this, SLOT(okay()));
}

void xKeyEditor::keyPressEvent(QKeyEvent* ev) {
	QString str;
	Qt::KeyboardModifiers mods = xNativeMods(ev->modifiers());
	if (mods & Qt::AltModifier) str += "Alt + ";
	if (mods & Qt::ControlModifier) str += "Ctrl + ";
	if (mods & Qt::ShiftModifier) str += "Shift + ";
#ifdef __APPLE__
	if (mods & Qt::MetaModifier) str += "Cmd + ";
#elif defined(__WIN32)
	if (mods & Qt::MetaModifier) str += "Win + ";	// unreachable: the shell grabs the Win key before Qt sees it
#else
	if (mods & Qt::MetaModifier) str += "Meta + ";
#endif
	int key = hotkey_key(ev);		// the key where it is, whatever the layout
	switch (key) {
		case Qt::Key_Alt:
		case Qt::Key_Multi_key:		// TODO: this is right-alt?
		case Qt::Key_Shift:
		case Qt::Key_Control:
		case Qt::Key_Meta:
			kseq = QKeySequence();
			break;
		default:
			str += QKeySequence(key).toString();
			kseq = QKeySequence(key | mods);
			break;
	}
	if (str.isEmpty()) str = QString("<press now>");
	lab.setText(str);
}

void xKeyEditor::keyReleaseEvent(QKeyEvent*) {
	if (kseq.isEmpty())
		lab.setText("<press now>");
}

void xKeyEditor::edit(int f, int s, const QString& name, const QKeySequence& seq) {
	foo = f;
	slot = s;
	kseq = seq;
	setWindowTitle(QString("%0: %1").arg(name).arg(s ? "alternate" : "key"));
	lab.setText(kseq.isEmpty() ? "<no key>" : shortcutText(kseq));
	show();
	grabKeyboard();
}

void xKeyEditor::clear() {
	kseq = QKeySequence();
	lab.setText("<no key>");
}

void xKeyEditor::okay() {
	reject();
	emit s_done(foo, slot, kseq);
}

void xKeyEditor::reject() {
	releaseKeyboard();
	hide();
}
