/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "base64.h"
#include "buffer.h"
#include "istream.h"
#include "strescape.h"
#include "charset-utf8.h"
#include "quoted-printable.h"
#include "message-parser.h"
#include "message-content-parser.h"
#include "message-header-search.h"
#include "message-body-search.h"

#define DECODE_BLOCK_SIZE 8192

struct body_search_context {
	pool_t pool;

	const char *key;
	size_t key_len;

	const char *charset;
	unsigned int unknown_charset:1;
	unsigned int search_header:1;
};

struct part_search_context {
	struct body_search_context *body_ctx;

	struct charset_translation *translation;

	buffer_t *decode_buf;
	buffer_t *match_buf;

	char *content_type;
	char *content_charset;

	unsigned int content_qp:1;
	unsigned int content_base64:1;
	unsigned int content_unknown:1;
	unsigned int content_type_text:1; /* text/any or message/any */
	unsigned int ignore_header:1;
};

static void parse_content_type(const unsigned char *value, size_t value_len,
			       void *context)
{
	struct part_search_context *ctx = context;

	if (ctx->content_type == NULL) {
		ctx->content_type = i_strndup(value, value_len);
		ctx->content_type_text =
			strncasecmp(ctx->content_type, "text/", 5) == 0 ||
			strncasecmp(ctx->content_type, "message/", 8) == 0;
	}
}

static void
parse_content_type_param(const unsigned char *name, size_t name_len,
			 const unsigned char *value, size_t value_len,
			 bool value_quoted, void *context)
{
	struct part_search_context *ctx = context;

	if (name_len == 7 && memcasecmp(name, "charset", 7) == 0 &&
	    ctx->content_charset == NULL) {
		ctx->content_charset = i_strndup(value, value_len);
		if (value_quoted) str_unescape(ctx->content_charset);
	}
}

static void parse_content_encoding(const unsigned char *value, size_t value_len,
				   void *context)
{
	struct part_search_context *ctx = context;

	switch (value_len) {
	case 4:
		if (memcasecmp(value, "7bit", 4) != 0 &&
		    memcasecmp(value, "8bit", 4) != 0)
			ctx->content_unknown = TRUE;
		break;
	case 6:
		if (memcasecmp(value, "base64", 6) == 0)
			ctx->content_base64 = TRUE;
		else if (memcasecmp(value, "binary", 6) != 0)
			ctx->content_unknown = TRUE;
		break;
	case 16:
		if (memcasecmp(value, "quoted-printable", 16) == 0)
			ctx->content_qp = TRUE;
		else
			ctx->content_unknown = TRUE;
		break;
	default:
		ctx->content_unknown = TRUE;
		break;
	}
}

static bool message_search_header(struct part_search_context *ctx,
				  struct istream *input,
				  const struct message_part *part)
{
	struct header_search_context *hdr_search_ctx;
	struct message_header_parser_ctx *hdr_ctx;
	struct message_header_line *hdr;
	int ret;
	bool found = FALSE;

	hdr_search_ctx = message_header_search_init(pool_datastack_create(),
						    ctx->body_ctx->key,
						    "UTF-8", NULL);
	/* Our key is in UTF-8. It can't be invalid. */
	i_assert(hdr_search_ctx != NULL);

	/* we default to text content-type */
	ctx->content_type_text = TRUE;

	input = i_stream_create_limit(default_pool, input, part->physical_pos,
				      part->header_size.physical_size);
	i_stream_seek(input, 0);

	hdr_ctx = message_parse_header_init(input, NULL, TRUE);
	while ((ret = message_parse_header_next(hdr_ctx, &hdr)) > 0) {
		if (hdr->eoh)
			continue;

		if (!ctx->ignore_header) {
			if (message_header_search(hdr->value, hdr->value_len,
						  hdr_search_ctx)) {
				found = TRUE;
				break;
			}
		}

		if (hdr->name_len == 12 &&
		    strcasecmp(hdr->name, "Content-Type") == 0) {
			if (hdr->continues) {
				hdr->use_full_value = TRUE;
				continue;
			}
			message_content_parse_header(hdr->full_value,
						     hdr->full_value_len,
						     parse_content_type,
						     parse_content_type_param,
						     ctx);
		} else if (hdr->name_len == 25 &&
			   strcasecmp(hdr->name,
				      "Content-Transfer-Encoding") == 0) {
			if (hdr->continues) {
				hdr->use_full_value = TRUE;
				continue;
			}
			message_content_parse_header(hdr->full_value,
				hdr->full_value_len,
				parse_content_encoding,
				null_parse_content_param_callback, ctx);
		}
	}
	i_assert(ret != 0);
	message_parse_header_deinit(&hdr_ctx);
	i_stream_destroy(&input);

	return found;
}

