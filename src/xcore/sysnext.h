#pragma once

#include <QString>
#include <QStringList>
#include <functional>

// System/Next, what the ZX Spectrum Next's SD card holds: its ROMs, NextZXOS and the
// programs that come with it. It is not shipped with Xpeccy+ - its license passes on
// exact entire copies only - so a card is made from the official release archive,
// downloaded or put beside the program.

struct snRelease {
	QString release;	// "24.11"
	QString tag;		// "system/next_24.11"
	QString archiveSha;	// sha256 of GitLab's .tar.gz, empty when not known
	QString treeSha;	// sn_tree_hash() of what it unpacks to, empty when not known
	qint64 bytes = 0;	// the .tar.gz's size, 0 when not known
	QString local;		// an archive of it here, else empty
	bool cached = false;	// ...downloaded before, in sn_cache_dir(), not beside the program
	bool checked() const {return !treeSha.isEmpty();}
};

QList<snRelease> sn_releases();			// the ones known here, newest first, with any local archive
void sn_add_tags(QList<snRelease>*, const QByteArray& json);	// tags from GitLab's API added, unchecked
QString sn_tags_url();				// GitLab's list of the repository's tags, as JSON
QString sn_archive_url(const snRelease&);
QString sn_archive_name(const snRelease&);	// tbblue-system-next_24.11.tar.gz
QString sn_repo_url();
QString sn_cache_dir();				// <config>/downloads/: archives downloaded, kept for the next card

// A .tar.gz or a .zip, by the name the archive goes by: both are read here, so making a
// card needs no tar, unzip or PowerShell on the machine. The destination must be empty;
// the caller discards it on failure or cancellation. tick(done, total) false stops it.
// pick, when given, is asked for each file: where it goes in dir, or empty to leave it.
bool sn_unpack_archive(const QString& archive, const QString& dir, QString* err,
	std::function<bool(qint64, qint64)> tick = nullptr,
	std::function<QString(const QString&)> pick = nullptr);

// the checksum of a tree: sha256 over the sorted "sha256  path" lines of its files, as
// sha256sum prints them - the same whatever packed them. tick() false stops it.
QString sn_tree_hash(const QString& dir, std::function<bool()> tick = nullptr);
QString sn_file_hash(const QString& file, std::function<bool()> tick = nullptr);
// the folder an archive unpacked to: the one folder in dir when that is all there is
QString sn_unpacked_root(const QString& dir);
// a FAT32 image of size bytes holding the tree; err says why not. tick(percent) false stops it.
bool sn_make_image(const QString& dir, const QString& file, qint64 size, QString* err, std::function<bool(int)> tick = nullptr);
// a card the Next can start from: System/Next's machine folder on it
bool sn_card_ok(const QString& path);

// System/Next as a program from elsewhere starts from (filer.cpp load_next), whatever card the
// user has: --next-system's folder; else a release unpacked into <config>/next-system/ from its
// archive, checked once - unpacked now when an archive here is newer than any unpacked; else
// sysnext/ beside the program. Empty, with *err saying why, when there is nothing to start from.
QString sn_system_dir(QString* err);
// a folder sn_system_dir() gives or gave: a snapshot's card record names one
bool sn_system_is(const QString& path);
QString sn_default_path(bool image);
