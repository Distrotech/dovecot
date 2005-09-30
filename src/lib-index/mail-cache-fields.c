/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "hash.h"
#include "file-cache.h"
#include "write-full.h"
#include "mail-cache-private.h"

#include <stddef.h>

#define CACHE_HDR_PREFETCH 1024

void mail_cache_register_fields(struct mail_cache *cache,
				struct mail_cache_field *fields,
				unsigned int fields_count)
{
	void *orig_key, *orig_value;
	unsigned int new_idx;
	size_t i;

	new_idx = cache->fields_count;
	for (i = 0; i < fields_count; i++) {
		if (hash_lookup_full(cache->field_name_hash, fields[i].name,
				     &orig_key, &orig_value)) {
			fields[i].idx =
				POINTER_CAST_TO(orig_value, unsigned int);
			continue;
		}

		fields[i].idx = new_idx++;
	}

	if (new_idx == cache->fields_count)
		return;

	/* @UNSAFE */
	cache->fields = i_realloc(cache->fields,
				  cache->fields_count * sizeof(*cache->fields),
				  new_idx * sizeof(*cache->fields));
	cache->field_file_map =
		i_realloc(cache->field_file_map,
			  cache->fields_count * sizeof(*cache->field_file_map),
			  new_idx * sizeof(*cache->field_file_map));

	for (i = 0; i < fields_count; i++) {
		unsigned int idx = fields[i].idx;

		if (idx < cache->fields_count)
			continue;

		/* new index - save it */
		cache->fields[idx].field = fields[i];
		cache->fields[idx].field.name =
			p_strdup(cache->field_pool, fields[i].name);
		cache->field_file_map[idx] = (uint32_t)-1;

		switch (cache->fields[idx].field.type) {
		case MAIL_CACHE_FIELD_FIXED_SIZE:
		case MAIL_CACHE_FIELD_BITMASK:
			break;
		case MAIL_CACHE_FIELD_VARIABLE_SIZE:
		case MAIL_CACHE_FIELD_STRING:
		case MAIL_CACHE_FIELD_HEADER:
			cache->fields[idx].field.field_size = (unsigned int)-1;
			break;
		}

		hash_insert(cache->field_name_hash,
			    (char *)cache->fields[idx].field.name,
			    POINTER_CAST(idx));
	}
	cache->fields_count = new_idx;
}

unsigned int
mail_cache_register_lookup(struct mail_cache *cache, const char *name)
{
	void *orig_key, *orig_value;

	if (hash_lookup_full(cache->field_name_hash, name,
			     &orig_key, &orig_value))
		return POINTER_CAST_TO(orig_value, unsigned int);
	else
		return (unsigned int)-1;
}

const struct mail_cache_field *
mail_cache_register_get_list(struct mail_cache *cache, pool_t pool,
			     unsigned int *count_r)
{
        struct mail_cache_field *list;
	unsigned int i;

	list = p_new(pool, struct mail_cache_field, cache->fields_count);
	for (i = 0; i < cache->fields_count; i++) {
		list[i] = cache->fields[i].field;
		list[i].name = p_strdup(pool, list[i].name);
	}

	*count_r = cache->fields_count;
	return list;
}

static int mail_cache_header_fields_get_offset(struct mail_cache *cache,
					       uint32_t *offset_r)
{
	const struct mail_cache_header_fields *field_hdr;
	uint32_t offset, next_offset;

	if (MAIL_CACHE_IS_UNUSABLE(cache)) {
		*offset_r = 0;
		return 0;
	}

	/* find the latest header */
	offset = 0;
	next_offset =
		mail_index_offset_to_uint32(cache->hdr->field_header_offset);
	while (next_offset != 0) {
		if (next_offset == offset) {
			mail_cache_set_corrupted(cache,
				"next_offset in field header loops");
			return -1;
		}
		offset = next_offset;

		if (cache->file_cache != NULL) {
			/* we can't trust that the cached data is valid */
			file_cache_invalidate(cache->file_cache, offset,
					      sizeof(*field_hdr) +
					      CACHE_HDR_PREFETCH);
		}
		if (mail_cache_map(cache, offset,
				   sizeof(*field_hdr) + CACHE_HDR_PREFETCH) < 0)
			return -1;

		field_hdr = CONST_PTR_OFFSET(cache->data, offset);
		next_offset =
			mail_index_offset_to_uint32(field_hdr->next_offset);
	}

	*offset_r = offset;
	return 0;
}