static bool message_search_decoded_block(struct part_search_context *ctx,
					 buffer_t *block)
{
	const unsigned char *p, *end, *key;
	size_t key_len, block_size, *matches, match_count, value;
	ssize_t i;

	key = (const unsigned char *) ctx->body_ctx->key;
	key_len = ctx->body_ctx->key_len;

	matches = buffer_get_modifiable_data(ctx->match_buf, &match_count);
	match_count /= sizeof(size_t);

	p = buffer_get_data(block, &block_size);
	end = p + block_size;
	for (; p != end; p++) {
		for (i = match_count-1; i >= 0; i--) {
			if (key[matches[i]] == *p) {
				if (++matches[i] == key_len) {
					/* full match */
					p++;
					return TRUE;
				}
			} else {
				/* non-match */
				buffer_delete(ctx->match_buf,
					      i * sizeof(size_t),
					      sizeof(size_t));
				match_count--;
			}
		}

		if (*p == key[0]) {
			if (key_len == 1) {
				/* only one character in search key */
				p++;
				return TRUE;
			}

			value = 1;
			buffer_append(ctx->match_buf, &value, sizeof(value));
			match_count++;
		}
	}

	return FALSE;
}

/* returns 1 = found, 0 = not found, -1 = error in input data */
static int message_search_body_block(struct part_search_context *ctx,
				     buffer_t *block)
{
	const unsigned char *inbuf;
	buffer_t *outbuf;
        enum charset_result result;
	size_t block_pos, inbuf_size, inbuf_left;

	outbuf = buffer_create_static_hard(pool_datastack_create(),
					   DECODE_BLOCK_SIZE);
	for (block_pos = 0; block_pos < buffer_get_used_size(block); ) {
		if (buffer_get_used_size(ctx->decode_buf) == 0) {
			/* we can use the buffer directly without copying */
			inbuf = buffer_get_data(block, &inbuf_size);
			inbuf += block_pos; inbuf_size -= block_pos;
			block_pos += buffer_get_used_size(block);
		} else {
			/* some characters already in buffer, ie. last
			   conversion contained partial data */
			buffer_append_buf(ctx->decode_buf, block,
					  block_pos, block->used);
                        block_pos += block->used;

			inbuf = buffer_get_data(ctx->decode_buf, &inbuf_size);
		}

		buffer_set_used_size(outbuf, 0);
		inbuf_left = inbuf_size;
		result = charset_to_ucase_utf8(ctx->translation,
					       inbuf, &inbuf_size, outbuf);
		inbuf_left -= inbuf_size;

		switch (result) {
		case CHARSET_RET_OUTPUT_FULL:
			/* we should have copied the incomplete sequence.. */
			i_assert(inbuf_left <= block_pos);
			/* fall through */
		case CHARSET_RET_OK:
			buffer_set_used_size(ctx->decode_buf, 0);
			block_pos -= inbuf_left;
			break;
		case CHARSET_RET_INCOMPLETE_INPUT:
			/* save the partial sequence to buffer */
			buffer_write(ctx->decode_buf, 0,
				     inbuf + inbuf_size, inbuf_left);
			buffer_set_used_size(ctx->decode_buf, inbuf_left);
			break;

		case CHARSET_RET_INVALID_INPUT:
			return -1;
		}

		if (message_search_decoded_block(ctx, outbuf))
			return 1;
	}

	return 0;
}

