/*
 * Copyright (c) 2004 Andrey Panin <pazke@donpac.ru>
 *
 * This software is released under the MIT license.
 */

#include "lib.h"
#include "ioloop-internal.h"
#include "ioloop-iolist.h"

bool ioloop_iolist_add(struct io_list *list, struct io *io)
{
	int i, idx;

	if ((io->condition & IO_READ) != 0)
		idx = IOLOOP_IOLIST_INPUT;
	else if ((io->condition & IO_WRITE) != 0)
		idx = IOLOOP_IOLIST_OUTPUT;
	else if ((io->condition & IO_ERROR) != 0)
		idx = IOLOOP_IOLIST_ERROR;
	else {
		i_unreached();
	}

	i_assert(list->ios[idx] == NULL);
	list->ios[idx] = io;

	/* check if this was the first one */
	for (i = 0; i < IOLOOP_IOLIST_IOS_PER_FD; i++) {
		if (i != idx && list->ios[i] != NULL)
			return FALSE;
	}

	return TRUE;
}

bool ioloop_iolist_del(struct io_list *list, struct io *io)
{
	bool last = TRUE;
	int i;

	for (i = 0; i < IOLOOP_IOLIST_IOS_PER_FD; i++) {
		if (list->ios[i] != NULL) {
			if (list->ios[i] == io)
				list->ios[i] = NULL;
			else
				last = FALSE;
		}
	}
	return last;
}
