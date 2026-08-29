#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t elapsed_us(uint64_t start, uint64_t end, uint64_t frequency)
{
	uint64_t ticks = end >= start ? end - start : 0U;
	uint64_t seconds = ticks / frequency;
	uint64_t remainder = ticks % frequency;
	uint64_t usec = seconds * UINT64_C(1000000) +
		remainder * UINT64_C(1000000) / frequency;

	return usec > UINT32_MAX ? UINT32_MAX : (uint32_t)usec;
}

int metrics_init(struct ui *ui)
{
	struct frame_metrics *metrics = &ui->metrics;

	memset(metrics, 0, sizeof(*metrics));
	metrics->samples_us = calloc(FRAME_SAMPLE_MAX,
					     sizeof(*metrics->samples_us));
	if (metrics->samples_us == NULL)
		return -1;
	metrics->capacity = FRAME_SAMPLE_MAX;
	metrics->frequency = SDL_GetPerformanceFrequency();
	if (metrics->frequency == 0U) {
		free(metrics->samples_us);
		memset(metrics, 0, sizeof(*metrics));
		return -1;
	}
	return 0;
}

uint64_t metrics_frame_begin(void)
{
	return SDL_GetPerformanceCounter();
}

void metrics_note_input(struct ui *ui)
{
	if (ui->metrics.input_counter == 0U) {
		ui->metrics.input_counter = SDL_GetPerformanceCounter();
		++ui->metrics.input_events;
	}
}

void metrics_frame_end(struct ui *ui, uint64_t frame_counter)
{
	struct frame_metrics *metrics = &ui->metrics;
	uint64_t present_counter = SDL_GetPerformanceCounter();
	uint32_t duration;

	if (metrics->samples_us == NULL || metrics->frequency == 0U)
		return;
	duration = elapsed_us(metrics->last_present != 0U ? metrics->last_present :
			      frame_counter, present_counter, metrics->frequency);
	metrics->last_present = present_counter;
	metrics->samples_us[metrics->cursor] = duration;
	metrics->cursor = (metrics->cursor + 1U) % metrics->capacity;
	if (metrics->count < metrics->capacity)
		++metrics->count;
	++metrics->total_frames;
	if (duration > 16700U)
		++metrics->over_budget;
	if (metrics->input_counter != 0U) {
		uint32_t input_us = elapsed_us(metrics->input_counter, present_counter,
					    metrics->frequency);

		if (input_us > metrics->input_max_us)
			metrics->input_max_us = input_us;
		metrics->input_counter = 0U;
	}
}

static int compare_u32(const void *left, const void *right)
{
	uint32_t a = *(const uint32_t *)left;
	uint32_t b = *(const uint32_t *)right;

	return a > b ? 1 : a < b ? -1 : 0;
}

static uint32_t percentile(const uint32_t *sorted, size_t count,
			   unsigned int percent)
{
	size_t rank = (count * percent + 99U) / 100U;

	if (rank == 0U)
		rank = 1U;
	return sorted[rank - 1U];
}

void metrics_print(const struct ui *ui)
{
	const struct frame_metrics *metrics = &ui->metrics;
	uint32_t *sorted;
	uint32_t maximum;

	if (metrics->count == 0U)
		return;
	sorted = malloc(metrics->count * sizeof(*sorted));
	if (sorted == NULL)
		return;
	memcpy(sorted, metrics->samples_us, metrics->count * sizeof(*sorted));
	qsort(sorted, metrics->count, sizeof(*sorted), compare_u32);
	maximum = sorted[metrics->count - 1U];
	printf("FRAME_METRICS samples=%zu total=%llu p50_ms=%.3f p95_ms=%.3f "
	       "p99_ms=%.3f max_ms=%.3f over_16_7=%llu input_events=%llu "
	       "input_present_max_ms=",
	       metrics->count, (unsigned long long)metrics->total_frames,
	       percentile(sorted, metrics->count, 50U) / 1000.0,
	       percentile(sorted, metrics->count, 95U) / 1000.0,
	       percentile(sorted, metrics->count, 99U) / 1000.0,
	       maximum / 1000.0, (unsigned long long)metrics->over_budget,
	       (unsigned long long)metrics->input_events);
	if (metrics->input_max_us > 0U)
		printf("%.3f\n", metrics->input_max_us / 1000.0);
	else
		printf("n/a\n");
	free(sorted);
}

void metrics_destroy(struct ui *ui)
{
	free(ui->metrics.samples_us);
	memset(&ui->metrics, 0, sizeof(ui->metrics));
}
