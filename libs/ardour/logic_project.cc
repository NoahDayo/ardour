/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/logic_project.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <glibmm/miscutils.h>
#include <sndfile.h>
#include "pbd/scoped_file_descriptor.h"
#include "aaf/libaaf.h"

namespace ARDOUR { namespace ProjectExchange {
namespace {
void require (bool value, const std::string& reason) { if (!value) { throw ExchangeError (reason); } }
double rational (const aafRational_t* value) {
	require (value && value->denominator != 0, "Invalid AAF rational value");
	return double (value->numerator) / value->denominator;
}
double seconds (aafPosition_t value, const aafRational_t* rate) {
	double r = rational (rate);
	require (value >= 0 && r > 0, "Invalid AAF timeline time");
	return value / r;
}
std::string name (const char* text, const std::string& fallback) { return text && *text ? text : fallback; }
}

struct LogicProject::Impl {
	Impl (CancelCheck check) : aaf (aafi_alloc (nullptr), [] (AAF_Iface* p) { aafi_release (&p); }), cancel (check) {
		require (bool (aaf), "Cannot allocate AAF reader");
		aafi_set_debug (aaf.get (), VERB_WARNING, 0, nullptr, log, this);
	}
	std::unique_ptr<AAF_Iface, void (*) (AAF_Iface*)> aaf;
	CancelCheck cancel;
	Project project;
	using Channel = std::pair<aafiAudioEssenceFile*, unsigned>;
	std::map<std::vector<Channel>, std::string> media_ids;
	std::map<std::string, std::vector<Channel>> channels;
	std::map<aafiAudioEssenceFile*, std::string> extracted;
	void check () { if (cancel && cancel ()) { throw ArchiveCancelled (); } }
	static void log (aafLog* log, void*, int, int type, const char*, const char*, int, const char* msg, void* user) {
		auto& self = *static_cast<Impl*> (user);
		if ((type == VERB_ERROR || type == VERB_WARNING) && self.project.report.size () < 32) {
			self.project.report.push_back ({type == VERB_ERROR ? Disposition::Unsupported : Disposition::Transformed,
			                               "AAF reader", name (msg, "Unsupported or unreadable AAF content")});
		} else if (type == VERB_ERROR && !has_unsupported_features (self.project)) {
			self.project.report.push_back ({Disposition::Unsupported, "AAF reader", "Further AAF errors prevent complete import"});
		}
		LOG_BUFFER_RESET (log);
	}
	double fixed (aafiAudioGain* gain, const std::string& label, double default_value) {
		if (!gain) { return default_value; }
		if (gain->pts_cnt != 1 || !gain->value) {
			project.report.push_back ({Disposition::Unsupported, label, "AAF gain/pan automation is not yet supported by project import"});
			return default_value;
		}
		return rational (&gain->value[0]);
	}
	std::string media (aafiAudioClip* clip) {
		std::vector<Channel> selected;
		for (auto ptr = clip->essencePointerList; ptr; ptr = ptr->next) {
			check ();
			auto source = ptr->essenceFile;
			require (source && source->channels > 0 && source->channels <= 64 && source->length > 0 && source->samplerate > 0,
			         "AAF audio source has invalid dimensions");
			if (ptr->essenceChannel) {
				require (ptr->essenceChannel <= source->channels, "AAF audio references an unavailable channel");
				selected.push_back ({source, ptr->essenceChannel - 1});
			} else {
				for (unsigned channel = 0; channel < source->channels; ++channel) { selected.push_back ({source, channel}); }
			}
			require (selected.size () <= 2, "AAF audio with more than two channels is not yet supported");
		}
		require (!selected.empty () && selected.size () == size_t (clip->channels), "AAF clip channel mapping is inconsistent");
		auto found = media_ids.find (selected);
		if (found != media_ids.end ()) { return found->second; }
		auto first = selected.front ().first;
		for (auto const& channel : selected) {
			require (channel.first->length == first->length && channel.first->samplerate == first->samplerate,
			         "AAF combined channels have differing lengths or sample rates");
		}
		std::string id = "aaf-media-" + std::to_string (media_ids.size ());
		media_ids[selected] = id; channels[id] = selected;
		project.media.push_back ({id, "audio/" + id + ".wav", first->samplerate, unsigned (selected.size ()), double (first->length) / first->samplerate});
		return id;
	}
	std::string source_file (aafiAudioEssenceFile* source, const std::string& parent) {
		auto found = extracted.find (source);
		if (found != extracted.end ()) { return found->second; }
		check ();
		if (source->is_embedded) {
			std::string filename = "aaf-essence-" + std::to_string (extracted.size ());
			require (source->length > 0 && source->samplesize > 0 &&
			         static_cast<long double> (source->length) * source->channels * source->samplesize / 8 < UINT32_MAX,
			         "Embedded AAF essence exceeds the reader's WAV extraction limit");
			require (aafi_extractAudioEssenceFile (aaf.get (), source, AAFI_EXTRACT_WAV, parent.c_str (), 0, 0, filename.c_str (), nullptr) == 0,
			         "Cannot extract embedded AAF audio");
		}
		require (source->usable_file_path && *source->usable_file_path, "AAF audio file is missing: " + name (source->name, "unnamed source"));
		extracted[source] = source->usable_file_path;
		check ();
		return extracted[source];
	}
};

LogicProject::LogicProject (const std::string& path, const std::string& midi, CancelCheck cancel) : _impl (new Impl (cancel))
{
	auto& self = *_impl; self.check ();
	GStatBuf stat;
	require (g_stat (path.c_str (), &stat) == 0 && S_ISREG (stat.st_mode) && stat.st_size > 0 &&
	         uint64_t (stat.st_size) <= ArchiveLimits ().entry_bytes, "AAF file is missing or exceeds the import size limit");
	require (aafi_load_file (self.aaf.get (), path.c_str ()) == 0, "Cannot read AAF project");
	self.check ();
	auto& project = self.project;
	project.application = "AAF and Standard MIDI File";
	if (has_unsupported_features (project)) { return; }
	Track master; master.id = "aaf-master"; master.name = "Master"; master.kind = TrackKind::Bus; master.master = true;
	project.tracks.push_back (master);
	if (self.aaf->Video && self.aaf->Video->Tracks) { project.report.push_back ({Disposition::Unsupported, "AAF video", "Video tracks are not yet supported by project import"}); }
	unsigned track_number = 0;
	aafiAudioTrack* track;
	AAFI_foreachAudioTrack (self.aaf.get (), track) {
		self.check ();
		require (++track_number <= 4096, "AAF exceeds the track limit");
		Track output; output.id = "aaf-track-" + std::to_string (track_number); output.name = name (track->name, "Audio " + std::to_string (track_number));
		output.destination = master.id; output.channels = track->format;
		/* An empty sequence has no channel layout from which to infer its width. */
		if (!output.channels && !track->timelineItems) {
			output.channels = 1;
			project.report.push_back ({Disposition::Transformed, output.name, "Empty AAF track has no channel layout; imported as mono"});
		}
		require (output.channels == 1 || output.channels == 2, "Unsupported AAF track channel layout");
		output.volume = self.fixed (track->gain, output.name + " gain", 1);
		output.pan = self.fixed (track->pan, output.name + " pan", .5);
		output.mute = track->mute; output.solo = track->solo;
		if (output.channels == 2 && output.pan != .5) { project.report.push_back ({Disposition::Unsupported, output.name, "AAF stereo panning semantics require explicit conversion"}); }
		aafiTimelineItem* item; unsigned clip_number = 0;
		AAFI_foreachTrackItem (track, item) {
			self.check ();
			auto clip = aafi_timelineItemToAudioClip (item);
			if (!clip) { project.report.push_back ({Disposition::Unsupported, output.name, "AAF fades/transitions are not yet supported by project import"}); continue; }
			require (++clip_number <= 100000, "AAF exceeds the clip limit");
			Clip value; value.media = self.media (clip);
			value.name = name (clip->subClipName, name (clip->essencePointerList->essenceFile->name, "Audio"));
			value.position = Time (seconds (clip->pos, track->edit_rate), TimeUnit::Seconds);
			value.duration = seconds (clip->len, track->edit_rate);
			value.offset = Time (seconds (clip->essence_offset, track->edit_rate), TimeUnit::Seconds);
			value.enabled = !clip->mute;
			value.gain = self.fixed (clip->gain, value.name + " gain", 1);
			if (clip->automation) { project.report.push_back ({Disposition::Unsupported, value.name, "AAF clip automation is not yet supported by project import"}); }
			output.clips.push_back (value);
		}
		project.tracks.push_back (output);
	}
	double origin = self.aaf->Timecode ? seconds (self.aaf->Timecode->start, self.aaf->Timecode->edit_rate) : 0;
	if (!midi.empty ()) { read_project_midi (midi, project, origin, cancel); }
	else { project.report.push_back ({Disposition::Transformed, "Transport", "AAF carries no musical tempo map; imported audio uses 120 BPM and 4/4 until a companion MIDI file is supplied"}); }
	aafiMarker* marker;
	AAFI_foreachMarker (self.aaf.get (), marker) {
		if (marker->length) { project.report.push_back ({Disposition::Unsupported, "AAF markers", "Range markers are not yet supported by project import"}); continue; }
		Marker value {name (marker->name, name (marker->comment, "Marker")), Time (seconds (marker->start, marker->edit_rate), TimeUnit::Seconds)};
		bool duplicate = false;
		for (auto const& existing : project.markers) {
			if (existing.name == value.name && std::fabs (convert_time (existing.position, TimeUnit::Seconds, project.tempo) - value.position.value) < 1e-6) { duplicate = true; break; }
		}
		if (!duplicate) { project.markers.push_back (value); }
	}
	project.report.push_back ({Disposition::Transformed, "AAF timeline", "Composition start is mapped to session zero; original timecode origin was " + std::to_string (origin) + " seconds"});
	project.report.push_back ({Disposition::Transformed, "AAF mixer", "Static audio gain and mono pan are imported; source and Ardour pan laws may differ"});
	project.report.push_back ({Disposition::Transformed, "AAF audio", "Selected source channels are copied as 32-bit floating-point audio for the Ardour session"});
	project.report.push_back ({Disposition::Transformed, "AAF clip metadata", "Clip names use AAF subclip or source names; external metadata sidecars and colors are not yet restored"});
	validate_project (project);
}

LogicProject::~LogicProject () = default;
const Project& LogicProject::project () const { return _impl->project; }

void LogicProject::copy_media (const Media& media, const std::string& destination)
{
	auto& self = *_impl; self.check ();
	const auto& channels = self.channels.at (media.id);
	struct Input {
		Input (const std::string& path) : fd (g_open (path.c_str (), O_RDONLY
#ifdef O_BINARY
		                                               | O_BINARY
#endif
		                                               , 0)), info {}, file (fd < 0 ? nullptr : sf_open_fd (fd, SFM_READ, &info, SF_FALSE), sf_close) {}
		PBD::ScopedFileDescriptor fd;
		SF_INFO info;
		std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> file;
		std::vector<float> samples;
	};
	std::map<aafiAudioEssenceFile*, std::unique_ptr<Input>> inputs;
	for (auto const& channel : channels) {
		if (inputs.count (channel.first)) { continue; }
		std::unique_ptr<Input> input (new Input (self.source_file (channel.first, Glib::path_get_dirname (destination))));
		require (bool (input->file) && input->info.channels == channel.first->channels && input->info.frames == channel.first->length &&
		         input->info.samplerate == int (media.sample_rate), "AAF audio does not match its descriptor");
		input->samples.resize (8192 * input->info.channels);
		inputs[channel.first] = std::move (input);
	}
	int binary = 0;
#ifdef O_BINARY
	binary = O_BINARY;
#endif
	PBD::ScopedFileDescriptor fd (g_open (destination.c_str (), O_WRONLY | O_CREAT | O_EXCL | binary, 0600));
	SF_INFO info {}; info.channels = media.channels; info.samplerate = media.sample_rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
	std::unique_ptr<SNDFILE, int (*) (SNDFILE*)> output (fd < 0 ? nullptr : sf_open_fd (fd, SFM_WRITE, &info, SF_FALSE), sf_close);
	require (bool (output), "Cannot create staged AAF audio");
	std::vector<float> buffer (8192 * channels.size ());
	sf_count_t frames = channels.front ().first->length;
	require (frames <= sf_count_t ((UINT32_MAX - 4096) / (4 * channels.size ())), "AAF audio exceeds the staging WAV size limit");
	for (sf_count_t offset = 0; offset < frames;) {
		self.check ();
		sf_count_t count = std::min<sf_count_t> (8192, frames - offset);
		for (auto const& input : inputs) { require (sf_readf_float (input.second->file.get (), input.second->samples.data (), count) == count, "Cannot read complete AAF audio"); }
		for (size_t channel = 0; channel < channels.size (); ++channel) {
			auto& input = *inputs.at (channels[channel].first);
			for (sf_count_t frame = 0; frame < count; ++frame) {
				float value = input.samples[frame * input.info.channels + channels[channel].second];
				require (std::isfinite (value), "AAF audio contains non-finite samples");
				buffer[frame * channels.size () + channel] = value;
			}
		}
		require (sf_writef_float (output.get (), buffer.data (), count) == count, "Cannot write staged AAF audio");
		offset += count;
	}
	require (sf_close (output.release ()) == 0, "Cannot finalize staged AAF audio");
	self.check ();
}
} }
