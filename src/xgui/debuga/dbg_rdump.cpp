#include "dbg_rdump.h"
#include "dbg_dump.h"		// dump_cell_width: all three dumps share it

// widget

xRDumpWidget::xRDumpWidget(QString i, QString t, QWidget* p):xDockWidget(i,t,p) {
//	setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
	QWidget* wid = new QWidget;
	setWidget(wid);
	ui.setupUi(wid);
	setObjectName("REG DUMP");
	connect(this, &QDockWidget::visibilityChanged, this, &xRDumpWidget::draw);
	hwList << HWG_ZX;
}

void xRDumpWidget::draw() {
	ui.tabRegDump->update();
}

// table

xRDumpTable::xRDumpTable(QWidget* p):QTableView(p) {
	model = new xRDumpModel;
	setModel(model);
}

void xRDumpTable::update() {
	model->refill();
}

void xRDumpTable::resizeEvent(QResizeEvent* e) {
	// one byte takes as much room as it does in the other dumps. The form asks
	// for a wider minimum, which would clamp the default back up, and setupUi
	// applies it after the constructor - so it has to be lowered here
	QFontMetrics fm(conf.dbg.font);
	int wd = dump_cell_width(fm);
	horizontalHeader()->setMinimumSectionSize(4);
	horizontalHeader()->setDefaultSectionSize(wd);
	// the form stretches the last section, which would leave one byte adrift
	// at the far edge instead of next to its group
	horizontalHeader()->setStretchLastSection(false);
	// the name column carries "BC' (0000):", and the default section size above
	// is far too narrow for that
#if (QT_VERSION >= QT_VERSION_CHECK(5,11,0))
	int w0 = fm.horizontalAdvance("BC' (0000):") + 12;
#else
	int w0 = fm.width("BC' (0000):") + 12;
#endif
	setColumnWidth(0, w0);
	int w = width();
	if (w >= w0) {
		// a group of DUMP_GROUP costs one gap on top of its cells
		int cnt = ((w - w0) * DUMP_GROUP) / (wd * DUMP_GROUP + DUMP_GAP);
		cnt -= cnt % DUMP_GROUP;		// whole groups only
		if (cnt < DUMP_GROUP) cnt = DUMP_GROUP;
		model->setCols(cnt + 1);
		for (int c = 1; c <= cnt; c++) {
			int extra = ((c > 1) && (((c - 1) % DUMP_GROUP) == 0)) ? DUMP_GAP : 0;
			setColumnWidth(c, wd + extra);
		}
	}
	QTableView::resizeEvent(e);
}

// model

xRDumpModel::xRDumpModel(QObject* p):xTableModel(p) {
	row_count = 1;
	col_count = 9;
	nameWidth = 0;		// refill() measures it, but data() may run first
}

void xRDumpModel::refill() {
	xRegBunch rz = cpuGetRegs(conf.zx->cpu);
	regs.clear();
	int i = 0;
	nameWidth = 0;
	while (rz.regs[i].id != REG_EOT) {
		if (rz.regs[i].flag & REG_RDMP) {
			regs.append(rz.regs[i]);
			// the primed names are a character longer: pad them all to the
			// same width, or the brackets do not line up in a column
			int len = QString(rz.regs[i].name).length();
			if (len > nameWidth) nameWidth = len;
		}
		i++;
	}
	setRows(regs.size());
}

QVariant xRDumpModel::data(const QModelIndex& idx, int role) const {
	QVariant res;
	if (!idx.isValid()) return res;
	int row = idx.row();
	int col = idx.column();
	if (row >= row_count) return res;
	if (col >= col_count) return res;
	Memory* mem = conf.zx->mem;
	MemPage* pg;
	int adr;
	int fadr;
	switch(role) {
		case Qt::TextAlignmentRole:
			// right, like the other dumps: the extra width of a group's first
			// cell then shows up as a gap in front of it
			if (col > 0) res = (int)(Qt::AlignRight | Qt::AlignVCenter);
			break;
		case Qt::DisplayRole:
			if (col == 0) {
				// "HL (1234):" - the row dumps memory at that address, and
				// the colon keeps the name from running into the bytes
				res = QString("%0 (%1):")
					.arg(QString(regs.at(row).name).leftJustified(nameWidth))
					.arg(gethexword(regs.at(row).value & 0xffff));
			} else {
				adr = regs.at(row).value + col - 1;
				pg = mem_get_page(mem, adr);
				fadr = mem_get_phys_adr(mem, adr);	// = pg->num << 8) | (adr & 0xff);
				switch (pg->type) {
					case MEM_ROM: res = gethexbyte(mem->romData[fadr & mem->romMask]); break;
					case MEM_RAM: res = gethexbyte(mem->ramData[fadr & mem->ramMask]); break;
					case MEM_SLOT: res = gethexbyte(memRd(mem, adr)); break;
					default: res = "--";
						break;
				}
			}
			break;
	}
	return res;
}
