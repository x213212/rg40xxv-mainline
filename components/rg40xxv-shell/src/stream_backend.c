#define _GNU_SOURCE

#include "stream_backend.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
	STREAM_DISCOVERY_TIMEOUT_MS = 1500,
	STREAM_PAIR_TIMEOUT_MS = 120000,
	STREAM_CHILD_TERM_MS = 500,
	STREAM_FIXTURE_MAX_BYTES = 64 * 1024,
	STREAM_PAIR_OUTPUT_MAX = 16 * 1024,
	MDNS_PACKET_MAX = 4096,
	MDNS_RECORD_MAX = 512,
	MDNS_SERVICE_MAX = NS_MAX_HOSTS,
};

struct mdns_service {
	char instance[NS_HOST_ADDRESS_MAX_BYTES + 1];
	char target[NS_HOST_ADDRESS_MAX_BYTES + 1];
};

struct mdns_address {
	char owner[NS_HOST_ADDRESS_MAX_BYTES + 1];
	char address[INET6_ADDRSTRLEN];
	bool ipv4;
};

struct stream_backend {
	SDL_Thread *thread;
	SDL_mutex *mutex;
	SDL_cond *condition;
	struct stream_backend_snapshot published;
	NsHost pair_host;
	NsHost settings_host;
	char state_dir[PATH_MAX];
	char launcher_path[PATH_MAX];
	char fixture_path[PATH_MAX];
	pid_t child_pgid;
	bool running;
	bool discover_requested;
	bool pair_requested;
	bool settings_requested;
};

static uint64_t monotonic_ms(void)
{
	struct timespec value;

	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return 0;
	return (uint64_t)value.tv_sec * 1000U +
		(uint64_t)value.tv_nsec / 1000000U;
}

static void wake_ui(void)
{
	SDL_Event event;

	memset(&event, 0, sizeof(event));
	event.type = SDL_USEREVENT;
	(void)SDL_PushEvent(&event);
}

static void publish(struct stream_backend *backend,
		    enum stream_backend_phase phase, const NsHostDb *hosts,
		    bool store_loaded, const char *pin, const char *detail,
		    size_t discovered_count, uint32_t discovery_ms)
{
	(void)SDL_LockMutex(backend->mutex);
	if (hosts != NULL)
		backend->published.hosts = *hosts;
	backend->published.phase = phase;
	backend->published.store_loaded = store_loaded;
	backend->published.discovered_count = discovered_count;
	backend->published.discovery_ms = discovery_ms;
	(void)snprintf(backend->published.pin,
		       sizeof(backend->published.pin), "%s", pin != NULL ? pin : "");
	(void)snprintf(backend->published.detail,
		       sizeof(backend->published.detail), "%s",
		       detail != NULL ? detail : "");
	++backend->published.generation;
	if (backend->published.generation == 0U)
		backend->published.generation = 1U;
	(void)SDL_UnlockMutex(backend->mutex);
	wake_ui();
}

static bool stopping(struct stream_backend *backend)
{
	bool result;

	(void)SDL_LockMutex(backend->mutex);
	result = !backend->running;
	(void)SDL_UnlockMutex(backend->mutex);
	return result;
}

static int load_hosts(const struct stream_backend *backend, NsHostDb *hosts,
		      char *error, size_t error_size)
{
	NsStore store = { .dir_fd = -1, .lock_fd = -1 };
	int result;

	if (ns_store_open(&store, backend->state_dir, error, error_size) != 0)
		return -1;
	result = ns_hosts_load(&store, hosts, error, error_size);
	ns_store_close(&store);
	return result;
}

static int save_hosts(const struct stream_backend *backend,
		      const NsHostDb *hosts, char *error, size_t error_size)
{
	NsStore store = { .dir_fd = -1, .lock_fd = -1 };
	int result;

	if (ns_store_open(&store, backend->state_dir, error, error_size) != 0)
		return -1;
	result = ns_hosts_save(&store, hosts, error, error_size);
	ns_store_close(&store);
	return result;
}

static int host_find_address(const NsHostDb *hosts, const char *address)
{
	for (size_t i = 0; i < hosts->count; ++i) {
		if (strcmp(hosts->hosts[i].address, address) == 0)
			return (int)i;
	}
	return -1;
}

static int merge_discovered(NsHostDb *hosts, const NsHostDb *found,
			    size_t *changed, char *error, size_t error_size)
{
	*changed = 0U;
	for (size_t i = 0; i < found->count; ++i) {
		const NsHost *candidate = &found->hosts[i];
		int index = host_find_address(hosts, candidate->address);

		if (index < 0)
			index = ns_host_find(hosts, candidate->name);
		if (index >= 0) {
			NsHost *existing = &hosts->hosts[index];

			if (strcmp(existing->address, candidate->address) != 0) {
				(void)snprintf(existing->address,
					       sizeof(existing->address), "%s",
					       candidate->address);
				++*changed;
			}
			continue;
		}
		if (ns_host_upsert(hosts, candidate, error, error_size) != 0)
			return -1;
		++*changed;
	}
	if (hosts->default_name[0] == '\0' && hosts->count > 0U)
		(void)snprintf(hosts->default_name, sizeof(hosts->default_name),
			       "%s", hosts->hosts[0].name);
	return 0;
}

