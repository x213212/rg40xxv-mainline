#include <stdlib.h>
#include <string.h>

#ifdef RG40XXV_LSAN_SELF_TEST
#include <sanitizer/lsan_interface.h>
#endif

/*
 * This is a checker self-test, not application coverage.  A leak checker is
 * admitted only after it accepts the clean control and rejects this deliberate
 * leak.  Keeping the allocation behind a volatile function pointer prevents
 * the compiler from deleting the negative control as dead code.
 */
static void *(*volatile allocate_bytes)(size_t) = malloc;

__attribute__((noinline))
static int exercise_allocation(int leak)
{
	unsigned char *allocation = allocate_bytes(64U);

	if (allocation == NULL)
		return 2;
	memset(allocation, 0xa5, 64U);
	if (allocation[31] != 0xa5)
		return 3;
	if (!leak)
		free(allocation);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 1)
		return exercise_allocation(0);
	if (argc == 2 && strcmp(argv[1], "leak") == 0) {
		int result = exercise_allocation(1);

		if (result != 0)
			return result;
#ifdef RG40XXV_LSAN_SELF_TEST
		/*
		 * Check after exercise_allocation() returned, so its dead pointer is
		 * outside the active stack.  A zero result proves that the supposed
		 * negative control was not observed and is itself a test failure.
		 */
		return __lsan_do_recoverable_leak_check() == 0 ? 65 : 97;
#else
		return 0;
#endif
	}
	return 64;
}
