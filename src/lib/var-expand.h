#ifndef VAR_EXPAND_H
#define VAR_EXPAND_H

struct var_expand_table {
	char key;
	const char *value;
};

/* Expand % variables in src and append the string in dest.
   table must end with key = 0. */
void var_expand(string_t *dest, const char *str,
		const struct var_expand_table *table);

/* Returns the actual key character for given string, ie. skip any modifiers
   that are before it. The string should be the data after the '%' character. */
char var_get_key(const char *str);

const struct var_expand_table *
var_expand_table_build(char key, const char *value, char key2, ...);

#endif
