/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "base64.h"
#include "buffer.h"
#include "quoted-printable.h"
#include "message-header-decode.h"

static int split_encoded(const unsigned char *data, size_t *size_p,
			 const char **charset, const char **encoding,
			 const unsigned char **text, size_t *text_size_r)
{
	size_t size, pos, textpos;

	size = *size_p;

	/* get charset */
	for (pos = 0; pos < size && data[pos] != '?'; pos++) ;
	if (data[pos] != '?') return FALSE;
	*charset = t_strndup(data, pos);

	/* get encoding */
	pos++;
	if (pos+2 >= size || data[pos+1] != '?')
		return FALSE;

	if (data[pos] == 'Q' || data[pos] == 'q')
		*encoding = "Q";
	else if (data[pos] == 'B' || data[pos] == 'b')
		*encoding = "B";
	else
		return FALSE;

	/* get text */
	pos += 2;
	textpos = pos;
	while (pos < size && data[pos] != '?') pos++;
	if (data[pos] != '?' || pos+1 >= size || data[pos+1] != '=')
		return FALSE;

	*text = data + textpos;
	*text_size_r = pos - textpos;
	*size_p = pos+2;
	return TRUE;
}

static int
message_header_decode_encoded(const unsigned char *data, size_t *size,
			      MessageHeaderDecodeFunc func, void *context)
{
	const unsigned char *text;
	const char *charset, *encoding;
	Buffer *decodebuf;
	size_t text_size;
	int ret;

	t_push();

	/* first split the string charset?encoding?text?= */
	if (!split_encoded(data, size, &charset, &encoding,
			   &text, &text_size)) {
		t_pop();
		return TRUE;
	}

	decodebuf = buffer_create_static_hard(data_stack_pool, text_size);

	if (*encoding == 'Q')
		quoted_printable_decode(text, text_size, NULL, decodebuf);
	else {
		if (base64_decode(text, text_size, NULL, decodebuf) < 0) {
			/* corrupted encoding */
			t_pop();
			return TRUE;
		}
	}

	ret = func(buffer_get_data(decodebuf, NULL),
		   buffer_get_used_size(decodebuf), charset, context);

	t_pop();
	return ret;
}

void message_header_decode(const unsigned char *data, size_t size,
			   MessageHeaderDecodeFunc func, void *context)
{
	size_t pos, start_pos, subsize;

	start_pos = pos = 0;
	while (pos < size) {
		if (data[pos] == '=' && pos+1 < size && data[pos+1] == '?') {
			/* encoded string beginning */
			if (pos != start_pos) {
				/* send the unencoded data so far */
				if (!func(data + start_pos, pos - start_pos,
					  NULL, context))
					return;
			}

			pos += 2;
			subsize = size - pos;
			if (!message_header_decode_encoded(data + pos, &subsize,
							   func, context))
				return;

			pos += subsize;
			start_pos = pos;
		} else {
			pos++;
		}
	}

	(void)func(data + start_pos, size - start_pos, NULL, context);
}
