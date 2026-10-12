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

struct Project {
	std::string application;
	std::string application_version;
	double tempo = 120; // Quarter notes per minute; this initial model is constant.
	unsigned int numerator = 4;
	unsigned int denominator = 4;
	std::vector<Media> media;
	std::vector<Track> tracks;
	std::vector<Marker> markers;
	std::vector<Diagnostic> report;
};

LIBARDOUR_API double convert_time (Time, TimeUnit, double tempo);
LIBARDOUR_API void validate_project (const Project&);
LIBARDOUR_API bool has_unsupported_features (const Project&);

} }
#endif
