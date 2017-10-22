/*
 * Copyright 2008 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can
 * be found in the LICENSE file.
 */


#ifndef SERVICE_RUNTIME_NACL_SYSCALL_HANDLERS_H__
#define SERVICE_RUNTIME_NACL_SYSCALL_HANDLERS_H__ 1

#include "native_client/src/include/nacl_base.h"
#include "native_client/src/trusted/service_runtime/include/bits/nacl_syscalls.h"

struct NaClAppThread;

struct NaClSyscallTableEntry {
  int32_t (*handler)(struct NaClAppThread *natp);
};

/* these are defined in the platform specific code */
extern struct NaClSyscallTableEntry nacl_syscall[];

void NaClSyscallTableInit(void);

// yiwen
int syscall_counter;
int nacl_syscall_invoked_times[NACL_MAX_SYSCALLS];
double nacl_syscall_execution_time[NACL_MAX_SYSCALLS];

EXTERN_C_END

#endif
