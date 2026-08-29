#include "ui.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

enum { CHIME_COUNT = 5, CHIME_MS = 115 };

struct cached_chime {
	double pitch;
	int16_t *samples;
	uint32_t bytes;
};

static struct cached_chime chimes[CHIME_COUNT] = {
	{ 1640.0, NULL, 0 }, { 1710.0, NULL, 0 }, { 1780.0, NULL, 0 },
	{ 1840.0, NULL, 0 }, { 1870.0, NULL, 0 },
};

static void prepare_chime(const SDL_AudioSpec *spec, struct cached_chime *chime)
{
	const double tau = 6.28318530717958647692;
	int frames = spec->freq * CHIME_MS / 1000;
	size_t count = (size_t)frames * spec->channels;

	chime->samples = calloc(count, sizeof(*chime->samples));
	if (chime->samples == NULL)
		return;
	for (int frame = 0; frame < frames; ++frame) {
		double t = (double)frame / spec->freq;
		double attack = fmin(1.0, t * 950.0);
		double envelope = attack * exp(-t * 31.0);
		double tone = sin(tau * chime->pitch * t) * 0.55 +
			sin(tau * chime->pitch * 1.487 * t + 0.31) * 0.28 +
			sin(tau * chime->pitch * 2.319 * t + 0.77) * 0.17;
		int value = (int)lrint(tone * envelope * 5200.0);

		value = value > INT16_MAX ? INT16_MAX :
			value < INT16_MIN ? INT16_MIN : value;
		for (int channel = 0; channel < spec->channels; ++channel)
			chime->samples[(size_t)frame * spec->channels + channel] =
				(int16_t)value;
	}
	chime->bytes = (uint32_t)(count * sizeof(*chime->samples));
}

void audio_init(struct ui *ui)
{
	SDL_AudioSpec wanted;

	ui->audio_device = 0;
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return;
	SDL_zero(wanted);
	wanted.freq = 48000;
	wanted.format = AUDIO_S16SYS;
	wanted.channels = 2;
	wanted.samples = 512;
	ui->audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted,
						&ui->audio_spec, 0);
	if (ui->audio_device != 0) {
		for (size_t i = 0; i < CHIME_COUNT; ++i)
			prepare_chime(&ui->audio_spec, &chimes[i]);
		SDL_PauseAudioDevice(ui->audio_device, 0);
	}
}

void audio_close(struct ui *ui)
{
	if (ui->audio_device != 0)
		SDL_CloseAudioDevice(ui->audio_device);
	for (size_t i = 0; i < CHIME_COUNT; ++i) {
		free(chimes[i].samples);
		chimes[i].samples = NULL;
		chimes[i].bytes = 0;
	}
	ui->audio_device = 0;
}

void audio_play_chime(struct ui *ui, double pitch)
{
	struct cached_chime *selected = &chimes[0];

	if (ui->audio_device == 0 || ui->audio_spec.format != AUDIO_S16SYS ||
	    ui->audio_spec.channels == 0 || ui->audio_spec.freq <= 0)
		return;
	for (size_t i = 1; i < CHIME_COUNT; ++i) {
		if (fabs(chimes[i].pitch - pitch) < fabs(selected->pitch - pitch))
			selected = &chimes[i];
	}
	if (selected->samples == NULL)
		return;
	if (SDL_GetQueuedAudioSize(ui->audio_device) >
	    (uint32_t)(ui->audio_spec.freq * ui->audio_spec.channels * 2 / 8))
		SDL_ClearQueuedAudio(ui->audio_device);
	(void)SDL_QueueAudio(ui->audio_device, selected->samples, selected->bytes);
}
