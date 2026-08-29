#include "ui.h"

#include <string.h>
#include <sys/resource.h>

static int monitor_thread(void *argument)
{
	struct ui *ui = argument;
	struct hardware_snapshot snapshot;
	struct hardware_wifi wifi = { 0 };
	unsigned int cycle = 0;

	(void)setpriority(PRIO_PROCESS, 0, 19);
	while (true) {
		int include_wifi = cycle++ % 3U == 0U;

		(void)hardware_refresh(&ui->hardware_backend, &snapshot, include_wifi);
		if (include_wifi)
			wifi = snapshot.wifi;
		else
			snapshot.wifi = wifi;
		(void)SDL_LockMutex(ui->monitor.mutex);
		if (!ui->monitor.running) {
			(void)SDL_UnlockMutex(ui->monitor.mutex);
			break;
		}
		ui->monitor.latest = snapshot;
		ui->monitor.ready = true;
		(void)SDL_UnlockMutex(ui->monitor.mutex);
		for (int slice = 0; slice < 10; ++slice) {
			bool running;

			SDL_Delay(100U);
			(void)SDL_LockMutex(ui->monitor.mutex);
			running = ui->monitor.running;
			(void)SDL_UnlockMutex(ui->monitor.mutex);
			if (!running)
				return 0;
		}
	}
	return 0;
}

int monitor_start(struct ui *ui)
{
	memset(&ui->monitor, 0, sizeof(ui->monitor));
	ui->monitor.mutex = SDL_CreateMutex();
	if (ui->monitor.mutex == NULL)
		return -1;
	ui->monitor.running = true;
	ui->monitor.thread = SDL_CreateThread(monitor_thread, "hardware-monitor", ui);
	if (ui->monitor.thread == NULL) {
		SDL_DestroyMutex(ui->monitor.mutex);
		memset(&ui->monitor, 0, sizeof(ui->monitor));
		return -1;
	}
	return 0;
}

void monitor_copy_latest(struct ui *ui)
{
	if (ui->monitor.mutex == NULL)
		return;
	(void)SDL_LockMutex(ui->monitor.mutex);
	if (ui->monitor.ready)
		ui->hardware = ui->monitor.latest;
	(void)SDL_UnlockMutex(ui->monitor.mutex);
}

void monitor_stop(struct ui *ui)
{
	if (ui->monitor.mutex != NULL) {
		(void)SDL_LockMutex(ui->monitor.mutex);
		ui->monitor.running = false;
		(void)SDL_UnlockMutex(ui->monitor.mutex);
	}
	if (ui->monitor.thread != NULL)
		SDL_WaitThread(ui->monitor.thread, NULL);
	if (ui->monitor.mutex != NULL)
		SDL_DestroyMutex(ui->monitor.mutex);
	memset(&ui->monitor, 0, sizeof(ui->monitor));
}
