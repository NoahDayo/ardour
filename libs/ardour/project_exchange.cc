/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/project_exchange.h"

#include <cmath>
#include <algorithm>
#include <functional>
#include <map>
#include <limits>
#include <set>
#include <tuple>
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

void
append_automation_segment (Automation& output, double end, double value,
                           const std::function<double (double)>& evaluate, double tolerance)
{
	require (!output.points.empty () && end > output.points.back ().time && std::isfinite (end) && std::isfinite (value) && tolerance > 0,
	         "Invalid automation segment");
	size_t evaluations = 0;
	auto eval = [&] (double time) {
		require (++evaluations <= 2000000, "Automation curve exceeds the conversion work limit");
		double v = evaluate (time); require (std::isfinite (v), "Nonfinite automation curve"); return v;
	};
	std::function<void (double, double, double, double, unsigned)> split;
	split = [&] (double a, double va, double b, double vb, unsigned depth) {
		auto line = [&] (double t) { return va + (vb - va) * ((t - a) / (b - a)); };
		std::function<bool (double, double, double, double, unsigned)> bounded;
		bounded = [&] (double l, double vl, double r, double vr, unsigned level) {
			// A monotonic curve is trapped between its endpoint values. The
			// secant is linear, so these opposite-corner errors bound the
			// entire interval even when its midpoint happens to lie on it.
			double bound = std::max (std::fabs (vl - line (r)), std::fabs (vr - line (l)));
			if (bound <= tolerance) { return true; }
			double mid = l + (r - l) * .5;
			if (mid == l || mid == r || level >= 48) { return false; }
			double vm = eval (mid);
			require (vm >= std::min (vl, vr) - tolerance * .01 && vm <= std::max (vl, vr) + tolerance * .01, "Non-monotonic automation segment");
			if (std::fabs (vm - line (mid)) > tolerance * .5) { return false; }
			return bounded (l, vl, mid, vm, level + 1) && bounded (mid, vm, r, vr, level + 1);
		};
		if (bounded (a, va, b, vb, 0)) {
			require (output.points.size () < 100000, "Automation exceeds the event limit");
			output.points.back ().linear = true; output.points.push_back ({b, vb, true}); return;
		}
		double mid = a + (b - a) * .5;
		require (depth < 48 && mid > a && mid < b, "Automation curve exceeds conversion precision");
		double vm = eval (mid); split (a, va, mid, vm, depth + 1); split (mid, vm, b, vb, depth + 1);
	};
	auto start = output.points.back ();
	split (start.time, start.value, end, value, 0);
}

Automation
convert_automation (const Automation& input, const TimeMap& timing, TimeUnit unit)
{
	if (input.unit == unit || input.points.empty ()) { return input; }
	Automation output; output.unit = unit;
	for (size_t i = 0; i < input.points.size (); ++i) {
		auto const& point = input.points[i];
		double at = timing.convert (Time (point.time, input.unit), unit);
		if (i && input.points[i - 1].linear) {
			auto const& before = input.points[i - 1];
			append_automation_segment (output, at, point.value, [&] (double t) {
				double original = timing.convert (Time (t, unit), input.unit);
				return before.value + (point.value - before.value) * ((original - before.time) / (point.time - before.time));
			});
		} else { output.points.push_back ({at, point.value, point.linear}); }
		output.points.back ().linear = point.linear;
	}
	return output;
}

double
automation_value (const Automation& curve, double time)
{
	if (curve.points.empty ()) { return 1; }
	auto after = std::upper_bound (curve.points.begin (), curve.points.end (), time,
	                             [] (double t, const AutomationPoint& p) { return t < p.time; });
	if (after == curve.points.begin ()) { return after->value; }
	auto before = std::prev (after);
	if (after == curve.points.end () || !before->linear) { return before->value; }
	return before->value + (after->value - before->value) * (time - before->time) / (after->time - before->time);
}

