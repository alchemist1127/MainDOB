#ifndef MAINDOB_PROC_FUTEX_H
#define MAINDOB_PROC_FUTEX_H

#include "lib/types.h"

#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

#define FUTEX_OK         0
#define FUTEX_EAGAIN   (-1)
#define FUTEX_ETIMEDOUT (-2)
#define FUTEX_EFAULT   (-3)

void futex_init(void);
int  futex_wait(uint32_t uaddr, uint32_t val, uint32_t timeout_ms);
int  futex_wake(uint32_t uaddr, uint32_t count);

#endif
