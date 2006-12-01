/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "fts-squat-plugin.h"

void fts_squat_plugin_init(void)
{
	fts_backend_register(&fts_backend_squat);
}

void fts_squat_plugin_deinit(void)
{
	fts_backend_unregister(fts_backend_squat.name);
}