static int parse_delay_ms(void)
{
	const char *value = getenv("RG40XXV_STREAM_FIXTURE_DELAY_MS");
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0')
		return 0;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed > 10000U)
		return 0;
	return (int)parsed;
}

static int fixture_delay(struct stream_backend *backend)
{
	int remaining = parse_delay_ms();

	while (remaining > 0) {
		int slice = remaining > 50 ? 50 : remaining;
		struct timespec delay = {
			.tv_sec = 0,
			.tv_nsec = (long)slice * 1000000L,
		};

		if (stopping(backend))
			return -1;
		(void)nanosleep(&delay, NULL);
		remaining -= slice;
	}
	return 0;
}

static int fixture_discover(struct stream_backend *backend, NsHostDb *found,
			    char *error, size_t error_size)
{
	struct stat info;
	char *line = NULL;
	size_t capacity = 0U;
	ssize_t length;
	FILE *input;
	int fd;
	bool header = false;
	int result = -1;

	fd = open(backend->fixture_path,
		  O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		(void)snprintf(error, error_size, "fixture open: %s",
			       strerror(errno));
		return -1;
	}
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
	    info.st_uid != geteuid() || (info.st_mode & 077) != 0 ||
	    info.st_size < 1 || info.st_size > STREAM_FIXTURE_MAX_BYTES) {
		(void)snprintf(error, error_size,
			       "fixture must be owner-only regular file <=64KiB");
		(void)close(fd);
		return -1;
	}
	input = fdopen(fd, "r");
	if (input == NULL) {
		(void)snprintf(error, error_size, "fixture fdopen: %s",
			       strerror(errno));
		(void)close(fd);
		return -1;
	}
	memset(found, 0, sizeof(*found));
	while ((length = getline(&line, &capacity, input)) >= 0) {
		char *name;
		char *address;
		char *tab;
		NsHost host;

		if (length == 0 || line[length - 1] != '\n' ||
		    memchr(line, '\0', (size_t)length) != NULL) {
			(void)snprintf(error, error_size, "fixture truncated");
			goto out;
		}
		line[--length] = '\0';
		if (!header) {
			header = strcmp(line, "RG40XXV_STREAM_DISCOVERY\t1") == 0;
			if (!header) {
				(void)snprintf(error, error_size,
					       "fixture header is invalid");
				goto out;
			}
			continue;
		}
		if (strncmp(line, "H\t", 2) != 0) {
			(void)snprintf(error, error_size, "fixture row is invalid");
			goto out;
		}
		name = line + 2;
		tab = strchr(name, '\t');
		if (tab == NULL || strchr(tab + 1, '\t') != NULL) {
			(void)snprintf(error, error_size, "fixture fields are invalid");
			goto out;
		}
		*tab = '\0';
		address = tab + 1;
		ns_host_defaults(&host);
		if (snprintf(host.name, sizeof(host.name), "%s", name) < 0 ||
		    snprintf(host.address, sizeof(host.address), "%s", address) < 0 ||
		    ns_host_upsert(found, &host, error, error_size) != 0)
			goto out;
	}
	if (ferror(input) != 0 || !header) {
		(void)snprintf(error, error_size, "fixture read failed");
		goto out;
	}
	if (fixture_delay(backend) != 0) {
		(void)snprintf(error, error_size, "discovery cancelled");
		goto out;
	}
	result = 0;
out:
	free(line);
	(void)fclose(input);
	return result;
}

static uint16_t dns_u16(const unsigned char *value)
{
	return (uint16_t)(((uint16_t)value[0] << 8) | value[1]);
}

static int dns_name(const unsigned char *packet, size_t packet_size,
		    size_t offset, char *output, size_t output_size,
		    size_t *consumed)
{
	size_t position = offset;
	size_t output_length = 0U;
	size_t original_end = 0U;
	unsigned int steps = 0U;
	bool jumped = false;

	if (output_size == 0U || offset >= packet_size)
		return -1;
	while (steps++ < 128U) {
		unsigned int label;

		if (position >= packet_size)
			return -1;
		label = packet[position++];
		if ((label & 0xc0U) == 0xc0U) {
			size_t pointer;

			if (position >= packet_size)
				return -1;
			pointer = ((size_t)(label & 0x3fU) << 8) |
				 packet[position++];
			if (pointer >= packet_size)
				return -1;
			if (!jumped)
				original_end = position;
			jumped = true;
			position = pointer;
			continue;
		}
		if ((label & 0xc0U) != 0U || label > 63U ||
		    position + label > packet_size)
			return -1;
		if (label == 0U) {
			if (!jumped)
				original_end = position;
			break;
		}
		if (output_length != 0U) {
			if (output_length + 1U >= output_size)
				return -1;
			output[output_length++] = '.';
		}
		if (output_length + label >= output_size)
			return -1;
		for (unsigned int i = 0U; i < label; ++i) {
			unsigned char c = packet[position + i];

			if (c == 0U || c < 0x20U || c == 0x7fU)
				return -1;
			output[output_length++] = (char)c;
		}
		position += label;
	}
	if (steps >= 128U || original_end == 0U)
		return -1;
	output[output_length] = '\0';
	*consumed = original_end - offset;
	return 0;
}

