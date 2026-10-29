#include "emulwin.h"
#include "filer.h"
#include "libxpeccy/filetypes/filetypes.h"

#include <algorithm>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QTabBar>
#include <QMenu>
#include <QMenuBar>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QTimer>
#include <QWindow>
#include "xcore/vscalers.h"
#include <QToolButton>
#include <QPainter>

// socket

#ifdef USENETWORK

void MainWin::openServer() {
	int i = 0;
	while (!srv.listen(QHostAddress::LocalHost, (conf.port + i) & 0xffff) && (i < 0x10000)) {
		i++;
	}
	if (!srv.isListening()) {
		shitHappens("Listen server can't start");
	} else {
		xlog(XLG_NET, XLL_INFO, "listening on port %i", (conf.port + i) & 0xffff);
	}
}

void MainWin::closeServer() {
	if (srv.isListening()) {
		foreach(QTcpSocket* sock, clients) {
			sock->close();
			sock->deleteLater();
		}
		srv.close();
	}
}

void MainWin::connected() {
	QTcpSocket* sock = srv.nextPendingConnection();
	clients.append(sock);
	sock->write("Who's there?\n> ");
	connect(sock,SIGNAL(destroyed()),this,SLOT(disconnected()));
	connect(sock,SIGNAL(readyRead()),this,SLOT(socketRead()));
}

void MainWin::disconnected() {
	QTcpSocket* sock = (QTcpSocket*)sender();
	disconnect(sock);
	clients.removeAll(sock);
	sock->deleteLater();
}

static char dasmbuf[256];
extern int dasmrd(int adr, void* ptr);
extern int str_to_adr(Computer* comp, QString str);
extern QString asm_labels(const QString&);

