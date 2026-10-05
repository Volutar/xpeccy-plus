#include "opt_hotkeytab.h"
#include "../../xcore/xcore.h"

#include <QVBoxLayout>
#include <QHeaderView>
#include <QFont>
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

xHotkeyModel::xHotkeyModel(QObject* p):xTableModel(p) {
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

int xHotkeyModel::rowOf(int id) const {
	xShortcut* tab = shortcut_tab();
	for (int r = 0; r < list.size(); r++)
		if ((list[r].tab >= 0) && (tab[list[r].tab].id == id)) return r;
	return -1;
}

int xHotkeyModel::columnCount(const QModelIndex&) const {
	return 2;
}

int xHotkeyModel::rowCount(const QModelIndex&) const {
	return list.size();
}

QVariant xHotkeyModel::data(const QModelIndex& idx, int role) const {
	QVariant var;
	if (!idx.isValid()) return var;
	int row = idx.row();
	int col = idx.column();
	if ((row < 0) || (row >= list.size())) return var;
	xShortcut* tab = shortcut_tab();
	int t = list[row].tab;
	switch (role) {
		case Qt::DisplayRole:
			if (col == 0) {
				var = list[row].text;
			} else if (t >= 0) {
				var = shortcutText(tab[t].seq);
			}
			break;
		case Qt::FontRole:
			if (t < 0) {
				QFont fnt;
				fnt.setBold(true);
				var = fnt;
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

/*
void xHotkeyModel::updateCell(int row, int col) {
	emit dataChanged(index(row, col), index(row, col));
}
*/

// table

xHotkeyTable::xHotkeyTable(QWidget* p):QTableView(p) {
	model = new xHotkeyModel();
	edt = new xKeyEditor();
	setModel(model);
	setItemDelegate(new xHotkeyItem(this));
	for (int r = 0; r < model->rowCount(); r++)
		if (model->cut(r) < 0) setSpan(r, 0, 1, 2);
	// name column fits the longest name, keys take the rest
	horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

	connect(this, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(dbl_click(QModelIndex)));
	connect(edt, SIGNAL(s_done(int, QKeySequence)), this, SLOT(set_seq(int, QKeySequence)));
}

void xHotkeyTable::dbl_click(QModelIndex idx) {
	if (!idx.isValid()) return;
	int t = model->cut(idx.row());
	if (t < 0) return;			// a heading
	xShortcut* tab = shortcut_tab();
	edt->edit(tab[t].id);
}

void xHotkeyTable::set_seq(int id, QKeySequence seq) {
	set_shortcut_id(id, seq);
	int row = model->rowOf(id);
	if (row >= 0) model->updateCell(row, 1);
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
	switch (ev->key()) {
		case Qt::Key_Alt:
		case Qt::Key_Multi_key:		// TODO: this is right-alt?
		case Qt::Key_Shift:
		case Qt::Key_Control:
		case Qt::Key_Meta:
			kseq = QKeySequence();
			break;
		default:
			str += QKeySequence(ev->key()).toString();
			kseq = QKeySequence(ev->key() | mods);
			break;
	}
	if (str.isEmpty()) str = QString("<press now>");
	lab.setText(str);
}

void xKeyEditor::keyReleaseEvent(QKeyEvent* ev) {
	if (kseq.isEmpty())
		lab.setText("<press now>");
}

void xKeyEditor::edit(int f) {
	foo = f;
	xShortcut* cut = find_shortcut_id(f);
	kseq = cut->seq;
	lab.setText(kseq.isEmpty() ? "<no key>" : shortcutText(kseq));
	grabKeyboard();
	show();
}

void xKeyEditor::clear() {
	kseq = QKeySequence();
	lab.setText("<no key>");
}

void xKeyEditor::okay() {
	emit s_done(foo, kseq);
	reject();
}

void xKeyEditor::reject() {
	releaseKeyboard();
	hide();
}
