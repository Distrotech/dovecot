/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "base64.h"
#include "buffer.h"
#include "charset-utf8.h"
#include "quoted-printable.h"
#include "message-parser.h"
#include "message-header-decode.h"
#include "message-header-search.h"

#include <ctype.h>

struct _HeaderSearchContext {
	Pool pool;

	unsigned char *key;
	size_t key_len;
	char *key_charset;

	Buffer *match_buf;

	unsigned int found:1;
	unsigned int last_newline:1;
	unsigned int submatch:1;
	unsigned int key_ascii:1;
	unsigned int unknown_charset:1;
};

static void search_loop(const unsigned char *data, size_t size,
			HeaderSearchContext *ctx);

HeaderSearchContext *
message_header_search_init(Pool pool, const char *key, const char *charset,
			   int *unknown_charset)
{
	HeaderSearchContext *ctx;
	size_t key_len;
	const char *p;

	ctx = p_new(pool, HeaderSearchContext, 1);
	ctx->pool = pool;

	/* get the key uppercased */
	key = charset_to_ucase_utf8_string(charset, unknown_charset,
					   key, strlen(key), &key_len);

	if (key == NULL) {
		/* invalid key */
		return NULL;
	}

	ctx->key = p_strdup(pool, key);
	ctx->key_len = key_len;
	ctx->key_charset = p_strdup(pool, charset);
	ctx->unknown_charset = charset == NULL;

	ctx->key_ascii = TRUE;
	for (p = ctx->key; *p != '\0'; p++) {
		if ((*p & 0x80) != 0) {
			ctx->key_ascii = FALSE;
			break;
		}
	}

	i_assert(ctx->key_len <= SSIZE_T_MAX/sizeof(size_t));
	ctx->match_buf = buffer_create_static_hard(pool, sizeof(size_t) *
						   ctx->key_len);
	return ctx;
}

void message_header_search_free(HeaderSearchContext *ctx)
{
	Pool pool;

	buffer_free(ctx->match_buf);

	pool = ctx->pool;
	p_free(pool, ctx->key);
	p_free(pool, ctx->key_charset);
	p_free(pool, ctx);
}

static void search_with_charset(const unsigned char *data, size_t size,
				const char *charset, HeaderSearchContext *ctx)
{
	const char *utf8_data;
	size_t utf8_size;

	if (ctx->unknown_charset) {
		/* we don't know the source charset, so assume we want to
		   match using same charsets */
		charset = NULL;
	} else if (charset != NULL && strcasecmp(charset, "x-unknown") == 0) {
		/* compare with same charset as search key. the key is already
		   in utf-8 so we can't use charset = NULL comparing. */
		charset = ctx->key_charset;
	}

	utf8_data = charset_to_ucase_utf8_string(charset, NULL,
						 data, size, &utf8_size);

	if (utf8_data == NULL) {
		/* unknown character set, or invalid data */
	} else {
		ctx->submatch = TRUE;
		search_loop(utf8_data, utf8_size, ctx);
		ctx->submatch = FALSE;
	}
}

static void search_loop(const unsigned char *data, size_t size,
			HeaderSearchContext *ctx)
{
	size_t pos, *matches, match_count, value;
	ssize_t i;
	unsigned char chr;
	int last_newline;

	matches = buffer_get_modifyable_data(ctx->match_buf, &match_count);
	match_count /= sizeof(size_t);

	last_newline = ctx->last_newline;
	for (pos = 0; pos < size; pos++) {
		chr = data[pos];

		if (!ctx->submatch) {
			if ((chr & 0x80) == 0)
				chr = i_toupper(chr);
			else if (!ctx->key_ascii && !ctx->unknown_charset) {
				/* we have non-ascii in header and key contains
				   non-ascii characters. treat the rest of the
				   header as encoded with the key's charset */
				search_with_charset(data + pos, size - pos,
						    ctx->key_charset, ctx);
				break;
			}
		}

		if (last_newline && !ctx->submatch) {
			if (!IS_LWSP(chr)) {
				/* not a long header, reset matches */
				buffer_set_used_size(ctx->match_buf, 0);
				match_count = 0;
			}
			chr = ' ';
		}
		last_newline = chr == '\n';

		if (chr == '\r' || chr == '\n')
			continue;

		for (i = match_count-1; i >= 0; i--) {
			if (ctx->key[matches[i]] == chr) {
				if (++matches[i] == ctx->key_len) {
					/* full match */
					ctx->found = TRUE;
					return;
				}
			} else {
				/* non-match */
				buffer_delete(ctx->match_buf,
					      i * sizeof(size_t),
					      sizeof(size_t));
				match_count--;
			}
		}

		if (chr == ctx->key[0]) {
			if (ctx->key_len == 1) {
				/* only one character in search key */
				ctx->found = TRUE;
				break;
			}

			value = 1;
			buffer_append(ctx->match_buf, &value, sizeof(value));
			match_count++;
		}
	}

	ctx->last_newline = last_newline;
}

static int search_block(const unsigned char *data, size_t size,
			const char *charset, void *context)
{
	HeaderSearchContext *ctx = context;

	t_push();
	if (charset != NULL) {
		/* need to convert to UTF-8 */
		search_with_charset(data, size, charset, ctx);
	} else {
		search_loop(data, size, ctx);
	}

	t_pop();
	return !ctx->found;
}

int message_header_search(const unsigned char *header_block, size_t size,
			  HeaderSearchContext *ctx)
{
	if (!ctx->found)
		message_header_decode(header_block, size, search_block, ctx);
	return ctx->found;
}

void message_header_search_reset(HeaderSearchContext *ctx)
{
	buffer_set_used_size(ctx->match_buf, 0);
	ctx->found = FALSE;
}
