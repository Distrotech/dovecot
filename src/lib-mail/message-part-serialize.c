/* Copyright (C) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "message-parser.h"
#include "message-part-serialize.h"

/*
   root part
     root's first children
       children's first children
       ...
     root's next children
     ...

   part
     unsigned int flags
     (not root part)
       uoff_t physical_pos
     uoff_t header_physical_size
     uoff_t header_virtual_size
     uoff_t body_physical_size
     uoff_t body_virtual_size
     (flags & (MESSAGE_PART_FLAG_TEXT | MESSAGE_PART_FLAG_MESSAGE_RFC822))
       unsigned int body_lines
     (flags & (MESSAGE_PART_FLAG_MULTIPART | MESSAGE_PART_FLAG_MESSAGE_RFC822))
       unsigned int children_count

*/

#define MINIMUM_SERIALIZED_SIZE \
	(sizeof(unsigned int) + sizeof(uoff_t) * 4)

struct deserialize_context {
	pool_t pool;
	const unsigned char *data, *end;

	uoff_t pos;
	const char *error;
};

static unsigned int
_message_part_serialize(struct message_part *part, buffer_t *dest)
{
	unsigned int count, children_count;
	size_t children_offset;
	int root = part->parent == NULL;

	count = 0;
	while (part != NULL) {
		/* create serialized part */
		buffer_append(dest, &part->flags, sizeof(part->flags));
		if (root)
			root = FALSE;
		else {
			buffer_append(dest, &part->physical_pos,
				      sizeof(part->physical_pos));
		}
		buffer_append(dest, &part->header_size.physical_size,
			      sizeof(part->header_size.physical_size));
		buffer_append(dest, &part->header_size.virtual_size,
			      sizeof(part->header_size.virtual_size));
		buffer_append(dest, &part->body_size.physical_size,
			      sizeof(part->body_size.physical_size));
		buffer_append(dest, &part->body_size.virtual_size,
			      sizeof(part->body_size.virtual_size));

		if ((part->flags & (MESSAGE_PART_FLAG_TEXT |
				    MESSAGE_PART_FLAG_MESSAGE_RFC822)) != 0) {
			buffer_append(dest, &part->body_size.lines,
				      sizeof(part->body_size.lines));
		}

		if ((part->flags & (MESSAGE_PART_FLAG_MULTIPART |
				    MESSAGE_PART_FLAG_MESSAGE_RFC822)) != 0) {
			children_offset = buffer_get_used_size(dest);
			children_count = 0;
			buffer_append(dest, &children_count,
				      sizeof(children_count));

			if (part->children != NULL) {
				children_count =
					_message_part_serialize(part->children,
								dest);

				buffer_write(dest, children_offset,
					     &children_count,
					     sizeof(children_count));
			}
		} else {
			i_assert(part->children == NULL);
		}

		count++;
		part = part->next;
	}

	return count;
}

void message_part_serialize(struct message_part *part, buffer_t *dest)
{
	_message_part_serialize(part, dest);
}

static int read_next(struct deserialize_context *ctx,
		     void *buffer, size_t buffer_size)
{
	if (ctx->data + buffer_size > ctx->end) {
		ctx->error = "Not enough data";
		return FALSE;
	}

	memcpy(buffer, ctx->data, buffer_size);
	ctx->data += buffer_size;
	return TRUE;
}

static int message_part_deserialize_part(struct deserialize_context *ctx,
					 struct message_part *parent,
					 unsigned int siblings,
                                         struct message_part **part_r)
{
	struct message_part *part, *first_part, **next_part;
	unsigned int children_count;
	uoff_t pos;
	int root = parent == NULL;

	first_part = NULL;
	next_part = NULL;
	while (siblings > 0) {
		siblings--;

		part = p_new(ctx->pool, struct message_part, 1);
		part->parent = parent;

		if (!read_next(ctx, &part->flags, sizeof(part->flags)))
			return FALSE;

		if (root)
			root = FALSE;
		else {
			if (!read_next(ctx, &part->physical_pos,
				       sizeof(part->physical_pos)))
				return FALSE;
		}

		if (part->physical_pos < ctx->pos) {
			ctx->error = "physical_pos less than expected";
			return FALSE;
		}

		if (!read_next(ctx, &part->header_size.physical_size,
			       sizeof(part->header_size.physical_size)))
			return FALSE;

		if (!read_next(ctx, &part->header_size.virtual_size,
			       sizeof(part->header_size.virtual_size)))
			return FALSE;

		if (part->header_size.virtual_size <
		    part->header_size.physical_size) {
			ctx->error = "header_size.virtual_size too small";
			return FALSE;
		}

		if (!read_next(ctx, &part->body_size.physical_size,
			       sizeof(part->body_size.physical_size)))
			return FALSE;

		if (!read_next(ctx, &part->body_size.virtual_size,
			       sizeof(part->body_size.virtual_size)))
			return FALSE;

		if ((part->flags & (MESSAGE_PART_FLAG_TEXT |
				    MESSAGE_PART_FLAG_MESSAGE_RFC822)) != 0) {
			if (!read_next(ctx, &part->body_size.lines,
				       sizeof(part->body_size.lines)))
				return FALSE;
		}

		if (part->body_size.virtual_size <
		    part->body_size.physical_size) {
			ctx->error = "body_size.virtual_size too small";
			return FALSE;
		}

		if ((part->flags & (MESSAGE_PART_FLAG_MULTIPART |
				    MESSAGE_PART_FLAG_MESSAGE_RFC822)) != 0) {
			if (!read_next(ctx, &children_count,
				       sizeof(children_count)))
				return FALSE;
		} else {
                        children_count = 0;
		}

		if (part->flags & MESSAGE_PART_FLAG_MESSAGE_RFC822) {
			/* Only one child is possible */
			if (children_count == 0) {
				ctx->error =
					"message/rfc822 part has no children";
				return FALSE;
			}
			if (children_count != 1) {
				ctx->error = "message/rfc822 part "
					"has multiple children";
				return FALSE;
			}
		}

		if (children_count > 0) {
			/* our children must be after our physical_pos and
			   the last child must be within our size. */
			ctx->pos = part->physical_pos;
			pos = part->physical_pos +
				part->header_size.physical_size +
				part->body_size.physical_size;

			if (!message_part_deserialize_part(ctx, part,
							   children_count,
							   &part->children))
				return FALSE;

			if (ctx->pos > pos) {
				ctx->error =
					"child part location exceeds our size";
				return FALSE;
			}
			ctx->pos = pos; /* save it for above check for parent */
		}

		if (first_part == NULL)
			first_part = part;
		if (next_part != NULL)
			*next_part = part;
		next_part = &part->next;
	}

