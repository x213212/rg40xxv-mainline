#define _POSIX_C_SOURCE 200809L

#include "hardware_internal.h"

#include <alsa/asoundlib.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

static int element_score(const char *name)
{
	if (hw_contains_casefold(name, "lineout volume"))
		return 100;
	if (hw_contains_casefold(name, "master"))
		return 90;
	if (hw_contains_casefold(name, "headphone"))
		return 80;
	if (hw_contains_casefold(name, "pcm"))
		return 70;
	if (hw_contains_casefold(name, "digital volume"))
		return 65;
	if (hw_contains_casefold(name, "playback"))
		return 60;
	return 1;
}

static snd_mixer_elem_t *select_element(snd_mixer_t *mixer)
{
	snd_mixer_elem_t *element;
	snd_mixer_elem_t *best = NULL;
	int best_score = -1;
	unsigned int seen = 0;

	for (element = snd_mixer_first_elem(mixer);
	     element != NULL && seen++ < 128;
	     element = snd_mixer_elem_next(element)) {
		int score;

		if (!snd_mixer_selem_is_active(element) ||
		    !snd_mixer_selem_has_playback_volume(element))
			continue;
		score = element_score(snd_mixer_selem_get_name(element));
		if (score > best_score) {
			best = element;
			best_score = score;
		}
	}
	return best;
}

static int read_element(snd_mixer_elem_t *element,
			struct hardware_audio *audio)
{
	int64_t sum = 0;
	long minimum;
	long maximum;
	unsigned int channels = 0;
	int any_switch = 0;
	int switch_channels = 0;

	if (snd_mixer_selem_get_playback_volume_range(element, &minimum,
						 &maximum) < 0)
		return -1;
	if (maximum <= minimum || minimum < INT_MIN || maximum > INT_MAX)
		return -1;
	for (int channel = 0; channel <= SND_MIXER_SCHN_LAST; ++channel) {
		long value;
		int enabled;

		if (!snd_mixer_selem_has_playback_channel(element,
				(snd_mixer_selem_channel_id_t)channel))
			continue;
		if (snd_mixer_selem_get_playback_volume(element,
				(snd_mixer_selem_channel_id_t)channel, &value) == 0 &&
		    value >= minimum && value <= maximum && channels < 32) {
			sum += value;
			++channels;
		}
		if (snd_mixer_selem_has_playback_switch(element) &&
		    snd_mixer_selem_get_playback_switch(element,
				(snd_mixer_selem_channel_id_t)channel, &enabled) == 0) {
			any_switch |= enabled != 0;
			++switch_channels;
		}
	}
	if (channels == 0)
		return -1;
	audio->volume_percent = (int)((((sum / (int64_t)channels) - minimum) *
		100 + (maximum - minimum) / 2) / (maximum - minimum));
	if (audio->volume_percent < 0 || audio->volume_percent > 100)
		return -1;
	audio->muted = switch_channels > 0 ? !any_switch : -1;
	return 0;
}

static int read_card(const char *card, struct hardware_audio *audio)
{
	snd_mixer_t *mixer = NULL;
	snd_mixer_elem_t *element;
	int result = -1;

	/* SND_CTL_NONBLOCK is passed through to the attached control handle. */
	if (snd_mixer_open(&mixer, SND_CTL_NONBLOCK) < 0)
		return -1;
	if (snd_mixer_attach(mixer, card) == 0 &&
	    snd_mixer_selem_register(mixer, NULL, NULL) == 0 &&
	    snd_mixer_load(mixer) == 0 &&
	    (element = select_element(mixer)) != NULL)
		result = read_element(element, audio);
	(void)snd_mixer_close(mixer);
	return result;
}

int hw_alsa_read(struct hardware_audio *audio)
{
	static const char *const cards[] = {
		/*
		 * Both the vendor and mainline H700 images expose the codec as card 0,
		 * but their symbolic card IDs differ.  Trying the vendor-only
		 * "audiocodec" ID once per monitor refresh makes libasound print an
		 * error every second on mainline (whose ID is "Codec").  The numeric
		 * control name is stable across both kernels and avoids that hot-loop.
		 */
		"hw:0", "default",
	};

	for (size_t index = 0; index < sizeof(cards) / sizeof(cards[0]); ++index)
		if (read_card(cards[index], audio) == 0)
			return 0;
	return -1;
}