Automation
slice_automation (const Automation& input, double start, double end)
{
	require (std::isfinite (start) && std::isfinite (end) && end > start, "Invalid envelope slice");
	Automation result; result.unit = input.unit;
	if (input.points.empty ()) { return result; }
	bool linear = true;
	for (auto const& point : input.points) { if (point.time <= start) { linear = point.linear; } else { break; } }
	result.points.push_back ({0, automation_value (input, start), linear});
	for (auto point : input.points) {
		if (point.time > start && point.time < end) { point.time -= start; result.points.push_back (point); }
	}
	result.points.push_back ({end - start, automation_value (input, end), true});
	return result;
}

Automation
linearize_automation (const Automation& input, double quantum)
{
	require (std::isfinite (quantum) && quantum > 0, "Invalid envelope resolution");
	Automation output; output.unit = input.unit;
	for (size_t i = 0; i < input.points.size (); ++i) {
		auto point = input.points[i]; point.time = std::round (point.time / quantum) * quantum;
		if (i) {
			// Two equally valued points can land on the same audio sample after
			// a clip is trimmed or its source is resampled. Keeping either one
			// gives the same rendered envelope at this resolution. Differing
			// values still need to be rejected rather than silently discarded.
			if (point.time == output.points.back ().time &&
			    std::fabs (point.value - output.points.back ().value) <= 1e-12 * std::max (1.0, std::fabs (point.value))) { continue; }
			require (point.time > output.points.back ().time, "Envelope points collide at audio resolution");
			if (!input.points[i - 1].linear && input.points[i - 1].value != point.value && point.time - quantum > output.points.back ().time + quantum * .001) {
				output.points.push_back ({point.time - quantum, input.points[i - 1].value, true});
			}
		}
		point.linear = true; output.points.push_back (point);
		require (output.points.size () <= 100000, "Envelope exceeds the event limit");
	}
	return output;
}

Automation
multiply_automation (const Automation& first, const Automation& second, double quantum, double tolerance)
{
	if (first.points.empty ()) { return linearize_automation (second, quantum); }
	if (second.points.empty ()) { return linearize_automation (first, quantum); }
	require (first.unit == second.unit && tolerance > 0 && std::isfinite (tolerance), "Incompatible envelopes");
	auto a = linearize_automation (first, quantum), b = linearize_automation (second, quantum);
	std::set<double> times;
	for (auto const& curve : {a, b}) { for (auto const& point : curve.points) { times.insert (point.time); } }
	Automation output; output.unit = a.unit;
	auto value = [&] (double t) { double v = automation_value (a, t) * automation_value (b, t); require (std::isfinite (v), "Envelope amplitude overflow"); return v; };
	output.points.push_back ({*times.begin (), value (*times.begin ()), true});
	std::function<void (double, double)> append;
	append = [&] (double start, double end) {
		// Product of two linear segments is quadratic. Its maximum secant
		// error is |delta_a * delta_b| / 4. No midpoint-only heuristic.
		double error = std::fabs ((automation_value (a, end) - automation_value (a, start)) * (automation_value (b, end) - automation_value (b, start))) / 4;
		double mid = std::round ((start + (end - start) * .5) / quantum) * quantum;
		if (error > tolerance && mid > start + quantum * .001 && mid < end - quantum * .001) { append (start, mid); append (mid, end); return; }
		require (output.points.size () < 100000, "Combined envelope exceeds the event limit");
		output.points.push_back ({end, value (end), true});
	};
	for (auto t = std::next (times.begin ()); t != times.end (); ++t) { append (*std::prev (t), *t); }
	return output;
}

bool
has_unsupported_features (const Project& project)
{
	for (auto const& item : project.report) {
		if (item.disposition == Disposition::Unsupported) { return true; }
	}
	return false;
}

std::string
canonical_vst3_id (const std::string& id)
{
	std::string result;
	if (id.size () != 32 && id.size () != 36) { throw ExchangeError ("Invalid VST3 class ID"); }
	for (size_t i = 0; i < id.size (); ++i) {
		if (id.size () == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
			if (id[i] != '-') { throw ExchangeError ("Invalid VST3 class ID"); }
			continue;
		}
		if (!g_ascii_isxdigit (id[i])) { throw ExchangeError ("Invalid VST3 class ID"); }
		result += g_ascii_toupper (id[i]);
	}
	return result;
}

