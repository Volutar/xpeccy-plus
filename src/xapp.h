#pragma once

#include <QApplication>
#include <QEvent>
#include <QColor>
#include <QString>

class xApp : public QApplication {
	Q_OBJECT
	public:
		xApp(int& ac, char** av, int iv):QApplication(ac, av, iv) {}
		QString pendingFile;	// a document macOS sent before the machine was up
		bool holdFiles = false;	// ...or while the start question is up
	public slots:
		void d_frame();
		void d_style();
	signals:
		void s_frame();
	protected:
		bool event(QEvent*);
		bool eventFilter(QObject*, QEvent*);
};
