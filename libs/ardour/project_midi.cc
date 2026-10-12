/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/logic_project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <glib.h>
#include <glib/gstdio.h>

namespace ARDOUR { namespace ProjectExchange {
namespace {
void require (bool condition, const char* reason) { if (!condition) { throw ExchangeError (reason); } }
struct Bytes {
	const unsigned char* pos;
	const unsigned char* end;
	unsigned byte () { require (pos != end, "Truncated MIDI data"); return *pos++; }
	uint32_t word (unsigned size) { uint32_t value = 0; while (size--) { value = value * 256 + byte (); } return value; }
	uint32_t vlq () {
		uint32_t value = 0;
		for (unsigned i = 0; i < 4; ++i) { unsigned b = byte (); value = (value << 7) | (b & 127); if (!(b & 128)) { return value; } }
		throw ExchangeError ("Invalid MIDI variable-length value");
	}
	Bytes take (size_t count) { require (count <= size_t (end - pos), "Truncated MIDI chunk"); Bytes value {pos, pos + count}; pos += count; return value; }
	std::string text () {
		std::string value (reinterpret_cast<const char*> (pos), end - pos);
		require (value.find ('\0') == std::string::npos && g_utf8_validate (value.data (), value.size (), nullptr), "MIDI text is not valid UTF-8");
		return value;
	}
};
struct Transport {
	unsigned tempo = 500000, numerator = 4, denominator = 4;
	bool tempo_set = false, meter_set = false, offset_set = false;
	double offset = 0;
};
}

void read_project_midi (const std::string& path, Project& project, double origin, CancelCheck cancel)
{
	if (cancel && cancel ()) { throw ArchiveCancelled (); }
	GStatBuf stat;
	require (g_stat (path.c_str (), &stat) == 0 && S_ISREG (stat.st_mode) && stat.st_size >= 14 && stat.st_size <= 64 * 1024 * 1024,
	         "MIDI file is missing, invalid, or exceeds 64 MiB");
	char* data = nullptr; gsize size = 0;
	require (g_file_get_contents (path.c_str (), &data, &size, nullptr), "Cannot read MIDI file");
	std::unique_ptr<char, void (*) (void*)> storage (data, g_free);
	require (size <= 64 * 1024 * 1024, "MIDI file exceeds 64 MiB");
	Bytes file {reinterpret_cast<unsigned char*> (data), reinterpret_cast<unsigned char*> (data) + size};
	require (file.word (4) == 0x4d546864, "MIDI header is missing");
	Bytes header = file.take (file.word (4));
	unsigned format = header.word (2), count = header.word (2), ppqn = header.word (2);
	require (format <= 1 && count > 0 && count <= 4096 && (format != 0 || count == 1), "Only type-0/1 MIDI projects are supported");
	require (ppqn > 0 && !(ppqn & 0x8000), "SMPTE-division MIDI files are not supported");
	Transport transport;
	size_t events = 0;
	std::vector<Track> tracks;
	std::vector<Marker> markers;
	for (unsigned index = 0; index < count; ++index) {
		require (file.word (4) == 0x4d54726b, "MIDI track chunk is missing");
		Bytes input = file.take (file.word (4));
		Track track; track.kind = TrackKind::MIDI; track.id = "midi-" + std::to_string (index);
		track.name = "MIDI " + std::to_string (index + 1);
		Clip clip; clip.kind = ClipKind::MIDI;
		std::map<unsigned, std::pair<uint64_t, unsigned>> sounding;
		uint64_t tick = 0; unsigned running = 0; bool ended = false, named = false, descriptive = false;
		while (input.pos != input.end) {
			if (cancel && cancel ()) { throw ArchiveCancelled (); }
			require (++events <= 2000000, "MIDI project exceeds the event limit");
			tick += input.vlq ();
			require (tick < (uint64_t (1) << 48), "MIDI timeline exceeds supported range");
			unsigned status = input.byte ();
			if (status < 128) { require (running != 0, "MIDI running status has no channel status"); --input.pos; status = running; }
			if (status == 0xff) {
				running = 0;
				unsigned type = input.byte ();
				Bytes payload = input.take (input.vlq ());
				size_t length = payload.end - payload.pos;
				if (type == 0x2f) { require (length == 0 && input.pos == input.end, "Invalid MIDI end-of-track"); ended = true; break; }
				if (type == 0x51) {
					require (length == 3, "Invalid MIDI tempo event"); unsigned value = payload.word (3);
					require (value > 0, "Invalid MIDI tempo");
					require ((tick == 0 && (!transport.tempo_set || value == transport.tempo)) || value == transport.tempo,
					         "Variable or conflicting MIDI tempo is not yet supported by project import");
					transport.tempo = value; transport.tempo_set = true;
				} else if (type == 0x58) {
					require (length == 4, "Invalid MIDI time signature");
					unsigned n = payload.byte (), power = payload.byte (); payload.byte (); unsigned notes = payload.byte ();
					require (n > 0 && power <= 6 && notes == 8, "Unsupported MIDI time signature units");
					unsigned d = 1u << power;
					require ((tick == 0 && (!transport.meter_set || (n == transport.numerator && d == transport.denominator))) ||
					         (n == transport.numerator && d == transport.denominator), "Variable or conflicting MIDI meter is not yet supported by project import");
					transport.numerator = n; transport.denominator = d; transport.meter_set = true;
				} else if (type == 0x54) {
					require (tick == 0 && length == 5, "Invalid MIDI SMPTE offset");
					unsigned hour = payload.byte (), minute = payload.byte (), second = payload.byte (), frame = payload.byte (), fraction = payload.byte ();
					unsigned mode = (hour >> 5) & 3, fps = mode == 0 ? 24 : mode == 1 ? 25 : 30;
					require (hour < 128 && (hour & 31) < 24 && minute < 60 && second < 60 && frame < fps && fraction < 100,
					         "Invalid MIDI SMPTE offset values");
					double offset;
					if (mode == 2) {
						require (minute % 10 == 0 || second != 0 || frame >= 2, "Invalid MIDI drop-frame timecode");
						unsigned minutes = (hour & 31) * 60 + minute;
						offset = (((minutes * 60 + second) * 30 + frame) - 2 * (minutes - minutes / 10) + fraction / 100.0) * 1001 / 30000.0;
					} else { offset = (hour & 31) * 3600 + minute * 60 + second + (frame + fraction / 100.0) / fps; }
					require (!transport.offset_set || std::fabs (offset - transport.offset) < 1e-6, "Conflicting MIDI timeline origins");
					transport.offset = offset; transport.offset_set = true;
				} else if (type == 3) { require (!named || payload.text () == track.name, "Changing MIDI track names are not supported"); track.name = payload.text (); named = true;
				} else if (type == 6) { markers.push_back ({payload.text (), Time (double (tick) / ppqn)});
				} else if (type == 0x20 || type == 0x21 || type == 0x7f) {
					throw ExchangeError ("MIDI channel prefixes, ports and sequencer-specific events need explicit conversion");
				} else if (type != 0 && type != 1 && type != 2 && type != 4 && type != 5 && type != 7 && type != 0x59) {
					throw ExchangeError ("Unsupported MIDI metadata event");
				} else if (length && type != 0 && !descriptive) {
					project.report.push_back ({Disposition::Transformed, track.name, "MIDI descriptive text/key-signature metadata is not represented in the session"});
					descriptive = true;
				}
				continue;
			}
			require (status >= 0x80 && status < 0xf0, "MIDI system messages need explicit conversion");
			running = status;
			unsigned a = input.byte (), b = (status & 0xe0) == 0xc0 ? 0 : input.byte ();
			require (a < 128 && b < 128, "Invalid MIDI channel data");
			unsigned type = status & 0xf0, key = (status & 15) * 128 + a;
			if (type == 0x90 && b != 0) {
				require (sounding.emplace (key, std::make_pair (tick, b)).second, "Overlapping MIDI notes of the same pitch/channel cannot be preserved");
			} else if (type == 0x80 || type == 0x90) {
				auto note = sounding.find (key);
				require (note != sounding.end () && tick > note->second.first, "Unmatched or zero-length MIDI note");
				Note value; value.time = double (note->second.first) / ppqn; value.duration = double (tick - note->second.first) / ppqn;
				value.channel = status & 15; value.key = a; value.velocity = note->second.second / 127.0; value.release_velocity = (type == 0x90 ? 64 : b) / 127.0;
				clip.notes.push_back (value); sounding.erase (note);
			} else { throw ExchangeError ("MIDI controllers, program changes and expression need explicit conversion"); }
		}
		require (ended && sounding.empty (), "MIDI track is truncated or has unterminated notes");
		if (!clip.notes.empty ()) {
			std::stable_sort (clip.notes.begin (), clip.notes.end (), [] (const Note& a, const Note& b) { return a.time < b.time; });
			clip.name = track.name; clip.duration = double (tick) / ppqn;
			track.clips.push_back (clip);
		}
		/* Type-1 track zero may contain only global timing. Preserve other
		 * named/empty tracks, including a completely empty type-0 project. */
		if (!clip.notes.empty () || index != 0 || format == 0) { tracks.push_back (track); }
	}
	require (file.pos == file.end, "Unexpected data after MIDI tracks");
	if (transport.offset_set) { require (std::fabs (transport.offset - origin) <= 0.001, "AAF and MIDI timeline origins do not match"); }
	else { project.report.push_back ({Disposition::Transformed, "Timeline", "MIDI has no SMPTE offset; beat zero is aligned with the AAF composition start"}); }
	project.tempo = 60000000.0 / transport.tempo;
	project.numerator = transport.numerator; project.denominator = transport.denominator;
	if (!transport.tempo_set || !transport.meter_set) { project.report.push_back ({Disposition::Transformed, "MIDI transport", "Absent tempo or meter uses the MIDI defaults (120 BPM, 4/4)"}); }
	project.tracks.insert (project.tracks.end (), tracks.begin (), tracks.end ());
	project.markers.insert (project.markers.end (), markers.begin (), markers.end ());
	project.report.push_back ({Disposition::Transformed, "MIDI clips", "Each MIDI note track becomes one clip starting at beat zero; instrument sounds are not included in SMF"});
}
} }