static bool service_name(const char *full, char *display, size_t display_size)
{
	static const char suffix[] = "._nvstream._tcp.local";
	size_t full_length = strlen(full);
	size_t suffix_length = sizeof(suffix) - 1U;
	size_t name_length;

	if (full_length <= suffix_length ||
	    strcasecmp(full + full_length - suffix_length, suffix) != 0)
		return false;
	name_length = full_length - suffix_length;
	if (name_length == 0U || name_length >= display_size)
		return false;
	memcpy(display, full, name_length);
	display[name_length] = '\0';
	return ns_validate_utf8_text(display, 1U,
				     NS_HOST_NAME_MAX_BYTES) != 0;
}

static struct mdns_service *mdns_service_get(struct mdns_service *services,
					     size_t *count,
					     const char *instance)
{
	for (size_t i = 0U; i < *count; ++i) {
		if (strcasecmp(services[i].instance, instance) == 0)
			return &services[i];
	}
	if (*count >= MDNS_SERVICE_MAX)
		return NULL;
	(void)snprintf(services[*count].instance,
		       sizeof(services[*count].instance), "%s", instance);
	return &services[(*count)++];
}

static void mdns_address_add(struct mdns_address *addresses, size_t *count,
			     const char *owner, const char *address, bool ipv4)
{
	for (size_t i = 0U; i < *count; ++i) {
		if (strcasecmp(addresses[i].owner, owner) == 0 &&
		    (addresses[i].ipv4 || !ipv4)) {
			if (ipv4) {
				(void)snprintf(addresses[i].address,
					       sizeof(addresses[i].address), "%s", address);
				addresses[i].ipv4 = true;
			}
			return;
		}
	}
	if (*count >= MDNS_SERVICE_MAX)
		return;
	(void)snprintf(addresses[*count].owner,
		       sizeof(addresses[*count].owner), "%s", owner);
	(void)snprintf(addresses[*count].address,
		       sizeof(addresses[*count].address), "%s", address);
	addresses[*count].ipv4 = ipv4;
	++*count;
}

static int mdns_parse_packet(const unsigned char *packet, size_t packet_size,
			     struct mdns_service *services,
			     size_t *service_count,
			     struct mdns_address *addresses,
			     size_t *address_count)
{
	size_t offset = 12U;
	unsigned int questions;
	unsigned int records;

	if (packet_size < 12U)
		return -1;
	questions = dns_u16(packet + 4U);
	records = (unsigned int)dns_u16(packet + 6U) +
		(unsigned int)dns_u16(packet + 8U) +
		(unsigned int)dns_u16(packet + 10U);
	if (questions > MDNS_RECORD_MAX || records > MDNS_RECORD_MAX)
		return -1;
	for (unsigned int i = 0U; i < questions; ++i) {
		char ignored[NS_HOST_ADDRESS_MAX_BYTES + 1];
		size_t consumed;

		if (dns_name(packet, packet_size, offset, ignored,
			     sizeof(ignored), &consumed) != 0)
			return -1;
		offset += consumed;
		if (offset + 4U > packet_size)
			return -1;
		offset += 4U;
	}
	for (unsigned int i = 0U; i < records; ++i) {
		char owner[NS_HOST_ADDRESS_MAX_BYTES + 1];
		size_t consumed;
		uint16_t type;
		uint16_t data_length;
		size_t data;

		if (dns_name(packet, packet_size, offset, owner,
			     sizeof(owner), &consumed) != 0)
			return -1;
		offset += consumed;
		if (offset + 10U > packet_size)
			return -1;
		type = dns_u16(packet + offset);
		data_length = dns_u16(packet + offset + 8U);
		data = offset + 10U;
		if (data + data_length > packet_size)
			return -1;
		if (type == 12U) {
			char target[NS_HOST_ADDRESS_MAX_BYTES + 1];

			if (dns_name(packet, packet_size, data, target,
				     sizeof(target), &consumed) == 0) {
				char display[NS_HOST_NAME_MAX_BYTES + 1];

				if (service_name(target, display, sizeof(display)))
					(void)mdns_service_get(services,
							       service_count, target);
			}
		} else if (type == 33U && data_length >= 7U) {
			char target[NS_HOST_ADDRESS_MAX_BYTES + 1];
			char display[NS_HOST_NAME_MAX_BYTES + 1];

			if (service_name(owner, display, sizeof(display)) &&
			    dns_name(packet, packet_size, data + 6U, target,
				     sizeof(target), &consumed) == 0) {
				struct mdns_service *service = mdns_service_get(
					services, service_count, owner);

				if (service != NULL)
					(void)snprintf(service->target,
						       sizeof(service->target), "%s", target);
			}
		} else if (type == 1U && data_length == 4U) {
			char address[INET_ADDRSTRLEN];

			if (inet_ntop(AF_INET, packet + data, address,
				      sizeof(address)) != NULL)
				mdns_address_add(addresses, address_count, owner,
						 address, true);
		} else if (type == 28U && data_length == 16U) {
			char address[INET6_ADDRSTRLEN];

			if (inet_ntop(AF_INET6, packet + data, address,
				      sizeof(address)) != NULL)
				mdns_address_add(addresses, address_count, owner,
						 address, false);
		}
		offset = data + data_length;
	}
	return 0;
}