	*part_r = first_part;
	return TRUE;
}

struct message_part *message_part_deserialize(pool_t pool, const void *data,
					      size_t size, const char **error)
{
	struct deserialize_context ctx;
        struct message_part *part;

	memset(&ctx, 0, sizeof(ctx));
	ctx.pool = pool;
	ctx.data = data;
	ctx.end = ctx.data + size;

	if (!message_part_deserialize_part(&ctx, NULL, 1, &part)) {
		*error = ctx.error;
		return NULL;
	}

	if (ctx.data != ctx.end) {
		*error = "Too much data";
		return NULL;
	}

	return part;
}

static size_t get_serialized_size(unsigned int flags)
{
	size_t size = sizeof(unsigned int) + sizeof(uoff_t) * 5;

	if ((flags & (MESSAGE_PART_FLAG_TEXT |
		      MESSAGE_PART_FLAG_MESSAGE_RFC822)) != 0)
		size += sizeof(unsigned int);
	if ((flags & (MESSAGE_PART_FLAG_MULTIPART |
		      MESSAGE_PART_FLAG_MESSAGE_RFC822)) != 0)
		size += sizeof(unsigned int);
	return size;
}

int message_part_serialize_update_header(void *data, size_t size,
					 struct message_size *hdr_size,
					 const char **error)
{
	unsigned char *buf = data;
	size_t offset, part_size;
	uoff_t uofft_size, old_size;
	off_t pos_diff;
	unsigned int flags;

	i_assert(hdr_size->physical_size <= OFF_T_MAX);

	if (size < MINIMUM_SERIALIZED_SIZE) {
		*error = "Not enough data";
		return FALSE;
	}

	memcpy(&flags, buf, sizeof(flags));
	memcpy(&uofft_size, buf + sizeof(unsigned int), sizeof(uoff_t));

	if (uofft_size > OFF_T_MAX) {
		*error = "Invalid physical_size";
		return FALSE;
	}
	pos_diff = (off_t)hdr_size->physical_size - (off_t)uofft_size;
	old_size = uofft_size;

	memcpy(buf + sizeof(unsigned int),
	       &hdr_size->physical_size, sizeof(uoff_t));
	memcpy(buf + sizeof(unsigned int) + sizeof(uoff_t),
	       &hdr_size->virtual_size, sizeof(uoff_t));

	if (pos_diff != 0) {
		/* have to update all positions, but skip the root */
		offset = get_serialized_size(flags) - sizeof(uoff_t);

		while (offset + sizeof(unsigned int) < size) {
			memcpy(buf + offset, &flags, sizeof(flags));

			part_size = get_serialized_size(flags);
			if (offset + part_size > size) {
				*error = "Not enough data";
				return FALSE;
			}
			memcpy(&uofft_size, buf + offset + sizeof(flags),
			       sizeof(uoff_t));

			if (uofft_size < old_size || uofft_size >= OFF_T_MAX) {
				/* invalid offset, might cause overflow */
				*error = "Invalid offset";
				return FALSE;
			}
			uofft_size += pos_diff;

			memcpy(buf + offset + sizeof(flags), &uofft_size,
			       sizeof(uoff_t));
			offset += part_size;
		}

		if (offset != size) {
			*error = "Invalid size";
			return FALSE;
		}
	}

	return TRUE;
}

int message_part_deserialize_size(const void *data, size_t size,
				  struct message_size *hdr_size,
				  struct message_size *body_size)
{
	const unsigned char *buf = data;
	unsigned int flags;

	/* make sure it looks valid */
	if (size < MINIMUM_SERIALIZED_SIZE)
		return FALSE;

	memcpy(&flags, buf, sizeof(flags));
	buf += sizeof(flags);
	memcpy(&hdr_size->physical_size, buf, sizeof(uoff_t));
	buf += sizeof(uoff_t);
	memcpy(&hdr_size->virtual_size, buf, sizeof(uoff_t));
	buf += sizeof(uoff_t);
	hdr_size->lines = 0;

	memcpy(&body_size->physical_size, buf, sizeof(uoff_t));
	buf += sizeof(uoff_t);
	memcpy(&body_size->virtual_size, buf, sizeof(uoff_t));
	buf += sizeof(uoff_t);

	if ((flags & (MESSAGE_PART_FLAG_TEXT |
		      MESSAGE_PART_FLAG_MESSAGE_RFC822)) == 0)
		body_size->lines = 0;
	else {
		if (size < MINIMUM_SERIALIZED_SIZE + sizeof(unsigned int))
			return FALSE;
		memcpy(&body_size->lines, buf, sizeof(unsigned int));
	}

	return TRUE;
}