void
validate_vst3_state (const std::string& id, const std::string& state)
{
	/* A VST3 preset stores its class ID as 32 ASCII hex digits in the header.
	 * Check it before handing opaque state to a plugin instance. */
	require (state.size () >= 48 && state.size () <= 64 * 1024 * 1024 && state.compare (0, 4, "VST3") == 0,
	         "Invalid VST3 preset header or size");
	require (canonical_vst3_id (state.substr (8, 32)) == canonical_vst3_id (id), "VST3 preset class ID does not match the device");
	uint64_t list = 0;
	for (unsigned i = 0; i < 8; ++i) { list |= uint64_t (uint8_t (state[40 + i])) << (8 * i); }
	require (list >= 48 && list <= state.size () - 8 && state.compare (size_t (list), 4, "List") == 0,
	         "Invalid VST3 preset chunk list");
	auto unsigned_at = [&] (size_t offset, unsigned bytes) {
		uint64_t value = 0;
		for (unsigned i = 0; i < bytes; ++i) { value |= uint64_t (uint8_t (state[offset + i])) << (8 * i); }
		return value;
	};
	uint64_t count = unsigned_at (size_t (list) + 4, 4);
	require (unsigned_at (4, 4) == 1, "Unsupported VST3 preset version");
	require (count > 0 && count <= 10000 && count <= (state.size () - size_t (list) - 8) / 20,
	         "Invalid VST3 preset chunk count");
	std::set<std::string> chunks;
	bool transferable = false;
	for (uint64_t i = 0; i < count; ++i) {
		size_t entry = size_t (list) + 8 + size_t (i) * 20;
		std::string type = state.substr (entry, 4);
		require ((type == "Comp" || type == "Cont" || type == "Info") && chunks.insert (type).second,
		         "Unsupported or duplicate VST3 preset chunk; provide a rendered fallback");
		transferable |= type == "Comp" || type == "Cont";
		uint64_t offset = unsigned_at (entry + 4, 8), length = unsigned_at (entry + 12, 8);
		require (offset >= 48 && offset <= list && length <= list - offset, "Invalid VST3 preset chunk range");
	}
	require (transferable, "VST3 preset has no component or controller state");
}

bool
supported_midi_control (unsigned type, unsigned number)
{
	if (type == 0xb0) {
		/* Bank selection, data entry/RPN/NRPN and channel-mode commands depend
		 * on cross-controller event order and must not become independent lanes. */
		return number < 96 && number != 0 && number != 32 && number != 6 && number != 38;
	}
	return (type == 0xa0 && number < 128) || ((type == 0xd0 || type == 0xe0) && number == 0);
}