static int mdns_results(const struct mdns_service *services,
			const size_t service_count,
			const struct mdns_address *addresses,
			const size_t address_count, NsHostDb *found,
			char *error, size_t error_size)
{
	memset(found, 0, sizeof(*found));
	for (size_t i = 0U; i < service_count; ++i) {
		char display[NS_HOST_NAME_MAX_BYTES + 1];
		const char *address = NULL;
		NsHost host;

		if (!service_name(services[i].instance, display, sizeof(display)) ||
		    services[i].target[0] == '\0')
			continue;
		for (size_t j = 0U; j < address_count; ++j) {
			if (strcasecmp(addresses[j].owner,
				       services[i].target) == 0 &&
			    (address == NULL || addresses[j].ipv4)) {
				address = addresses[j].address;
				if (addresses[j].ipv4)
					break;
			}
		}
		if (address == NULL)
			address = services[i].target;
		ns_host_defaults(&host);
		(void)snprintf(host.name, sizeof(host.name), "%s", display);
		(void)snprintf(host.address, sizeof(host.address), "%s", address);
		if (ns_host_upsert(found, &host, error, error_size) != 0)
			continue;
	}
	return 0;
}

#ifdef RG40XXV_STREAM_BACKEND_TESTING
int stream_backend_test_parse_mdns(const unsigned char *packet,
				   size_t packet_size, NsHostDb *hosts)
{
	struct mdns_service services[MDNS_SERVICE_MAX] = { 0 };
	struct mdns_address addresses[MDNS_SERVICE_MAX] = { 0 };
	size_t service_count = 0U;
	size_t address_count = 0U;
	char error[NS_ERROR_MAX] = { 0 };

	if (packet == NULL || hosts == NULL ||
	    mdns_parse_packet(packet, packet_size, services, &service_count,
			      addresses, &address_count) != 0)
		return -1;
	return mdns_results(services, service_count, addresses, address_count,
			    hosts, error, sizeof(error));
}
#endif

static int mdns_discover(struct stream_backend *backend, NsHostDb *found,
			 char *error, size_t error_size)
{
	static const unsigned char query[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x09, '_', 'n', 'v', 's', 't', 'r', 'e', 'a', 'm',
		0x04, '_', 't', 'c', 'p',
		0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
		0x00, 0x0c, 0x00, 0x01,
	};
	struct mdns_service services[MDNS_SERVICE_MAX] = { 0 };
	struct mdns_address addresses[MDNS_SERVICE_MAX] = { 0 };
	struct sockaddr_in bind_address;
	struct sockaddr_in multicast;
	struct ip_mreqn membership;
	struct ifaddrs *interfaces = NULL;
	struct in_addr interface_address = { 0 };
	unsigned int interface_index = 0U;
	char interface_name[IF_NAMESIZE] = { 0 };
	size_t service_count = 0U;
	size_t address_count = 0U;
	uint64_t deadline;
	int reuse = 1;
	int fd;

	if (getifaddrs(&interfaces) != 0) {
		(void)snprintf(error, error_size, "mDNS interfaces: %s",
			       strerror(errno));
		return -1;
	}
	for (int preferred = 1; preferred >= 0 && interface_index == 0U;
	     --preferred) {
		for (const struct ifaddrs *item = interfaces; item != NULL;
		     item = item->ifa_next) {
			const struct sockaddr_in *address;
			unsigned int index;

			if (item->ifa_addr == NULL || item->ifa_name == NULL ||
			    item->ifa_addr->sa_family != AF_INET ||
			    (item->ifa_flags & (IFF_UP | IFF_MULTICAST)) !=
				(IFF_UP | IFF_MULTICAST) ||
			    (item->ifa_flags & IFF_LOOPBACK) != 0 ||
			    (preferred != 0 && strncmp(item->ifa_name, "wlan", 4) != 0))
				continue;
			index = if_nametoindex(item->ifa_name);
			if (index == 0U)
				continue;
			address = (const struct sockaddr_in *)item->ifa_addr;
			if (address->sin_addr.s_addr == htonl(INADDR_ANY))
				continue;
			interface_index = index;
			interface_address = address->sin_addr;
			(void)snprintf(interface_name, sizeof(interface_name), "%s",
				       item->ifa_name);
			break;
		}
	}
	freeifaddrs(interfaces);
	if (interface_index == 0U) {
		errno = ENODEV;
		(void)snprintf(error, error_size,
			       "mDNS interface: no active IPv4 multicast link");
		return -1;
	}

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		(void)snprintf(error, error_size, "mDNS socket: %s", strerror(errno));
		return -1;
	}
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	memset(&bind_address, 0, sizeof(bind_address));
	bind_address.sin_family = AF_INET;
	bind_address.sin_port = htons(5353U);
	bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, (struct sockaddr *)&bind_address,
		 sizeof(bind_address)) != 0) {
		(void)snprintf(error, error_size, "mDNS bind: %s", strerror(errno));
		(void)close(fd);
		return -1;
	}
	memset(&membership, 0, sizeof(membership));
	membership.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
	membership.imr_address = interface_address;
	membership.imr_ifindex = (int)interface_index;
	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &membership,
		       sizeof(membership)) != 0) {
		(void)snprintf(error, error_size, "mDNS interface %s: %s",
			       interface_name, strerror(errno));
		(void)close(fd);
		return -1;
	}
	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
		       sizeof(membership)) != 0) {
		(void)snprintf(error, error_size, "mDNS join %s: %s",
			       interface_name, strerror(errno));
		(void)close(fd);
		return -1;
	}
	memset(&multicast, 0, sizeof(multicast));
	multicast.sin_family = AF_INET;
	multicast.sin_port = htons(5353U);
	multicast.sin_addr.s_addr = inet_addr("224.0.0.251");
	if (sendto(fd, query, sizeof(query), 0,
		   (struct sockaddr *)&multicast, sizeof(multicast)) !=
	    (ssize_t)sizeof(query)) {
		(void)snprintf(error, error_size, "mDNS query: %s", strerror(errno));
		(void)close(fd);
		return -1;
	}
	deadline = monotonic_ms() + STREAM_DISCOVERY_TIMEOUT_MS;
	while (!stopping(backend) && monotonic_ms() < deadline) {
		struct pollfd descriptor = { .fd = fd, .events = POLLIN };
		unsigned char packet[MDNS_PACKET_MAX];
		int wait_ms = (int)(deadline - monotonic_ms());
		int ready;

		if (wait_ms > 100)
			wait_ms = 100;
		if (wait_ms < 0)
			wait_ms = 0;
		ready = poll(&descriptor, 1, wait_ms);
		if (ready < 0 && errno != EINTR) {
			(void)snprintf(error, error_size, "mDNS poll: %s",
				       strerror(errno));
			(void)close(fd);
			return -1;
		}
		if (ready <= 0 || (descriptor.revents & POLLIN) == 0)
			continue;
		for (;;) {
			ssize_t count = recv(fd, packet, sizeof(packet), 0);

			if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (count < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			(void)mdns_parse_packet(packet, (size_t)count,
				services, &service_count, addresses, &address_count);
		}
	}
	(void)close(fd);
	if (stopping(backend)) {
		(void)snprintf(error, error_size, "discovery cancelled");
		return -1;
	}
	return mdns_results(services, service_count, addresses, address_count,
			    found, error, error_size);
}

