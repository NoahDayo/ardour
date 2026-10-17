/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ardour/logic_project.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
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
	double mixer (aafiAudioGain* gain, Automation& curve, const aafRational_t* rate, const std::string& label, double fallback) {
		// libAAF marks a flat two-point envelope as constant, but retains its
		// time array. Preserve those editable points rather than discarding them.
		if (!gain || !gain->time) { return fixed (gain, label, fallback); }
		require ((gain->flags & AAFI_AUDIO_GAIN_MASK) && gain->value && gain->pts_cnt > 0 && gain->pts_cnt <= 100000 && gain->length > 0,
		         "Invalid AAF mixer automation");
		unsigned interpolation = gain->flags & AAFI_INTERPOL_MASK;
		if (interpolation != AAFI_INTERPOL_LINEAR && interpolation != AAFI_INTERPOL_CONSTANT) {
			project.report.push_back ({Disposition::Unsupported, label, "AAF mixer curve interpolation needs explicit conversion"}); return fallback;
		}
		curve.unit = TimeUnit::Seconds;
		double duration = seconds (gain->length, rate), previous = -1;
		for (unsigned i = 0; i < gain->pts_cnt; ++i) {
			double fraction = rational (&gain->time[i]);
			require (fraction >= 0 && fraction <= 1 && fraction > previous, "Invalid or unordered AAF automation times");
			curve.points.push_back ({fraction * duration, rational (&gain->value[i]), interpolation == AAFI_INTERPOL_LINEAR});
			previous = fraction;
		}
		return curve.points.front ().value;
	}
	void transition (Clip& clip, const aafiTransition* fade, bool incoming, const aafRational_t* rate, double quantum) {
		require (fade && fade->len > 0 && fade->cut_pt >= 0 && fade->cut_pt <= fade->len, "Invalid AAF transition length or cut point");
		unsigned curves = fade->flags & AAFI_TRANS_CURVE_COUNT_MASK;
		bool second = curves == AAFI_TRANS_TWO_CURVE && incoming;
		unsigned interpolation = (second ? fade->flags_b : fade->flags) & AAFI_INTERPOL_MASK;
		if ((curves != AAFI_TRANS_SINGLE_CURVE && curves != AAFI_TRANS_TWO_CURVE) ||
		    (interpolation != AAFI_INTERPOL_LINEAR && interpolation != AAFI_INTERPOL_CONSTANT)) {
			project.report.push_back ({Disposition::Unsupported, clip.name, "AAF dissolve curve type requires explicit conversion"}); return;
		}
		int count = second ? fade->pts_cnt_b : fade->pts_cnt_a;
		auto times = second ? fade->time_b : fade->time_a;
		auto values = second ? fade->value_b : fade->value_a;
		require (count >= 2 && count <= 100000 && times && values, "Invalid AAF dissolve points");
		double duration = seconds (fade->len, rate);
		require (duration <= clip.duration, "AAF transition exceeds its audio clip");
		Automation curve {TimeUnit::Seconds, {}};
		double previous = -1;
		for (int i = 0; i < count; ++i) {
			check ();
			double t = rational (&times[i]), v = rational (&values[i]);
			require (t >= 0 && t <= 1 && t > previous && v >= 0 && v <= 1, "Invalid AAF dissolve timeline or level");
			curve.points.push_back ({t * duration, v, interpolation == AAFI_INTERPOL_LINEAR}); previous = t;
		}
		require (curve.points.front ().time == 0 && previous == 1, "AAF dissolve must define both endpoints");
		// Linearize holds before reversing: Level(1-t) is left-continuous at
		// its jumps. AS-01 12.19 specifies time reversal, not 1-Level(t).
		curve = linearize_automation (curve, quantum);
		duration = curve.points.back ().time;
		double end = std::round (clip.duration / quantum) * quantum;
		require (duration > 0 && end >= duration, "AAF transition is shorter than the audio resolution");
		if (!incoming && curves == AAFI_TRANS_SINGLE_CURVE) {
			std::reverse (curve.points.begin (), curve.points.end ());
			for (auto& point : curve.points) { point.time = duration - point.time; }
		}
		double start = incoming ? 0 : end - duration;
		std::map<double, double> points;
		for (auto const& point : curve.points) { points[start + point.time] = point.value; }
		if (start > 0) { points[0] = 1; points[std::max (0.0, start - quantum)] = 1; }
		if (incoming && duration < end) {
			// The transition is half-open. Return to unity on its first
			// excluded sample even when a custom endpoint is below unity.
			points[duration - quantum] = automation_value (curve, duration - quantum);
			points[duration] = 1; points[end] = 1;
		}
		curve.points.clear ();
		for (auto const& point : points) { curve.points.push_back ({point.first, point.second, true}); }
		clip.envelope = multiply_automation (clip.envelope, curve, quantum);
		project.report.push_back ({Disposition::Transformed, clip.name, curves == AAFI_TRANS_TWO_CURVE
		                          ? "AAF independent incoming/outgoing dissolve curves converted to editable clip envelopes; edges and hold steps rounded to audio samples"
		                          : "AAF dissolve converted to an editable clip envelope; outgoing curve mirrored in time, edges and hold steps rounded to audio samples"});
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
			require (selected.size () <= 8, "AAF audio with more than eight channels is not yet supported");
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
	void pair_stereo_tracks (const std::vector<aafiAudioTrack*>& sources) {
		/* AS-01 represents stereo composition audio as two mono slots. Regroup
		 * consecutive L/R slots only when their channel sources, clip timing
		 * and editable controls agree. Sources may be interleaved or split. */
		auto same_curve = [] (const Automation& a, const Automation& b) {
			if (a.unit != b.unit || a.points.size () != b.points.size ()) { return false; }
			for (size_t i = 0; i < a.points.size (); ++i) {
				if (a.points[i].time != b.points[i].time || a.points[i].value != b.points[i].value || a.points[i].linear != b.points[i].linear) { return false; }
			}
			return true;
		};
		auto same_clip = [&] (const Clip& a, const Clip& b) {
			return a.kind == b.kind && a.position.unit == b.position.unit && a.position.value == b.position.value &&
			       a.duration == b.duration && a.offset.unit == b.offset.unit && a.offset.value == b.offset.value &&
			       a.enabled == b.enabled && a.gain == b.gain && same_curve (a.envelope, b.envelope);
		};
		auto balance_curve = [] (const Track& left, const Track& right, double quantum, Automation& result, double& common) {
			if (left.volume_automation.points.empty () && right.volume_automation.points.empty ()) { return false; }
			if ((!left.volume_automation.points.empty () && left.volume_automation.unit != TimeUnit::Seconds) ||
			    (!right.volume_automation.points.empty () && right.volume_automation.unit != TimeUnit::Seconds)) { return false; }
			std::set<double> times {0};
			for (auto const& point : left.volume_automation.points) { times.insert (point.time); }
			for (auto const& point : right.volume_automation.points) { times.insert (point.time); }
			if (times.size () > 100000) { return false; }
			auto level = [] (const Track& track, double at) {
				return track.volume_automation.points.empty () ? track.volume : automation_value (track.volume_automation, at);
			};
			common = std::max (level (left, 0), level (right, 0));
			if (!std::isfinite (common) || common <= 0) { return false; }
			double tolerance = std::max (1e-6, common * 2e-5);
			auto valid = [&] (double at) {
				double l = level (left, at), r = level (right, at);
				return std::isfinite (l) && std::isfinite (r) && l >= 0 && r >= 0 &&
				       std::fabs (std::max (l, r) - common) <= tolerance;
			};
			for (auto it = times.begin (); it != times.end (); ++it) {
				if (!valid (*it)) { return false; }
				auto next = std::next (it);
				/* Colliding control times cannot be restored as distinct Ardour
				 * sample-time events. Leave such AAF slots as separate mono tracks. */
				if (next != times.end () && *next - *it < quantum * .5) { return false; }
				if (next != times.end () && *next - *it > quantum * 1.5 && !valid ((*it + *next) / 2)) { return false; }
			}
			bool linear = false;
			for (auto const& point : left.volume_automation.points) { linear |= point.linear; }
			for (auto const& point : right.volume_automation.points) { linear |= point.linear; }
			result.unit = TimeUnit::Seconds;
			for (double at : times) {
				double l = level (left, at), r = level (right, at);
				double position = l >= r ? r / (2 * common) : 1 - l / (2 * common);
				result.points.push_back ({at, std::max (0.0, std::min (1.0, position)), linear});
			}
			return true;
		};
		auto gain_with_balance = [] (const Track& left, const Track& right, double quantum, Automation& result, double& position) {
			auto const& l = left.volume_automation.points;
			auto const& r = right.volume_automation.points;
			if (l.empty () || l.size () != r.size () || left.volume_automation.unit != TimeUnit::Seconds ||
			    right.volume_automation.unit != TimeUnit::Seconds) { return false; }
			double reference_left = 0, reference_right = 0;
			for (size_t i = 0; i < l.size (); ++i) {
				if (l[i].time != r[i].time || l[i].linear != r[i].linear || !std::isfinite (l[i].value) ||
				    !std::isfinite (r[i].value) || l[i].value < 0 || r[i].value < 0 ||
				    (i && l[i].time - l[i - 1].time < quantum * .5)) { return false; }
				if (std::max (l[i].value, r[i].value) > 1e-6 && reference_left == 0 && reference_right == 0) {
					double common = std::max (l[i].value, r[i].value);
					reference_left = l[i].value / common;
					reference_right = r[i].value / common;
				}
			}
			if (reference_left == 0 && reference_right == 0) { return false; }
			for (size_t i = 0; i < l.size (); ++i) {
				double common = std::max (l[i].value, r[i].value);
				double tolerance = std::max (1e-6, common * 2e-5);
				if (std::fabs (l[i].value - common * reference_left) > tolerance ||
				    std::fabs (r[i].value - common * reference_right) > tolerance) { return false; }
				result.points.push_back ({l[i].time, common, l[i].linear});
			}
			result.unit = TimeUnit::Seconds;
			position = reference_left >= reference_right ? reference_right / 2 : 1 - reference_left / 2;
			return true;
		};
		auto tagged_gain_balance = [] (const Track& left, const Track& right, const char* left_tag, const char* right_tag,
		                               double quantum, Automation& gain, Automation& balance) {
			auto const& l = left.volume_automation.points;
			auto const& r = right.volume_automation.points;
			if (!left_tag || !right_tag || std::strcmp (left_tag, right_tag) || std::strlen (left_tag) > 4 * 1024 * 1024 ||
			    l.empty () || l.size () != r.size () || left.volume_automation.unit != TimeUnit::Seconds ||
			    right.volume_automation.unit != TimeUnit::Seconds) { return false; }
			std::map<long long, double> silent_balance;
			bool static_silence = std::strncmp (left_tag, "3|", 2) == 0;
			if (std::strcmp (left_tag, "1") != 0) {
				if ((!static_silence && std::strncmp (left_tag, "2|", 2)) || !left_tag[2]) { return false; }
				std::string text (left_tag);
				size_t start = 2;
				try {
					while (start < text.size ()) {
						size_t end = text.find (',', start);
						if (end == std::string::npos) { end = text.size (); }
						if (end + 1 == text.size ()) { return false; }
						size_t separator = text.find ('=', start);
						if (separator == std::string::npos || separator >= end || separator == start || separator + 1 == end) { return false; }
						std::string sample_text = text.substr (start, separator - start);
						std::string balance_text = text.substr (separator + 1, end - separator - 1);
						size_t used = 0;
						long long sample = std::stoll (sample_text, &used);
						if (used != sample_text.size () || std::to_string (sample) != sample_text || sample < 0) { return false; }
						double position = std::stod (balance_text, &used);
						if (used != balance_text.size () || !std::isfinite (position) || position < 0 || position > 1 ||
						    !silent_balance.emplace (sample, position).second || silent_balance.size () > 100000) { return false; }
						start = end + 1;
					}
				} catch (const std::exception&) { return false; }
			}
			gain.unit = balance.unit = TimeUnit::Seconds;
			size_t silent_points = 0;
			for (size_t i = 0; i < l.size (); ++i) {
				if (l[i].time != r[i].time || l[i].linear != r[i].linear || !std::isfinite (l[i].value) ||
				    !std::isfinite (r[i].value) || l[i].value < 0 || r[i].value < 0 ||
				    (i && l[i].time - l[i - 1].time < quantum * .5)) { return false; }
				double common = std::max (l[i].value, r[i].value);
				if (static_silence && common != 0) { return false; }
				double position;
				if (common == 0) {
					double sample = l[i].time / quantum;
					if (!std::isfinite (sample) || sample < 0 || sample > 9007199254740992.0) { return false; }
					auto found = silent_balance.find (std::llround (sample));
					if (found == silent_balance.end () || std::fabs (sample - found->first) > .25) { return false; }
					position = found->second;
					++silent_points;
				} else {
					position = l[i].value >= r[i].value ? r[i].value / (2 * common) : 1 - l[i].value / (2 * common);
				}
				gain.points.push_back ({l[i].time, common, l[i].linear});
				balance.points.push_back ({l[i].time, std::max (0.0, std::min (1.0, position)), l[i].linear});
			}
			return silent_points == silent_balance.size ();
		};
		std::vector<Track> paired;
		paired.reserve (project.tracks.size ());
		paired.push_back (std::move (project.tracks.front ()));
		for (size_t i = 0; i < sources.size (); ++i) {
			check ();
			auto& left = project.tracks[i + 1];
			bool can_pair = i + 1 < sources.size ();
			Automation moving_balance, common_gain_curve, tagged_balance; double balance_gain = 1, static_pan = .5;
			bool variable_balance = false;
			bool gain_balanced = false;
			bool combined_gain_balance = false;
			bool any_combined_tag = false;
			if (can_pair) {
				auto& right = project.tracks[i + 2];
				any_combined_tag = sources[i]->ardour_stereo_gain_balance || sources[i + 1]->ardour_stereo_gain_balance;
				combined_gain_balance = sources[i]->ardour_stereo_gain_balance && sources[i + 1]->ardour_stereo_gain_balance &&
				                        tagged_gain_balance (left, right, sources[i]->ardour_stereo_gain_balance,
				                                             sources[i + 1]->ardour_stereo_gain_balance,
				                                             seconds (1, sources[i]->edit_rate), common_gain_curve, tagged_balance);
				bool empty_pair = left.clips.empty () && right.clips.empty () &&
				                  !sources[i]->timelineItems && !sources[i + 1]->timelineItems &&
				                  !sources[i]->format && !sources[i + 1]->format;
				bool shared_gain = left.volume == right.volume && same_curve (left.volume_automation, right.volume_automation);
				bool static_balance = left.volume_automation.points.empty () && right.volume_automation.points.empty ();
				gain_balanced = !any_combined_tag && !shared_gain && !static_balance &&
				                gain_with_balance (left, right, seconds (1, sources[i]->edit_rate), common_gain_curve, static_pan);
				variable_balance = !any_combined_tag && !shared_gain && !static_balance && !gain_balanced &&
				                   balance_curve (left, right, seconds (1, sources[i]->edit_rate), moving_balance, balance_gain);
				can_pair = sources[i]->number == 1 && sources[i + 1]->number == 2 &&
				           ((sources[i]->format == 1 && sources[i + 1]->format == 1 && !left.clips.empty ()) || empty_pair) &&
				           left.name == right.name && left.channels == 1 && right.channels == 1 &&
				           (any_combined_tag ? combined_gain_balance : (shared_gain || static_balance || gain_balanced || variable_balance)) &&
				           left.pan == .5 && right.pan == .5 &&
				           left.mute == right.mute && left.solo == right.solo &&
				           left.pan_automation.points.empty () && right.pan_automation.points.empty () &&
				           left.clips.size () == right.clips.size ();
				if (can_pair) {
					for (size_t c = 0; c < left.clips.size (); ++c) {
						auto a = channels.find (left.clips[c].media), b = channels.find (right.clips[c].media);
						bool compatible_media = false;
						if (a != channels.end () && b != channels.end () && a->second.size () == 1 && b->second.size () == 1) {
							auto l = a->second[0], r = b->second[0];
							compatible_media = (l.first == r.first && l.second == 0 && r.second == 1) ||
							                   (l.first != r.first && l.first->channels == 1 && r.first->channels == 1 &&
							                    l.second == 0 && r.second == 0 && l.first->samplerate == r.first->samplerate &&
							                    l.first->length == r.first->length);
						}
						if (!same_clip (left.clips[c], right.clips[c]) || a == channels.end () || b == channels.end () ||
						    !compatible_media) {
							can_pair = false; break;
						}
					}
				}
			}
			if (can_pair) {
				auto& right = project.tracks[i + 2];
				left.channels = 2;
				if (combined_gain_balance) {
					left.volume = common_gain_curve.points.front ().value;
					left.pan = tagged_balance.points.front ().value;
					if (sources[i]->ardour_stereo_gain_balance[0] != '3') {
						left.volume_automation = std::move (common_gain_curve);
					} else {
						left.volume_automation = Automation ();
					}
					left.pan_automation = std::move (tagged_balance);
					project.report.push_back ({Disposition::Transformed, left.name,
					                           sources[i]->ardour_stereo_gain_balance[0] == '3'
					                           ? "Tagged paired AAF levels restored as static zero gain and editable stereo balance automation"
					                           : "Tagged paired AAF levels restored as editable stereo gain and balance automation"});
				} else if (gain_balanced) {
					left.volume = common_gain_curve.points.front ().value;
					left.volume_automation = std::move (common_gain_curve);
					left.pan = static_pan;
					project.report.push_back ({Disposition::Transformed, left.name, "Paired mono AAF levels restored as stereo gain automation and static balance"});
				} else if (variable_balance) {
					left.volume = balance_gain;
					left.volume_automation.points.clear ();
					left.pan = moving_balance.points.front ().value;
					left.pan_automation = std::move (moving_balance);
					project.report.push_back ({Disposition::Transformed, left.name, "Paired mono AAF levels restored as editable stereo balance automation"});
				} else if (left.volume != right.volume) {
					/* Two AS-01 mono levels can represent Ardour's stereo balance:
					 * one side stays at the common gain while the other is attenuated. */
					double common = std::max (left.volume, right.volume);
					left.pan = common == 0 ? .5 : left.volume > right.volume ? right.volume / (2 * common) : 1 - left.volume / (2 * common);
					left.volume = common;
					project.report.push_back ({Disposition::Transformed, left.name, "Paired mono AAF levels restored as stereo balance"});
				}
				bool renamed_clip = false;
				for (size_t c = 0; c < left.clips.size (); ++c) {
					renamed_clip |= left.clips[c].name != right.clips[c].name;
					std::vector<Channel> selected {channels.at (left.clips[c].media)[0], channels.at (right.clips[c].media)[0]};
					auto found = media_ids.find (selected);
					if (found == media_ids.end ()) {
						std::string id = "aaf-media-" + std::to_string (media_ids.size ());
						media_ids[selected] = id; channels[id] = selected;
						auto source = selected.front ().first;
						project.media.push_back ({id, "audio/" + id + ".wav", source->samplerate, 2, double (source->length) / source->samplerate});
						left.clips[c].media = id;
					} else { left.clips[c].media = found->second; }
				}
				if (renamed_clip) { project.report.push_back ({Disposition::Transformed, left.name, "Paired mono AAF clip names differed; retained the left-channel names"}); }
				paired.push_back (std::move (left));
				++i;
			} else {
				if (any_combined_tag) {
					project.report.push_back ({Disposition::Transformed, left.name, "Tagged stereo gain/balance could not be regrouped; channel levels remain separate mono tracks"});
				}
				if (!sources[i]->format && !sources[i]->timelineItems) {
					project.report.push_back ({Disposition::Transformed, left.name, "Empty AAF track has no channel layout; imported as mono"});
				}
				paired.push_back (std::move (left));
			}
		}
		project.tracks.swap (paired);
		std::set<std::string> used_media;
		for (auto const& track : project.tracks) {
			for (auto const& clip : track.clips) { if (!clip.media.empty ()) { used_media.insert (clip.media); } }
		}
		auto& media = project.media;
		media.erase (std::remove_if (media.begin (), media.end (), [&] (const Media& item) { return !used_media.count (item.id); }), media.end ());
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

bool has_aaf_reader_errors (const Project& project)
{
	return std::any_of (project.report.begin (), project.report.end (), [] (const Diagnostic& item) {
		return item.disposition == Disposition::Unsupported && item.object == "AAF reader";
	});
}

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
	if (has_aaf_reader_errors (project)) { return; }
	Track master; master.id = "aaf-master"; master.name = "Master"; master.kind = TrackKind::Bus; master.master = true;
	project.tracks.push_back (master);
	if (self.aaf->Video && self.aaf->Video->Tracks) { project.report.push_back ({Disposition::Unsupported, "AAF video", "Video tracks are not yet supported by project import"}); }
	unsigned track_number = 0;
	std::vector<aafiAudioTrack*> source_tracks;
	aafiAudioTrack* track;
	AAFI_foreachAudioTrack (self.aaf.get (), track) {
		self.check ();
		require (++track_number <= 4096, "AAF exceeds the track limit");
		Track output; output.id = "aaf-track-" + std::to_string (track_number); output.name = name (track->name, "Audio " + std::to_string (track_number));
		output.destination = master.id; output.channels = track->format;
		/* An empty sequence has no channel layout from which to infer its width. */
		if (!output.channels && !track->timelineItems) {
			output.channels = 1;
		}
		require (output.channels == 1 || output.channels == 2 || output.channels == 4 || output.channels == 6 || output.channels == 8, "Unsupported AAF track channel layout");
		output.volume = self.mixer (track->gain, output.volume_automation, track->edit_rate, output.name + " gain", 1);
		output.pan = self.mixer (track->pan, output.pan_automation, track->edit_rate, output.name + " pan", .5);
		output.mute = track->mute; output.solo = track->solo;
		if (output.channels == 2 && (output.pan != .5 || !output.pan_automation.points.empty ())) { project.report.push_back ({Disposition::Unsupported, output.name, "AAF stereo panning semantics require explicit conversion"}); }
		aafiTimelineItem* item; unsigned clip_number = 0;
		std::map<aafiAudioClip*, size_t> clip_indices;
		std::vector<aafiTransition*> transitions;
		AAFI_foreachTrackItem (track, item) {
			self.check ();
			if (item->type == AAFI_TRANS) {
				require (transitions.size () < 100000 && item->data, "Invalid or excessive AAF transitions");
				transitions.push_back (static_cast<aafiTransition*> (item->data)); continue;
			}
			auto clip = aafi_timelineItemToAudioClip (item);
			if (!clip) { project.report.push_back ({Disposition::Unsupported, output.name, "Unknown AAF audio timeline item"}); continue; }
			require (++clip_number <= 100000, "AAF exceeds the clip limit");
			Clip value; value.media = self.media (clip);
			value.name = name (clip->subClipName, name (clip->essencePointerList->essenceFile->name, "Audio"));
			value.position = Time (seconds (clip->pos, track->edit_rate), TimeUnit::Seconds);
			value.duration = seconds (clip->len, track->edit_rate);
			value.offset = Time (seconds (clip->essence_offset, track->edit_rate), TimeUnit::Seconds);
			value.enabled = !clip->mute;
			value.gain = self.mixer (clip->gain, value.envelope, track->edit_rate, value.name + " gain", 1);
			if (!value.envelope.points.empty ()) { value.gain = 1; }
			if (clip->automation) {
				Automation curve {TimeUnit::Seconds, {}};
				self.mixer (clip->automation, curve, track->edit_rate, value.name + " envelope", 1);
				auto source = std::find_if (project.media.begin (), project.media.end (), [&] (const Media& m) { return m.id == value.media; });
				value.envelope = multiply_automation (value.envelope, curve, 1.0 / source->sample_rate);
			}
			for (auto gain : {clip->gain, clip->automation}) {
				if (gain && gain->time && gain->length != clip->len) {
					project.report.push_back ({Disposition::Unsupported, value.name, "AAF clip envelope spans a different operation range; explicit trimming is required"});
				}
			}
			clip_indices[clip] = output.clips.size (); output.clips.push_back (value);
		}
		for (auto fade : transitions) {
			self.check ();
			auto item = fade->timelineItem;
			require (item, "AAF transition has no timeline item");
			auto before = aafi_timelineItemToAudioClip (item->prev), after = aafi_timelineItemToAudioClip (item->next);
			unsigned kind = fade->flags & AAFI_TRANS_FADE_MASK;
			require (kind == AAFI_TRANS_FADE_IN || kind == AAFI_TRANS_FADE_OUT || kind == AAFI_TRANS_XFADE, "Invalid AAF transition type");
			if (output.channels != 1) { project.report.push_back ({Disposition::Unsupported, output.name, "Mono AAF dissolve cannot be applied to a multichannel clip"}); continue; }
			if (kind == AAFI_TRANS_XFADE) {
				require (before && after && fade->len > 0 && before->len >= fade->len && after->len >= fade->len &&
				         after->pos - before->pos == before->len - fade->len, "Inconsistent AAF crossfade overlap");
			}
			for (bool incoming : {false, true}) {
				if ((incoming && kind == AAFI_TRANS_FADE_OUT) || (!incoming && kind == AAFI_TRANS_FADE_IN)) { continue; }
				auto target = clip_indices.find (incoming ? after : before);
				require (target != clip_indices.end (), "AAF transition references a missing audio clip");
				auto& clip = output.clips[target->second];
				auto media = std::find_if (project.media.begin (), project.media.end (), [&] (const Media& m) { return m.id == clip.media; });
				self.transition (clip, fade, incoming, track->edit_rate, 1.0 / media->sample_rate);
			}
		}
		project.tracks.push_back (output);
		source_tracks.push_back (track);
	}
	self.pair_stereo_tracks (source_tracks);
	double origin = self.aaf->Timecode ? seconds (self.aaf->Timecode->start, self.aaf->Timecode->edit_rate) : 0;
	if (!midi.empty ()) { read_project_midi (midi, project, origin, cancel); }
	else { project.report.push_back ({Disposition::Transformed, "Transport", "AAF carries no musical tempo map; imported audio uses 120 BPM and 4/4 until a companion MIDI file is supplied"}); }
	TimeMap timing (project);
	aafiMarker* marker;
	AAFI_foreachMarker (self.aaf.get (), marker) {
		Marker value {name (marker->name, name (marker->comment, "Marker")),
		              Time (seconds (marker->start, marker->edit_rate), TimeUnit::Seconds),
		              seconds (marker->length, marker->edit_rate)};
		bool duplicate = false;
		for (auto& existing : project.markers) {
			if (existing.name == value.name && std::fabs (timing.convert (existing.position, TimeUnit::Seconds) - value.position.value) < 1e-6) {
				// The companion MIDI carries marker names and positions but no range lengths.
				if (existing.duration == 0 && value.duration > 0) { existing = value; }
				else if (std::fabs (existing.duration - value.duration) > 1e-6) { continue; }
				duplicate = true; break;
			}
		}
		if (!duplicate) { project.markers.push_back (value); }
	}
	project.report.push_back ({Disposition::Transformed, "AAF timeline", "Composition start is mapped to session zero; original timecode origin was " + std::to_string (origin) + " seconds"});
	project.report.push_back ({Disposition::Transformed, "AAF mixer", "Audio gain and mono pan, including linear/hold automation, use Ardour controls; source and Ardour pan laws may differ"});
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
