#ifndef __HOME_EXPAND_H
#define __HOME_EXPAND_H

/* expand ~/ or ~user/ in beginning of path. If user is unknown, the original
   path is returned without modification. */
const char *home_expand(const char *path);
/* Returns 0 if ok, -1 if user wasn't found. */
int home_try_expand(const char **path);

#endif
