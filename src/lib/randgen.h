#ifndef __RANDGEN_H
#define __RANDGEN_H

void random_fill(void *buf, size_t size);

/* may be called multiple times */
void random_init(void);
void random_deinit(void);

#endif
