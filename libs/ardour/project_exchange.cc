/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/project_exchange.h"

#include <cmath>
#include <algorithm>
#include <functional>
#include <map>
#include <limits>
#include <set>
#include <glib.h>

namespace ARDOUR { namespace ProjectExchange {
namespace {
void require (bool condition, const std::string& reason)
{
	if (!condition) { throw ExchangeError (reason); }
}

void text (const std::string& value)
{
	require (g_utf8_validate (value.data (), value.size (), NULL), "Invalid UTF-8 project text");
}

bool nonnegative (double value) { return std::isfinite (value) && value >= 0; }
bool normalized (double value) { return nonnegative (value) && value <= 1; }
bool valid_tempo (double value) { return std::isfinite (value) && value > 0 && value <= 10000; }
bool valid_meter (unsigned n, unsigned d) { return n > 0 && n <= 128 && d > 0 && d <= 128 && (d & (d - 1)) == 0; }
}

TimeMap::TimeMap (const Project& project) : _initial (project.tempo)
{
	require (valid_tempo (_initial), "Invalid project tempo");
	require (project.tempos.size () <= 100000 && project.meters.size () <= 100000, "Project transport exceeds the event limit");
	std::vector<TempoChange> points = project.tempos;
	TimeUnit axis = points.empty () ? TimeUnit::Beats : points.front ().position.unit;
	double previous = -1;
	for (auto const& point : points) {
		require (nonnegative (point.position.value) && point.position.value >= previous && point.position.unit == axis && valid_tempo (point.bpm),
		         "Invalid or unordered tempo automation");
		previous = point.position.value;
	}
	if (points.empty () || points.front ().position.value > 0) { points.insert (points.begin (), {Time (0, axis), _initial, false}); }
	for (size_t i = 0; i < points.size (); ++i) {
		auto const& point = points[i];
		double beat = 0, second = 0;
		if (i) {
			beat = convert (point.position, TimeUnit::Beats);
			second = convert (point.position, TimeUnit::Seconds);
		}
		double slope = 0;
		if (point.linear && i + 1 < points.size () && points[i + 1].position.value > point.position.value) {
			slope = (points[i + 1].bpm - point.bpm) / (points[i + 1].position.value - point.position.value);
		}
		require (std::isfinite (slope), "Invalid tempo slope");
		_segments.push_back ({beat, second, point.bpm, slope, axis});
	}
	_initial = tempo_at (Time ());
	require (valid_meter (project.numerator, project.denominator), "Invalid project meter");
	previous = -1;
	for (auto const& point : project.meters) {
		double beat = convert (point.position, TimeUnit::Beats);
		require (nonnegative (point.position.value) && beat > previous && valid_meter (point.numerator, point.denominator),
		         "Invalid or unordered meter automation");
		previous = beat;
	}
}

size_t
TimeMap::segment (Time value) const
{
	auto next = std::upper_bound (_segments.begin (), _segments.end (), value.value,
	                            [value] (double time, const Segment& segment) { return time < (value.unit == TimeUnit::Beats ? segment.beat : segment.second); });
	return next == _segments.begin () ? 0 : size_t (next - _segments.begin () - 1);
}

double
TimeMap::convert (Time value, TimeUnit unit) const
{
	require (std::isfinite (value.value), "Invalid project time");
	if (value.unit == unit) { return value.value; }
	if (value.value < 0) { return convert_time (value, unit, _initial); }
	auto const& s = _segments[segment (value)];
	double delta = value.value - (value.unit == TimeUnit::Beats ? s.beat : s.second);
	double result;
	if (unit == TimeUnit::Seconds) {
		if (!s.slope) { result = 60 * delta / s.bpm; }
		else if (s.axis == TimeUnit::Beats) { result = 60 * std::log1p (s.slope * delta / s.bpm) / s.slope; }
		else { result = 120 * delta / (s.bpm + std::sqrt (s.bpm * s.bpm + 120 * s.slope * delta)); }
		result += s.second;
	} else {
		if (!s.slope) { result = delta * s.bpm / 60; }
		else if (s.axis == TimeUnit::Seconds) { result = (s.bpm * delta + .5 * s.slope * delta * delta) / 60; }
		else { result = s.bpm * std::expm1 (s.slope * delta / 60) / s.slope; }
		result += s.beat;
	}
	require (std::isfinite (result), "Project tempo conversion exceeds its range");
	return result;
}

double
TimeMap::duration (Time length, Time origin, TimeUnit unit) const
{
	require (std::isfinite (length.value) && std::isfinite (origin.value), "Invalid project duration");
	if (length.unit == unit) { return length.value; }
	Time end (convert (origin, length.unit) + length.value, length.unit);
	if (origin.value >= 0 && end.value >= 0 && segment (origin) == segment (end)) {
		auto const& s = _segments[segment (origin)];
		if (!s.slope) { return convert_time (length, unit, s.bpm); }
	}
	return convert (end, unit) - convert (origin, unit);
}

double
TimeMap::content (Time value, Time position, Time offset, TimeUnit unit) const
{
	Time origin (convert (position, offset.unit) - offset.value, offset.unit);
	return duration (value, origin, unit);
}

double
TimeMap::tempo_at (Time value) const
{
	if (value.value < 0) { return _initial; }
	auto const& s = _segments[segment (value)];
	return s.bpm + s.slope * (convert (value, s.axis) - (s.axis == TimeUnit::Beats ? s.beat : s.second));
}

std::vector<TempoChange>
TimeMap::stepped (double tolerance) const
{
	require (std::isfinite (tolerance) && tolerance > 0, "Invalid tempo conversion tolerance");
	std::vector<TempoChange> result;
	auto append = [&] (double beat, double bpm) {
		require (result.size () < 100000, "Tempo curve exceeds the conversion event limit");
		if (!result.empty () && result.back ().position.value == beat) { result.back ().bpm = bpm; }
		else { result.push_back ({Time (beat), bpm, false}); }
	};
	for (size_t i = 0; i < _segments.size (); ++i) {
		auto const& s = _segments[i];
		if (!s.slope || i + 1 == _segments.size ()) { append (s.beat, s.bpm); continue; }
		auto const& end = _segments[i + 1];
		std::function<void (double, double, double, double, unsigned)> split;
		split = [&] (double qa, double qb, double sa, double sb, unsigned depth) {
			double average = 60 * (qb - qa) / (sb - sa);
			/* The maximum timing error occurs where instantaneous BPM equals
			 * the interval average. Both supported ramps are monotonic. */
			double axis = (s.axis == TimeUnit::Beats ? s.beat : s.second) + (average - s.bpm) / s.slope;
			double critical = std::max (qa, std::min (qb, convert (Time (axis, s.axis), TimeUnit::Beats)));
			double error = std::fabs (sa + 60 * (critical - qa) / average - convert (Time (critical), TimeUnit::Seconds));
			if (error > tolerance) {
				require (depth < 48, "Tempo curve exceeds conversion precision");
				double mid = (qa + qb) * .5, seconds = convert (Time (mid), TimeUnit::Seconds);
				split (qa, mid, sa, seconds, depth + 1); split (mid, qb, seconds, sb, depth + 1);
			} else { append (qa, average); }
		};
		split (s.beat, end.beat, s.second, end.second, 0);
	}
	return result;
}

double
convert_time (Time value, TimeUnit unit, double tempo)
{
	require (std::isfinite (tempo) && tempo > 0, "Invalid project tempo");
	double result = value.value;
	if (value.unit != unit) {
		result *= unit == TimeUnit::Seconds ? 60 / tempo : tempo / 60;
	}
	require (std::isfinite (result), "Invalid project time");
	return result;
}

bool
has_unsupported_features (const Project& project)
{
	for (auto const& item : project.report) {
		if (item.disposition == Disposition::Unsupported) { return true; }
	}
	return false;
}

void
validate_project (const Project& project)
{
	TimeMap timing (project);
	text (project.application);
	text (project.application_version);
	for (auto const& marker : project.markers) {
		text (marker.name);
		require (nonnegative (marker.position.value), "Invalid marker time");
	}
	std::map<std::string, const Media*> media;
	std::set<std::string> paths;
	std::set<std::string> ids;
	for (auto const& source : project.media) {
		text (source.id);
		text (source.path);
		require (!source.id.empty () && ids.insert (source.id).second, "Duplicate or empty media ID");
		require (!source.path.empty () && paths.insert (source.path).second, "Duplicate or empty media path");
		require (source.sample_rate > 0 && source.sample_rate <= 1536000 && source.channels > 0 && source.channels <= 64,
		         "Invalid media sample rate or channel count");
		require (nonnegative (source.duration), "Invalid media duration");
		media.emplace (source.id, &source);
	}
	std::map<std::string, const Track*> tracks;
	size_t masters = 0;
	for (auto const& track : project.tracks) {
		text (track.id);
		text (track.name);
		text (track.color);
		require (!track.id.empty () && ids.insert (track.id).second, "Duplicate or empty track ID");
		tracks.emplace (track.id, &track);
		require (track.channels > 0 && track.channels <= 64, "Invalid track channel count");
		require (nonnegative (track.volume) && normalized (track.pan), "Invalid track volume or pan");
		if (track.master) {
			++masters;
			require (track.kind == TrackKind::Bus && track.destination.empty (), "Invalid master channel");
		}
		require ((track.kind != TrackKind::Folder && track.kind != TrackKind::Bus) || track.clips.empty (), "Clips on a folder or bus");
		for (auto const& clip : track.clips) {
			text (clip.name);
			text (clip.color);
			require (nonnegative (clip.gain) && clip.gain <= std::numeric_limits<float>::max () &&
			         (clip.kind == ClipKind::Audio || clip.gain == 1), "Invalid clip gain");
			require (nonnegative (clip.position.value) && nonnegative (clip.duration) && clip.duration > 0 &&
			         std::isfinite (clip.position.value + clip.duration) && nonnegative (clip.offset.value), "Invalid clip range");
			if (clip.kind == ClipKind::Audio) {
				require (track.kind == TrackKind::Audio && clip.notes.empty (), "Audio clip on incompatible track");
				auto source = media.find (clip.media);
				require (source != media.end (), "Unknown audio source: " + clip.media);
				require (clip.offset.unit == TimeUnit::Seconds, "Audio source offset must use seconds");
				double end = clip.offset.value + timing.duration (Time (clip.duration, clip.position.unit), clip.position, TimeUnit::Seconds);
				require (end <= source->second->duration + 1.0 / source->second->sample_rate, "Audio clip extends beyond source media");
			} else {
				require (track.kind == TrackKind::MIDI && clip.media.empty (), "MIDI clip on incompatible track");
				for (auto const& note : clip.notes) {
					require (nonnegative (note.time) && nonnegative (note.duration) && note.duration > 0 &&
					         std::isfinite (note.time + note.duration), "Invalid MIDI note range");
					require (note.key >= 0 && note.key <= 127 && note.channel >= 0 && note.channel <= 15 &&
					         normalized (note.velocity) && normalized (note.release_velocity), "Invalid MIDI note data");
				}
			}
		}
	}
	require (masters <= 1, "Multiple master channels");
	for (auto const& track : project.tracks) {
		for (bool routing : {false, true}) {
			std::set<std::string> visited;
			const Track* current = &track;
			while (current) {
				require (visited.size () < 128 && visited.insert (current->id).second, "Cyclic or excessively deep track structure");
				const std::string& id = routing ? current->destination : current->parent;
				if (id.empty ()) { break; }
				auto next = tracks.find (id);
				require (next != tracks.end (), "Unknown track reference: " + id);
				if (!routing) { require (next->second->kind == TrackKind::Folder, "Parent track is not a folder"); }
				current = next->second;
			}
		}
	}
}

} }