static void run_discovery(struct stream_backend *backend)
{
	char error[NS_ERROR_MAX] = { 0 };
	NsHostDb hosts;
	NsHostDb found;
	uint64_t started = monotonic_ms();
	size_t changed = 0U;
	int result;

	if (load_hosts(backend, &hosts, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, NULL, false, NULL, error,
			0U, 0U);
		return;
	}
	/* Make the persistent snapshot visible before any network wait. */
	publish(backend, STREAM_BACKEND_DISCOVERING, &hosts, true, NULL,
		"scanning", 0U, 0U);
	if (backend->fixture_path[0] != '\0')
		result = fixture_discover(backend, &found, error, sizeof(error));
	else
		result = mdns_discover(backend, &found, error, sizeof(error));
	if (stopping(backend))
		return;
	if (result != 0) {
		publish(backend, STREAM_BACKEND_PENDING, &hosts, true, NULL,
			error, 0U, (uint32_t)(monotonic_ms() - started));
		return;
	}
	if (merge_discovered(&hosts, &found, &changed, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, &hosts, true, NULL, error,
			found.count, (uint32_t)(monotonic_ms() - started));
		return;
	}
	if (changed > 0U && save_hosts(backend, &hosts, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, &hosts, true, NULL, error,
			found.count, (uint32_t)(monotonic_ms() - started));
		return;
	}
	publish(backend, STREAM_BACKEND_READY, &hosts, true, NULL,
		found.count == 0U ? "scan-complete" : "hosts-found",
		found.count, (uint32_t)(monotonic_ms() - started));
}

static bool executable_safe(const char *path)
{
	struct stat info;

	return path != NULL && path[0] == '/' && lstat(path, &info) == 0 &&
		S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode) &&
		info.st_uid == geteuid() && (info.st_mode & 022) == 0 &&
		(info.st_mode & S_IXUSR) != 0;
}

static bool pin_from_output(const char *output, char pin[5])
{
	size_t length = strlen(output);

	for (size_t i = 0U; i + 3U < length; ++i) {
		if (i + 3U <= length && strncasecmp(output + i, "pin", 3U) == 0) {
			size_t end = i + 96U < length ? i + 96U : length;

			for (size_t j = i + 3U; j + 3U < end; ++j) {
				bool left = j == 0U || !isdigit((unsigned char)output[j - 1U]);
				bool right = j + 4U >= length ||
					!isdigit((unsigned char)output[j + 4U]);

				if (left && right &&
				    isdigit((unsigned char)output[j]) &&
				    isdigit((unsigned char)output[j + 1U]) &&
				    isdigit((unsigned char)output[j + 2U]) &&
				    isdigit((unsigned char)output[j + 3U])) {
					memcpy(pin, output + j, 4U);
					pin[4] = '\0';
					return true;
				}
			}
		}
	}
	return false;
}

static void set_child_pgid(struct stream_backend *backend, pid_t pgid)
{
	(void)SDL_LockMutex(backend->mutex);
	backend->child_pgid = pgid;
	(void)SDL_UnlockMutex(backend->mutex);
}