static bool message_search_body(struct part_search_context *ctx,
				struct istream *input,
				const struct message_part *part)
{
	const unsigned char *data;
	buffer_t *decodebuf;
	pool_t pool;
	size_t data_size, pos;
	ssize_t ret;
	bool found;

	if (ctx->content_unknown) {
		/* unknown content-encoding-type, ignore */
		return FALSE;
	}

	if (!ctx->content_type_text) {
		/* non-text content, ignore - FIXME: should be configurable? */
		return FALSE;
	}

	ctx->translation = ctx->content_charset == NULL ? NULL :
		charset_to_utf8_begin(ctx->content_charset, NULL);
	if (ctx->translation == NULL)
		ctx->translation = charset_to_utf8_begin("ascii", NULL);

	ctx->decode_buf = buffer_create_dynamic(default_pool, 256);
	ctx->match_buf = buffer_create_static_hard(pool_datastack_create(),
						   sizeof(size_t) *
						   ctx->body_ctx->key_len);

	input = i_stream_create_limit(default_pool, input,
				      part->physical_pos +
				      part->header_size.physical_size,
				      part->body_size.physical_size);
	i_stream_seek(input, 0);

	found = FALSE; pos = 0;
	while (i_stream_read_data(input, &data, &data_size, pos) > 0) {
		/* limit the size of t_malloc()s */
		if (data_size > DECODE_BLOCK_SIZE)
			data_size = DECODE_BLOCK_SIZE;
		pos = data_size;

		t_push();
		pool = pool_datastack_create();
		if (ctx->content_qp) {
			decodebuf = buffer_create_static_hard(pool, data_size);
			quoted_printable_decode(data, data_size,
						&data_size, decodebuf);
		} else if (ctx->content_base64) {
			size_t size = MAX_BASE64_DECODED_SIZE(data_size);
			decodebuf = buffer_create_static_hard(pool, size);

			if (base64_decode(data, data_size,
					  &data_size, decodebuf) < 0) {
				/* corrupted base64 data, don't bother with
				   the rest of it */
				t_pop();
				break;
			}
		} else {
			decodebuf = buffer_create_const_data(pool, data,
							     data_size);
		}

		ret = message_search_body_block(ctx, decodebuf);
		t_pop();

		if (ret != 0) {
			found = ret > 0;
			break;
		}

		i_stream_skip(input, data_size);
		pos -= data_size;
	}

	i_stream_destroy(&input);

	if (ctx->translation != NULL)
		charset_to_utf8_end(&ctx->translation);
	buffer_free(ctx->decode_buf);
	return found;
}

static bool
message_body_search_init(struct body_search_context *ctx,
			 const char *key, const char *charset,
			 bool *unknown_charset_r, bool search_header)
{
	size_t key_len;

	memset(ctx, 0, sizeof(struct body_search_context));

	/* get the key uppercased */
	key = charset_to_ucase_utf8_string(charset, unknown_charset_r,
					   (const unsigned char *) key,
					   strlen(key), &key_len);
	if (key == NULL)
		return FALSE;

	ctx->key = key;
	ctx->key_len = key_len;
	ctx->charset = charset;
	ctx->unknown_charset = charset == NULL;
	ctx->search_header = search_header;

	i_assert(ctx->key_len <= SSIZE_T_MAX/sizeof(size_t));

	return TRUE;
}

static int message_body_search_ctx(struct body_search_context *ctx,
				   struct istream *input,
				   const struct message_part *part)
{
	struct part_search_context part_ctx;
	int ret;

	ret = 0;
	while (part != NULL && ret == 0) {
		i_assert(input->v_offset <= part->physical_pos);

		i_stream_skip(input, part->physical_pos - input->v_offset);

		memset(&part_ctx, 0, sizeof(part_ctx));
		part_ctx.body_ctx = ctx;
		part_ctx.ignore_header =
			part->parent == NULL && !ctx->search_header;

		t_push();

		if (message_search_header(&part_ctx, input, part)) {
			/* found / invalid search key */
			ret = 1;
		} else if (part->children != NULL) {
			/* multipart/xxx or message/rfc822 */
			if (message_body_search_ctx(ctx, input, part->children))
				ret = 1;
		} else {
			if (input->v_offset != part->physical_pos +
			    part->header_size.physical_size) {
				/* header size changed. */
				ret = -1;
			} else if (message_search_body(&part_ctx, input, part))
				ret = 1;
		}

		i_free(part_ctx.content_type);
		i_free(part_ctx.content_charset);

		t_pop();

		part = part->next;
	}

	return ret;
}

int message_body_search(const char *key, const char *charset,
			struct istream *input,
			const struct message_part *part, bool search_header,
                        enum message_body_search_error *error_r)
{
        struct body_search_context ctx;
	int ret;
	bool unknown_charset;

	if (!message_body_search_init(&ctx, key, charset, &unknown_charset,
				      search_header)) {
		*error_r = unknown_charset ?
			MESSAGE_BODY_SEARCH_ERROR_UNKNOWN_CHARSET :
                        MESSAGE_BODY_SEARCH_ERROR_INVALID_KEY;
		return -1;
	}

	if ((ret = message_body_search_ctx(&ctx, input, part)) < 0)
		*error_r = MESSAGE_BODY_SEARCH_ERROR_MESSAGE_PART_BROKEN;
	return ret;
}
