/* Copyright (c) 2001-2003 Timo Sirainen */

#include "lib.h"
#include "alarm-hup.h"
#include "hostpid.h"

#include <stdlib.h>
#include <time.h>

size_t nearest_power(size_t num)
{
	size_t n = 1;

	i_assert(num <= ((size_t)1 << (BITS_IN_SIZE_T-1)));

	while (n < num) n <<= 1;
	return n;
}

void lib_init(void)
{
	/* standard way to get rand() return different values. */
	srand((unsigned int) time(NULL));

	data_stack_init();
	imem_init();
	hostpid_init();
}

void lib_deinit(void)
{
	alarm_hup_deinit(); /* doesn't harm even if init is never called */

        imem_deinit();
	data_stack_deinit();
        failures_deinit();
}