static bool process_group_alive(pid_t pgid)
{
	if (pgid <= 1)
		return false;
	if (kill(-pgid, 0) == 0)
		return true;
	return errno == EPERM;
}

static void terminate_pair(pid_t pid)
{
	uint64_t deadline = monotonic_ms() + STREAM_CHILD_TERM_MS;
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
	int status;

	(void)kill(-pid, SIGTERM);
	while (monotonic_ms() < deadline) {
		pid_t result = waitpid(pid, &status, WNOHANG);

		if (result == pid || (result < 0 && errno == ECHILD))
			break;
		(void)nanosleep(&delay, NULL);
	}
	if (process_group_alive(pid))
		(void)kill(-pid, SIGKILL);
	do {
		if (waitpid(pid, &status, 0) >= 0 || errno == ECHILD)
			break;
	} while (errno == EINTR);
}

static int generate_pin(char pin[5], char *error, size_t error_size)
{
	uint32_t random_value;
	unsigned char *cursor = (unsigned char *)&random_value;
	size_t remaining = sizeof(random_value);
	int fd = -1;

	while (remaining > 0U) {
		ssize_t count = getrandom(cursor, remaining, 0);

		if (count > 0) {
			cursor += count;
			remaining -= (size_t)count;
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		break;
	}
	if (remaining > 0U) {
		fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0) {
			(void)snprintf(error, error_size, "PIN random source unavailable");
			return -1;
		}
		while (remaining > 0U) {
			ssize_t count = read(fd, cursor, remaining);

			if (count > 0) {
				cursor += count;
				remaining -= (size_t)count;
				continue;
			}
			if (count < 0 && errno == EINTR)
				continue;
			(void)close(fd);
			(void)snprintf(error, error_size, "PIN random read failed");
			return -1;
		}
		(void)close(fd);
	}
	(void)snprintf(pin, 5U, "%04u", random_value % 9999U + 1U);
	return 0;
}

static int pair_process(struct stream_backend *backend, const NsHost *host,
			const char pin[5], char *error, size_t error_size)
{
	char output[STREAM_PAIR_OUTPUT_MAX + 1U] = { 0 };
	size_t output_length = 0U;
	uint64_t deadline = monotonic_ms() + STREAM_PAIR_TIMEOUT_MS;
	int descriptors[2];
	pid_t child;
	int status = 0;
	bool finished = false;

	if (!executable_safe(backend->launcher_path)) {
		(void)snprintf(error, error_size, "pair runner unavailable");
		return -1;
	}
	if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0) {
		(void)snprintf(error, error_size, "pair pipe: %s", strerror(errno));
		return -1;
	}
	child = fork();
	if (child < 0) {
		(void)snprintf(error, error_size, "pair fork: %s", strerror(errno));
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		return -1;
	}
	if (child == 0) {
		char *arguments[] = {
			backend->launcher_path, (char *)"pair", (char *)host->address,
			(char *)pin, NULL,
		};

		(void)setpgid(0, 0);
		(void)dup2(descriptors[1], STDOUT_FILENO);
		(void)dup2(descriptors[1], STDERR_FILENO);
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);
		execv(backend->launcher_path, arguments);
		_exit(127);
	}
	(void)close(descriptors[1]);
	(void)setpgid(child, child);
	set_child_pgid(backend, child);
	while (!finished && !stopping(backend) && monotonic_ms() < deadline) {
		struct pollfd descriptor = {
			.fd = descriptors[0],
			.events = POLLIN | POLLHUP | POLLERR,
		};
		pid_t waited;

		(void)poll(&descriptor, 1, 100);
		for (;;) {
			char chunk[512];
			ssize_t count = read(descriptors[0], chunk, sizeof(chunk));

			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				break;
			if (output_length < STREAM_PAIR_OUTPUT_MAX) {
				size_t room = STREAM_PAIR_OUTPUT_MAX - output_length;
				size_t copy = (size_t)count < room ? (size_t)count : room;

				memcpy(output + output_length, chunk, copy);
				output_length += copy;
				output[output_length] = '\0';
			}
		}
		do {
			waited = waitpid(child, &status, WNOHANG);
		} while (waited < 0 && errno == EINTR);
		finished = waited == child || (waited < 0 && errno == ECHILD);
	}
	if (finished) {
		for (;;) {
			char chunk[512];
			ssize_t count = read(descriptors[0], chunk, sizeof(chunk));

			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				break;
			if (output_length < STREAM_PAIR_OUTPUT_MAX) {
				size_t room = STREAM_PAIR_OUTPUT_MAX - output_length;
				size_t copy = (size_t)count < room ? (size_t)count : room;

				memcpy(output + output_length, chunk, copy);
				output_length += copy;
				output[output_length] = '\0';
			}
		}
	}
	(void)close(descriptors[0]);
	if (!finished) {
		bool cancelled = stopping(backend);

		terminate_pair(child);
		set_child_pgid(backend, 0);
		(void)snprintf(error, error_size, "%s",
			       cancelled ? "pairing cancelled" : "pairing timed out");
		return -1;
	}
	set_child_pgid(backend, 0);
	if (process_group_alive(child))
		terminate_pair(child);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		(void)snprintf(error, error_size,
			       WIFSIGNALED(status) ? "pair runner signal %d" :
			       "pair runner exit %d",
			       WIFSIGNALED(status) ? WTERMSIG(status) :
			       WEXITSTATUS(status));
		return -1;
	}
	{
		char observed_pin[5] = { 0 };

		if (!pin_from_output(output, observed_pin) ||
		    strcmp(observed_pin, pin) != 0 ||
		    strstr(output, "Succesfully paired") == NULL) {
			(void)snprintf(error, error_size,
				       "pair protocol did not confirm success");
			return -1;
		}
	}
	return 0;
}

