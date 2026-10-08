#include "emulwin.h"
#include "filer.h"

#include <QApplication>
#include <QMouseEvent>
#include <QContextMenuEvent>
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
		if (prm.size() > 2) {
			if (!cpu_set_reg(comp->cpu, prm[1].toUpper().toLocal8Bit().data(), str_to_adr(comp, prm[2])))
				sock->write("Wrong register name\r\n");
		}
	} else if (com == "cpuregs") {
		xRegBunch rb = cpuGetRegs(comp->cpu);
		xRegister reg;
		for (cnt = 0; cnt < 32; cnt++) {
			reg = rb.regs[cnt];
			if ((reg.id != REG_EOT) && (reg.id != REG_EMPTY)) {
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
		if (prm.size() > 2) {
			adr = str_to_adr(comp, prm[1]);
			val = str_to_adr(comp, prm[2]);
			comp->hw->mwr(comp, adr, val & 0xff);
		}
	} else if ((com == "pokew") || (com == "memwrw")) {
		if (prm.size() > 2) {
			adr = str_to_adr(comp, prm[1]);
			val = str_to_adr(comp, prm[2]);
			comp->hw->mwr(comp, adr, val & 0xff);
			comp->hw->mwr(comp, adr+1, (val >> 8) & 0xff);
		}
	} else if (com == "memfill") {
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
				sprintf(dasmbuf, "%.6X : ", adr);
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
				sprintf(dasmbuf, "%.6X : ", adr);
				sock->write(dasmbuf);
				dasmbuf[0] = 0x00;
				do {
					sprintf(dasmbuf, "%.2X ", dasmrd(adr, comp));
					sock->write(dasmbuf);
					adr++;
					cnt--;
				} while (cnt & 15);
				sock->write("\r\n");
			}
		}
	} else if (com == "winshot") {
		// the window as painted, or the menu or dialog open over it: a path may have spaces
		QWidget* w = QApplication::activePopupWidget();
		if (!w) w = QApplication::activeModalWidget();
		if (!w && QApplication::activeWindow() && (QApplication::activeWindow() != frame))
			w = QApplication::activeWindow();		// a tool window
		QPixmap pic = w ? w->grab() : QPixmap();
#if defined(USEOPENGL) && !BLOCKGL && !ISLEGACYGL
		// The picture is read from the framebuffer, where the last frame left
		// it, and the window rendered around it: rendering the picture widget
		// itself would clear its framebuffer.
		if (!w) {
			const qreal r = widgetDpr(this);
			QImage img(int(width() * r + 0.5), int(height() * r + 0.5), QImage::Format_RGBA8888);
			makeCurrent();
			glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
			glPixelStorei(GL_PACK_ALIGNMENT, 4);
			glReadPixels(0, 0, img.width(), img.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
			doneCurrent();
			img.setDevicePixelRatio(r);
			QRect scr(mapTo(frame, QPoint(0, 0)), size());
			QRegion around = QRegion(frame->rect()).subtracted(scr);
			pic = QPixmap(frame->size() * widgetDpr(frame));
			pic.setDevicePixelRatio(widgetDpr(frame));
			pic.fill(Qt::black);
			if (!around.isEmpty())		// an empty region would render the lot
				frame->render(&pic, QPoint(), around);
			QPainter pnt(&pic);
			pnt.drawImage(scr.topLeft(), img.mirrored());
			if (fsBar && fsBar->isVisible())		// it is over the picture
				fsBar->render(&pnt, fsBar->pos());
			if (fsTool && fsTool->isVisible())
				fsTool->render(&pnt, fsTool->pos());
			// the window's own, laid out over the picture when its arrow is open
			if (toolBar->isVisible() && toolBar->geometry().intersects(scr))
				toolBar->render(&pnt, toolBar->pos());
		}
#endif
		if (pic.isNull()) pic = frame->grab();
		QString path = QString(arr).trimmed().mid(com.size()).trimmed();
		if (!path.isEmpty() && pic.save(path)) {
			// where it is, in the coordinates click takes
			QRect rc((w ? w : frame)->geometry());
			rc.moveTopLeft(frame->mapFromGlobal(w ? w->mapToGlobal(QPoint(0, 0)) : frame->mapToGlobal(QPoint(0, 0))));
			sock->write(QString("ok %0 %1 %2 %3\r\n").arg(rc.x()).arg(rc.y()).arg(rc.width()).arg(rc.height()).toUtf8());
		} else {
			sock->write("failed\r\n");
		}
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