int mail_cache_header_fields_read(struct mail_cache *cache)
{
	const struct mail_cache_header_fields *field_hdr = NULL;
	struct mail_cache_field field;
	const uint32_t *last_used, *sizes;
	const uint8_t *types, *decisions;
	const char *p, *names, *end;
	void *orig_key, *orig_value;
	uint32_t offset, i;

	if (mail_cache_header_fields_get_offset(cache, &offset) < 0)
		return -1;

	if (offset == 0) {
		/* no fields - the file is empty */
		return 0;
	}

	field_hdr = CONST_PTR_OFFSET(cache->data, offset);
	if (offset + field_hdr->size > cache->mmap_length) {
		mail_cache_set_corrupted(cache,
					 "field header points outside file");
		return -1;
	}

	/* check the fixed size of the header. name[] has to be checked
	   separately */
	if (field_hdr->size < sizeof(*field_hdr) +
	    field_hdr->fields_count * (sizeof(uint32_t)*2 + 1 + 2)) {
		mail_cache_set_corrupted(cache, "invalid field header size");
		return -1;
	}

	if (field_hdr->size > sizeof(*field_hdr) + CACHE_HDR_PREFETCH) {
		if (cache->file_cache != NULL) {
			/* we can't trust that the cached data is valid */
			file_cache_invalidate(cache->file_cache, offset,
					      sizeof(*field_hdr) +
					      CACHE_HDR_PREFETCH);
		}
		if (mail_cache_map(cache, offset, field_hdr->size) < 0)
			return -1;
	}
	field_hdr = CONST_PTR_OFFSET(cache->data, offset);

	cache->file_field_map =
		i_realloc(cache->file_field_map,
			  cache->file_fields_count * sizeof(unsigned int),
			  field_hdr->fields_count * sizeof(unsigned int));
	cache->file_fields_count = field_hdr->fields_count;

	last_used = CONST_PTR_OFFSET(field_hdr, MAIL_CACHE_FIELD_LAST_USED());
	sizes = CONST_PTR_OFFSET(field_hdr,
		MAIL_CACHE_FIELD_SIZE(field_hdr->fields_count));
	types = CONST_PTR_OFFSET(field_hdr,
		MAIL_CACHE_FIELD_TYPE(field_hdr->fields_count));
	decisions = CONST_PTR_OFFSET(field_hdr,
		MAIL_CACHE_FIELD_DECISION(field_hdr->fields_count));
	names = CONST_PTR_OFFSET(field_hdr,
		MAIL_CACHE_FIELD_NAMES(field_hdr->fields_count));
	end = CONST_PTR_OFFSET(field_hdr, field_hdr->size);

	/* clear the old mapping */
	for (i = 0; i < cache->fields_count; i++)
		cache->field_file_map[i] = (uint32_t)-1;

	memset(&field, 0, sizeof(field));
	for (i = 0; i < field_hdr->fields_count; i++) {
		for (p = names; p != end && *p != '\0'; p++) ;
		if (p == end || *names == '\0') {
			mail_cache_set_corrupted(cache,
				"field header names corrupted");
			return -1;
		}

		if (hash_lookup_full(cache->field_name_hash, names,
				     &orig_key, &orig_value)) {
			/* already exists, see if decision can be updated */
			field.idx = POINTER_CAST_TO(orig_value, unsigned int);
			if (!cache->fields[field.idx].decision_dirty) {
				cache->fields[field.idx].field.decision =
					decisions[i];
			}
			if (cache->fields[field.idx].field.type != types[i]) {
				mail_cache_set_corrupted(cache,
					"registered field type changed");
				return -1;
			}
		} else {
			field.name = names;
			field.type = types[i];
			field.field_size = sizes[i];
			field.decision = decisions[i];
			mail_cache_register_fields(cache, &field, 1);
		}
		if (cache->field_file_map[field.idx] != (uint32_t)-1) {
			mail_cache_set_corrupted(cache,
				"Duplicated field in header: %s", names);
			return -1;
		}
		cache->field_file_map[field.idx] = i;
		cache->file_field_map[i] = field.idx;

		/* update last_used if it's newer than ours */
		if (last_used[i] > cache->fields[field.idx].last_used)
			cache->fields[field.idx].last_used = last_used[i];

                names = p + 1;
	}
	return 0;
}

static void copy_to_buf(struct mail_cache *cache, buffer_t *dest,
			size_t offset, size_t size)
{
	const void *data;
	unsigned int i, field;

	for (i = 0; i < cache->file_fields_count; i++) {
		field = cache->file_field_map[i];
                data = CONST_PTR_OFFSET(&cache->fields[field], offset);
		buffer_append(dest, data, size);
	}
	for (i = 0; i < cache->fields_count; i++) {
		if (cache->field_file_map[i] != (uint32_t)-1)
			continue;
		data = CONST_PTR_OFFSET(&cache->fields[i], offset);
		buffer_append(dest, data, size);
	}
}