static void run_pair(struct stream_backend *backend, const NsHost *requested)
{
	char error[NS_ERROR_MAX] = { 0 };
	char pin[5] = { 0 };
	NsHostDb hosts;
	int index;

	if (load_hosts(backend, &hosts, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, NULL, false, NULL, error,
			0U, 0U);
		return;
	}
	if (generate_pin(pin, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, &hosts, true, NULL, error,
			0U, 0U);
		return;
	}
	publish(backend, STREAM_BACKEND_PAIRING, &hosts, true, pin,
		"enter-pin", 0U, 0U);
	if (pair_process(backend, requested, pin, error, sizeof(error)) != 0) {
		if (!stopping(backend))
			publish(backend, STREAM_BACKEND_ERROR, &hosts, true, pin,
				error, 0U, 0U);
		return;
	}
	if (load_hosts(backend, &hosts, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, NULL, false, pin, error,
			0U, 0U);
		return;
	}
	index = host_find_address(&hosts, requested->address);
	if (index < 0)
		index = ns_host_find(&hosts, requested->name);
	if (index < 0) {
		if (ns_host_upsert(&hosts, requested, error, sizeof(error)) != 0) {
			publish(backend, STREAM_BACKEND_ERROR, &hosts, true, pin,
				error, 0U, 0U);
			return;
		}
		index = ns_host_find(&hosts, requested->name);
	}
	if (index < 0) {
		publish(backend, STREAM_BACKEND_ERROR, &hosts, true, pin,
			"paired host disappeared", 0U, 0U);
		return;
	}
	hosts.hosts[index].paired = 1;
	(void)snprintf(hosts.default_name, sizeof(hosts.default_name), "%s",
		       hosts.hosts[index].name);
	if (save_hosts(backend, &hosts, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, &hosts, true, pin, error,
			0U, 0U);
		return;
	}
	publish(backend, STREAM_BACKEND_PAIRED, &hosts, true, pin,
		"pair-complete", 0U, 0U);
}

static void run_settings_save(struct stream_backend *backend,
			      const NsHost *requested)
{
	char error[NS_ERROR_MAX] = { 0 };
	NsHostDb hosts;
	NsHostDb confirmed;
	int index;

	if (load_hosts(backend, &hosts, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, NULL, false, NULL, error,
			0U, 0U);
		return;
	}
	index = host_find_address(&hosts, requested->address);
	if (index < 0)
		index = ns_host_find(&hosts, requested->name);
	if (index < 0) {
		publish(backend, STREAM_BACKEND_ERROR, &hosts, true, NULL,
			"settings host disappeared", 0U, 0U);
		return;
	}
	confirmed = hosts;
	hosts.hosts[index].resolution = requested->resolution;
	hosts.hosts[index].fps = requested->fps;
	hosts.hosts[index].bitrate_kbps = requested->bitrate_kbps;
	hosts.hosts[index].packet_size = requested->packet_size;
	hosts.hosts[index].codec = requested->codec;
	hosts.hosts[index].aspect = requested->aspect;
	publish(backend, STREAM_BACKEND_SAVING, &hosts, true, NULL, "saving",
		0U, 0U);
	if (save_hosts(backend, &hosts, error, sizeof(error)) != 0) {
		publish(backend, STREAM_BACKEND_ERROR, &confirmed, true, NULL, error,
			0U, 0U);
		return;
	}
	publish(backend, STREAM_BACKEND_READY, &hosts, true, NULL,
		"settings-saved", 0U, 0U);
}

static int worker_main(void *context)
{
	struct stream_backend *backend = context;

	for (;;) {
		enum { WORK_NONE, WORK_PAIR, WORK_SETTINGS, WORK_DISCOVER } work =
			WORK_NONE;
		NsHost host;

		memset(&host, 0, sizeof(host));
		(void)SDL_LockMutex(backend->mutex);
		while (backend->running && !backend->pair_requested &&
		       !backend->settings_requested && !backend->discover_requested)
			(void)SDL_CondWait(backend->condition, backend->mutex);
		if (!backend->running) {
			(void)SDL_UnlockMutex(backend->mutex);
			break;
		}
		if (backend->pair_requested) {
			work = WORK_PAIR;
			host = backend->pair_host;
			backend->pair_requested = false;
		} else if (backend->settings_requested) {
			work = WORK_SETTINGS;
			host = backend->settings_host;
			backend->settings_requested = false;
		} else if (backend->discover_requested) {
			work = WORK_DISCOVER;
			backend->discover_requested = false;
		}
		(void)SDL_UnlockMutex(backend->mutex);
		if (work == WORK_PAIR)
			run_pair(backend, &host);
		else if (work == WORK_SETTINGS)
			run_settings_save(backend, &host);
		else if (work == WORK_DISCOVER)
			run_discovery(backend);
	}
	return 0;
}

