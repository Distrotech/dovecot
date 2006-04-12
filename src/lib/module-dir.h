#ifndef __MODULE_DIR_H
#define __MODULE_DIR_H

struct module {
	char *path, *name;

	void *handle;
	void (*deinit)(void);

        struct module *next;
};

/* Load modules in given directory. module_names is a space separated list of
   module names to load, or NULL to load everything. */
struct module *module_dir_load(const char *dir, const char *module_names,
			       bool require_init_funcs);
/* Call deinit() in all modules and mark them NULL so module_dir_unload()
   won't do it again. */
void module_dir_deinit(struct module *modules);
/* Unload all modules */
void module_dir_unload(struct module **modules);

void *module_get_symbol(struct module *module, const char *symbol);

/* Returns module's base name from the filename. */
const char *module_file_get_name(const char *fname);

#endif