void MainWin::socketRead() {
	QTcpSocket* sock = (QTcpSocket*)sender();
	QByteArray arr = sock->readAll();
	QString com(arr);
	com = com.remove("\n");
	com = com.remove("\r");
	QStringList prm = com.split(" ",X_SkipEmptyParts);
	if (prm.size() == 0) return;
	com = prm[0];
	xMnem mnm;
	bool f;
	int adr, cnt, val;
	Computer* comp = conf.zx;
	// and do something with this
	if ((com == "debug") || (com == "dbg")) {
		doDebug();
	} else if (com == "closedbg") {
		emit s_debug_off();
	} else if (com == "quit") {
		frame->close();
	} else if (com == "exit") {
		sock->close();
	} else if (com == "pause") {
		pause(true, PR_PAUSE);
	} else if ((com == "cont") || (com == "unpause")) {
		pause(false, PR_PAUSE);
	} else if (com == "step") {
		emit s_step();
	} else if (com == "reset") {
		resetMachine(RES_DEFAULT);
	} else if (com == "cpu") {
		//sock->write(getCoreName(comp->cpu->type));
		sock->write(comp->cpu->core->name);
		sock->write("\n");
	} else if (com == "getreg") {
		if (prm.size() > 1) {
			val = cpu_get_reg(comp->cpu, prm[1].toUpper().toLocal8Bit().data(), &f);
			if (!f) {
				sock->write(QString::number(val, 16).toUpper().toUtf8());
				sock->write("\r\n");
			} else {
				sock->write("Wrong register name\r\n");
			}
		}
	} else if (com == "setreg") {
		rzx_rec_touch();
		if (prm.size() > 2) {
			if (!cpu_set_reg(comp->cpu, prm[1].toUpper().toLocal8Bit().data(), str_to_adr(comp, prm[2])))
				sock->write("Wrong register name\r\n");
		}
	} else if (com == "cpuregs") {
		xRegBunch rb = cpuGetRegs(comp->cpu);
		xRegister reg;
		for (cnt = 0; cnt < 32; cnt++) {
			reg = rb.regs[cnt];
			if (reg.id == REG_EOT) break;		// past it the bunch is not filled
			if (reg.id != REG_EMPTY) {
				sock->write(reg.name);
				sock->write(" : ");
				switch(reg.size) {
					case REG_BIT: sock->write(reg.value ? "1" : "0"); break;
					case REG_2: sock->write(gethexbyte(reg.value & 3).toUtf8()); break;
					case REG_BYTE: sock->write(gethexbyte(reg.value).toUtf8()); break;
					case REG_WORD: sock->write(gethexword(reg.value).toUtf8()); break;
					case REG_24: sock->write(gethex6(reg.value).toUtf8()); break;
					case REG_32: sock->write(gethexint(reg.value).toUtf8()); break;
					default: sock->write("??"); break;
				}
				sock->write("\r\n");
			}
		}
	} else if (com == "eval") {
		if (prm.size() > 1) {
			xResult xr = xEval(prm[1].toLocal8Bit().data());
			if (xr.err) {
				sock->write("Syntax error");
			} else {
				sock->write(gethexint(xr.value).toUtf8());
			}
			sock->write("\r\n");
		}
	} else if (com == "load") {
		if (prm.size() > 1) {
			emu_lock();
			load_file(comp, prm[1].toLocal8Bit().data(), FG_ALL, 0);
			emu_unlock();
		}
	} else if (com == "asm") {
		rzx_rec_touch();
		// asm ADR instruction: what the listing does with one typed into it
		if (prm.size() > 2) {
			adr = str_to_adr(comp, prm[1]);
			QString src = QString(arr).trimmed().mid(com.size()).trimmed();
			src = src.mid(src.indexOf(' ') + 1).replace("#", "0x");
			char buf[16];
			cnt = cpuAsm(comp->cpu, asm_labels(src).toLocal8Bit().data(), buf, adr);
			for (int i = 0; i < cnt; i++)
				comp->hw->mwr(comp, adr + i, buf[i] & 0xff);
			sock->write(QString("%0 bytes\r\n").arg(qMax(cnt, 0)).toUtf8());
		}
	} else if ((com == "poke") || (com == "memwr")) {
		rzx_rec_touch();
		if (prm.size() > 2) {
			adr = str_to_adr(comp, prm[1]);
			val = str_to_adr(comp, prm[2]);
			comp->hw->mwr(comp, adr, val & 0xff);
		}
	} else if ((com == "pokew") || (com == "memwrw")) {
		rzx_rec_touch();
		if (prm.size() > 2) {
			adr = str_to_adr(comp, prm[1]);
			val = str_to_adr(comp, prm[2]);
			comp->hw->mwr(comp, adr, val & 0xff);
			comp->hw->mwr(comp, adr+1, (val >> 8) & 0xff);
		}
	} else if (com == "memfill") {
		rzx_rec_touch();
		if (prm.size() > 3) {
			adr = str_to_adr(comp, prm[1]);
			cnt = str_to_adr(comp, prm[2]);
			val = str_to_adr(comp, prm[3]);
			while (cnt > 0) {
				comp->hw->mwr(comp, adr, val);
				adr++;
				cnt--;
			}
		}
	} else if (com == "memcopy") {
		rzx_rec_touch();
		if (prm.size() > 3) {
			adr = str_to_adr(comp, prm[1]);
			cnt = str_to_adr(comp, prm[2]);
			val = str_to_adr(comp, prm[3]);
			qDebug() << adr << cnt << val;
			while (cnt > 0) {
				if (adr > val) {
					comp->hw->mwr(comp, val, comp->hw->mrd(comp, adr, 0));		// dst < src
					adr++;
					val++;
				} else {								// dst >= src
					comp->hw->mwr(comp, val+cnt-1, comp->hw->mrd(comp, adr+cnt-1, 0));
				}
				cnt--;
			}
		}
	} else if (com == "disasm") {
		if (prm.size() > 1) {
			adr = str_to_adr(comp, prm[1]);
			cnt = (prm.size() > 2) ? prm[2].toInt() : 1;
			// qDebug() << cnt;
			while (cnt > 0) {
				snprintf(dasmbuf, sizeof(dasmbuf), "%.6X : ", adr);
				sock->write(dasmbuf);
				mnm = cpuDisasm(comp->cpu, adr, dasmbuf, dasmrd, comp);
				sock->write(dasmbuf);
				sock->write("\r\n");
				adr += mnm.len;
				cnt--;
			}
		}
	} else if (com == "dump") {
		if (prm.size() > 1) {
			adr = str_to_adr(comp, prm[1]);
			if (prm.size() > 2) {
				cnt = prm[2].toInt();
				if (cnt < 1) {
					cnt = 16;
				} else {
					cnt <<= 4;
				}
			} else {
				cnt = 16;
			}
			while (cnt > 0) {
				snprintf(dasmbuf, sizeof(dasmbuf), "%.6X : ", adr);
				sock->write(dasmbuf);
				dasmbuf[0] = 0x00;
				do {
					snprintf(dasmbuf, sizeof(dasmbuf), "%.2X ", dasmrd(adr, comp));
					sock->write(dasmbuf);
					adr++;
					cnt--;
				} while (cnt & 15);
				sock->write("\r\n");
			}
		}
	} else if ((com == "winshot") || (com == "winshot-menus")) {
		// winshot FILE: the window as painted, or the menu or dialog open over it.
		// winshot-menus FILE: the window with every menu open over it, where it
		// stands; the answer adds a "menu x y w h" line for each. A path may have spaces
		QWidget* w = NULL;
		if (com == "winshot") {
			w = QApplication::activePopupWidget();
			if (!w) w = QApplication::activeModalWidget();
			if (!w && QApplication::activeWindow() && (QApplication::activeWindow() != frame))
				w = QApplication::activeWindow();		// a tool window
		}
		QPixmap pic = w ? w->grab() : QPixmap();
#if defined(USEOPENGL) && !BLOCKGL && !ISLEGACYGL
		// The picture is read from the framebuffer, where the last frame left
		// it, and the window rendered around it: rendering the picture widget
		// itself would clear its framebuffer.
		if (!w && glw) {
			const qreal r = widgetDpr(this);
			QImage img(int(width() * r + 0.5), int(height() * r + 0.5), QImage::Format_RGBA8888);
			glw->makeCurrent();
			glBindFramebuffer(GL_FRAMEBUFFER, glw->defaultFramebufferObject());
			glPixelStorei(GL_PACK_ALIGNMENT, 4);
			glReadPixels(0, 0, img.width(), img.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
			glw->doneCurrent();
			img.setDevicePixelRatio(r);
			QRect scr(mapTo(frame, QPoint(0, 0)), size());
			QRegion around = QRegion(frame->rect()).subtracted(scr);
			pic = QPixmap(frame->size() * widgetDpr(frame));
			pic.setDevicePixelRatio(widgetDpr(frame));
			pic.fill(Qt::black);
			if (!around.isEmpty())		// an empty region would render the lot
				frame->render(&pic, QPoint(), around);
			QPainter pnt(&pic);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
			pnt.drawImage(scr.topLeft(), img.flipped(Qt::Vertical));
#else
			pnt.drawImage(scr.topLeft(), img.mirrored());
#endif
			if (fsBar && fsBar->isVisible())		// it is over the picture
				fsBar->render(&pnt, fsBar->pos());
			if (mbHint && mbHint->isVisible())
				mbHint->render(&pnt, mbHint->pos(), QRegion(), QWidget::DrawChildren);	// see-through: no background
			if (fsTool && fsTool->isVisible())
				fsTool->render(&pnt, fsTool->pos());
			// the window's own, laid out over the picture when its arrow is open
			if (toolBar->isVisible() && toolBar->geometry().intersects(scr))
				toolBar->render(&pnt, toolBar->pos());
		}
#endif
		if (pic.isNull()) pic = frame->grab();
		QString menus;
		if (com == "winshot-menus") {
			// the menus are windows of their own: each drawn where it stands, a
			// submenu after the menu it came out of, over the window they belong
			// to - a tool window such as the Debugger, or the main window
			QWidget* base = frame;
			foreach(QWidget* t, QApplication::topLevelWidgets()) {
				if ((t != frame) && t->isVisible() && !qobject_cast<QMenu*>(t) && t->isActiveWindow())
					base = t;
			}
			if (base != frame) pic = base->grab();
			QList<QMenu*> pops;
			foreach(QWidget* t, QApplication::topLevelWidgets()) {
				QMenu* m = qobject_cast<QMenu*>(t);
				if (m && m->isVisible()) pops.append(m);
			}
			std::sort(pops.begin(), pops.end(), [](QMenu* a, QMenu* b) {return a->x() < b->x();});
			QRect all(QPoint(0, 0), base->size());
			foreach(QMenu* m, pops)
				all |= QRect(base->mapFromGlobal(m->mapToGlobal(QPoint(0, 0))), m->size());
			const qreal r = widgetDpr(base);
			QPixmap out(all.size() * r);
			out.setDevicePixelRatio(r);
			out.fill(Qt::transparent);
			QPainter pnt(&out);
			pnt.drawPixmap(-all.topLeft(), pic);
			foreach(QMenu* m, pops) {
				QRect rc(base->mapFromGlobal(m->mapToGlobal(QPoint(0, 0))), m->size());
				pnt.drawPixmap(rc.topLeft() - all.topLeft(), m->grab());
				menus += QString("menu %0 %1 %2 %3\r\n").arg(rc.x() - all.x()).arg(rc.y() - all.y()).arg(rc.width()).arg(rc.height());
			}
			pnt.end();
			pic = out;
		}
		QString path = QString(arr).trimmed().mid(com.size()).trimmed();
		if (!path.isEmpty() && pic.save(path)) {
			// where it is, in the coordinates click takes
			QRect rc((w ? w : frame)->geometry());
			rc.moveTopLeft(frame->mapFromGlobal(w ? w->mapToGlobal(QPoint(0, 0)) : frame->mapToGlobal(QPoint(0, 0))));
			sock->write(QString("ok %0 %1 %2 %3\r\n").arg(rc.x()).arg(rc.y()).arg(rc.width()).arg(rc.height()).toUtf8());
			sock->write(menus.toUtf8());
		} else {
			sock->write("failed\r\n");
		}
	} else if ((com == "open-menu") || (com == "menu-action")) {
		// open-menu Machine/Reset: a menu bar menu and its submenus, opened as a
		// click opens them but without the click's wait for the menu to close, so
		// winshot-menus can be asked next. @picture/Machine starts from the menu
		// over the picture, @tbTool from the menu of the tool button of that name.
		// menu-action Machine/Pause: what picking that item does, without opening
		// anything - the answer comes first, a dialog it opens after it
		QStringList names = QString(arr).trimmed().mid(com.size()).trimmed().split('/', X_SkipEmptyParts);
		const bool pick = (com == "menu-action");
		auto plain = [](QString t) {return t.remove('&').section('\t', 0, 0).trimmed();};
		QMenu* menu = NULL;
		QPoint at;
		if (names.isEmpty()) {
		} else if (names[0] == "@picture") {
			menu = userMenu;
			at = mapToGlobal(QPoint(width() / 4, height() / 5));
		} else if (names[0].startsWith("@")) {
			foreach(QWidget* w, QApplication::allWidgets()) {
				if (w->objectName() != names[0].mid(1)) continue;
				QToolButton* tb = qobject_cast<QToolButton*>(w);
				if (tb && tb->menu()) {
					menu = tb->menu();
					at = tb->mapToGlobal(QPoint(0, tb->height()));
					break;
				}
				// a button that lists its own actions rather than a menu: those
				// can be picked, not opened
				QAction* own = NULL;
				foreach(QAction* a, w->actions())
					if ((names.size() == 2) && (plain(a->text()) == names[1])) own = a;
				if (own && pick) {
					sock->write("ok\r\n");
					QTimer::singleShot(0, own, &QAction::trigger);
					if (sock->isOpen()) sock->write("> ");
					return;
				}
			}
		} else {
			QMenuBar* bar = frame->menuBar();
			foreach(QAction* a, bar->actions()) {
				if (a->menu() && (plain(a->text()) == names[0])) {
					menu = a->menu();
					at = bar->mapToGlobal(bar->actionGeometry(a).bottomLeft());
					break;
				}
			}
		}
		if (menu) {
			if (pick) emit menu->aboutToShow();		// some are filled only then
			else menu->popup(at);
		}
		QAction* hit = NULL;
		for (int i = 1; menu && (i < names.size()); i++) {
			hit = NULL;
			foreach(QAction* a, menu->actions())
				if (plain(a->text()) == names[i]) hit = a;
			if (!pick) menu->setActiveAction(hit);
			if (!hit) {
				menu = NULL;
			} else if (hit->menu()) {
				if (pick) {
					emit hit->menu()->aboutToShow();
				} else {
					hit->menu()->popup(menu->mapToGlobal(menu->actionGeometry(hit).topRight()));
				}
				menu = hit->menu();
			}
		}
		if (pick && menu && hit && !hit->menu()) {
			sock->write("ok\r\n");
			QTimer::singleShot(0, hit, &QAction::trigger);
		} else if (pick) {
			sock->write("no such item\r\n");
		} else {
			sock->write(menu ? "ok\r\n" : "no such menu\r\n");
		}
	} else if (com == "show-panel") {
		// show-panel NAME: a panel of a window - a dock, by object name or title -
		// brought to the front of the tabs it shares its place with
		QString name = QString(arr).trimmed().mid(com.size()).trimmed();
		QDockWidget* hit = NULL;
		foreach(QWidget* w, QApplication::allWidgets()) {
			QDockWidget* d = qobject_cast<QDockWidget*>(w);
			if (d && !name.isEmpty() && ((d->objectName() == name) || (d->windowTitle() == name))) {
				hit = d;
				break;
			}
		}
		if (hit) {
			hit->show();
			hit->raise();
		}
		sock->write(hit ? "ok\r\n" : "nothing\r\n");
	} else if (com == "move") {
		// move X Y: the pointer seen at a point of the window, buttons up
		if (prm.size() > 2) {
			QPoint gp = frame->mapToGlobal(QPoint(prm[1].toInt(), prm[2].toInt()));
			QWidget* w = QApplication::widgetAt(gp);
			if (w) {
				QMouseEvent mev(QEvent::MouseMove, w->mapFromGlobal(gp), gp, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
				QApplication::sendEvent(w, &mev);
			}
			sock->write(w ? w->metaObject()->className() : "nothing");
			sock->write("\r\n");
		}
	} else if (com == "click") {
		// click X Y [right]: at a point of the window, whatever is there
		if (prm.size() > 2) {
			QPoint gp = frame->mapToGlobal(QPoint(prm[1].toInt(), prm[2].toInt()));
			Qt::MouseButton btn = ((prm.size() > 3) && (prm[3] == "right")) ? Qt::RightButton : Qt::LeftButton;
			QWidget* w = QApplication::widgetAt(gp);
			if (w) {
				QMouseEvent prs(QEvent::MouseButtonPress, w->mapFromGlobal(gp), gp, btn, btn, Qt::NoModifier);
				QApplication::sendEvent(w, &prs);
				w = QApplication::widgetAt(gp);		// the press may have put a menu there
				if (w) {
					QMouseEvent rls(QEvent::MouseButtonRelease, w->mapFromGlobal(gp), gp, btn, Qt::NoButton, Qt::NoModifier);
					QApplication::sendEvent(w, &rls);
					if (btn == Qt::RightButton) {	// the window system makes this one of a real click
						QContextMenuEvent cme(QContextMenuEvent::Mouse, w->mapFromGlobal(gp), gp);
						QApplication::sendEvent(w, &cme);
					}
				}
			}
			sock->write(w ? w->metaObject()->className() : "nothing");
			sock->write("\r\n");
		}
	} else if (com == "click-widget") {
		// click-widget NAME DX DY [right]: at that point of a widget, by object name or
		// else class name - the one in the window in front first - wherever the window is
		QWidget* hit = NULL;
		if (prm.size() > 3) {
			foreach(QWidget* w, QApplication::allWidgets()) {
				if (!w->isVisible() || ((w->objectName() != prm[1]) && (prm[1] != w->metaObject()->className()))) continue;
				if (!hit || w->window()->isActiveWindow()) hit = w;
			}
		}
		if (hit) {
			QPoint pos(prm[2].toInt(), prm[3].toInt());
			QPoint gp = hit->mapToGlobal(pos);
			// what is under that point takes it, as with a real mouse
			QWidget* in = hit->childAt(pos);
			if (in) {
				pos = in->mapFrom(hit, pos);
				hit = in;
			}
			Qt::MouseButton btn = ((prm.size() > 4) && (prm[4] == "right")) ? Qt::RightButton : Qt::LeftButton;
			sock->write(QString("%0\r\n").arg(hit->metaObject()->className()).toUtf8());
			sock->flush();		// a menu or a dialog the click opens holds the rest
			QMouseEvent prs(QEvent::MouseButtonPress, pos, gp, btn, btn, Qt::NoModifier);
			QApplication::sendEvent(hit, &prs);
			QMouseEvent rls(QEvent::MouseButtonRelease, pos, gp, btn, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(hit, &rls);
			if (btn == Qt::RightButton) {
				QContextMenuEvent cme(QContextMenuEvent::Mouse, pos, gp);
				QApplication::sendEvent(hit, &cme);
			}
		} else {
			sock->write("nothing\r\n");
		}
	} else if (com == "set-value") {
		// set-value NAME VALUE: a field filled in, by object name - the one in the window
		// in front first. A hex field takes hex, a spin box a number, a list the text of
		// an item (an editable one any text), a tab bar the text of a tab, a box to tick 1 or 0,
		// a line the rest of the command, spaces and all
		QWidget* hit = NULL;
		foreach(QWidget* w, QApplication::allWidgets()) {
			if (!w->isVisible() || (w->objectName() != prm.value(1))) continue;
			if (!hit || w->window()->isActiveWindow()) hit = w;
		}
		QString val = QString(arr).trimmed().mid(com.size()).trimmed().mid(prm.value(1).size()).trimmed();
		bool ok = (hit != NULL);
		if (xHexSpin* hs = qobject_cast<xHexSpin*>(hit)) {
			hs->setValue(val.toInt(&ok, 16));
		} else if (QSpinBox* sb = qobject_cast<QSpinBox*>(hit)) {
			sb->setValue(val.toInt(&ok));
		} else if (QComboBox* cb = qobject_cast<QComboBox*>(hit)) {
			int idx = cb->findText(val);
			ok = (idx >= 0) || cb->isEditable();
			if (idx >= 0) cb->setCurrentIndex(idx);
			else if (ok) cb->setEditText(val);		// an editable list takes any text
		} else if (QTabBar* tb = qobject_cast<QTabBar*>(hit)) {
			int idx = -1;
			for (int i = 0; i < tb->count(); i++)
				if (tb->tabText(i) == val) idx = i;
			ok = (idx >= 0);
			if (ok) tb->setCurrentIndex(idx);
		} else if (QAbstractButton* ab = qobject_cast<QAbstractButton*>(hit)) {
			ab->setChecked(val == "1");
		} else if (QLineEdit* le = qobject_cast<QLineEdit*>(hit)) {
			le->setText(val);
		} else {
			ok = false;
		}
		sock->write(ok ? "ok\r\n" : "nothing\r\n");
	} else if (com == "type") {
		// type NAME TEXT: keys typed into a widget, by object name or else class name - the
		// one in the window in front first - as its own key events, so it reacts as to
		// typing; \n is Enter
		QWidget* hit = NULL;
		foreach(QWidget* w, QApplication::allWidgets()) {
			if (!w->isVisible() || ((w->objectName() != prm.value(1)) && (prm.value(1) != w->metaObject()->className()))) continue;
			if (!hit || w->window()->isActiveWindow() || (w->window() == QApplication::activeModalWidget())) hit = w;
		}
		QString txt = QString(arr).trimmed().mid(com.size()).trimmed().mid(prm.value(1).size()).trimmed();
		txt.replace("\\n", "\n");
		if (hit) {
			hit->setFocus();
			foreach(QChar ch, txt) {
				// to whatever has the focus in that window now: Enter may have moved it
				QWidget* to = hit->window()->focusWidget();
				if (!to) to = hit;
				bool enter = (ch == '\n');
				int key = enter ? Qt::Key_Return : ch.toUpper().unicode();
				QString text = enter ? QString("\r") : QString(ch);
				QKeyEvent prs(QEvent::KeyPress, key, Qt::NoModifier, text);
				QApplication::sendEvent(to, &prs);
				QKeyEvent rls(QEvent::KeyRelease, key, Qt::NoModifier, text);
				QApplication::sendEvent(to, &rls);
			}
		}
		sock->write(hit ? "ok\r\n" : "nothing\r\n");
	} else if (com == "run-action") {
		// run-action key.options: what a main window hotkey does, by its config name
		xShortcut* cut = (prm.size() > 1) ? find_shortcut_name(prm[1].toUtf8().constData()) : NULL;
		if (cut && (cut->grp & SCG_MAIN)) {
			// the action may open a modal window and not come back until it is closed
			sock->write("ok\r\n");
			sock->flush();
			xkey_press(cut->id, true);
			xcut_release(cut->id);
		} else {
			sock->write("unknown action\r\n");
		}
	} else if (com == "activate-window") {
		// activate-window TITLE: a window of the program brought to the front, by the
		// start of its title, object name or class - shown is not always in front
		QString name = QString(arr).trimmed().mid(com.size()).trimmed();
		QWidget* hit = NULL;
		foreach(QWidget* w, QApplication::topLevelWidgets()) {
			if (w->isVisible() && !name.isEmpty() && (w->windowTitle().startsWith(name)
					|| (w->objectName() == name) || (name == w->metaObject()->className()))) {
				hit = w;
				break;
			}
		}
		if (hit) {
			hit->raise();
			hit->activateWindow();
		}
		sock->write(hit ? "ok\r\n" : "nothing\r\n");
	} else if ((com == "widget-rect") || (com == "widget-list")) {
		// widget-rect NAME: a visible widget, by object name or else class name, as
		// "x y w h" in the coordinates of its window - the ones winshot pictures have;
		// one in the window winshot takes comes first.
		// widget-list [WINDOW]: every visible named widget of that window (or of the
		// window with this object name), one "name class x y w h" a line
		QString name = prm.value(1);
		QWidget* top = QApplication::activePopupWidget();
		if (!top) top = QApplication::activeModalWidget();
		if (!top) top = QApplication::activeWindow();
		if (!top) top = frame;
		auto line = [](QWidget* w) {
			QRect rc(w->mapTo(w->window(), QPoint(0, 0)), w->size());
			return QString("%0 %1 %2 %3").arg(rc.x()).arg(rc.y()).arg(rc.width()).arg(rc.height());
		};
		QWidget* hit = NULL;
		QString list;
		foreach(QWidget* w, QApplication::allWidgets()) {
			if (!w->isVisible()) continue;
			if (com == "widget-list") {
				if (w->objectName().isEmpty() || w->objectName().startsWith("qt_")) continue;
				if (name.isEmpty() ? (w->window() != top) : (w->window()->objectName() != name)) continue;
				list += QString("%0 %1 %2\r\n").arg(w->objectName(), w->metaObject()->className(), line(w));
			} else if ((w->objectName() == name) || (name == w->metaObject()->className())) {
				if (!hit || (w->window() == top)) hit = w;
				if (w->window() == top) break;
			}
		}
		if (hit) list = line(hit) + "\r\n";
		sock->write(list.isEmpty() ? "nothing\r\n" : list.toUtf8());
	} else if (com == "uistate") {
		// uistate: what holds the input and where the picture goes - the modal window,
		// the popup, the grabs, the window's state, the crop and the draw rectangle
		auto wname = [](QWidget* w) {
			if (!w) return QString("none");
			QRect g = w->geometry();
			return QString("%0 \"%1\" %2,%3 %4x%5%6%7").arg(w->metaObject()->className(), w->windowTitle())
				.arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
				.arg(w->isVisible() ? "" : " hidden").arg(w->isModal() ? " modal" : "");
		};
		QString out;
		out += QString("modal: %0\r\n").arg(wname(QApplication::activeModalWidget()));
		out += QString("popup: %0\r\n").arg(wname(QApplication::activePopupWidget()));
		out += QString("active: %0\r\n").arg(wname(QApplication::activeWindow()));
		out += QString("focus: %0\r\n").arg(wname(QApplication::focusWidget()));
		out += QString("kbgrab: %0\r\n").arg(wname(QWidget::keyboardGrabber()));
		out += QString("mgrab: %0\r\n").arg(wname(QWidget::mouseGrabber()));
		QWindow* win = frame->windowHandle();
		out += QString("frame: state %0 qwindow %1 fullscreen %2 geo %3,%4 %5x%6 picture %7x%8\r\n")
			.arg(int(frame->windowState()), 0, 16).arg(win ? int(win->windowStates()) : -1, 0, 16)
			.arg(frame->isFullScreen() ? 1 : 0).arg(frame->x()).arg(frame->y())
			.arg(frame->width()).arg(frame->height()).arg(width()).arg(height());
		out += QString("conf: fullScreen %0 keepRatio %1 scale %2 border %3 lowLatency %4 fsTall %5 pause %6\r\n")
			.arg(conf.vid.fullScreen).arg(conf.vid.keepRatio).arg(conf.vid.scale).arg(conf.vid.border)
			.arg(conf.vid.lowLatency).arg(fsTall).arg(conf.emu.pause, 0, 16);
		Video* vid = comp->vid;
		out += QString("crop: vsze %0x%1 lcut %2,%3 rcut %4,%5 full %6x%7 brdmode %8\r\n")
			.arg(vid->vsze.x).arg(vid->vsze.y).arg(vid->lcut.x).arg(vid->lcut.y)
			.arg(vid->rcut.x).arg(vid->rcut.y).arg(vid->full.x).arg(vid->full.y).arg(vid->brdmode);
		out += QString("draw: %0,%1 %2x%3 zoom %4\r\n").arg(drawX).arg(drawY).arg(drawW).arg(drawH).arg(drawZoom);
		out += QString("fsbar: %0 fstool: %1\r\n").arg(fsBar && fsBar->isVisible() ? 1 : 0).arg(fsTool && fsTool->isVisible() ? 1 : 0);
		// the size the icons are drawn at, and the one set (0 = Auto)
		out += QString("toolbar: icons %0 set %1 fstool %2\r\n").arg(toolBar->iconSize().width()).arg(conf.win.tbIcons)
			.arg(fsTool ? fsTool->iconSize().width() : 0);
		foreach(QWidget* w, QApplication::topLevelWidgets()) {
			if (!w->isVisible()) continue;
			out += QString("top: %0 type %1\r\n").arg(wname(w)).arg(int(w->windowType()), 0, 16);
		}
		sock->write(out.toUtf8());
	} else {
		sock->write("Unrecognized command\r\n");
	}
	if (sock->isOpen())
		sock->write("> ");
}

#else

void MainWin::connected() {}
void MainWin::disconnected() {}
void MainWin::socketRead() {}

#endif