void
validate_project (const Project& project)
{
	TimeMap timing (project);
	text (project.application);
	text (project.application_version);
	for (auto const& marker : project.markers) {
		text (marker.name);
		require (nonnegative (marker.position.value) && nonnegative (marker.duration) &&
		         std::isfinite (marker.position.value + marker.duration), "Invalid marker range");
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
	size_t plugin_state_bytes = 0;
	for (auto const& track : project.tracks) {
		text (track.id);
		text (track.name);
		text (track.color);
		require (!track.id.empty () && ids.insert (track.id).second, "Duplicate or empty track ID");
		tracks.emplace (track.id, &track);
		require (track.channels > 0 && track.channels <= 64, "Invalid track channel count");
		require (nonnegative (track.volume) && normalized (track.pan), "Invalid track volume or pan");
		require (track.sends.size () <= 128 && (track.sends.empty () || (!track.master && track.kind != TrackKind::Folder)), "Invalid send count or source");
		std::set<std::string> send_targets;
		for (auto const& send : track.sends) {
			text (send.destination);
			require (!send.destination.empty () && send_targets.insert (send.destination).second &&
			         nonnegative (send.volume) && send.volume <= std::numeric_limits<float>::max () && normalized (send.pan), "Invalid send destination, volume or pan");
		}
		require (track.plugins.size () <= 128 && (track.plugins.empty () || track.kind != TrackKind::Folder), "Invalid plugin count or track");
		for (auto const& plugin : track.plugins) {
			text (plugin.id); text (plugin.name); text (plugin.role); text (plugin.state_path);
			require (canonical_vst3_id (plugin.id) == plugin.id && !plugin.name.empty () &&
			         (plugin.role == "audioFX" || plugin.role == "instrument") && !plugin.state_path.empty () &&
			         paths.insert (plugin.state_path).second, "Invalid VST3 plugin metadata or duplicate state path");
			plugin_state_bytes += plugin.state.size ();
			require (plugin_state_bytes <= 512 * 1024 * 1024, "Project plugin state exceeds the size limit");
			if (!plugin.state.empty ()) { validate_vst3_state (plugin.id, plugin.state); }
		}
		for (bool pan : {false, true}) {
			auto const& curve = pan ? track.pan_automation : track.volume_automation;
			require (curve.points.size () <= 100000, "Automation exceeds the event limit");
			double previous = -1;
			for (auto const& point : curve.points) {
				require (nonnegative (point.time) && point.time > previous && (pan ? normalized (point.value) : nonnegative (point.value)), "Invalid or unordered mixer automation");
				previous = point.time;
			}
		}
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
			require (clip.envelope.unit == TimeUnit::Seconds && clip.envelope.points.size () <= 100000 &&
			         (clip.kind == ClipKind::Audio || clip.envelope.points.empty ()), "Invalid clip envelope");
			double previous_envelope = -1;
			double duration = timing.duration (Time (clip.duration, clip.position.unit), clip.position, TimeUnit::Seconds);
			auto envelope_media = media.find (clip.media);
			double envelope_slack = envelope_media == media.end () ? 0 : 1.0 / envelope_media->second->sample_rate;
			for (auto const& point : clip.envelope.points) {
				require (nonnegative (point.time) && point.time > previous_envelope && point.time <= duration + envelope_slack && nonnegative (point.value), "Invalid clip envelope point");
				previous_envelope = point.time;
			}
			if (clip.kind == ClipKind::Audio) {
				require (track.kind == TrackKind::Audio && clip.notes.empty () && clip.midi_controls.empty (), "Audio clip on incompatible track");
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
				std::set<std::tuple<unsigned, unsigned, unsigned>> targets;
				size_t control_events = 0;
				for (auto const& control : clip.midi_controls) {
					require (supported_midi_control (control.type, control.number) && control.channel < 16, "Unsupported MIDI controller target");
					require (targets.emplace (control.type, control.channel, control.number).second, "Duplicate MIDI controller lane");
					double previous = -1;
					for (auto const& point : control.points) {
						require (++control_events <= 100000 && nonnegative (point.time) && point.time > previous, "Invalid or excessive MIDI controller timeline");
						require (!point.linear && nonnegative (point.value) && point.value == std::floor (point.value) && point.value <= (control.type == 0xe0 ? 16383 : 127), "MIDI controllers require discrete integer values");
						previous = point.time;
					}
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
	std::map<std::string, int> routing_state;
	std::function<void (const Track&, size_t)> visit = [&] (const Track& track, size_t depth) {
		require (depth < 128 && routing_state[track.id] != 1, "Cyclic or excessively deep track routing");
		if (routing_state[track.id] == 2) { return; }
		routing_state[track.id] = 1;
		if (!track.destination.empty ()) { visit (*tracks.at (track.destination), depth + 1); }
		for (auto const& send : track.sends) {
			auto target = tracks.find (send.destination);
			require (target != tracks.end () && target->second->kind == TrackKind::Bus && !target->second->master,
			         "Send destination must be an existing audio bus");
			visit (*target->second, depth + 1);
		}
		routing_state[track.id] = 2;
	};
	for (auto const& track : project.tracks) { visit (track, 0); }
}

} }