int stream_backend_start(struct stream_backend **output,
			 const char *state_dir, const char *launcher_path)
{
	const char *fixture = getenv("RG40XXV_STREAM_DISCOVERY_FIXTURE");
	struct stream_backend *backend;

	if (output == NULL || state_dir == NULL || state_dir[0] != '/' ||
	    launcher_path == NULL || launcher_path[0] != '/')
		return EINVAL;
	*output = NULL;
	backend = calloc(1U, sizeof(*backend));
	if (backend == NULL)
		return ENOMEM;
	if (snprintf(backend->state_dir, sizeof(backend->state_dir), "%s",
		     state_dir) < 0 ||
	    strlen(state_dir) >= sizeof(backend->state_dir) ||
	    snprintf(backend->launcher_path, sizeof(backend->launcher_path), "%s",
		     launcher_path) < 0 ||
	    strlen(launcher_path) >= sizeof(backend->launcher_path) ||
	    (fixture != NULL && (fixture[0] != '/' ||
		strlen(fixture) >= sizeof(backend->fixture_path)))) {
		free(backend);
		return ENAMETOOLONG;
	}
	if (fixture != NULL)
		(void)snprintf(backend->fixture_path,
		       sizeof(backend->fixture_path), "%s", fixture);
	backend->mutex = SDL_CreateMutex();
	backend->condition = SDL_CreateCond();
	if (backend->mutex == NULL || backend->condition == NULL) {
		stream_backend_stop(&backend);
		return ENOMEM;
	}
	backend->running = true;
	backend->published.phase = STREAM_BACKEND_LOADING;
	backend->published.generation = 1U;
	backend->thread = SDL_CreateThread(worker_main, "stream-backend", backend);
	if (backend->thread == NULL) {
		stream_backend_stop(&backend);
		return EAGAIN;
	}
	*output = backend;
	return 0;
}

int stream_backend_request_discovery(struct stream_backend *backend)
{
	if (backend == NULL)
		return EINVAL;
	(void)SDL_LockMutex(backend->mutex);
	if (!backend->running) {
		(void)SDL_UnlockMutex(backend->mutex);
		return ESHUTDOWN;
	}
	backend->discover_requested = true;
	(void)SDL_CondSignal(backend->condition);
	(void)SDL_UnlockMutex(backend->mutex);
	return 0;
}

int stream_backend_request_pair(struct stream_backend *backend,
				const NsHost *host)
{
	char error[NS_ERROR_MAX];

	if (backend == NULL || host == NULL ||
	    ns_validate_host(host, error, sizeof(error)) != 0)
		return EINVAL;
	(void)SDL_LockMutex(backend->mutex);
	if (!backend->running) {
		(void)SDL_UnlockMutex(backend->mutex);
		return ESHUTDOWN;
	}
	if (backend->pair_requested || backend->child_pgid > 0) {
		(void)SDL_UnlockMutex(backend->mutex);
		return EBUSY;
	}
	backend->pair_host = *host;
	backend->pair_requested = true;
	(void)SDL_CondSignal(backend->condition);
	(void)SDL_UnlockMutex(backend->mutex);
	return 0;
}

int stream_backend_request_settings_save(struct stream_backend *backend,
					 const NsHost *host)
{
	char error[NS_ERROR_MAX];

	if (backend == NULL || host == NULL ||
	    ns_validate_host(host, error, sizeof(error)) != 0)
		return EINVAL;
	(void)SDL_LockMutex(backend->mutex);
	if (!backend->running) {
		(void)SDL_UnlockMutex(backend->mutex);
		return ESHUTDOWN;
	}
	backend->settings_host = *host;
	backend->settings_requested = true;
	(void)SDL_CondSignal(backend->condition);
	(void)SDL_UnlockMutex(backend->mutex);
	return 0;
}

int stream_backend_poll(struct stream_backend *backend,
			struct stream_backend_snapshot *snapshot,
			uint64_t *last_generation)
{
	int result = 0;

	if (backend == NULL || snapshot == NULL || last_generation == NULL)
		return -EINVAL;
	(void)SDL_LockMutex(backend->mutex);
	if (backend->published.generation != *last_generation) {
		*snapshot = backend->published;
		*last_generation = backend->published.generation;
		result = 1;
	}
	(void)SDL_UnlockMutex(backend->mutex);
	return result;
}

void stream_backend_stop(struct stream_backend **pointer)
{
	struct stream_backend *backend;
	pid_t child = 0;

	if (pointer == NULL || *pointer == NULL)
		return;
	backend = *pointer;
	if (backend->mutex != NULL) {
		(void)SDL_LockMutex(backend->mutex);
		backend->running = false;
		child = backend->child_pgid;
		if (backend->condition != NULL)
			(void)SDL_CondSignal(backend->condition);
		(void)SDL_UnlockMutex(backend->mutex);
	}
	if (child > 1)
		(void)kill(-child, SIGTERM);
	if (backend->thread != NULL)
		SDL_WaitThread(backend->thread, NULL);
	if (backend->condition != NULL)
		SDL_DestroyCond(backend->condition);
	if (backend->mutex != NULL)
		SDL_DestroyMutex(backend->mutex);
	memset(backend, 0, sizeof(*backend));
	free(backend);
	*pointer = NULL;
}
