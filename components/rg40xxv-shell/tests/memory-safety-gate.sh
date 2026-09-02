#!/bin/sh

# Source this file after creating the test's private temporary directory.
# It selects a leak checker only after a positive and negative control.  ASan
# and UBSan remain mandatory in the caller regardless of the selected backend.

rg40xxv_select_leak_backend()
{
	_rg_cc=$1
	_rg_temporary=$2
	_rg_probe_source=$3
	_rg_asan_probe="$_rg_temporary/memory-safety-probe-asan"
	_rg_plain_probe="$_rg_temporary/memory-safety-probe-plain"
	_rg_clean_log="$_rg_temporary/lsan-clean.stderr"
	_rg_leak_log="$_rg_temporary/lsan-leak.stderr"

	"$_rg_cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
		-DRG40XXV_LSAN_SELF_TEST \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		-fno-sanitize-recover=all \
		"$_rg_probe_source" -o "$_rg_asan_probe"

	if (ulimit -c 0; \
	    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0 \
	    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	    "$_rg_asan_probe") >"$_rg_temporary/lsan-clean.stdout" \
		2>"$_rg_clean_log"; then
		_rg_lsan_clean_rc=0
	else
		_rg_lsan_clean_rc=$?
	fi

	if test "$_rg_lsan_clean_rc" -eq 0; then
		if (ulimit -c 0; \
		    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=0 \
		    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		    "$_rg_asan_probe" leak) \
			>"$_rg_temporary/lsan-leak.stdout" 2>"$_rg_leak_log"; then
			_rg_lsan_leak_rc=0
		else
			_rg_lsan_leak_rc=$?
		fi
		if test "$_rg_lsan_leak_rc" -ne 0 &&
		   grep -Fq 'ERROR: LeakSanitizer: detected memory leaks' \
			"$_rg_leak_log"; then
			RG40XXV_LEAK_BACKEND=lsan
			RG40XXV_LEAK_SKIP_REASON=
			printf '%s\n' \
				'MEMORY_SAFETY_CHECKER backend=lsan controls=clean-pass,leak-rejected'
			return 0
		fi
		printf '%s\n' \
			'LeakSanitizer clean control passed but negative control was not rejected' >&2
		cat "$_rg_leak_log" >&2
		return 1
	fi

	# The managed WSL runner denies LSan's stop-the-world child permission to
	# ptrace its parent (PTRACE_ATTACH returns EPERM).  Recognize only that exact
	# capability failure; any other LSan startup error remains a hard failure.
	if ! grep -Eq \
		'LeakSanitizer does not work under ptrace|Could not attach to thread|Failed suspending threads' \
		"$_rg_clean_log"; then
		printf 'LeakSanitizer clean control failed unexpectedly (rc=%s)\n' \
			"$_rg_lsan_clean_rc" >&2
		cat "$_rg_clean_log" >&2
		return 1
	fi

	if ! command -v valgrind >/dev/null 2>&1; then
		RG40XXV_LEAK_BACKEND=skip
		RG40XXV_LEAK_SKIP_REASON=lsan-ptrace-restricted-and-valgrind-unavailable
		printf '%s\n' \
			'MEMORY_SAFETY_CHECKER backend=none leak=SKIP reason=lsan-ptrace-restricted-and-valgrind-unavailable'
		return 0
	fi

	"$_rg_cc" -std=c11 -O1 -g -Wall -Wextra -Werror \
		"$_rg_probe_source" -o "$_rg_plain_probe"
	if ! valgrind --tool=memcheck --leak-check=full \
		--show-leak-kinds=all \
		--errors-for-leak-kinds=definite,indirect,possible \
		--track-origins=yes --error-exitcode=97 \
		"$_rg_plain_probe" >"$_rg_temporary/valgrind-clean.stdout" \
		2>"$_rg_temporary/valgrind-clean.stderr"; then
		printf '%s\n' 'Valgrind clean control failed' >&2
		cat "$_rg_temporary/valgrind-clean.stderr" >&2
		return 1
	fi
	if valgrind --tool=memcheck --leak-check=full \
		--show-leak-kinds=all \
		--errors-for-leak-kinds=definite,indirect,possible \
		--track-origins=yes --error-exitcode=97 \
		"$_rg_plain_probe" leak \
		>"$_rg_temporary/valgrind-leak.stdout" \
		2>"$_rg_temporary/valgrind-leak.stderr"; then
		_rg_valgrind_leak_rc=0
	else
		_rg_valgrind_leak_rc=$?
	fi
	if test "$_rg_valgrind_leak_rc" -ne 97 ||
	   ! grep -Eq 'definitely lost: [1-9][0-9,]* bytes in [1-9][0-9,]* blocks' \
		"$_rg_temporary/valgrind-leak.stderr"; then
		printf 'Valgrind negative control was not rejected (rc=%s)\n' \
			"$_rg_valgrind_leak_rc" >&2
		cat "$_rg_temporary/valgrind-leak.stderr" >&2
		return 1
	fi
	RG40XXV_LEAK_BACKEND=valgrind
	RG40XXV_LEAK_SKIP_REASON=
	printf '%s\n' \
		'MEMORY_SAFETY_CHECKER backend=valgrind-memcheck controls=clean-pass,leak-rejected lsan=ptrace-restricted'
}

rg40xxv_valgrind_run()
{
	_rg_valgrind_log=$1
	shift
	valgrind --tool=memcheck --leak-check=full \
		--show-leak-kinds=all \
		--errors-for-leak-kinds=definite,indirect,possible \
		--track-origins=yes --error-exitcode=97 \
		"$@" 2>"$_rg_valgrind_log"
	grep -Fq 'All heap blocks were freed -- no leaks are possible' \
		"$_rg_valgrind_log"
	grep -Eq 'ERROR SUMMARY: 0 errors from 0 contexts' \
		"$_rg_valgrind_log"
}
