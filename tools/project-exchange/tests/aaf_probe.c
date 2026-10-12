/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent test probe: use Ardour's bundled AAF reader and media extractor. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "aaf/libaaf.h"

static void
json_string (const char* text)
{
	const unsigned char* p = (const unsigned char*) (text ? text : "");
	putchar ('"');
	for (; *p; ++p) {
		if (*p == '"' || *p == '\\') {
			putchar ('\\');
			putchar (*p);
		} else if (*p < 32) {
			printf ("\\u%04x", *p);
		} else {
			putchar (*p);
		}
	}
	putchar ('"');
}

int
main (int argc, char** argv)
{
	AAF_Iface* aafi;
	aafiAudioTrack* track;
	aafiAudioEssenceFile* essence;
	unsigned int count = 0;
	if (argc != 3) {
		fprintf (stderr, "Usage: aaf-probe INPUT.aaf EXTRACTION-DIRECTORY\n");
		return 2;
	}
	aafi = aafi_alloc (NULL);
	if (!aafi) { return 3; }
	aafi_set_debug (aafi, VERB_WARNING, 0, stderr, NULL, NULL);
	if (aafi_load_file (aafi, argv[1]) != 0) {
		aafi_release (&aafi);
		return 4;
	}
	printf ("{\"tracks\":[");
	AAFI_foreachAudioTrack (aafi, track) {
		aafiTimelineItem* item;
		unsigned int clips = 0;
		if (count++) { putchar (','); }
		printf ("{\"name\":");
		json_string (track->name);
		if (track->gain && track->gain->pts_cnt == 1) { printf (",\"gain\":%.12g", aafRationalToDouble (track->gain->value[0])); }
		if (track->gain && track->gain->pts_cnt > 1) {
			printf (",\"gain_curve\":[");
			for (unsigned int point = 0; point < track->gain->pts_cnt; ++point) {
				if (point) { putchar (','); }
				printf ("[%.12g,%.12g]", aafRationalToDouble (track->gain->time[point]), aafRationalToDouble (track->gain->value[point]));
			}
			putchar (']');
		}
		if (track->pan && track->pan->pts_cnt == 1) { printf (",\"pan\":%.12g", aafRationalToDouble (track->pan->value[0])); }
		printf (",\"channels\":%u,\"rate\":[%d,%d],\"clips\":[",
		        track->format, track->edit_rate->numerator, track->edit_rate->denominator);
		AAFI_foreachTrackItem (track, item) {
			aafiAudioClip* clip = aafi_timelineItemToAudioClip (item);
			if (!clip) { continue; }
			if (clips++) { putchar (','); }
			printf ("{\"position\":%" PRId64 ",\"length\":%" PRId64 ",\"offset\":%" PRId64 ",\"channels\":%d",
			        clip->pos, clip->len, clip->essence_offset, clip->channels);
			if (clip->gain && clip->gain->pts_cnt == 1) { printf (",\"gain\":%.12g", aafRationalToDouble (clip->gain->value[0])); }
			putchar ('}');
		}
		printf ("]}");
	}
	printf ("],\"media\":[");
	count = 0;
	AAFI_foreachAudioEssenceFile (aafi, essence) {
		char filename[64];
		snprintf (filename, sizeof (filename), "essence-%u", count);
		if (!essence->is_embedded || aafi_extractAudioEssenceFile (aafi, essence, AAFI_EXTRACT_WAV,
		        argv[2], 0, 0, filename, NULL) != 0) {
			aafi_release (&aafi);
			return 5;
		}
		if (count++) { putchar (','); }
		printf ("{\"name\":");
		json_string (essence->name);
		printf (",\"path\":");
		json_string (essence->usable_file_path);
		printf (",\"channels\":%u,\"rate\":%u,\"frames\":%" PRId64 "}",
		        essence->channels, essence->samplerate, essence->length);
	}
	printf ("]}\n");
	aafi_release (&aafi);
	return 0;
}
