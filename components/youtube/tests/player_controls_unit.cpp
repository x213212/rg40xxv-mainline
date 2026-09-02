#include "../src/player_controls.h"

#include <cstring>

int main(int argc, char **argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--contract") == 0) {
		rg40xxv_youtube::player_controls_print_contract();
		return 0;
	}
	if (argc == 2 && std::strcmp(argv[1], "--self-test") == 0)
		return rg40xxv_youtube::player_controls_self_test();
	return 64;
}
