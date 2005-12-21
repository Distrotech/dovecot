/* Copyright (C) 2005 Timo Sirainen */

#include "lib.h"
#include "bsearch-insert-pos.h"

void *bsearch_insert_pos(const void *key, const void *base, unsigned int nmemb,
			 size_t size, int (*cmp)(const void *, const void *))
{
	const void *p;
	unsigned int idx, left_idx, right_idx;
	int ret;

	/* we're probably appending it, check */
	idx = 0; left_idx = 0; right_idx = nmemb;
	while (left_idx < right_idx) {
		idx = (left_idx + right_idx) / 2;

		p = CONST_PTR_OFFSET(base, idx * size);
		ret = cmp(key, p);
		if (ret > 0)
			left_idx = idx+1;
		else if (ret < 0)
			right_idx = idx;
		else
			return (void *)p;
	}

	p = CONST_PTR_OFFSET(base, idx * size);
	if (idx < nmemb && cmp(key, p) > 0)
		p = CONST_PTR_OFFSET(p, size);
	return (void *)p;
}