static void copy_to_buf_byte(struct mail_cache *cache, buffer_t *dest,
			     size_t offset)
{
	const int *data;
	unsigned int i, field;
	uint8_t byte;

	for (i = 0; i < cache->file_fields_count; i++) {
		field = cache->file_field_map[i];
                data = CONST_PTR_OFFSET(&cache->fields[field], offset);
		byte = (uint8_t)*data;
		buffer_append(dest, &byte, 1);
	}
	for (i = 0; i < cache->fields_count; i++) {
		if (cache->field_file_map[i] != (uint32_t)-1)
			continue;
		data = CONST_PTR_OFFSET(&cache->fields[i], offset);
		byte = (uint8_t)*data;
		buffer_append(dest, &byte, 1);
	}
}

static int mail_cache_header_fields_update_locked(struct mail_cache *cache)
{
	buffer_t *buffer;
	uint32_t i, offset;
	int ret = 0;

	if (mail_cache_header_fields_read(cache) < 0 ||
	    mail_cache_header_fields_get_offset(cache, &offset) < 0)
		return -1;

	t_push();
	buffer = buffer_create_dynamic(pool_datastack_create(), 256);

	copy_to_buf(cache, buffer,
		    offsetof(struct mail_cache_field_private, last_used),
		    sizeof(uint32_t));
	ret = mail_cache_write(cache, buffer->data,
			       sizeof(uint32_t) * cache->file_fields_count,
			       offset + MAIL_CACHE_FIELD_LAST_USED());
	if (ret == 0) {
		buffer_set_used_size(buffer, 0);
		copy_to_buf_byte(cache, buffer,
				 offsetof(struct mail_cache_field, decision));

		ret = mail_cache_write(cache, buffer->data,
			sizeof(uint8_t) * cache->file_fields_count, offset +
			MAIL_CACHE_FIELD_DECISION(cache->file_fields_count));

		if (ret == 0) {
			for (i = 0; i < cache->file_fields_count; i++)
				cache->fields[i].decision_dirty = FALSE;
		}
	}
	t_pop();

	if (ret == 0)
		cache->field_header_write_pending = FALSE;

	return ret;
}

int mail_cache_header_fields_update(struct mail_cache *cache)
{
	int ret;

	if (cache->locked)
		return mail_cache_header_fields_update_locked(cache);

	if (mail_cache_lock(cache) <= 0)
		return -1;

	ret = mail_cache_header_fields_update_locked(cache);
	if (mail_cache_unlock(cache) < 0)
		ret = -1;
	return ret;
}

void mail_cache_header_fields_get(struct mail_cache *cache, buffer_t *dest)
{
	struct mail_cache_header_fields hdr;
	unsigned int field;
	const char *name;
	uint32_t i;

	memset(&hdr, 0, sizeof(hdr));
	hdr.fields_count = cache->fields_count;
	buffer_append(dest, &hdr, sizeof(hdr));

	/* we have to keep the field order for the existing fields. */
	copy_to_buf(cache, dest,
		    offsetof(struct mail_cache_field_private, last_used),
		    sizeof(uint32_t));
	copy_to_buf(cache, dest, offsetof(struct mail_cache_field, field_size),
		    sizeof(uint32_t));
	copy_to_buf_byte(cache, dest, offsetof(struct mail_cache_field, type));
	copy_to_buf_byte(cache, dest,
			 offsetof(struct mail_cache_field, decision));

	i_assert(buffer_get_used_size(dest) == sizeof(hdr) +
		 (sizeof(uint32_t)*2 + 2) * hdr.fields_count);

	for (i = 0; i < cache->file_fields_count; i++) {
		field = cache->file_field_map[i];
		name = cache->fields[field].field.name;
		buffer_append(dest, name, strlen(name)+1);
	}
	for (i = 0; i < cache->fields_count; i++) {
		if (cache->field_file_map[i] != (uint32_t)-1)
			continue;
		name = cache->fields[i].field.name;
		buffer_append(dest, name, strlen(name)+1);
	}

	hdr.size = buffer_get_used_size(dest);
	buffer_write(dest, 0, &hdr, sizeof(hdr));

	if ((hdr.size & 3) != 0)
		buffer_append_zero(dest, 4 - (hdr.size & 3));
}

int mail_cache_header_fields_get_next_offset(struct mail_cache *cache,
					     uint32_t *offset_r)
{
	if (mail_cache_header_fields_get_offset(cache, offset_r) < 0)
		return -1;

	if (*offset_r == 0) {
		*offset_r = offsetof(struct mail_cache_header,
				     field_header_offset);
	} else {
		*offset_r += offsetof(struct mail_cache_header_fields,
				      next_offset);
	}
	return 0;
}
