/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/project_archive.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <limits>
#include <set>
#include <utility>
#include <glib.h>
#include <glib/gstdio.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <archive.h>
#include <archive_entry.h>

namespace ARDOUR { namespace ProjectExchange {
namespace {

void check_cancel (const CancelCheck& cancel)
{
	if (cancel && cancel ()) {
		throw ArchiveCancelled ();
	}
}

void check_archive (int status, struct archive* a)
{
	if (status != ARCHIVE_OK) {
		const char* error = archive_error_string (a);
		throw ArchiveError (error ? error : "ZIP operation failed");
	}
}

typedef std::unique_ptr<FILE, int (*) (FILE*)> File;
typedef std::unique_ptr<struct archive, int (*) (struct archive*)> Archive;
typedef std::unique_ptr<struct archive_entry, void (*) (struct archive_entry*)> Entry;

File open_file (const std::string& path, const char* mode)
{
	if (path.empty () || !g_utf8_validate (path.data (), path.size (), NULL)) {
		throw ArchiveError ("Invalid UTF-8 filesystem path");
	}
	File f (g_fopen (path.c_str (), mode), fclose);
	if (!f) {
		throw ArchiveError ("Cannot open file: " + path + ": " + g_strerror (errno));
	}
	return f;
}

/* Case-fold and normalize for portable collision detection (including implicit
 * parent directories). Reject Windows aliases even when importing on Unix. */
std::string portable_name (const std::string& name)
{
	if (name.empty () || name.size () > 4096 || !g_utf8_validate (name.data (), name.size (), NULL)) {
		throw ArchiveError ("Invalid UTF-8 ZIP path");
	}
	size_t start = 0;
	while (start < name.size ()) {
		size_t end = name.find ('/', start);
		if (end == std::string::npos) { end = name.size (); }
		std::string part = name.substr (start, end - start);
		if (part.empty () || part == "." || part == ".." || part.back () == '.' || part.back () == ' ') {
			throw ArchiveError ("Non-portable ZIP path: " + name);
		}
		for (unsigned char c : part) {
			if (c < 32 || c == 127 || std::strchr ("\\:*?\"<>|", c)) {
				throw ArchiveError ("Non-portable ZIP path: " + name);
			}
		}
		gchar* normalized_part = g_utf8_normalize (part.c_str (), -1, G_NORMALIZE_ALL_COMPOSE);
		std::string stem (normalized_part);
		g_free (normalized_part);
		stem = stem.substr (0, stem.find ('.'));
		while (!stem.empty () && stem.back () == ' ') { stem.pop_back (); }
		std::transform (stem.begin (), stem.end (), stem.begin (), [] (unsigned char c) { return g_ascii_toupper (c); });
		if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$" ||
		    (stem.size () == 4 && (stem.substr (0, 3) == "COM" || stem.substr (0, 3) == "LPT") && stem[3] >= '1' && stem[3] <= '9')) {
			throw ArchiveError ("Reserved ZIP path: " + name);
		}
		start = end + 1;
	}
	if (name.back () == '/') { throw ArchiveError ("Empty ZIP path component"); }
	gchar* folded = g_utf8_casefold (name.c_str (), -1);
	gchar* normalized = g_utf8_normalize (folded, -1, G_NORMALIZE_ALL_COMPOSE);
	std::string result (normalized);
	g_free (normalized);
	g_free (folded);
	return result;
}

class NameIndex {
public:
	void add (const std::string& name, bool directory)
	{
		std::string key = portable_name (name);
		if (!_explicit.insert (key).second) { throw ArchiveError ("Duplicate ZIP path: " + name); }
		size_t end = 0;
		while ((end = name.find ('/', end)) != std::string::npos) {
			insert (name.substr (0, end), true);
			++end;
		}
		insert (name, directory);
	}
private:
	void insert (const std::string& name, bool directory)
	{
		std::string key = portable_name (name);
		auto found = _paths.find (key);
		if (found != _paths.end ()) {
			if (found->second.first != name || !found->second.second || !directory) {
				throw ArchiveError ("Conflicting ZIP path: " + name);
			}
		} else {
			_paths.emplace (key, std::make_pair (name, directory));
		}
	}
	std::set<std::string> _explicit;
	std::map<std::string, std::pair<std::string, bool> > _paths;
};

struct Reader {
	explicit Reader (const std::string& filename)
		: file (open_file (filename, "rb")), handle (archive_read_new (), archive_read_free)
	{
		if (!handle) { throw ArchiveError ("Cannot allocate ZIP reader"); }
		check_archive (archive_read_support_format_zip (handle.get ()), handle.get ());
		check_archive (archive_read_open_FILE (handle.get (), file.get ()), handle.get ());
	}
	bool next (struct archive_entry*& entry)
	{
		int status = archive_read_next_header (handle.get (), &entry);
		if (status == ARCHIVE_EOF) { return false; }
		check_archive (status, handle.get ());
		return true;
	}
	/* Destroy the archive first: it uses the FILE until closed. */
	File file;
	Archive handle;
};

ArchiveMember inspect (struct archive_entry* entry, bool& directory)
{
	if (archive_entry_is_encrypted (entry) || archive_entry_symlink (entry) || archive_entry_hardlink (entry)) {
		throw ArchiveError ("Encrypted or linked ZIP entries are not supported");
	}
	directory = archive_entry_filetype (entry) == AE_IFDIR;
	if (!directory && archive_entry_filetype (entry) != AE_IFREG) {
		throw ArchiveError ("ZIP entry is not a regular file");
	}
	const char* raw = archive_entry_pathname_utf8 (entry);
	if (!raw) { throw ArchiveError ("ZIP path cannot be decoded as UTF-8"); }
	std::string name (raw);
	if (directory && !name.empty () && name.back () == '/') { name.pop_back (); }
	portable_name (name);
	if (!archive_entry_size_is_set (entry) || archive_entry_size (entry) < 0 || (directory && archive_entry_size (entry) != 0)) {
		throw ArchiveError ("Invalid ZIP entry size: " + name);
	}
	return {name, static_cast<uint64_t> (archive_entry_size (entry))};
}

void account (uint64_t size, uint64_t& total, size_t& entries, const ArchiveLimits& limits)
{
	if (entries >= limits.entries || size > static_cast<uint64_t> (std::numeric_limits<int64_t>::max ()) ||
	    size > limits.entry_bytes || total > limits.total_bytes || size > limits.total_bytes - total) {
		throw ArchiveError ("Project archive exceeds configured size or entry limits");
	}
	++entries;
	total += size;
}

struct TemporaryFile {
	explicit TemporaryFile (const std::string& destination) : file (NULL, fclose), target (destination)
	{
		if (destination.empty () || !g_utf8_validate (destination.data (), destination.size (), NULL)) {
			throw ArchiveError ("Invalid UTF-8 destination path");
		}
		std::string pattern = destination + ".part-XXXXXX";
		std::vector<char> writable (pattern.begin (), pattern.end ());
		writable.push_back (0);
		int flags = O_RDWR;
#ifdef O_BINARY
		flags |= O_BINARY;
#endif
		int fd = g_mkstemp_full (writable.data (), flags, 0600);
		if (fd < 0) { throw ArchiveError ("Cannot create temporary project file: " + std::string (g_strerror (errno))); }
		path = writable.data ();
		file.reset (fdopen (fd, "w+b"));
		if (!file) {
			close (fd);
			g_unlink (path.c_str ());
			throw ArchiveError ("Cannot open temporary project file");
		}
	}
	~TemporaryFile ()
	{
		file.reset ();
		g_unlink (path.c_str ());
	}
	void publish ()
	{
		if (fclose (file.release ()) != 0) { throw ArchiveError ("Cannot flush project file"); }
		/* Both operations fail atomically if the destination already exists. */
#ifdef _WIN32
		gunichar2* src = g_utf8_to_utf16 (path.c_str (), -1, NULL, NULL, NULL);
		gunichar2* dst = g_utf8_to_utf16 (target.c_str (), -1, NULL, NULL, NULL);
		bool success = src && dst && MoveFileExW (reinterpret_cast<LPCWSTR> (src), reinterpret_cast<LPCWSTR> (dst), 0);
		g_free (src);
		g_free (dst);
#else
		bool success = link (path.c_str (), target.c_str ()) == 0;
#endif
		if (!success) { throw ArchiveError ("Cannot publish project file (destination must not exist): " + target); }
	}
	File file;
	std::string target;
	std::string path;
};

} // namespace

ProjectArchive::ProjectArchive (const std::string& filename, const ArchiveLimits& limits, CancelCheck cancel)
	: _filename (filename), _limits (limits), _cancel (cancel)
{
	check_cancel (_cancel);
	Reader reader (_filename);
	NameIndex names;
	uint64_t total = 0;
	size_t entries = 0;
	struct archive_entry* entry;
	while (reader.next (entry)) {
		check_cancel (_cancel);
		bool directory;
		ArchiveMember member = inspect (entry, directory);
		names.add (member.name, directory);
		account (member.size, total, entries, _limits);
		if (!directory) { _members.push_back (member); }
		check_archive (archive_read_data_skip (reader.handle.get ()), reader.handle.get ());
	}
}

void
ProjectArchive::read (const std::string& name, const std::function<void (const char*, size_t)>& output) const
{
	auto found = std::find_if (_members.begin (), _members.end (), [&name] (const ArchiveMember& m) { return m.name == name; });
	if (found == _members.end ()) { throw ArchiveError ("Missing ZIP member: " + name); }
	check_cancel (_cancel);
	Reader reader (_filename);
	struct archive_entry* entry;
	while (reader.next (entry)) {
		check_cancel (_cancel);
		bool directory;
		ArchiveMember member = inspect (entry, directory);
		if (member.name != name || directory) {
			check_archive (archive_read_data_skip (reader.handle.get ()), reader.handle.get ());
			continue;
		}
		if (member.size != found->size) { throw ArchiveError ("ZIP changed while reading: " + name); }
		uint64_t total = 0;
		char buffer[65536];
		for (;;) {
			check_cancel (_cancel);
			la_ssize_t count = archive_read_data (reader.handle.get (), buffer, sizeof (buffer));
			if (count < 0) { check_archive (ARCHIVE_FATAL, reader.handle.get ()); }
			if (!count) { break; }
			if (static_cast<uint64_t> (count) > member.size - total) { throw ArchiveError ("ZIP data exceeds declared size"); }
			output (buffer, static_cast<size_t> (count));
			total += count;
		}
		if (total != member.size) { throw ArchiveError ("Truncated ZIP member: " + name); }
		return;
	}
	throw ArchiveError ("ZIP changed while reading: " + name);
}

std::string
ProjectArchive::read_text (const std::string& member, size_t max_bytes) const
{
	for (auto const& entry : _members) {
		if (entry.name == member && entry.size > max_bytes) { throw ArchiveError ("Project text exceeds configured size limit"); }
	}
	std::string result;
	read (member, [&] (const char* data, size_t size) {
		if (size > max_bytes - result.size ()) { throw ArchiveError ("Project text exceeds configured size limit"); }
		result.append (data, size);
	});
	return result;
}

void
ProjectArchive::copy_file (const std::string& member, const std::string& destination) const
{
	TemporaryFile output (destination);
	read (member, [&] (const char* data, size_t size) {
		if (fwrite (data, 1, size, output.file.get ()) != size) { throw ArchiveError ("Cannot write extracted media"); }
	});
	check_cancel (_cancel);
	output.publish ();
}

struct ProjectArchiveWriter::Impl {
	Impl (const std::string& destination, const ArchiveLimits& l, CancelCheck c)
		: limits (l), cancel (c), output (destination), handle (archive_write_new (), archive_write_free)
	{
		check_cancel (cancel);
		if (!handle) { throw ArchiveError ("Cannot allocate ZIP writer"); }
		check_archive (archive_write_set_format_zip (handle.get ()), handle.get ());
		check_archive (archive_write_set_format_option (handle.get (), "zip", "hdrcharset", "UTF-8"), handle.get ());
		/* ZIP readers may require the end-of-central-directory record at EOF. */
		check_archive (archive_write_set_bytes_in_last_block (handle.get (), 1), handle.get ());
		check_archive (archive_write_open_FILE (handle.get (), output.file.get ()), handle.get ());
	}
	void begin (const std::string& name, uint64_t size)
	{
		if (failed || finished) { throw ArchiveError ("ZIP writer is no longer writable"); }
		/* Remain failed until the whole entry, including CRC, has been written. */
		failed = true;
		check_cancel (cancel);
		names.add (name, false);
		account (size, total, entries, limits);
		Entry entry (archive_entry_new (), archive_entry_free);
		if (!entry) { throw ArchiveError ("Cannot allocate ZIP entry"); }
		archive_entry_set_pathname_utf8 (entry.get (), name.c_str ());
		archive_entry_set_filetype (entry.get (), AE_IFREG);
		archive_entry_set_perm (entry.get (), 0644);
		archive_entry_set_size (entry.get (), size);
		check_archive (archive_write_header (handle.get (), entry.get ()), handle.get ());
	}
	void data (const char* buffer, size_t count)
	{
		while (count) {
			check_cancel (cancel);
			size_t chunk = std::min (count, size_t (65536));
			la_ssize_t written = archive_write_data (handle.get (), buffer, chunk);
			if (written <= 0) { check_archive (ARCHIVE_FATAL, handle.get ()); }
			buffer += written;
			count -= written;
		}
	}
	void end ()
	{
		check_archive (archive_write_finish_entry (handle.get ()), handle.get ());
		failed = false;
	}
	ArchiveLimits limits;
	CancelCheck cancel;
	TemporaryFile output;
	Archive handle;
	NameIndex names;
	uint64_t total = 0;
	size_t entries = 0;
	bool failed = false;
	bool finished = false;
};

ProjectArchiveWriter::ProjectArchiveWriter (const std::string& destination, const ArchiveLimits& limits, CancelCheck cancel)
	: _impl (new Impl (destination, limits, cancel)) {}

ProjectArchiveWriter::~ProjectArchiveWriter () {}

void
ProjectArchiveWriter::add_text (const std::string& member, const std::string& contents)
{
	_impl->begin (member, contents.size ());
	_impl->data (contents.data (), contents.size ());
	_impl->end ();
}

void
ProjectArchiveWriter::add_file (const std::string& member, const std::string& source)
{
	/* Poison the writer even if opening/statting the source fails, so callers
	 * cannot accidentally publish an incomplete project after catching errors. */
	if (_impl->failed || _impl->finished) { throw ArchiveError ("ZIP writer is no longer writable"); }
	_impl->failed = true;
	GStatBuf info;
	if (g_stat (source.c_str (), &info) != 0 || !S_ISREG (info.st_mode) || info.st_size < 0) {
		throw ArchiveError ("Invalid media source: " + source);
	}
	File input (open_file (source, "rb"));
#ifdef _WIN32
	struct _stat64 opened;
	int stat_status = _fstat64 (_fileno (input.get ()), &opened);
#else
	struct stat opened;
	int stat_status = fstat (fileno (input.get ()), &opened);
#endif
	if (stat_status != 0 || !S_ISREG (opened.st_mode) || opened.st_size != info.st_size) {
		throw ArchiveError ("Media source changed or cannot be read: " + source);
	}
	_impl->failed = false;
	_impl->begin (member, info.st_size);
	uint64_t remaining = info.st_size;
	char buffer[65536];
	while (remaining) {
		check_cancel (_impl->cancel);
		size_t count = fread (buffer, 1, static_cast<size_t> (std::min (remaining, uint64_t (sizeof (buffer)))), input.get ());
		if (!count) { throw ArchiveError ("Media source changed or cannot be read: " + source); }
		_impl->data (buffer, count);
		remaining -= count;
	}
	if (fgetc (input.get ()) != EOF || ferror (input.get ())) { throw ArchiveError ("Media source changed or cannot be read: " + source); }
	_impl->end ();
}

void
ProjectArchiveWriter::finish ()
{
	if (_impl->failed || _impl->finished) { throw ArchiveError ("ZIP writer is no longer writable"); }
	_impl->failed = true;
	check_cancel (_impl->cancel);
	check_archive (archive_write_close (_impl->handle.get ()), _impl->handle.get ());
	_impl->handle.reset ();
	check_cancel (_impl->cancel);
	_impl->output.publish ();
	_impl->finished = true;
}

} }
