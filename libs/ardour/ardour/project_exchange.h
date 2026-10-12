/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARDOUR_PROJECT_EXCHANGE_H
#define ARDOUR_PROJECT_EXCHANGE_H

#include <stdexcept>
#include <string>
#include <vector>
#include "ardour/libardour_visibility.h"

namespace ARDOUR { namespace ProjectExchange {

class LIBARDOUR_API ExchangeError : public std::runtime_error {
public:
	explicit ExchangeError (const std::string& message) : std::runtime_error (message) {}
};

enum class TimeUnit { Beats, Seconds };
struct Time {
	Time (double v = 0, TimeUnit u = TimeUnit::Beats) : value (v), unit (u) {}
	double value;
	TimeUnit unit;
};

enum class Disposition { Preserved, Transformed, Rendered, Unsupported };
struct Diagnostic {
	Disposition disposition;
	std::string object;
	std::string detail;
};

struct Media {
	std::string id;
	std::string path; // Package member; never implicitly resolve external files.
	unsigned int sample_rate = 0;
	unsigned int channels = 0;
	double duration = 0; // Seconds, for the complete source file.
};

struct Note {
	double time = 0;
	double duration = 0;
	int key = 60;
	int channel = 0;
	double velocity = 1;
	double release_velocity = 0.5;
};

enum class ClipKind { Audio, MIDI };
struct Clip {
	std::string name;
	std::string color;
	ClipKind kind = ClipKind::Audio;
	Time position;
	double duration = 0; // Same unit as position.
	Time offset; // Source time, independent of arrangement position.
	bool enabled = true;
	double gain = 1; // Linear audio-region amplitude; MIDI clips use unity.
	std::string media; // Audio source ID.
	std::vector<Note> notes; // Note times use offset.unit, relative to the source.
};

enum class TrackKind { Audio, MIDI, Folder, Bus };
struct Track {
	std::string id;
	std::string name;
	std::string color;
	std::string parent;
	std::string destination;
	TrackKind kind = TrackKind::Audio;
	unsigned int channels = 2;
	bool master = false;
	bool mute = false;
	bool solo = false;
	double volume = 1; // Linear amplitude.
	double pan = 0.5; // Normalized left-to-right.
	std::vector<Clip> clips;
};

struct Marker {
	std::string name;
	Time position;
};

struct TempoChange {
	Time position;
	double bpm = 120;
	bool linear = false; // Interpolate to the following point in position.unit.
};

struct MeterChange {
	Time position;
	unsigned int numerator = 4;
	unsigned int denominator = 4;
};

struct Project {
	std::string application;
	std::string application_version;
	double tempo = 120; // Transport defaults, before any timeline changes.
	unsigned int numerator = 4;
	unsigned int denominator = 4;
	std::vector<TempoChange> tempos;
	std::vector<MeterChange> meters;
	std::vector<Media> media;
	std::vector<Track> tracks;
	std::vector<Marker> markers;
	std::vector<Diagnostic> report;
};

/* Converts absolute positions and anchored durations separately. In particular,
 * a four-beat duration at bar ten need not last as long as the first four beats.
 * A map is immutable after construction; reuse it across clips and notes. */
class LIBARDOUR_API TimeMap {
public:
	explicit TimeMap (const Project&);
	double convert (Time, TimeUnit) const;
	double duration (Time length, Time origin, TimeUnit) const;
	double content (Time value, Time position, Time offset, TimeUnit) const;
	double tempo_at (Time) const;
	// Equivalent step changes, with bounded error in the beat-to-second mapping.
	std::vector<TempoChange> stepped (double max_error_seconds) const;
private:
	struct Segment {
		double beat, second, bpm, slope;
		TimeUnit axis;
	};
	std::vector<Segment> _segments;
	double _initial;
	size_t segment (Time) const;
};

LIBARDOUR_API double convert_time (Time, TimeUnit, double tempo);
LIBARDOUR_API void validate_project (const Project&);
LIBARDOUR_API bool has_unsupported_features (const Project&);

} }
#endif
