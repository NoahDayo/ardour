/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_PROJECT_ARCHIVE_H
#define ARDOUR_PROJECT_ARCHIVE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ardour/libardour_visibility.h"

namespace ARDOUR { namespace ProjectExchange {

class LIBARDOUR_API ArchiveError : public std::runtime_error {
public:
	explicit ArchiveError (const std::string& message) : std::runtime_error (message) {}
};

class LIBARDOUR_API ArchiveCancelled : public ArchiveError {
public:
	ArchiveCancelled () : ArchiveError ("Project exchange cancelled") {}
};

typedef std::function<bool ()> CancelCheck;

struct ArchiveLimits {
	uint64_t entry_bytes = uint64_t (16) << 30;
	uint64_t total_bytes = uint64_t (128) << 30;
	size_t entries = 100000;
};

struct ArchiveMember {
	std::string name;
	uint64_t size;
};

/* ZIP paths are identifiers, never extraction destinations. All paths in this
 * API are UTF-8. Never restore entry types or permissions to the filesystem. */
class LIBARDOUR_API ProjectArchive {
public:
	explicit ProjectArchive (const std::string& filename, const ArchiveLimits& = ArchiveLimits (), CancelCheck = CancelCheck ());
	const std::vector<ArchiveMember>& members () const { return _members; }
	std::string read_text (const std::string& member, size_t max_bytes = 16 * 1024 * 1024) const;
	/* Publish to a new, caller-chosen filename. Never overwrite an existing file
	 * or leave a partial destination on cancellation/error. */
	void copy_file (const std::string& member, const std::string& destination) const;

private:
	void read (const std::string&, const std::function<void (const char*, size_t)>&) const;
	std::string _filename;
	ArchiveLimits _limits;
	CancelCheck _cancel;
	std::vector<ArchiveMember> _members;
};

/* Write into a temporary sibling file. finish() publishes the complete ZIP to
 * a new filename; destruction or any write failure discards the temporary. */
class LIBARDOUR_API ProjectArchiveWriter {
public:
	explicit ProjectArchiveWriter (const std::string& destination, const ArchiveLimits& = ArchiveLimits (), CancelCheck = CancelCheck ());
	~ProjectArchiveWriter ();
	void add_text (const std::string& member, const std::string& contents);
	void add_file (const std::string& member, const std::string& source);
	void finish ();

private:
	ProjectArchiveWriter (const ProjectArchiveWriter&) = delete;
	ProjectArchiveWriter& operator= (const ProjectArchiveWriter&) = delete;
	struct Impl;
	std::unique_ptr<Impl> _impl;
};

} }
#endif
