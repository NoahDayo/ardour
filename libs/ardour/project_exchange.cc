/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/project_exchange.h"

#include <cmath>
#include <map>
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
	require (std::isfinite (project.tempo) && project.tempo > 0 && project.tempo <= 10000, "Invalid project tempo");
	require (project.numerator > 0 && project.numerator <= 128 && project.denominator > 0 && project.denominator <= 128 &&
	         (project.denominator & (project.denominator - 1)) == 0, "Invalid project meter");
	text (project.application);
	text (project.application_version);
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
			require (nonnegative (clip.position.value) && nonnegative (clip.duration) && clip.duration > 0 &&
			         std::isfinite (clip.position.value + clip.duration) && nonnegative (clip.offset.value), "Invalid clip range");
			if (clip.kind == ClipKind::Audio) {
				require (track.kind == TrackKind::Audio && clip.notes.empty (), "Audio clip on incompatible track");
				auto source = media.find (clip.media);
				require (source != media.end (), "Unknown audio source: " + clip.media);
				require (clip.offset.unit == TimeUnit::Seconds, "Audio source offset must use seconds");
				double end = clip.offset.value + convert_time (Time (clip.duration, clip.position.unit), TimeUnit::Seconds, project.tempo);
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
