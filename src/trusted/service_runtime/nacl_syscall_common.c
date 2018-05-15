/*
 * Copyright (c) 2012 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * NaCl service run-time, non-platform specific system call helper routines.
 */
#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <Python.h>
#include <string.h>
#include <unistd.h>

// yiwen
#include <time.h>

#include "native_client/src/trusted/service_runtime/nacl_syscall_common.h"

#include "native_client/src/include/nacl_assert.h"
#include "native_client/src/include/nacl_macros.h"
#include "native_client/src/include/nacl_platform.h"
#include "native_client/src/include/portability_process.h"
#include "native_client/src/include/portability_string.h"

#include "native_client/src/shared/platform/nacl_check.h"
#include "native_client/src/shared/platform/nacl_clock.h"
#include "native_client/src/shared/platform/nacl_exit.h"
#include "native_client/src/shared/platform/nacl_host_desc.h"
#include "native_client/src/shared/platform/nacl_host_dir.h"
#include "native_client/src/shared/platform/nacl_sync_checked.h"
#include "native_client/src/shared/platform/nacl_time.h"

#include "native_client/src/trusted/desc/nacl_desc_base.h"
#include "native_client/src/trusted/desc/nacl_desc_cond.h"
#include "native_client/src/trusted/desc/nacl_desc_dir.h"
#include "native_client/src/trusted/desc/nacl_desc_effector_trusted_mem.h"
#include "native_client/src/trusted/desc/nacl_desc_imc.h"
#include "native_client/src/trusted/desc/nacl_desc_imc_shm.h"
#include "native_client/src/trusted/desc/nacl_desc_invalid.h"
#include "native_client/src/trusted/desc/nacl_desc_io.h"
#include "native_client/src/trusted/desc/nacl_desc_mutex.h"
#include "native_client/src/trusted/desc/nacl_desc_semaphore.h"
#include "native_client/src/trusted/desc/nrd_xfer.h"

#include "native_client/src/trusted/fault_injection/fault_injection.h"

#include "native_client/src/trusted/service_runtime/include/bits/mman.h"
#include "native_client/src/trusted/service_runtime/include/bits/nacl_syscalls.h"
#include "native_client/src/trusted/service_runtime/include/sys/errno.h"
#include "native_client/src/trusted/service_runtime/include/sys/fcntl.h"
#include "native_client/src/trusted/service_runtime/include/sys/stat.h"

#include "native_client/src/trusted/service_runtime/include/sys/nacl_test_crash.h"
#include "native_client/src/trusted/service_runtime/internal_errno.h"

#include "native_client/src/trusted/service_runtime/nacl_app_thread.h"
#include "native_client/src/trusted/service_runtime/nacl_copy.h"
#include "native_client/src/trusted/service_runtime/nacl_globals.h"
#include "native_client/src/trusted/service_runtime/nacl_signal.h"
#include "native_client/src/trusted/service_runtime/nacl_syscall_handlers.h"
#include "native_client/src/trusted/service_runtime/nacl_text.h"
#include "native_client/src/trusted/service_runtime/nacl_thread_nice.h"
#include "native_client/src/trusted/service_runtime/nacl_tls.h"
#include "native_client/src/trusted/service_runtime/sel_ldr.h"
#include "native_client/src/trusted/service_runtime/sel_memory.h"
#include "native_client/src/trusted/service_runtime/thread_suspension.h"
#include "native_client/src/trusted/service_runtime/win/debug_exception_handler.h"

#if NACL_WINDOWS
#include "native_client/src/trusted/service_runtime/win/debug_exception_handler.h"
#include "native_client/src/shared/platform/win/xlate_system_error.h"
#endif

#include "native_client/src/trusted/validator/ncvalidate.h"
#include "native_client/src/trusted/validator/validation_metadata.h"
// yiwen
#include "native_client/src/trusted/service_runtime/env_cleanser.h"
#include "native_client/src/trusted/service_runtime/lind_syscalls.h"
#include "native_client/src/trusted/service_runtime/nacl_all_modules.h"
#include "native_client/src/trusted/service_runtime/nacl_app.h"
#include "native_client/src/trusted/service_runtime/load_file.h"

struct NaClDescQuotaInterface;

static size_t const kdefault_io_buffer_bytes_to_log = 64;

// yiwen: my data for the in-process-pipe
// char pipe_buffer[16*4096];
// char* buffer_ptr;
// int pipe_mutex; // 0: pipe is empty, ready to write, cannot read; 1: pipe is full, ready to read, cannot write.
                   // at initialization, it should be set to 0.

// yiwen: this is for debugging in fork()
int fork_mark;

static int32_t MunmapInternal(struct NaClApp *nap,
                              uintptr_t sysaddr, size_t length);

/*
 * OSX defines SIZE_T_MAX in i386/limits.h; Linux has SIZE_MAX;
 * Windows has none.
 *
 * TODO(bsy): remove when we put SIZE_T_MAX in a common header file.
 */
#if !defined(SIZE_T_MAX)
# define SIZE_T_MAX   (~(size_t) 0)
#endif

static const size_t kMaxUsableFileSize = (SIZE_T_MAX >> 1);

static INLINE size_t  size_min(size_t a, size_t b) {
  return (a < b) ? a : b;
}

static int const kKnownInvalidDescNumber = -1;

struct NaClSyscallTableEntry nacl_syscall[NACL_MAX_SYSCALLS] = {{0}};


int32_t NaClSysNotImplementedDecoder(struct NaClAppThread *natp) {
  NaClCopyDropLock(natp->nap);
  return -NACL_ABI_ENOSYS;
}

void NaClAddSyscall(int num, int32_t (*fn)(struct NaClAppThread *)) {
  if (nacl_syscall[num].handler != &NaClSysNotImplementedDecoder) {
    NaClLog(LOG_FATAL, "Duplicate syscall number %d\n", num);
  }
  nacl_syscall[num].handler = fn;
}

int32_t NaClSysNull(struct NaClAppThread *natp) {
  UNREFERENCED_PARAMETER(natp);
  return 0;
}

int32_t NaClSysBrk(struct NaClAppThread *natp,
                   uintptr_t            new_break) {
  struct NaClApp        *nap = natp->nap;
  uintptr_t             break_addr;
  int32_t               rv = -NACL_ABI_EINVAL;
  struct NaClVmmapIter  iter;
  struct NaClVmmapEntry *ent;
  struct NaClVmmapEntry *next_ent;
  uintptr_t             sys_break;
  uintptr_t             sys_new_break;
  uintptr_t             usr_last_data_page;
  uintptr_t             usr_new_last_data_page;
  uintptr_t             last_internal_data_addr;
  uintptr_t             last_internal_page;
  uintptr_t             start_new_region;
  uintptr_t             region_size;

  break_addr = nap->break_addr;

  NaClLog(3, "Entered NaClSysBrk(new_break 0x%08"NACL_PRIxPTR")\n",
          new_break);

  sys_new_break = NaClUserToSysAddr(nap, new_break);
  NaClLog(3, "sys_new_break 0x%08"NACL_PRIxPTR"\n", sys_new_break);

  if (kNaClBadAddress == sys_new_break) {
    goto cleanup_no_lock;
  }
  if (NACL_SYNC_OK != NaClMutexLock(&nap->mu)) {
    NaClLog(LOG_ERROR, "Could not get app lock for 0x%08"NACL_PRIxPTR"\n",
            (uintptr_t) nap);
    goto cleanup_no_lock;
  }
  if (new_break < nap->data_end) {
    NaClLog(4, "new_break before data_end (0x%"NACL_PRIxPTR")\n",
            nap->data_end);
    goto cleanup;
  }
  if (new_break <= nap->break_addr) {
    /* freeing memory */
    NaClLog(4, "new_break before break (0x%"NACL_PRIxPTR"); freeing\n",
            nap->break_addr);
    nap->break_addr = new_break;
    break_addr = new_break;
  } else {
    /*
     * See if page containing new_break is in mem_map; if so, we are
     * essentially done -- just update break_addr.  Otherwise, we
     * extend the VM map entry from the page containing the current
     * break to the page containing new_break.
     */

    sys_break = NaClUserToSys(nap, nap->break_addr);

    usr_last_data_page = (nap->break_addr - 1) >> NACL_PAGESHIFT;

    usr_new_last_data_page = (new_break - 1) >> NACL_PAGESHIFT;

    last_internal_data_addr = NaClRoundAllocPage(new_break) - 1;
    last_internal_page = last_internal_data_addr >> NACL_PAGESHIFT;

    NaClLog(4, ("current break sys addr 0x%08"NACL_PRIxPTR", "
                "usr last data page 0x%"NACL_PRIxPTR"\n"),
            sys_break, usr_last_data_page);
    NaClLog(4, "new break usr last data page 0x%"NACL_PRIxPTR"\n",
            usr_new_last_data_page);
    NaClLog(4, "last internal data addr 0x%08"NACL_PRIxPTR"\n",
            last_internal_data_addr);

    if (NULL == NaClVmmapFindPageIter(&nap->mem_map,
                                      usr_last_data_page,
                                      &iter)
        || NaClVmmapIterAtEnd(&iter)) {
      NaClLog(LOG_FATAL, ("current break (0x%08"NACL_PRIxPTR", "
                          "sys 0x%08"NACL_PRIxPTR") "
                          "not in address map\n"),
              nap->break_addr, sys_break);
    }
    ent = NaClVmmapIterStar(&iter);
    NaClLog(4, ("segment containing current break"
                ": page_num 0x%08"NACL_PRIxPTR", npages 0x%"NACL_PRIxS"\n"),
            ent->page_num, ent->npages);
    if (usr_new_last_data_page < ent->page_num + ent->npages) {
      NaClLog(4, "new break within break segment, just bumping addr\n");
      nap->break_addr = new_break;
      break_addr = new_break;
    } else {
      NaClVmmapIterIncr(&iter);
      if (!NaClVmmapIterAtEnd(&iter)
          && ((next_ent = NaClVmmapIterStar(&iter))->page_num
              <= last_internal_page)) {
        /* ran into next segment! */
        NaClLog(4,
                ("new break request of usr address "
                 "0x%08"NACL_PRIxPTR" / usr page 0x%"NACL_PRIxPTR
                 " runs into next region, page_num 0x%"NACL_PRIxPTR", "
                 "npages 0x%"NACL_PRIxS"\n"),
                new_break, usr_new_last_data_page,
                next_ent->page_num, next_ent->npages);
        goto cleanup;
      }
      NaClLog(4,
              "extending segment: page_num 0x%08"NACL_PRIxPTR", "
              "npages 0x%"NACL_PRIxS"\n",
              ent->page_num, ent->npages);
      /* go ahead and extend ent to cover, and make pages accessible */
      start_new_region = (ent->page_num + ent->npages) << NACL_PAGESHIFT;
      ent->npages = (last_internal_page - ent->page_num + 1);
      region_size = (((last_internal_page + 1) << NACL_PAGESHIFT)
                     - start_new_region);
      if (0 != NaClMprotect((void *) NaClUserToSys(nap, start_new_region),
                            region_size,
                            PROT_READ | PROT_WRITE)) {
        NaClLog(LOG_FATAL,
                ("Could not mprotect(0x%08"NACL_PRIxPTR", "
                 "0x%08"NACL_PRIxPTR", "
                 "PROT_READ|PROT_WRITE)\n"),
                start_new_region,
                region_size);
      }
      NaClLog(4, "segment now: page_num 0x%08"NACL_PRIxPTR", "
              "npages 0x%"NACL_PRIxS"\n",
              ent->page_num, ent->npages);
      nap->break_addr = new_break;
      break_addr = new_break;
    }
    /*
     * Zero out memory between old break and new break.
     */
    ASSERT(sys_new_break > sys_break);
    memset((void *) sys_break, 0, sys_new_break - sys_break);
  }



cleanup:
  NaClXMutexUnlock(&nap->mu);
cleanup_no_lock:

  /*
   * This cast is safe because the incoming value (new_break) cannot
   * exceed the user address space--even though its type (uintptr_t)
   * theoretically allows larger values.
   */
  rv = (int32_t) break_addr;

  NaClLog(3, "NaClSysBrk: returning 0x%08"NACL_PRIx32"\n", rv);
  return rv;
}

int NaClAclBypassChecks = 0;

void NaClInsecurelyBypassAllAclChecks(void) {
#ifdef  _DEBUG
  NaClLog(LOG_WARNING, "BYPASSING ALL ACL CHECKS\n");
#endif
  NaClAclBypassChecks = 1;
}

int NaClHighResolutionTimerEnabled(void) {
  return NaClAclBypassChecks;
}

/*
 * NaClOpenAclCheck: Is the NaCl app authorized to open this file?  The
 * return value is syscall return convention, so 0 is success and
 * small negative numbers are negated errno values.
 */
int32_t NaClOpenAclCheck(struct NaClApp *nap,
                         char const     *path,
                         int            flags,
                         int            mode) {
  /*
   * TODO(bsy): provide some minimal authorization check, based on
   * whether a debug flag is set; eventually provide a data-driven
   * authorization configuration mechanism, perhaps persisted via
   * gears.  need GUI for user configuration, as well as designing an
   * appropriate language (with sufficient expressiveness), however.
   */
  NaClLog(1, "NaClOpenAclCheck(0x%08"NACL_PRIxPTR", %s, 0%o, 0%o)\n",
          (uintptr_t) nap, path, flags, mode);
  if (3 < NaClLogGetVerbosity()) {
    NaClLog(0, "O_ACCMODE: 0%o\n", flags & NACL_ABI_O_ACCMODE);
    NaClLog(0, "O_RDONLY = %d\n", NACL_ABI_O_RDONLY);
    NaClLog(0, "O_WRONLY = %d\n", NACL_ABI_O_WRONLY);
    NaClLog(0, "O_RDWR   = %d\n", NACL_ABI_O_RDWR);
#define FLOG(VAR, BIT) do {\
      NaClLog(1, "%s: %s\n", #BIT, (VAR & BIT) ? "yes" : "no");\
    } while (0)
    FLOG(flags, NACL_ABI_O_CREAT);
    FLOG(flags, NACL_ABI_O_TRUNC);
    FLOG(flags, NACL_ABI_O_APPEND);
#undef FLOG
  }
  if (NaClAclBypassChecks) {
    return 0;
  }
  return -NACL_ABI_EACCES;
}

/*
 * NaClStatAclCheck: Is the NaCl app authorized to stat this pathname?  The
 * return value is syscall return convention, so 0 is success and
 * small negative numbers are negated errno values.
 *
 * This is primarily for debug use.  File access should be through
 * SRPC-based file servers.
 */
int32_t NaClStatAclCheck(struct NaClApp *nap,
                         char const     *path) {
  NaClLog(2,
          "NaClStatAclCheck(0x%08"NACL_PRIxPTR", %s)\n", (uintptr_t) nap, path);
  if (NaClAclBypassChecks) {
    return 0;
  }
  return -NACL_ABI_EACCES;
}

int32_t NaClIoctlAclCheck(struct NaClApp  *nap,
                          struct NaClDesc *ndp,
                          int             request,
                          void            *arg) {
  NaClLog(2,
          ("NaClIoctlAclCheck(0x%08"NACL_PRIxPTR", 0x%08"NACL_PRIxPTR","
           " %d, 0x%08"NACL_PRIxPTR"\n"),
          (uintptr_t) nap, (uintptr_t) ndp, request, (uintptr_t) arg);
  if (NaClAclBypassChecks) {
    return 0;
  }
  return -NACL_ABI_EINVAL;
}

int32_t NaClSysGetpid(struct NaClAppThread *natp) {
  int32_t pid;
  struct NaClApp *nap = natp->nap;

  /* UNREFERENCED_PARAMETER(natp); */
  return ++nap->num_children;

  if (NaClAclBypassChecks) {
    pid = GETPID();
  } else {
    pid = -NACL_ABI_EACCES;
  }
  NaClLog(4, "NaClSysGetpid: returning %d\n", pid);

  return pid;
}

int32_t NaClSysExit(struct NaClAppThread  *natp,
                    int                   status) {
  struct NaClApp *nap = natp->nap;

  NaClLog(1, "Exit syscall handler: %d\n", status);

  (void) NaClReportExitStatus(nap, NACL_ABI_W_EXITCODE(status, 0));

  NaClAppThreadTeardown(natp);
  /* NOTREACHED */
  return -NACL_ABI_EINVAL;
}

int32_t NaClSysThreadExit(struct NaClAppThread  *natp,
                          int32_t               *stack_flag) {
  uint32_t  zero = 0;

  NaClLog(4, "NaClSysThreadExit(0x%08"NACL_PRIxPTR", "
          "0x%08"NACL_PRIxPTR"\n",
          (uintptr_t) natp,
          (uintptr_t) stack_flag);
  /*
   * NB: NaClThreads are never joinable, but the abstraction for NaClApps
   * are.
   */

  if (NULL != stack_flag) {
    NaClLog(4,
            "NaClSysThreadExit: stack_flag is %"NACL_PRIxPTR"\n",
            (uintptr_t) stack_flag);
    if (!NaClCopyOutToUser(natp->nap, (uintptr_t) stack_flag,
                           &zero, sizeof zero)) {
      NaClLog(4,
              ("NaClSysThreadExit: ignoring invalid"
               " stack_flag 0x%"NACL_PRIxPTR"\n"),
              (uintptr_t) stack_flag);
    }
  }

  NaClAppThreadTeardown(natp);
  /* NOTREACHED */
  return -NACL_ABI_EINVAL;
}

int32_t NaClSysNameService(struct NaClAppThread *natp,
                           int32_t              *desc_addr) {
  struct NaClApp *nap = natp->nap;
  int32_t   retval = -NACL_ABI_EINVAL;
  int32_t   desc;

  NaClLog(3,
          ("NaClSysNameService(0x%08"NACL_PRIxPTR","
           " 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp,
          (uintptr_t) desc_addr);

  if (!NaClCopyInFromUser(nap, &desc, (uintptr_t) desc_addr, sizeof desc)) {
    NaClLog(LOG_ERROR,
            "Invalid address argument to NaClSysNameService\n");
    retval = -NACL_ABI_EFAULT;
    goto done;
  }

  if (-1 == desc) {
    /* read */
    desc = NaClSetAvail(nap, NaClDescRef(nap->name_service_conn_cap));
    if (NaClCopyOutToUser(nap, (uintptr_t) desc_addr,
                          &desc, sizeof desc)) {
      retval = 0;
    } else {
      retval = -NACL_ABI_EFAULT;
    }
  } else {
    struct NaClDesc *desc_obj_ptr = NaClGetDesc(nap, desc);

    if (NULL == desc_obj_ptr) {
      retval = -NACL_ABI_EBADF;
      goto done;
    }
    if (NACL_DESC_CONN_CAP != NACL_VTBL(NaClDesc, desc_obj_ptr)->typeTag &&
        NACL_DESC_CONN_CAP_FD != NACL_VTBL(NaClDesc, desc_obj_ptr)->typeTag) {
      retval = -NACL_ABI_EINVAL;
      goto done;
    }
    /* write */
    NaClXMutexLock(&nap->mu);
    NaClDescUnref(nap->name_service_conn_cap);
    nap->name_service_conn_cap = desc_obj_ptr;
    NaClXMutexUnlock(&nap->mu);
    retval = 0;
  }

 done:
  return retval;
}

/* jp */
int32_t NaClSysDup(struct NaClAppThread *natp, int oldfd) {
  struct NaClApp  *nap = natp->nap;
  int             retval, newfd;
  struct NaClDesc *old_nd;

  NaClLog(3, "NaClSysDup(0x%08"NACL_PRIxPTR", %d)\n",
          (uintptr_t) natp, oldfd);
  old_nd = NaClGetDesc(nap, oldfd);
  if (!old_nd) {
    retval = -NACL_ABI_EBADF;
    goto done;
  }

  old_nd = NaClGetDesc(nap, oldfd);
  if (NULL == old_nd) {
    retval = -NACL_ABI_EBADF;
    goto done;
  }
  retval = newfd = NaClSetAvail(nap, old_nd);
  NaClSetDesc(nap, newfd, old_nd);
  fd_cage_table[nap->cage_id][newfd] = fd_cage_table[nap->cage_id][oldfd];

  if ((oldfd == 8000) || (oldfd == 8001)) {
     fd_cage_table[nap->cage_id][newfd] = oldfd;
     printf("[cage %d][fd %d] = %d \n", nap->cage_id, newfd, fd_cage_table[nap->cage_id][newfd]);
     retval = 0;
     goto done;
  }

  // yiwen: debug output
  DPRINTF("[dup2] cage %d fd max = %d \n", nap->cage_id, nap->fd);
  for (int i = 0; i < nap->fd; i++)
     DPRINTF("[dup2] cage %d fd[%d] = %d \n", nap->cage_id, i, fd_cage_table[nap->cage_id][i]);

done:
  return retval;
}

// yiwen: my dup2 implementation
int32_t NaClSysDup2(struct NaClAppThread  *natp,
                    int                   oldfd,
                    int                   newfd) {
  struct NaClApp  *nap = natp->nap;
  int             retval;

  // yiwen: debug
  DPRINTF("[dup2] Entered dup2! \n");
  DPRINTF("[dup2] cage id = %d \n", nap->cage_id);
  DPRINTF("[dup2] oldfd = %d \n", oldfd);
  DPRINTF("[dup2] newfd = %d \n", newfd);

  if ((oldfd == 8000) || (oldfd == 8001)) {
     fd_cage_table[nap->cage_id][newfd] = oldfd;
     printf("[cage %d][fd %d] = %d \n", nap->cage_id, newfd, fd_cage_table[nap->cage_id][newfd]);
     retval = 0;
     return retval;
  }

  fd_cage_table[nap->cage_id][newfd] = fd_cage_table[nap->cage_id][oldfd];

  retval = newfd;

  // yiwen: debug output
  DPRINTF("[dup2] cage %d fd max = %d \n", nap->cage_id, nap->fd);
  for (int i = 0; i < nap->fd; i++)
     DPRINTF("[dup2] cage %d fd[%d] = %d \n", nap->cage_id, i, fd_cage_table[nap->cage_id][i]);

  return newfd;
}

// yiwen: my dup3 implementation
int32_t NaClSysDup3(struct NaClAppThread  *natp,
                    int                   oldfd,
                    int                   newfd,
                    int                   flags) {
  struct NaClApp  *nap = natp->nap;
  int             retval;

  DPRINTF("[dup3] Entered dup3! \n");
  DPRINTF("[dup3] cage id = %d \n", nap->cage_id);
  DPRINTF("[dup3] oldfd = %d \n", oldfd);
  DPRINTF("[dup3] newfd = %d \n", newfd);

  if ((oldfd == 8000) || (oldfd == 8001)) {
     fd_cage_table[nap->cage_id][newfd] = oldfd;
     DPRINTF("[cage %d][fd %d] = %d \n", nap->cage_id, newfd, fd_cage_table[nap->cage_id][newfd]);
     retval = 0;
     return retval;
  }

  if (newfd < nap->fd)
     return -1;

  fd_cage_table[nap->cage_id][nap->fd] = fd_cage_table[nap->cage_id][oldfd];
  retval = nap->fd;
  nap->fd++;
  flags = flags;

  return retval;
}

static uint32_t CopyPathFromUser(struct NaClApp *nap,
                                 char           *dest,
                                 size_t         num_bytes,
                                 uintptr_t      src) {
  /*
   * NaClCopyInFromUserZStr may (try to) get bytes that is outside the
   * app's address space and generate a fault.
   */
  if (!NaClCopyInFromUserZStr(nap, dest, num_bytes, src)) {
    if (dest[0] == '\0') {
      NaClLog(LOG_ERROR, "NaClSys: invalid address for pathname\n");
      return -NACL_ABI_EFAULT;
    }

    NaClLog(LOG_ERROR, "NaClSys: pathname string too long\n");
    return -NACL_ABI_ENAMETOOLONG;
  }

  return 0;
}

int32_t NaClSysOpen(struct NaClAppThread  *natp,
                    char                  *pathname,
                    int                   flags,
                    int                   mode) {
  struct NaClApp       *nap = natp->nap;
  uint32_t             retval = -NACL_ABI_EINVAL;
  char                 path[NACL_CONFIG_PATH_MAX];
  nacl_host_stat_t     stbuf;
  int                  allowed_flags;

  // yiwen
  int                  fd_retval; // this is the virtual fd returned to the cage

  NaClLog(3, "NaClSysOpen(0x%08"NACL_PRIxPTR", "
          "0x%08"NACL_PRIxPTR", 0x%x, 0x%x)\n",
          (uintptr_t) natp, (uintptr_t) pathname, flags, mode);

  retval = CopyPathFromUser(nap, path, sizeof path, (uintptr_t) pathname);

  if (0 != retval)
    goto cleanup;

  allowed_flags = (NACL_ABI_O_ACCMODE | NACL_ABI_O_CREAT
                   | NACL_ABI_O_TRUNC | NACL_ABI_O_APPEND);
  if (0 != (flags & ~allowed_flags)) {
#ifdef  _DEBUG
    NaClLog(LOG_WARNING, "Invalid open flags 0%o, ignoring extraneous bits\n",
            flags);
#endif
    flags &= allowed_flags;
  }
  if (0 != (mode & ~0600)) {
    NaClLog(1, "IGNORING Invalid access mode bits 0%o\n", mode);
    mode &= 0600;
  }

  retval = NaClOpenAclCheck(nap, path, flags, mode);
  if (0 != retval) {
    NaClLog(3, "Open ACL check rejected \"%s\".\n", path);
    goto cleanup;
  }

  /*
   * Perform a stat to determine whether the file is a directory.
   *
   * NB: it is okay for the stat to fail, since the request may be to
   * create a new file.
   *
   * There is a race conditions here: between the stat and the
   * open-as-a-file and open-as-a-dir, the type of the object that the
   * path refers to can change.
   */
  retval = NaClHostDescStat(path, &stbuf);

  /* Windows does not have S_ISDIR(m) macro */
  if (0 == retval && S_IFDIR == (S_IFDIR & stbuf.st_mode)) {
    struct NaClHostDir  *hd;

    hd = malloc(sizeof *hd);
    if (NULL == hd) {
      retval = -NACL_ABI_ENOMEM;
      goto cleanup;
    }
    retval = NaClHostDirOpen(hd, path);
    NaClLog(1, "NaClHostDirOpen(0x%08"NACL_PRIxPTR", %s) returned %d\n",
            (uintptr_t) hd, path, retval);
    if (0 == retval) {
      retval = NaClSetAvail(nap,
                            ((struct NaClDesc *) NaClDescDirDescMake(hd)));
      NaClLog(1, "Entered directory into open file table at %d\n",
              retval);
    }
  } else {
    struct NaClHostDesc  *hd;

    hd = malloc(sizeof *hd);
    if (NULL == hd) {
      retval = -NACL_ABI_ENOMEM;
      goto cleanup;
    }
    retval = NaClHostDescOpen(hd, path, flags, mode);
    NaClLog(1,
            "NaClHostDescOpen(0x%08"NACL_PRIxPTR", %s, 0%o, 0%o) returned %d\n",
            (uintptr_t) hd, path, flags, mode, retval);
    if (0 == retval) {
      retval = NaClSetAvail(nap,
                            ((struct NaClDesc *) NaClDescIoDescMake(hd)));
      NaClLog(1, "Entered into open file table at %d\n", retval);
    }
  }
cleanup:
  // yiwen: now translate the real fds to virtual fds and return them to the cage
  // printf("[Debug!][NaClSysOpen] cage id = %d, real NaCl fd = %d, filepath = %s \n", nap->cage_id, retval, path);
  fd_cage_table[nap->cage_id][nap->fd] = retval;
  fd_retval = nap->fd;
  nap->fd++;

  // yiwen: debug
  // NaClLog(LOG_WARNING, "[NaClSysOpen] <cage> = %i; file =  %s; fd = %i \n", nap->cage_id, path, fd_retval);
  // NaClLog(LOG_WARNING, "[NaClSysOpen] fd_table_test = %i \n", fd_cage_table[1][3]);
  // printf("[Debug!][NaClSysOpen] fd = %d, filepath = %s \n", fd_retval, path);

  // yiwen: register the fd and lib_path info for the cage, in lib_table[CACHED_LIB_NUM_MAX]
  //        this will be used when trying to check if a lib has been cached in our system
  // yiwen: do sanity check for the given fd first before our registration
  if ((fd_retval >= CACHED_LIB_NUM_MAX) || (fd_retval < 0)) {
     // printf("[Error!][NaClSysOpen] Cannot register the given fd with the filepath in lib_table! fd is out of the allowed range! \n");
  }
  else {
     strncpy(nap->lib_table[fd_retval].path, path, strlen(path) + 1);
     nap->num_lib++;
     // printf("[Debug!][NaClSysOpen] num_lib = %d, filepath = %s \n", nap->num_lib, nap->lib_table[fd_retval].path);
  }

  printf("[*** Debug ***][Open] fd = %d, filepath = %s \n", fd_retval, path);

  return fd_retval;
}

int32_t NaClSysClose(struct NaClAppThread *natp,
                     int                  d) {
  struct NaClApp  *nap = natp->nap;
  int             retval = -NACL_ABI_EBADF;
  struct NaClDesc *ndp;

  // yiwen
  int fd;

  NaClLog(3, "Entered NaClSysClose(0x%08"NACL_PRIxPTR", %d)\n",
          (uintptr_t) natp, d);

  NaClFastMutexLock(&nap->desc_mu);

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];
  if ((fd == 8000) | (fd == 8001)) {
     retval = 0;
     return retval;
  }

  ndp = NaClGetDescMu(nap, fd);
  if (NULL != ndp) {
    NaClSetDescMu(nap, d, NULL);  /* Unref the desc_tbl */
  }

  NaClFastMutexUnlock(&nap->desc_mu);
  NaClLog(5, "Invoking Close virtual function of object 0x%08"NACL_PRIxPTR"\n",
          (uintptr_t) ndp);
  if (NULL != ndp) {
    NaClDescUnref(ndp);
    retval = 0;
  }

  return retval;
}

int32_t NaClSysGetdents(struct NaClAppThread *natp,
                        int                  d,
                        void                 *dirp,
                        size_t               count) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  ssize_t         getdents_ret;
  uintptr_t       sysaddr;
  struct NaClDesc *ndp;

  // yiwen
  int fd;

  NaClLog(3,
          ("Entered NaClSysGetdents(0x%08"NACL_PRIxPTR", "
           "%d, 0x%08"NACL_PRIxPTR", "
           "%"NACL_PRIdS"[0x%"NACL_PRIxS"])\n"),
          (uintptr_t) natp, d, (uintptr_t) dirp, count, count);

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];

  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  /*
   * Generic NaClCopyOutToUser is not sufficient, since buffer size
   * |count| is arbitrary and we wouldn't want to have to allocate
   * memory in trusted address space to match.
   */
  sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) dirp, count);
  if (kNaClBadAddress == sysaddr) {
    NaClLog(4, " illegal address for directory data\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup_unref;
  }

  /*
   * Clamp count to INT32_MAX to avoid the possibility of Getdents returning
   * a value that is outside the range of an int32.
   */
  if (count > INT32_MAX) {
    count = INT32_MAX;
  }
  /*
   * Grab addr space lock; getdents should not normally block, though
   * if the directory is on a networked filesystem this could, and
   * cause mmap to be slower on Windows.
   */
  NaClXMutexLock(&nap->mu);
  getdents_ret = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
                  Getdents)(ndp,
                            (void *) sysaddr,
                            count);
  NaClXMutexUnlock(&nap->mu);
  /* drop addr space lock */
  if ((getdents_ret < INT32_MIN && !NaClSSizeIsNegErrno(&getdents_ret))
      || INT32_MAX < getdents_ret) {
    /* This should never happen, because we already clamped the input count */
    NaClLog(LOG_FATAL, "Overflow in Getdents: return value is %"NACL_PRIxS,
            getdents_ret);
  } else {
    retval = (int32_t) getdents_ret;
  }
  if (retval > 0) {
    NaClLog(4, "getdents returned %d bytes\n", retval);
    NaClLog(8, "getdents result: %.*s\n", retval, (char *) sysaddr);
  } else {
    NaClLog(4, "getdents returned %d\n", retval);
  }

cleanup_unref:
  NaClDescUnref(ndp);

cleanup:
  return retval;
}

int32_t NaClSysRead(struct NaClAppThread  *natp,
                    int                   d,
                    void                  *buf,
                    size_t                count) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  ssize_t         read_result = -NACL_ABI_EINVAL;
  uintptr_t       sysaddr;
  struct NaClDesc *ndp;
  size_t          log_bytes;
  char const      *ellipsis = "";
  char* string;

  // yiwen
  int             fd;
  int             read_data_size;

  NaClLog(3,
          ("Entered NaClSysRead(0x%08"NACL_PRIxPTR", "
           "%d, 0x%08"NACL_PRIxPTR", "
           "%"NACL_PRIdS"[0x%"NACL_PRIxS"])\n"),
          (uintptr_t) natp, d, (uintptr_t) buf, count, count);

  fd = fd_cage_table[nap->cage_id][d];

  // printf("[Debug][Cage %d] From NaClSysRead: d = %d, fd = %d \n", nap->cage_id, d, fd);

  // yiwen: try to use the kernel pipe
  /*
  if (fd == 8000) {
     printf("[Debug][Cage %d][fd = 8000] NaCl Read Begins! \n", nap->cage_id);
     while (pipe_mutex != 1) {
          // NaClLog(LOG_WARNING, "[NaClSysRead] Waiting for the writer to write data! \n");
     }
     sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) buf, count);
     string = (char*)sysaddr;
     read(31, string, count);
     pipe_mutex = 0;
     retval = 0;
     printf("[Debug][Cage %d][fd = 8000] From NaCl Read Succeed! \n", nap->cage_id);
     goto cleanup;
  }

  // fd = fd_cage_table[nap->cage_id][d];
  // printf("[Debug][Cage %d][fd = %d] From NaClSysRead! \n", nap->cage_id, fd);
  */
  // yiwen: this is the read end of my pipe

  if (((nap->cage_id == 3)||(nap->cage_id == 4)||(nap->cage_id == 5)||(nap->cage_id == 6)
      ||(nap->cage_id == 7))&&(fd == 0)) {
     if (((pipe_transfer_over[0] == 1)&&(nap->cage_id == 3))||((pipe_transfer_over[1] == 1)&&(nap->cage_id == 4))
              ||((pipe_transfer_over[2] == 1)&&(nap->cage_id == 5))||((pipe_transfer_over[3] == 1)&&(nap->cage_id == 6))||((pipe_transfer_over[4] == 1)&&(nap->cage_id == 7)))  {
             retval = 0;
             goto cleanup;
     }
     while (((nap->cage_id == 3)&&(pipe_mutex[0] != 1))||((nap->cage_id == 4)&&(pipe_mutex[1] != 1))
            ||((nap->cage_id == 5)&&(pipe_mutex[2] != 1))||((nap->cage_id == 6)&&(pipe_mutex[3] != 1))
            ||((nap->cage_id == 7)&&(pipe_mutex[4] != 1))) {
     }
     sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) buf, count);
     string = (char*)sysaddr;
     // NaClLog(LOG_WARNING, "[NaClSysRead] string = %s \n", buffer_ptr);
     if (nap->cage_id == 3) {
        memcpy(string, pipe_buffer[0], count);
        pipe_mutex[0] = 0;
     }
     if (nap->cage_id == 4) {
        memcpy(string, pipe_buffer[1], count);
        pipe_mutex[1] = 0;
     }
     if (nap->cage_id == 5) {
        memcpy(string, pipe_buffer[2], count);
        pipe_mutex[2] = 0;
     }
     if (nap->cage_id == 6) {
        memcpy(string, pipe_buffer[3], count);
        pipe_mutex[3] = 0;
     }
     if (nap->cage_id == 7) {
        memcpy(string, pipe_buffer[4], count);
        pipe_mutex[4] = 0;
     }

     // printf("[Debug][Cage %d] From NaCl Read Succeed! \n", nap->cage_id);
     // printf("[Debug][Cage %d] From NaCl Read Data: \n %s \n", nap->cage_id, string);
     read_data_size = strlen(string);
     // printf("[Debug][Cage %d] From NaCl Read Data Size: %d \n", nap->cage_id, read_data_size);
     retval = read_data_size;
     goto cleanup;
  }

  // yiwen
  // fd = fd_cage_table[nap->cage_id][d];

  // NaClLog(LOG_WARNING, "[NaClSysRead] <cage> = %i; fd = %i \n", nap->cage_id, fd);

  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) buf, count);
  if (kNaClBadAddress == sysaddr) {
    NaClDescUnref(ndp);
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  /*
   * The maximum length for read and write is INT32_MAX--anything larger and
   * the return value would overflow. Passing larger values isn't an error--
   * we'll just clamp the request size if it's too large.
   */
  if (count > INT32_MAX) {
    count = INT32_MAX;
  }

  NaClVmIoWillStart(nap,
                    (uint32_t) (uintptr_t) buf,
                    (uint32_t) (((uintptr_t) buf) + count - 1));
  read_result = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
                 Read)(ndp, (void *) sysaddr, count);
  NaClVmIoHasEnded(nap,
                    (uint32_t) (uintptr_t) buf,
                    (uint32_t) (((uintptr_t) buf) + count - 1));
  if (read_result > 0) {
    NaClLog(4, "read returned %"NACL_PRIdS" bytes\n", read_result);
    log_bytes = (size_t) read_result;
    if (log_bytes > INT32_MAX) {
      log_bytes = INT32_MAX;
      ellipsis = "...";
    }
    if (NaClLogGetVerbosity() < 10) {
      if (log_bytes > kdefault_io_buffer_bytes_to_log) {
        log_bytes = kdefault_io_buffer_bytes_to_log;
        ellipsis = "...";
      }
    }
    NaClLog(8, "read result: %.*s%s\n",
            (int) log_bytes, (char *) sysaddr, ellipsis);
  } else {
    NaClLog(4, "read returned %"NACL_PRIdS"\n", read_result);
  }
  NaClDescUnref(ndp);

  /* This cast is safe because we clamped count above.*/
  retval = (int32_t) read_result;
cleanup:
  return retval;
}

int32_t NaClSysWrite(struct NaClAppThread *natp,
                     int                  d,
                     void                 *buf,
                     size_t               count) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  ssize_t         write_result = -NACL_ABI_EINVAL;
  uintptr_t       sysaddr;
  char const      *ellipsis = "";
  struct NaClDesc *ndp;
  size_t          log_bytes;
  char* string;

  // yiwen
  int             fd;
  int		  write_data_size;

  NaClLog(3,
          "Entered NaClSysWrite(0x%08"NACL_PRIxPTR", "
          "%d, 0x%08"NACL_PRIxPTR", "
          "%"NACL_PRIdS"[0x%"NACL_PRIxS"])\n",
          (uintptr_t) natp, d, (uintptr_t) buf, count, count);

  fd = fd_cage_table[nap->cage_id][d];

  // printf("[Debug][Cage %d] From NaClSysWrite: d = %d, fd = %d \n", nap->cage_id, d, fd);

  // yiwen: try to use the kernel pipe
  /*
  if (fd == 8001) {
     printf("[Debug][Cage %d][fd = 8001] NaCl Write Begins! \n", nap->cage_id);
     while (pipe_mutex != 0) {
          // NaClLog(LOG_WARNING, "[NaClSysWrite] Waiting for the reader to read data! \n");
     }
     sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) buf, count);
     string = (char*)sysaddr;
     write(32, string, count);
     pipe_mutex = 1;
     retval = 0;
     printf("[Debug][Cage %d][fd = 8001] From NaCl Write Succeed! \n", nap->cage_id);
     goto cleanup;
  }
  */
  // yiwen: this is the write end of my pipe

  if (((nap->cage_id == 2)||(nap->cage_id == 3)||(nap->cage_id == 4)
      ||(nap->cage_id == 5)||(nap->cage_id == 6))&&(fd == 1)) {
     // printf("[Debug][Cage %d] From NaCl Write. \n", nap->cage_id);
     while (((nap->cage_id == 2)&&(pipe_mutex[0] != 0))||((nap->cage_id == 3)&&(pipe_mutex[1] != 0))
            ||((nap->cage_id == 4)&&(pipe_mutex[2] != 0))||((nap->cage_id == 5)&&(pipe_mutex[3] != 0))
            ||((nap->cage_id == 6)&&(pipe_mutex[4] != 0))) {
          // NaClLog(LOG_WARNING, "[NaClSysWrite] Waiting for the reader to read data! \n");
     }
     sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) buf, count);
     string = (char*)sysaddr;
     // NaClLog(LOG_WARNING, "[NaClSysWrite] string = %s \n", string);
     if (nap->cage_id == 2) {
        memcpy(pipe_buffer[0], string, count);
        pipe_mutex[0] = 1;
        pipe_transfer_over[0] = 1;
     }
     if (nap->cage_id == 3) {
        memcpy(pipe_buffer[1], string, count);
        pipe_mutex[1] = 1;
        pipe_transfer_over[1] = 1;
     }
     if (nap->cage_id == 4) {
        memcpy(pipe_buffer[2], string, count);
        pipe_mutex[2] = 1;
        pipe_transfer_over[2] = 1;
     }
     if (nap->cage_id == 5) {
        memcpy(pipe_buffer[3], string, count);
        pipe_mutex[3] = 1;
        pipe_transfer_over[3] = 1;
     }
     if (nap->cage_id == 6) {
        memcpy(pipe_buffer[4], string, count);
        pipe_mutex[4] = 1;
        pipe_transfer_over[4] = 1;
     }

     // pipe_mutex = 1; // the buffer is full, after an immediate write

     // printf("[Debug][Cage %d] From NaCl Write Succeed! \n", nap->cage_id);
     // printf("[Debug][Cage %d] From NaCl Write Data: \n %s \n", nap->cage_id, string);
     write_data_size = strlen(string);
     // printf("[Debug][Cage %d] From NaCl Write Data Size: %d \n", nap->cage_id, write_data_size);
     // pipe_transfer_over = 1;
     retval = write_data_size;
     goto cleanup;
  }

  // yiwen
  // fd = fd_cage_table[nap->cage_id][d];

  ndp = NaClGetDesc(nap, fd);
  NaClLog(4, " ndp = %"NACL_PRIxPTR"\n", (uintptr_t) ndp);
  if (NULL == ndp) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) buf, count);
  if (kNaClBadAddress == sysaddr) {
    NaClDescUnref(ndp);
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  log_bytes = count;
  if (log_bytes > INT32_MAX) {
    log_bytes = INT32_MAX;
    ellipsis = "...";
  }
  if (NaClLogGetVerbosity() < 10) {
    if (log_bytes > kdefault_io_buffer_bytes_to_log) {
      log_bytes = kdefault_io_buffer_bytes_to_log;
      ellipsis = "...";
    }
  }
  NaClLog(8, "In NaClSysWrite(%d, %.*s%s, %"NACL_PRIdS")\n",
          d, (int) log_bytes, (char *) sysaddr, ellipsis, count);

  /*
   * The maximum length for read and write is INT32_MAX--anything larger and
   * the return value would overflow. Passing larger values isn't an error--
   * we'll just clamp the request size if it's too large.
   */
  if (count > INT32_MAX) {
    count = INT32_MAX;
  }

  NaClVmIoWillStart(nap,
                    (uint32_t) (uintptr_t) buf,
                    (uint32_t) (((uintptr_t) buf) + count - 1));
  write_result = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
                  Write)(ndp, (void *) sysaddr, count);
  NaClVmIoHasEnded(nap,
                   (uint32_t) (uintptr_t) buf,
                   (uint32_t) (((uintptr_t) buf) + count - 1));

  NaClDescUnref(ndp);

  /* This cast is safe because we clamped count above.*/
  retval = (int32_t) write_result;

cleanup:
  return retval;
}

/*
 * This implements 64-bit offsets, so we use |offp| as an in/out
 * address so we can have a 64 bit return value.
 */
int32_t NaClSysLseek(struct NaClAppThread *natp,
                     int                  d,
                     nacl_abi_off_t       *offp,
                     int                  whence) {
  struct NaClApp  *nap = natp->nap;
  nacl_abi_off_t  offset;
  nacl_off64_t    retval64;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *ndp;
  // yiwen
  int             fd;

  NaClLog(3,
          ("Entered NaClSysLseek(0x%08"NACL_PRIxPTR", %d,"
           " 0x%08"NACL_PRIxPTR", %d)\n"),
          (uintptr_t) natp, d, (uintptr_t) offp, whence);

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];

  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  if (!NaClCopyInFromUser(nap, &offset, (uintptr_t) offp, sizeof offset)) {
    retval = -NACL_ABI_EFAULT;
    goto cleanup_unref;
  }
  NaClLog(4, "offset 0x%08"NACL_PRIxNACL_OFF"\n", offset);

  retval64 = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
              Seek)(ndp, (nacl_off64_t) offset, whence);
  if (NaClOff64IsNegErrno(&retval64)) {
    retval = (int32_t) retval64;
  } else {
    if (NaClCopyOutToUser(nap, (uintptr_t) offp, &retval64, sizeof retval64)) {
      retval = 0;
    } else {
      NaClLog(LOG_FATAL,
              "NaClSysLseek: in/out ptr became invalid at copyout?\n");
    }
  }
cleanup_unref:
  NaClDescUnref(ndp);
cleanup:
  return retval;
}

int32_t NaClSysIoctl(struct NaClAppThread *natp,
                     int                  d,
                     int                  request,
                     void                 *arg) {
  struct NaClApp  *nap = natp->nap;
  int             retval = -NACL_ABI_EINVAL;
  uintptr_t       sysaddr;
  struct NaClDesc *ndp;
  int             fd;

  NaClLog(3,
          ("Entered NaClSysIoctl(0x%08"NACL_PRIxPTR
           ", %d, %d, 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp, d, request,
          (uintptr_t) arg);
  /*
   * Note that NaClUserToSysAddrRange is not feasible right now, since
   * the size of the arg argument depends on the request.  We do not
   * have an enumeration of allowed ioctl requests yet.
   *
   * Furthermore, some requests take no arguments, so sysaddr might
   * end up being kNaClBadAddress and that is perfectly okay.
   */
  sysaddr = NaClUserToSysAddr(nap, (uintptr_t) arg);
  /*
   ****************************************
   * NOTE: sysaddr may be kNaClBadAddress *
   ****************************************
   */

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];

  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    NaClLog(4, "bad desc\n");
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = NaClIoctlAclCheck(nap, ndp, request, arg);
  if (0 != retval) {
    NaClLog(3, "Ioctl ACL check rejected descriptor %d\n", d);
    goto cleanup_unref;
  }

  /*
   * We need a virtual function that, given request, returns max
   * anticipated buffer size so we can do the right thing wrt VM locks
   * if the ioctl might be blocking.  For now, we assume that ioctls
   * aren't.  Since we have at least 1 guard page, even if |arg|
   * points to near the end of the address space, we should be fine
   * for reasonable sizes of arguments from the point of view of
   * staying within the untrusted address space.
   */
  NaClXMutexLock(&nap->mu);
  retval = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
            Ioctl)(ndp, request, (void *) sysaddr);
  NaClXMutexUnlock(&nap->mu);
cleanup_unref:
  NaClDescUnref(ndp);
cleanup:
  return retval;
}


int32_t NaClSysFstat(struct NaClAppThread *natp,
                     int                  d,
                     struct nacl_abi_stat *nasp) {
  struct NaClApp        *nap = natp->nap;
  int32_t               retval = -NACL_ABI_EINVAL;
  struct NaClDesc       *ndp;
  struct nacl_abi_stat  result;
  int                   fd;

  NaClLog(3,
          ("Entered NaClSysFstat(0x%08"NACL_PRIxPTR
           ", %d, 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp,
          d, (uintptr_t) nasp);

  NaClLog(4,
          " sizeof(struct nacl_abi_stat) = %"NACL_PRIdS" (0x%"NACL_PRIxS")\n",
          sizeof *nasp, sizeof *nasp);

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];

  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    NaClLog(4, "bad desc\n");
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
            Fstat)(ndp, &result);
  if (0 == retval) {
    if (!NaClCopyOutToUser(nap, (uintptr_t) nasp,
                           &result, sizeof result)) {
      retval = -NACL_ABI_EFAULT;
    }
  }

  NaClDescUnref(ndp);
cleanup:
  return retval;
}

int32_t NaClSysStat(struct NaClAppThread  *natp,
                    const char            *pathname,
                    struct nacl_abi_stat  *buf) {
  struct NaClApp      *nap = natp->nap;
  int32_t             retval = -NACL_ABI_EINVAL;
  char                path[NACL_CONFIG_PATH_MAX];
  nacl_host_stat_t    stbuf;

  NaClLog(3,
          ("Entered NaClSysStat(0x%08"NACL_PRIxPTR", 0x%08"NACL_PRIxPTR","
           " 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp, (uintptr_t) pathname, (uintptr_t) buf);

  retval = CopyPathFromUser(nap, path, sizeof path, (uintptr_t) pathname);
  if (0 != retval)
    goto cleanup;

  retval = NaClStatAclCheck(nap, path);
  if (0 != retval)
    goto cleanup;

  /*
   * Perform a host stat.
   */
  retval = NaClHostDescStat(path, &stbuf);
  if (0 == retval) {
    struct nacl_abi_stat abi_stbuf;

    retval = NaClAbiStatHostDescStatXlateCtor(&abi_stbuf,
                                              &stbuf);
    if (!NaClCopyOutToUser(nap, (uintptr_t) buf,
                           &abi_stbuf, sizeof abi_stbuf)) {
      retval = -NACL_ABI_EFAULT;
    }
  }
cleanup:
  return retval;
}

int32_t NaClSysMkdir(struct NaClAppThread *natp,
                     uint32_t             pathname,
                     int                  mode) {
  struct NaClApp *nap = natp->nap;
  char           path[NACL_CONFIG_PATH_MAX];
  int32_t        retval = -NACL_ABI_EINVAL;

  if (!NaClAclBypassChecks) {
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  retval = CopyPathFromUser(nap, path, sizeof path, pathname);
  if (0 != retval)
    goto cleanup;

  retval = NaClHostDescMkdir(path, mode);
cleanup:
  return retval;
}

int32_t NaClSysRmdir(struct NaClAppThread *natp,
                     uint32_t             pathname) {
  struct NaClApp *nap = natp->nap;
  char           path[NACL_CONFIG_PATH_MAX];
  int32_t        retval = -NACL_ABI_EINVAL;

  if (!NaClAclBypassChecks) {
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  retval = CopyPathFromUser(nap, path, sizeof path, pathname);
  if (0 != retval)
    goto cleanup;

  retval = NaClHostDescRmdir(path);
cleanup:
  return retval;
}

int32_t NaClSysChdir(struct NaClAppThread *natp,
                     uint32_t             pathname) {
  struct NaClApp *nap = natp->nap;
  char           path[NACL_CONFIG_PATH_MAX];
  int32_t        retval = -NACL_ABI_EINVAL;

  if (!NaClAclBypassChecks) {
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  retval = CopyPathFromUser(nap, path, sizeof path, pathname);
  if (0 != retval)
    goto cleanup;

  retval = NaClHostDescChdir(path);
cleanup:
  return retval;
}

int32_t NaClSysGetcwd(struct NaClAppThread *natp,
                      uint32_t             buffer,
                      int                  len) {
  struct NaClApp *nap = natp->nap;
  int32_t        retval = -NACL_ABI_EINVAL;
  char           path[NACL_CONFIG_PATH_MAX];

  if (!NaClAclBypassChecks) {
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  if (len >= NACL_CONFIG_PATH_MAX)
    len = NACL_CONFIG_PATH_MAX - 1;

  retval = NaClHostDescGetcwd(path, len);
  if (retval != 0)
    goto cleanup;

  if (!NaClCopyOutToUser(nap, buffer, &path, strlen(path) + 1))
    retval = -NACL_ABI_EFAULT;

cleanup:
  return retval;
}

int32_t NaClSysUnlink(struct NaClAppThread *natp,
                      uint32_t             pathname) {
  struct NaClApp *nap = natp->nap;
  char           path[NACL_CONFIG_PATH_MAX];
  int32_t        retval = -NACL_ABI_EINVAL;

  if (!NaClAclBypassChecks) {
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  retval = CopyPathFromUser(nap, path, sizeof path, pathname);
  if (0 != retval)
    goto cleanup;

  retval = NaClHostDescUnlink(path);
cleanup:
  return retval;
}

int NaClSysCommonAddrRangeContainsExecutablePages(struct NaClApp *nap,
                                                  uintptr_t usraddr,
                                                  size_t length) {
  /*
   * NOTE: currently only trampoline and text region are executable,
   * and they are at the beginning of the address space, so this code
   * is fine.  We will probably never allow users to mark other pages
   * as executable; but if so, we will have to revisit how this check
   * is implemented.
   *
   * nap->static_text_end is a multiple of 4K, the memory protection
   * granularity.  Since this routine is used for checking whether
   * memory map adjustments / allocations -- which has 64K granularity
   * -- is okay, usraddr must be an allocation granularity value.  Our
   * callers (as of this writing) does this, but we truncate it down
   * to an allocation boundary to be sure.
   */
  UNREFERENCED_PARAMETER(length);
  usraddr = NaClTruncAllocPage(usraddr);
  return usraddr < nap->dynamic_text_end;
}

int NaClSysCommonAddrRangeInAllowedDynamicCodeSpace(struct NaClApp *nap,
                                                    uintptr_t usraddr,
                                                    size_t length) {
  uintptr_t usr_region_end = usraddr + length;

  if (usr_region_end < usraddr) {
    /* Check for unsigned addition overflow */
    return 0;
  }
  usr_region_end = NaClRoundAllocPage(usr_region_end);
  if (usr_region_end < usraddr) {
    /* 32-bit systems only, rounding caused uint32_t overflow */
    return 0;
  }
  return (nap->dynamic_text_start <= usraddr &&
          usr_region_end <= nap->dynamic_text_end);
}


/* Warning: sizeof(nacl_abi_off_t)!=sizeof(off_t) on OSX */
int32_t NaClSysMmapIntern(struct NaClApp        *nap,
                          void                  *start,
                          size_t                length,
                          int                   prot,
                          int                   flags,
                          int                   d,
                          nacl_abi_off_t        offset) {
  int                         allowed_flags;
  struct NaClDesc             *ndp;
  uintptr_t                   usraddr;
  uintptr_t                   usrpage;
  uintptr_t                   sysaddr;
  uintptr_t                   endaddr;
  int                         mapping_code;
  uintptr_t                   map_result;
  int                         holding_app_lock;
  struct nacl_abi_stat        stbuf;
  size_t                      alloc_rounded_length;
  nacl_off64_t                file_size;
  nacl_off64_t                file_bytes;
  nacl_off64_t                host_rounded_file_bytes;
  size_t                      alloc_rounded_file_bytes;
  // yiwen
  int fd;

  holding_app_lock = 0;
  ndp = NULL;

  allowed_flags = (NACL_ABI_MAP_FIXED | NACL_ABI_MAP_SHARED
                   | NACL_ABI_MAP_PRIVATE | NACL_ABI_MAP_ANONYMOUS);

  usraddr = (uintptr_t) start;

  if (0 != (flags & ~allowed_flags)) {
    NaClLog(2, "invalid mmap flags 0%o, ignoring extraneous bits\n", flags);
    flags &= allowed_flags;
  }

  if (0 != (flags & NACL_ABI_MAP_ANONYMOUS)) {
    /*
     * anonymous mmap, so backing store is just swap: no descriptor is
     * involved, and no memory object will be created to represent the
     * descriptor.
     */
    ndp = NULL;
  } else {
    // yiwen
    fd = fd_cage_table[nap->cage_id][d];
    ndp = NaClGetDesc(nap, fd);
    if (NULL == ndp) {
      map_result = -NACL_ABI_EBADF;
      goto cleanup;
    }
  }

  mapping_code = 0;
  /*
   * Check if application is trying to do dynamic code loading by
   * mmaping a file.
   */
  if (0 != (NACL_ABI_PROT_EXEC & prot) &&
      0 != (NACL_ABI_MAP_FIXED & flags) &&
      NULL != ndp &&
      NaClSysCommonAddrRangeInAllowedDynamicCodeSpace(nap, usraddr, length)) {
    if (!nap->enable_dyncode_syscalls) {
#ifdef  _DEBUG
      NaClLog(LOG_WARNING,
              "NaClSysMmap: PROT_EXEC when dyncode syscalls are disabled.\n");
#endif
      map_result = -NACL_ABI_EINVAL;
      goto cleanup;
    }
    if (0 != (NACL_ABI_PROT_WRITE & prot)) {
      NaClLog(3,
              "NaClSysMmap: asked for writable and executable code pages?!?\n");
      map_result = -NACL_ABI_EINVAL;
      goto cleanup;
    }
    mapping_code = 1;
  } else if (0 != (prot & NACL_ABI_PROT_EXEC)) {
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }

  /*
   * Starting address must be aligned to worst-case allocation
   * granularity.  (Windows.)
   */
  if (!NaClIsAllocPageMultiple(usraddr)) {
    NaClLog(2, "NaClSysMmap: address not allocation granularity aligned\n");
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  /*
   * Offset should be non-negative (nacl_abi_off_t is signed).  This
   * condition is caught when the file is stat'd and checked, and
   * offset is ignored for anonymous mappings.
   */
  if (offset < 0) {
    NaClLog(1,  /* application bug */
            "NaClSysMmap: negative file offset: %"NACL_PRIdNACL_OFF"\n",
            offset);
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  /*
   * And offset must be a multiple of the allocation unit.
   */
  if (!NaClIsAllocPageMultiple((uintptr_t) offset)) {
    NaClLog(1,
            ("NaClSysMmap: file offset 0x%08"NACL_PRIxPTR" not multiple"
             " of allocation size\n"),
            (uintptr_t) offset);
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }

  if (0 == length) {
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  alloc_rounded_length = NaClRoundAllocPage(length);
  if (alloc_rounded_length != length) {
    if (mapping_code) {
      NaClLog(3, "NaClSysMmap: length not a multiple of allocation size\n");
      map_result = -NACL_ABI_EINVAL;
      goto cleanup;
    }
    NaClLog(1,
            "NaClSysMmap: rounded length to 0x%"NACL_PRIxS"\n",
            alloc_rounded_length);
  }

  if (NULL == ndp) {
    /*
     * Note: sentinel values are bigger than the NaCl module addr space.
     */
    file_size                = kMaxUsableFileSize;
    file_bytes               = kMaxUsableFileSize;
    host_rounded_file_bytes  = kMaxUsableFileSize;
    alloc_rounded_file_bytes = kMaxUsableFileSize;
  } else {
    /*
     * We stat the file to figure out its actual size.
     *
     * This is necessary because the POSIXy interface we provide
     * allows mapping beyond the extent of a file but Windows'
     * interface does not.  We simulate the POSIX behaviour on
     * Windows.
     */
    map_result = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
                  Fstat)(ndp, &stbuf);
    if (0 != map_result) {
      goto cleanup;
    }

    /*
     * Preemptively refuse to map anything that's not a regular file or
     * shared memory segment.  Other types usually report st_size of zero,
     * which the code below will handle by just doing a dummy PROT_NONE
     * mapping for the requested size and never attempting the underlying
     * NaClDesc Map operation.  So without this check, the host OS never
     * gets the chance to refuse the mapping operation on an object that
     * can't do it.
     */
    if (!NACL_ABI_S_ISREG(stbuf.nacl_abi_st_mode) &&
        !NACL_ABI_S_ISSHM(stbuf.nacl_abi_st_mode) &&
        !NACL_ABI_S_ISSHM_SYSV(stbuf.nacl_abi_st_mode)) {
      map_result = -NACL_ABI_ENODEV;
      goto cleanup;
    }

    /*
     * BUG(bsy): there's a race between this fstat and the actual mmap
     * below.  It's probably insoluble.  Even if we fstat again after
     * mmap and compared, the mmap could have "seen" the file with a
     * different size, after which the racing thread restored back to
     * the same value before the 2nd fstat takes place.
     */
    file_size = stbuf.nacl_abi_st_size;

    if (file_size < offset) {
      map_result = -NACL_ABI_EINVAL;
      goto cleanup;
    }

    file_bytes = file_size - offset;
    NaClLog(4,
            "NaClSysMmapIntern: file_bytes 0x%016"NACL_PRIxNACL_OFF"\n",
            file_bytes);
    if ((nacl_off64_t) kMaxUsableFileSize < file_bytes) {
      host_rounded_file_bytes = kMaxUsableFileSize;
    } else {
      host_rounded_file_bytes = NaClRoundHostAllocPage((size_t) file_bytes);
    }

    ASSERT(host_rounded_file_bytes <= (nacl_off64_t) kMaxUsableFileSize);
    /*
     * We need to deal with NaClRoundHostAllocPage rounding up to zero
     * from ~0u - n, where n < 4096 or 65536 (== 1 alloc page).
     *
     * Luckily, file_bytes is at most kMaxUsableFileSize which is
     * smaller than SIZE_T_MAX, so it should never happen, but we
     * leave the explicit check below as defensive programming.
     */
    alloc_rounded_file_bytes =
      NaClRoundAllocPage((size_t) host_rounded_file_bytes);

    if (0 == alloc_rounded_file_bytes && 0 != host_rounded_file_bytes) {
      map_result = -NACL_ABI_ENOMEM;
      goto cleanup;
    }

    /*
     * NB: host_rounded_file_bytes and alloc_rounded_file_bytes can be
     * zero.  Such an mmap just makes memory (offset relative to
     * usraddr) in the range [0, alloc_rounded_length) inaccessible.
     */
  }

  /*
   * host_rounded_file_bytes is how many bytes we can map from the
   * file, given the user-supplied starting offset.  It is at least
   * one page.  If it came from a real file, it is a multiple of
   * host-OS allocation size.  it cannot be larger than
   * kMaxUsableFileSize.
   */
  if (mapping_code && (size_t) file_bytes < alloc_rounded_length) {
    NaClLog(3,
            "NaClSysMmap: disallowing partial allocation page extension for"
            " short files\n");
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  length = size_min(alloc_rounded_length, (size_t) host_rounded_file_bytes);

  /*
   * Lock the addr space.
   */
  NaClXMutexLock(&nap->mu);

  NaClVmHoleOpeningMu(nap);

  holding_app_lock = 1;

  if (0 == (flags & NACL_ABI_MAP_FIXED)) {
    /*
     * The user wants us to pick an address range.
     */
    if (0 == usraddr) {
      /*
       * Pick a hole in addr space of appropriate size, anywhere.
       * We pick one that's best for the system.
       */
      usrpage = NaClVmmapFindMapSpace(&nap->mem_map,
                                      alloc_rounded_length >> NACL_PAGESHIFT);
      NaClLog(4, "NaClSysMmap: FindMapSpace: page 0x%05"NACL_PRIxPTR"\n",
              usrpage);
      if (0 == usrpage) {
        map_result = -NACL_ABI_ENOMEM;
        goto cleanup;
      }
      usraddr = usrpage << NACL_PAGESHIFT;
      NaClLog(4, "NaClSysMmap: new starting addr: 0x%08"NACL_PRIxPTR
              "\n", usraddr);
    } else {
      /*
       * user supplied an addr, but it's to be treated as a hint; we
       * find a hole of the right size in the app's address space,
       * according to the usual mmap semantics.
       */
      usrpage = NaClVmmapFindMapSpaceAboveHint(&nap->mem_map,
                                               usraddr,
                                               (alloc_rounded_length
                                                >> NACL_PAGESHIFT));
      NaClLog(4, "NaClSysMmap: FindSpaceAboveHint: page 0x%05"NACL_PRIxPTR"\n",
              usrpage);
      if (0 == usrpage) {
        NaClLog(4, "NaClSysMmap: hint failed, doing generic allocation\n");
        usrpage = NaClVmmapFindMapSpace(&nap->mem_map,
                                        alloc_rounded_length >> NACL_PAGESHIFT);
      }
      if (0 == usrpage) {
        map_result = -NACL_ABI_ENOMEM;
        goto cleanup;
      }
      usraddr = usrpage << NACL_PAGESHIFT;
      NaClLog(4, "NaClSysMmap: new starting addr: 0x%08"NACL_PRIxPTR"\n",
              usraddr);
    }
  }

  /*
   * Validate [usraddr, endaddr) is okay.
   */
  if (usraddr >= ((uintptr_t) 1 << nap->addr_bits)) {
    NaClLog(2,
            ("NaClSysMmap: start address (0x%08"NACL_PRIxPTR") outside address"
             " space\n"),
            usraddr);
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  endaddr = usraddr + alloc_rounded_length;
  if (endaddr < usraddr) {
    NaClLog(0,
            ("NaClSysMmap: integer overflow -- "
             "NaClSysMmap(0x%08"NACL_PRIxPTR",0x%"NACL_PRIxS",0x%x,0x%x,%d,"
             "0x%08"NACL_PRIxPTR"\n"),
            usraddr, length, prot, flags, d, (uintptr_t) offset);
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  /*
   * NB: we use > instead of >= here.
   *
   * endaddr is the address of the first byte beyond the target region
   * and it can equal the address space limit.  (of course, normally
   * the main thread's stack is there.)
   */
  if (endaddr > ((uintptr_t) 1 << nap->addr_bits)) {
    NaClLog(2,
            ("NaClSysMmap: end address (0x%08"NACL_PRIxPTR") is beyond"
             " the end of the address space\n"),
            endaddr);
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }

  if (mapping_code) {
    NaClLog(4,
            "NaClSysMmap: PROT_EXEC requested, usraddr 0x%08"NACL_PRIxPTR
            ", length %"NACL_PRIxS"\n",
            usraddr, length);
    if (!NACL_FI("MMAP_BYPASS_DESCRIPTOR_SAFETY_CHECK",
                 NaClDescIsSafeForMmap(ndp),
                 1)) {
      NaClLog(4, "NaClSysMmap: descriptor not blessed\n");
      map_result = -NACL_ABI_EINVAL;
      goto cleanup;
    }
    NaClLog(4, "NaClSysMmap: allowed\n");
  } else if (NaClSysCommonAddrRangeContainsExecutablePages(nap,
                                                           usraddr,
                                                           length)) {
    NaClLog(2, "NaClSysMmap: region contains executable pages\n");
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }

  NaClVmIoPendingCheck_mu(nap,
                          (uint32_t) usraddr,
                          (uint32_t) (usraddr + length - 1));

  /*
   * Force NACL_ABI_MAP_FIXED, since we are specifying address in NaCl
   * app address space.
   */
  flags |= NACL_ABI_MAP_FIXED;

  /*
   * Turn off PROT_EXEC -- normal user mmapped pages should not be
   * executable.  This is primarily for the service runtime's own
   * bookkeeping -- prot is used in NaClVmmapAddWithOverwrite and will
   * be needed for remapping data pages on Windows if page protection
   * is set to PROT_NONE and back.
   *
   * NB: we've captured the notion of mapping executable memory for
   * dynamic library loading etc in mapping_code, so when we do map
   * text we will explicitly OR in NACL_ABI_PROT_EXEC as needed.
   */
  prot &= ~NACL_ABI_PROT_EXEC;

  /*
   * Exactly one of NACL_ABI_MAP_SHARED and NACL_ABI_MAP_PRIVATE is set.
   */
  if ((0 == (flags & NACL_ABI_MAP_SHARED)) ==
      (0 == (flags & NACL_ABI_MAP_PRIVATE))) {
    map_result = -NACL_ABI_EINVAL;
    goto cleanup;
  }

  sysaddr = NaClUserToSys(nap, usraddr);

  /* [0, length) */
  if (length > 0) {
    if (NULL == ndp) {
      NaClLog(4,
              ("NaClSysMmap: NaClDescIoDescMap(,,0x%08"NACL_PRIxPTR","
               "0x%08"NACL_PRIxS",0x%x,0x%x,0x%08"NACL_PRIxPTR")\n"),
              sysaddr, length, prot, flags, (uintptr_t) offset);
      map_result = NaClDescIoDescMapAnon(nap->effp,
                                         (void *) sysaddr,
                                         length,
                                         prot,
                                         flags,
                                         (off_t) offset);
    } else if (mapping_code) {
      /*
       * Map a read-only view in trusted memory, ask validator if
       * valid without patching; if okay, then map in untrusted
       * executable memory.  Fallback to using the dyncode_create
       * interface otherwise.
       *
       * On Windows, threads are already stopped by the
       * NaClVmHoleOpeningMu invocation above.
       *
       * For mmap, stopping threads on Windows is needed to ensure
       * that nothing gets allocated into the temporary address space
       * hole.  This would otherwise have been particularly dangerous,
       * since the hole is in an executable region.  We must abort the
       * program if some other trusted thread (or injected thread)
       * allocates into this space.  We also need interprocessor
       * interrupts to flush the icaches associated other cores, since
       * they may contain stale data.  NB: mmap with PROT_EXEC should
       * do this for us, since otherwise loading shared libraries in a
       * multithreaded environment cannot work in a portable fashion.
       * (Mutex locks only ensure dcache coherency.)
       *
       * For eventual munmap, stopping threads also involve looking at
       * their registers to make sure their %rip/%eip/%ip are not
       * inside the region being modified (impossible for initial
       * insertion).  This is needed because mmap->munmap->mmap could
       * cause problems due to scheduler races.
       *
       * Use NaClDynamicRegionCreate to mark region as allocated.
       *
       * See NaClElfFileMapSegment in elf_util.c for corresponding
       * mmap-based main executable loading.
       */
      uintptr_t image_sys_addr;
      NaClValidationStatus validator_status = NaClValidationFailed;
      struct NaClValidationMetadata metadata;
      int sys_ret;  /* syscall return convention */
      int ret;

      NaClLog(4, "NaClSysMmap: checking descriptor type\n");
      if (NACL_VTBL(NaClDesc, ndp)->typeTag != NACL_DESC_HOST_IO) {
        NaClLog(4, "NaClSysMmap: not supported type, got %d\n",
                NACL_VTBL(NaClDesc, ndp)->typeTag);
        map_result = -NACL_ABI_EINVAL;
        goto cleanup;
      }

      /*
       * First, try to mmap.  Check if target address range is
       * available.  It must be neither in use by NaClText interface,
       * nor used by previous mmap'd code.  We record mmap'd code
       * regions in the NaClText's data structures to avoid having to
       * deal with looking in two data structures.
       *
       * This mapping is PROT_READ | PROT_WRITE, MAP_PRIVATE so that
       * if validation fails in read-only mode, we can re-run the
       * validator to patch in place.
       */

      image_sys_addr = (*NACL_VTBL(NaClDesc, ndp)->
                        Map)(ndp,
                             NaClDescEffectorTrustedMem(),
                             (void *) NULL,
                             length,
                             NACL_ABI_PROT_READ | NACL_ABI_PROT_WRITE,
                             NACL_ABI_MAP_PRIVATE,
                             offset);
      if (NaClPtrIsNegErrno(&image_sys_addr)) {
        map_result = image_sys_addr;
        goto cleanup;
      }

      /* Ask validator / validation cache */
      NaClMetadataFromNaClDescCtor(&metadata, ndp);
      validator_status = NACL_FI("MMAP_FORCE_MMAP_VALIDATION_FAIL",
                                 (*nap->validator->
                                  Validate)(usraddr,
                                            (uint8_t *) image_sys_addr,
                                            length,
                                            0,  /* stubout_mode: no */
                                            1,  /* readonly_text: yes */
                                            nap->cpu_features,
                                            &metadata,
                                            nap->validation_cache),
                                 NaClValidationFailed);
      NaClLog(3, "NaClSysMmap: prot_exec, validator_status %d\n",
              validator_status);
      NaClMetadataDtor(&metadata);

      if (NaClValidationSucceeded == validator_status) {
        /*
         * Check if target address range is actually available.  It
         * must be neither in use by NaClText interface, nor used by
         * previous mmap'd code.  We record mmap'd code regions in the
         * NaClText's data structures to avoid lo both having to deal
         * with looking in two data structures.  We could do this
         * first since this is a cheaper check, but it shouldn't
         * matter since application errors ought to be rare and we
         * shouldn't optimize for error handling, and this makes the
         * code simpler (releasing a created region is more code).
         */
        NaClXMutexLock(&nap->dynamic_load_mutex);
        ret = NaClDynamicRegionCreate(nap, NaClUserToSys(nap, usraddr), length,
                                      1);
        NaClXMutexUnlock(&nap->dynamic_load_mutex);
        if (!ret) {
          NaClLog(3, "NaClSysMmap: PROT_EXEC region"
                  " overlaps other dynamic code\n");
          map_result = -NACL_ABI_EINVAL;
          goto cleanup;
        }
        /*
         * Remove scratch mapping.
         */
        NaClDescUnmapUnsafe(ndp, (void *) image_sys_addr, length);
        /*
         * We must succeed in mapping into the untrusted executable
         * space, since otherwise it would mean that the temporary
         * hole (for Windows) was filled by some other thread, and
         * that's unrecoverable.  For Linux and OSX, this should never
         * happen, since it's an atomic overmap.
         */
        NaClLog(3, "NaClSysMmap: mapping into executable memory\n");
        image_sys_addr = (*NACL_VTBL(NaClDesc, ndp)->
                          Map)(ndp,
                               nap->effp,
                               (void *) sysaddr,
                               length,
                               NACL_ABI_PROT_READ | NACL_ABI_PROT_EXEC,
                               NACL_ABI_MAP_PRIVATE | NACL_ABI_MAP_FIXED,
                               offset);
        if (image_sys_addr != sysaddr) {
          NaClLog(LOG_FATAL,
                  "NaClSysMmap: map into executable memory failed:"
                  " got 0x%"NACL_PRIxPTR"\n", image_sys_addr);
        }
        map_result = (int32_t) usraddr;
        goto cleanup;
      }

      NaClLog(3,
              "NaClSysMmap: did not validate in readonly_text mode;"
              " attempting to use dyncode interface.\n");

      if (holding_app_lock) {
        NaClVmHoleClosingMu(nap);
        NaClXMutexUnlock(&nap->mu);
      }

      if (NACL_FI("MMAP_STUBOUT_EMULATION", 0, 1)) {
        NaClLog(3, "NaClSysMmap: emulating stubout mode by touching memory\n");
        *(volatile uint8_t *) image_sys_addr =
            *(volatile uint8_t *) image_sys_addr;
      }

      /*
       * Fallback implementation.  Use the mapped memory as source for
       * the dynamic code insertion interface.
       */
      sys_ret = NaClTextDyncodeCreate(nap,
                                      (uint32_t) usraddr,
                                      (uint8_t *) image_sys_addr,
                                      (uint32_t) length,
                                      NULL);
      if (sys_ret < 0) {
        map_result = sys_ret;
      } else {
        map_result = (int32_t) usraddr;
      }

#if NACL_WINDOWS
      sys_ret = (*NACL_VTBL(NaClDesc, ndp)->
                 UnmapUnsafe)(ndp, (void *) image_sys_addr, length);
#else
      sys_ret = munmap((void *) image_sys_addr, length);
#endif
      if (0 != sys_ret) {
        NaClLog(LOG_FATAL,
                "NaClSysMmap: could not unmap text at 0x%"NACL_PRIxPTR","
                " length 0x%"NACL_PRIxS", NaCl errno %d\n",
                image_sys_addr, length, -sys_ret);
      }
      goto cleanup_no_locks;
    } else {
      /*
       * This is a fix for Windows, where we cannot pass a size that
       * goes beyond the non-page-rounded end of the file.
       */
      size_t length_to_map = size_min(length, (size_t) file_bytes);

      NaClLog(4,
              ("NaClSysMmap: (*ndp->Map)(,,0x%08"NACL_PRIxPTR","
               "0x%08"NACL_PRIxS",0x%x,0x%x,0x%08"NACL_PRIxPTR")\n"),
              sysaddr, length, prot, flags, (uintptr_t) offset);

      map_result = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
                    Map)(ndp,
                         nap->effp,
                         (void *) sysaddr,
                         length_to_map,
                         prot,
                         flags,
                         (off_t) offset);
    }
    /*
     * "Small" negative integers are errno values.  Larger ones are
     * virtual addresses.
     */
    if (NaClPtrIsNegErrno(&map_result)) {
      if ((uintptr_t) -NACL_ABI_E_MOVE_ADDRESS_SPACE == map_result) {
        NaClLog(LOG_FATAL,
                ("NaClSysMmap: Map failed, but we"
                 " cannot handle address space move, error %"NACL_PRIuS"\n"),
                (size_t) map_result);
      }
      /*
       * Propagate all other errors to user code.
       */
      goto cleanup;
    }
    if (map_result != sysaddr) {
      NaClLog(LOG_FATAL, "system mmap did not honor NACL_ABI_MAP_FIXED\n");
    }
  }
  /*
   * If we are mapping beyond the end of the file, we fill this space
   * with PROT_NONE pages.
   *
   * Windows forces us to expose a mixture of 64k and 4k pages, and we
   * expose the same mappings on other platforms.  For example,
   * suppose untrusted code requests to map 0x40000 bytes from a file
   * of extent 0x100.  We will create the following regions:
   *
   *       0-  0x100  A: Bytes from the file
   *   0x100- 0x1000  B: The rest of the 4k page is accessible but undefined
   *  0x1000-0x10000  C: The rest of the 64k page is inaccessible (PROT_NONE)
   * 0x10000-0x40000  D: Further 64k pages are also inaccessible (PROT_NONE)
   *
   * On Windows, a single MapViewOfFileEx() call creates A, B and C.
   * This call will not accept a size greater than 0x100, so we have
   * to create D separately.  The hardware requires B to be accessible
   * (whenever A is accessible), but Windows does not allow C to be
   * mapped as accessible.  This is unfortunate because it interferes
   * with how ELF dynamic linkers usually like to set up an ELF
   * object's BSS.
   */
  /* inaccessible: [length, alloc_rounded_length) */
  if (length < alloc_rounded_length) {
    /*
     * On Unix, this maps regions C and D as inaccessible.  On
     * Windows, it just maps region D; region C has already been made
     * inaccessible.
     */
    size_t map_len = alloc_rounded_length - length;
    map_result = MunmapInternal(nap, sysaddr + length, map_len);
    if (map_result != 0) {
      goto cleanup;
    }
  }

  if (alloc_rounded_length > 0) {
    NaClVmmapAddWithOverwrite(&nap->mem_map,
                              NaClSysToUser(nap, sysaddr) >> NACL_PAGESHIFT,
                              alloc_rounded_length >> NACL_PAGESHIFT,
                              prot,
                              flags,
                              ndp,
                              offset,
                              file_size);
  }

  map_result = usraddr;

 cleanup:
  if (holding_app_lock) {
    NaClVmHoleClosingMu(nap);
    NaClXMutexUnlock(&nap->mu);
  }
 cleanup_no_locks:
  if (NULL != ndp) {
    NaClDescUnref(ndp);
  }

  /*
   * Check to ensure that map_result will fit into a 32-bit value. This is
   * a bit tricky because there are two valid ranges: one is the range from
   * 0 to (almost) 2^32, the other is from -1 to -4096 (our error range).
   * For a 32-bit value these ranges would overlap, but if the value is 64-bit
   * they will be disjoint.
   */
  if (map_result > UINT32_MAX
      && !NaClPtrIsNegErrno(&map_result)) {
    NaClLog(LOG_FATAL, "Overflow in NaClSysMmap: return address is "
                       "0x%"NACL_PRIxPTR"\n", map_result);
  }
  NaClLog(3, "NaClSysMmap: returning 0x%08"NACL_PRIxPTR"\n", map_result);

  return (int32_t) map_result;
}

int32_t NaClSysMmap(struct NaClAppThread  *natp,
                    void                  *start,
                    size_t                length,
                    int                   prot,
                    int                   flags,
                    int                   d,
                    nacl_abi_off_t        *offp) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval;
  uintptr_t       sysaddr;
  nacl_abi_off_t  offset;
  //PyGILState_STATE gstate;

  NaClLog(3,
          "Entered NaClSysMmap(0x%08"NACL_PRIxPTR",0x%"NACL_PRIxS","
          "0x%x,0x%x,%d,0x%08"NACL_PRIxPTR")\n",
          (uintptr_t) start, length, prot, flags, d, (uintptr_t) offp);

  if ((nacl_abi_off_t *) 0 == offp) {
    /*
     * This warning is really targetted towards trusted code,
     * especially tests that didn't notice the argument type change.
     * Unfortunatey, zero is a common and legitimate offset value, and
     * the compiler will not complain since an automatic type
     * conversion works.
     */
#ifdef  _DEBUG
    NaClLog(LOG_WARNING,
            "NaClSysMmap: NULL pointer used"
            " for offset in/out argument\n");
#endif
    return -NACL_ABI_EINVAL;
  }

  //gstate = PyGILState_Ensure();
  sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) offp, sizeof offset);
  if (kNaClBadAddress == sysaddr) {
    NaClLog(3,
            "NaClSysMmap: offset in a bad untrusted memory location\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }
  offset = *(nacl_abi_off_t volatile *) sysaddr;

  NaClLog(4, " offset = 0x%08"NACL_PRIxNACL_OFF"\n", offset);

  retval = NaClSysMmapIntern(nap, start, length, prot, flags, d, offset);
  // yiwen: debug, output NaClSysMmapIntern key parameters
  // printf("[Debug!][NaClSysMmap] cage id = %d, mem_addr = %p, length = %zd, fd = %d, flags = %x \n", nap->cage_id, start, length, d, flags);
  // printf("[Debug!][NaClSysMmap] fd = %d, filepath = %s \n", d, nap->lib_table[d].path);
cleanup:
  //PyGILState_Release(gstate);
  return retval;
}

#if NACL_WINDOWS
static int32_t MunmapInternal(struct NaClApp *nap,
                              uintptr_t sysaddr, size_t length) {
  uintptr_t addr;
  uintptr_t endaddr = sysaddr + length;
  uintptr_t usraddr;
  for (addr = sysaddr; addr < endaddr; addr += NACL_MAP_PAGESIZE) {
    struct NaClVmmapEntry const *entry;
    uintptr_t                   page_num;
    uintptr_t                   offset;

    usraddr = NaClSysToUser(nap, addr);

    entry = NaClVmmapFindPage(&nap->mem_map, usraddr >> NACL_PAGESHIFT);
    if (NULL == entry) {
      continue;
    }
    NaClLog(3,
            ("NaClSysMunmap: addr 0x%08x, desc 0x%08"NACL_PRIxPTR"\n"),
            addr, (uintptr_t) entry->desc);

    page_num = usraddr - (entry->page_num << NACL_PAGESHIFT);
    offset = (uintptr_t) entry->offset + page_num;

    if (NULL != entry->desc &&
        offset < (uintptr_t) entry->file_size) {
      if (!UnmapViewOfFile((void *) addr)) {
        NaClLog(LOG_FATAL,
                ("MunmapInternal: UnmapViewOfFile failed to at addr"
                 " 0x%08"NACL_PRIxPTR", error %d\n"),
                addr, GetLastError());
      }
      /*
      * Fill the address space hole that we opened
      * with UnmapViewOfFile().
      */
      if (!VirtualAlloc((void *) addr, NACL_MAP_PAGESIZE, MEM_RESERVE,
                        PAGE_READWRITE)) {
        NaClLog(LOG_FATAL, "MunmapInternal: "
                "failed to fill hole with VirtualAlloc(), error %d\n",
                GetLastError());
      }
    } else {
      /*
       * Anonymous memory; we just decommit it and thus
       * make it inaccessible.
       */
      if (!VirtualFree((void *) addr,
                       NACL_MAP_PAGESIZE,
                       MEM_DECOMMIT)) {
        int error = GetLastError();
        NaClLog(LOG_FATAL,
                ("MunmapInternal: Could not VirtualFree MEM_DECOMMIT"
                 " addr 0x%08x, error %d (0x%x)\n"),
                addr, error, error);
      }
    }
    NaClVmmapRemove(&nap->mem_map,
                    usraddr >> NACL_PAGESHIFT,
                    NACL_PAGES_PER_MAP);
  }
  return 0;
}
#else
static int32_t MunmapInternal(struct NaClApp *nap,
                              uintptr_t sysaddr, size_t length) {
  UNREFERENCED_PARAMETER(nap);
  NaClLog(3, "MunmapInternal(0x%08"NACL_PRIxPTR", 0x%"NACL_PRIxS")\n",
          (uintptr_t) sysaddr, length);
  /*
   * Overwrite current mapping with inaccessible, anonymous
   * zero-filled pages, which should be copy-on-write and thus
   * relatively cheap.  Do not open up an address space hole.
   */
  if (MAP_FAILED == mmap((void *) sysaddr,
                         length,
                         PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                         -1,
                         (off_t) 0)) {
    NaClLog(4, "mmap to put in anonymous memory failed, errno = %d\n", errno);
    return -NaClXlateErrno(errno);
  }
  NaClVmmapRemove(&nap->mem_map,
                  NaClSysToUser(nap, (uintptr_t) sysaddr) >> NACL_PAGESHIFT,
                  length >> NACL_PAGESHIFT);
  return 0;
}
#endif

int32_t NaClSysMunmap(struct NaClAppThread  *natp,
                      void                  *start,
                      size_t                length) {
  struct NaClApp *nap = natp->nap;
  int32_t   retval = -NACL_ABI_EINVAL;
  uintptr_t sysaddr;
  int       holding_app_lock = 0;
  size_t    alloc_rounded_length;

  NaClLog(3, "Entered NaClSysMunmap(0x%08"NACL_PRIxPTR", "
          "0x%08"NACL_PRIxPTR", 0x%"NACL_PRIxS")\n",
          (uintptr_t) natp, (uintptr_t) start, length);

  if (!NaClIsAllocPageMultiple((uintptr_t) start)) {
    NaClLog(4, "start addr not allocation multiple\n");
    retval = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  if (0 == length) {
    /*
     * Without this check we would get the following inconsistent
     * behaviour:
     *  * On Linux, an mmap() of zero length yields a failure.
     *  * On Mac OS X, an mmap() of zero length returns no error,
     *    which would lead to a NaClVmmapUpdate() of zero pages, which
     *    should not occur.
     *  * On Windows we would iterate through the 64k pages and do
     *    nothing, which would not yield a failure.
     */
    retval = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  alloc_rounded_length = NaClRoundAllocPage(length);
  if (alloc_rounded_length != length) {
    length = alloc_rounded_length;
    NaClLog(2, "munmap: rounded length to 0x%"NACL_PRIxS"\n", length);
  }
  sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) start, length);
  if (kNaClBadAddress == sysaddr) {
    NaClLog(4, "munmap: region not user addresses\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  NaClXMutexLock(&nap->mu);

  NaClVmHoleOpeningMu(nap);

  holding_app_lock = 1;

  /*
   * User should be unable to unmap any executable pages.  We check here.
   */
  if (NaClSysCommonAddrRangeContainsExecutablePages(nap,
                                                    (uintptr_t) start,
                                                    length)) {
    NaClLog(2, "NaClSysMunmap: region contains executable pages\n");
    retval = -NACL_ABI_EINVAL;
    goto cleanup;
  }

  NaClVmIoPendingCheck_mu(nap,
                          (uint32_t) (uintptr_t) start,
                          (uint32_t) ((uintptr_t) start + length - 1));

  retval = MunmapInternal(nap, sysaddr, length);
cleanup:
  if (holding_app_lock) {
    NaClVmHoleClosingMu(nap);
    NaClXMutexUnlock(&nap->mu);
  }
  return retval;
}

#if NACL_WINDOWS
static int32_t MprotectInternal(struct NaClApp *nap,
                                uintptr_t sysaddr, size_t length, int prot) {
  uintptr_t addr;
  uintptr_t endaddr = sysaddr + length;
  uintptr_t usraddr;
  DWORD     flProtect;
  DWORD     flOldProtect;

  /*
   * VirtualProtect region cannot span allocations, all addresses must be
   * in one region of memory returned from VirtualAlloc or VirtualAllocEx.
   */
  for (addr = sysaddr; addr < endaddr; addr += NACL_MAP_PAGESIZE) {
    struct NaClVmmapEntry const *entry;
    uintptr_t                   page_num;
    uintptr_t                   offset;

    usraddr = NaClSysToUser(nap, addr);

    entry = NaClVmmapFindPage(&nap->mem_map, usraddr >> NACL_PAGESHIFT);
    if (NULL == entry) {
      continue;
    }
    NaClLog(3, "MprotectInternal: addr 0x%08x, desc 0x%08"NACL_PRIxPTR"\n",
            addr, (uintptr_t) entry->desc);

    page_num = usraddr - (entry->page_num << NACL_PAGESHIFT);
    offset = (uintptr_t) entry->offset + page_num;

    if (NULL == entry->desc) {
      flProtect = NaClflProtectMap(prot);

      /* Change the page protection */
      if (!VirtualProtect((void *) addr,
                          NACL_MAP_PAGESIZE,
                          flProtect,
                          &flOldProtect)) {
        int error = GetLastError();
        NaClLog(LOG_FATAL, "MprotectInternal: "
                "failed to change the memory protection with VirtualProtect,"
                " addr 0x%08x, error %d (0x%x)\n",
                addr, error, error);
        return -NaClXlateSystemError(error);
      }
    } else if (offset < (uintptr_t) entry->file_size) {
      nacl_off64_t  file_bytes;
      size_t        chunk_size;
      size_t        rounded_chunk_size;
      int           desc_flags;
      char const    *err_msg;

      desc_flags = (*NACL_VTBL(NaClDesc, entry->desc)->GetFlags)(entry->desc);
      NaClflProtectAndDesiredAccessMap(prot,
                                       (entry->flags
                                        & NACL_ABI_MAP_PRIVATE) != 0,
                                       (desc_flags & NACL_ABI_O_ACCMODE),
                                       /* flMaximumProtect= */ NULL,
                                       &flProtect,
                                       /* dwDesiredAccess= */ NULL,
                                       &err_msg);
      if (0 == flProtect) {
        /*
         * This shouldn't really happen since we already checked the address
         * space using NaClVmmapCheckExistingMapping, but better be safe.
         */
        NaClLog(LOG_FATAL, "MprotectInternal: %s\n", err_msg);
      }

      file_bytes = entry->file_size - offset;
      chunk_size = size_min((size_t) file_bytes, NACL_MAP_PAGESIZE);
      rounded_chunk_size = NaClRoundPage(chunk_size);

      NaClLog(4, "VirtualProtect(0x%08x, 0x%"NACL_PRIxS", %x)\n",
              addr, rounded_chunk_size, flProtect);

      /* Change the page protection */
      if (!VirtualProtect((void *) addr,
                          rounded_chunk_size,
                          flProtect,
                          &flOldProtect)) {
        int error = GetLastError();
        NaClLog(LOG_FATAL, "MprotectInternal: "
                "failed to change the memory protection with VirtualProtect()"
                " addr 0x%08x, error %d (0x%x)\n",
                addr, error, error);
        return -NaClXlateSystemError(error);
      }
    }
  }

  return 0;
}
#else
static int32_t MprotectInternal(struct NaClApp *nap,
                                uintptr_t sysaddr, size_t length, int prot) {
  uintptr_t               addr;
  uintptr_t               usraddr;
  uintptr_t               last_page_num;
  int                     host_prot;
  struct NaClVmmapIter    iter;
  struct NaClVmmapEntry   *entry;

  host_prot = NaClProtMap(prot);

  usraddr = NaClSysToUser(nap, sysaddr);
  last_page_num = (usraddr + length) >> NACL_PAGESHIFT;

  for (NaClVmmapFindPageIter(&nap->mem_map,
                             usraddr >> NACL_PAGESHIFT,
                             &iter);
       !NaClVmmapIterAtEnd(&iter) &&
         (entry = NaClVmmapIterStar(&iter))->page_num < last_page_num;
       NaClVmmapIterIncr(&iter)) {
    size_t entry_len = entry->npages << NACL_PAGESHIFT;

    usraddr = entry->page_num << NACL_PAGESHIFT;
    addr = NaClUserToSys(nap, usraddr);

    NaClLog(3, "MprotectInternal: "
            "addr 0x%08"NACL_PRIxPTR", desc 0x%08"NACL_PRIxPTR"\n",
            addr, (uintptr_t) entry->desc);

    if (NULL == entry->desc) {
      if (0 != mprotect((void *) addr, entry_len, host_prot)) {
        NaClLog(LOG_FATAL, "MprotectInternal: "
                "mprotect on anonymous memory failed, errno = %d\n", errno);
        return -NaClXlateErrno(errno);
      }
    } else if (entry->offset < entry->file_size) {
      nacl_abi_off64_t  file_bytes;
      size_t            rounded_file_bytes;
      size_t            prot_len;

      file_bytes = entry->file_size - entry->offset;
      rounded_file_bytes = NaClRoundPage((size_t) file_bytes);
      prot_len = size_min(rounded_file_bytes, entry_len);

      if (0 != mprotect((void *) addr, prot_len, host_prot)) {
        NaClLog(LOG_FATAL, "MprotectInternal: "
                "mprotect on file-backed memory failed, errno = %d\n", errno);
        return -NaClXlateErrno(errno);
      }
    }
  }

  return 0;
}
#endif

int32_t NaClSysMprotectInternal(struct NaClApp  *nap,
                                uint32_t        start,
                                size_t          length,
                                int             prot) {
  int32_t     retval = -NACL_ABI_EINVAL;
  uintptr_t   sysaddr;
  int         holding_app_lock = 0;

  if (!NaClIsAllocPageMultiple((uintptr_t) start)) {
    NaClLog(4, "mprotect: start addr not allocation multiple\n");
    retval = -NACL_ABI_EINVAL;
    goto cleanup;
  }
  length = NaClRoundAllocPage(length);
  sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) start, length);
  if (kNaClBadAddress == sysaddr) {
    NaClLog(4, "mprotect: region not user addresses\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }
  if (0 != (~(NACL_ABI_PROT_READ | NACL_ABI_PROT_WRITE) & prot)) {
    NaClLog(4, "mprotect: prot has other bits than PROT_{READ|WRITE}\n");
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  NaClXMutexLock(&nap->mu);

  holding_app_lock = 1;

  if (!NaClVmmapCheckExistingMapping(
           &nap->mem_map, NaClSysToUser(nap, sysaddr) >> NACL_PAGESHIFT,
           length >> NACL_PAGESHIFT, prot)) {
    NaClLog(4, "mprotect: no such region\n");
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  /*
   * User should be unable to change protection of any executable pages.
   */
  if (NaClSysCommonAddrRangeContainsExecutablePages(nap,
                                                    (uintptr_t) start,
                                                    length)) {
    NaClLog(2, "NaClSysMprotect: region contains executable pages\n");
    retval = -NACL_ABI_EACCES;
    goto cleanup;
  }

  NaClVmIoPendingCheck_mu(nap,
                          (uint32_t) (uintptr_t) start,
                          (uint32_t) ((uintptr_t) start + length - 1));

  retval = MprotectInternal(nap, sysaddr, length, prot);
  if (retval == 0 &&
      !NaClVmmapChangeProt(&nap->mem_map,
                           NaClSysToUser(nap, sysaddr) >> NACL_PAGESHIFT,
                           length >> NACL_PAGESHIFT,
                           prot)) {
    retval = -NACL_ABI_EACCES;
  }
cleanup:
  if (holding_app_lock) {
    NaClXMutexUnlock(&nap->mu);
  }
  return retval;
}

int32_t NaClSysMprotect(struct NaClAppThread  *natp,
                        uint32_t              start,
                        size_t                length,
                        int                   prot) {
  struct NaClApp  *nap = natp->nap;

  NaClLog(3, "Entered NaClSysMprotect(0x%08"NACL_PRIxPTR", "
          "0x%08"NACL_PRIxPTR", 0x%"NACL_PRIxS", 0x%x)\n",
          (uintptr_t) natp, (uintptr_t) start, length, prot);

  if (!NaClAclBypassChecks) {
    return -NACL_ABI_EACCES;
  }

  return NaClSysMprotectInternal(nap, start, length, prot);
}

int32_t NaClSysImcMakeBoundSock(struct NaClAppThread *natp,
                                int32_t              *sap) {
  /*
   * Create a bound socket descriptor and a socket address descriptor.
   */
  struct NaClApp              *nap = natp->nap;
  int32_t                     retval = -NACL_ABI_EINVAL;
  struct NaClDesc             *pair[2];
  int32_t                     usr_pair[2];

  NaClLog(3,
          ("Entered NaClSysImcMakeBoundSock(0x%08"NACL_PRIxPTR","
           " 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp, (uintptr_t) sap);

  retval = NaClCommonDescMakeBoundSock(pair);
  if (0 != retval) {
    goto cleanup;
  }

  usr_pair[0] = NaClSetAvail(nap, pair[0]);
  usr_pair[1] = NaClSetAvail(nap, pair[1]);
  if (!NaClCopyOutToUser(nap, (uintptr_t) sap,
                         usr_pair, sizeof usr_pair)) {
    /*
     * NB: The descriptors were briefly observable to untrusted code
     * in this window, even though the syscall had not returned yet,
     * and another thread which guesses their numbers could actually
     * use them, so the NaClDescSafeUnref inside NaClSetDesc below
     * might not actually deallocate right away.  To avoid this, we
     * could grab the descriptor lock and hold it until after the
     * copyout is done, but that imposes an ordering between the
     * descriptor lock and the VM lock which can cause problems
     * elsewhere.
     */
    NaClSetDesc(nap, usr_pair[0], NULL);
    NaClSetDesc(nap, usr_pair[1], NULL);
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  retval = 0;

cleanup:
  return retval;
}

int32_t NaClSysImcAccept(struct NaClAppThread  *natp,
                         int                   d) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *ndp;
  int             fd;

  NaClLog(3, "Entered NaClSysImcAccept(0x%08"NACL_PRIxPTR", %d)\n",
          (uintptr_t) natp, d);

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];
  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    retval = -NACL_ABI_EBADF;
  } else {
    struct NaClDesc *result_desc;
    retval = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
              AcceptConn)(ndp, &result_desc);
    if (retval == 0) {
      retval = NaClSetAvail(nap, result_desc);
    }
    NaClDescUnref(ndp);
  }

  return retval;
}

int32_t NaClSysImcConnect(struct NaClAppThread *natp,
                          int                  d) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *ndp;
  int             fd;

  NaClLog(3, "Entered NaClSysImcConnectAddr(0x%08"NACL_PRIxPTR", %d)\n",
          (uintptr_t) natp, d);

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];
  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    retval = -NACL_ABI_EBADF;
  } else {
    struct NaClDesc *result;
    retval = (*((struct NaClDescVtbl const *) ndp->base.vtbl)->
              ConnectAddr)(ndp, &result);
    if (retval == 0) {
      retval = NaClSetAvail(nap, result);
    }
    NaClDescUnref(ndp);
  }

  return retval;
}

/*
 * This function converts addresses from user addresses to system
 * addresses, copying into kernel space as needed to avoid TOCvTOU
 * races, then invokes the descriptor's SendMsg() method.
 */
int32_t NaClSysImcSendmsg(struct NaClAppThread         *natp,
                          int                          d,
                          struct NaClAbiNaClImcMsgHdr *nanimhp,
                          int                          flags) {
  struct NaClApp                *nap = natp->nap;
  int32_t                       retval = -NACL_ABI_EINVAL;
  ssize_t                       ssize_retval;
  uintptr_t                     sysaddr;
  /* copy of user-space data for validation */
  struct NaClAbiNaClImcMsgHdr   kern_nanimh;
  struct NaClAbiNaClImcMsgIoVec kern_naiov[NACL_ABI_IMC_IOVEC_MAX];
  struct NaClImcMsgIoVec        kern_iov[NACL_ABI_IMC_IOVEC_MAX];
  int32_t                       usr_desc[NACL_ABI_IMC_USER_DESC_MAX];
  /* kernel-side representatin of descriptors */
  struct NaClDesc               *kern_desc[NACL_ABI_IMC_USER_DESC_MAX];
  struct NaClImcTypedMsgHdr     kern_msg_hdr;
  struct NaClDesc               *ndp;
  size_t                        i;
  int                           fd;

  NaClLog(3,
          ("Entered NaClSysImcSendmsg(0x%08"NACL_PRIxPTR", %d,"
           " 0x%08"NACL_PRIxPTR", 0x%x)\n"),
          (uintptr_t) natp, d, (uintptr_t) nanimhp, flags);

  if (!NaClCopyInFromUser(nap, &kern_nanimh, (uintptr_t) nanimhp,
                          sizeof kern_nanimh)) {
    NaClLog(4, "NaClImcMsgHdr not in user address space\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup_leave;
  }
  /* copy before validating contents */

  /*
   * Some of these checks duplicate checks that will be done in the
   * nrd xfer library, but it is better to check before doing the
   * address translation of memory/descriptor vectors if those vectors
   * might be too long.  Plus, we need to copy and validate vectors
   * for TOCvTOU race protection, and we must prevent overflows.  The
   * nrd xfer library's checks should never fire when called from the
   * service runtime, but the nrd xfer library might be called from
   * other code.
   */
  if (kern_nanimh.iov_length > NACL_ABI_IMC_IOVEC_MAX) {
    NaClLog(4, "gather/scatter array too large\n");
    retval = -NACL_ABI_EINVAL;
    goto cleanup_leave;
  }
  if (kern_nanimh.desc_length > NACL_ABI_IMC_USER_DESC_MAX) {
    NaClLog(4, "handle vector too long\n");
    retval = -NACL_ABI_EINVAL;
    goto cleanup_leave;
  }

  if (kern_nanimh.iov_length > 0) {
    if (!NaClCopyInFromUser(nap, kern_naiov, (uintptr_t) kern_nanimh.iov,
                            (kern_nanimh.iov_length * sizeof kern_naiov[0]))) {
      NaClLog(4, "gather/scatter array not in user address space\n");
      retval = -NACL_ABI_EFAULT;
      goto cleanup_leave;
    }

    for (i = 0; i < kern_nanimh.iov_length; ++i) {
      sysaddr = NaClUserToSysAddrRange(nap,
                                       (uintptr_t) kern_naiov[i].base,
                                       kern_naiov[i].length);
      if (kNaClBadAddress == sysaddr) {
        retval = -NACL_ABI_EFAULT;
        goto cleanup_leave;
      }
      kern_iov[i].base = (void *) sysaddr;
      kern_iov[i].length = kern_naiov[i].length;
    }
  }

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];
  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    retval = -NACL_ABI_EBADF;
    goto cleanup_leave;
  }

  /*
   * make things easier for cleaup exit processing
   */
  memset(kern_desc, 0, sizeof kern_desc);
  retval = -NACL_ABI_EINVAL;

  kern_msg_hdr.iov = kern_iov;
  kern_msg_hdr.iov_length = kern_nanimh.iov_length;

  if (0 == kern_nanimh.desc_length) {
    kern_msg_hdr.ndescv = 0;
    kern_msg_hdr.ndesc_length = 0;
  } else {
    if (!NaClCopyInFromUser(nap, usr_desc, kern_nanimh.descv,
                            kern_nanimh.desc_length * sizeof usr_desc[0])) {
      retval = -NACL_ABI_EFAULT;
      goto cleanup;
    }

    for (i = 0; i < kern_nanimh.desc_length; ++i) {
      if (kKnownInvalidDescNumber == usr_desc[i]) {
        kern_desc[i] = (struct NaClDesc *) NaClDescInvalidMake();
      } else {
        /* NaCl modules are ILP32, so this works on ILP32 and LP64 systems */
        kern_desc[i] = NaClGetDesc(nap, usr_desc[i]);
      }
      if (NULL == kern_desc[i]) {
        retval = -NACL_ABI_EBADF;
        goto cleanup;
      }
    }
    kern_msg_hdr.ndescv = kern_desc;
    kern_msg_hdr.ndesc_length = kern_nanimh.desc_length;
  }
  kern_msg_hdr.flags = kern_nanimh.flags;

  /* lock user memory ranges in kern_naiov */
  for (i = 0; i < kern_nanimh.iov_length; ++i) {
    NaClVmIoWillStart(nap,
                      kern_naiov[i].base,
                      kern_naiov[i].base + kern_naiov[i].length - 1);
  }
  ssize_retval = NACL_VTBL(NaClDesc, ndp)->SendMsg(ndp, &kern_msg_hdr, flags);
  /* unlock user memory ranges in kern_naiov */
  for (i = 0; i < kern_nanimh.iov_length; ++i) {
    NaClVmIoHasEnded(nap,
                     kern_naiov[i].base,
                     kern_naiov[i].base + kern_naiov[i].length - 1);
  }

  if (NaClSSizeIsNegErrno(&ssize_retval)) {
    /*
     * NaClWouldBlock uses TSD (for both the errno-based and
     * GetLastError()-based implementations), so this is threadsafe.
     */
    if (0 != (flags & NACL_DONT_WAIT) && NaClWouldBlock()) {
      retval = -NACL_ABI_EAGAIN;
    } else if (-NACL_ABI_EMSGSIZE == ssize_retval) {
      /*
       * Allow the caller to handle the case when imc_sendmsg fails because
       * the message is too large for the system to send in one piece.
       */
      retval = -NACL_ABI_EMSGSIZE;
    } else {
      /*
       * TODO(bsy): the else case is some mysterious internal error.
       * Should we destroy the ndp or otherwise mark it as bad?  Was
       * the failure atomic?  Did it send some partial data?  Linux
       * implementation appears okay.
       */
      retval = -NACL_ABI_EIO;
    }
  } else if (ssize_retval > INT32_MAX || ssize_retval < INT32_MIN) {
    retval = -NACL_ABI_EOVERFLOW;
  } else {
    /* cast is safe due to range checks above */
    retval = (int32_t)ssize_retval;
  }

cleanup:
  for (i = 0; i < kern_nanimh.desc_length; ++i) {
    if (NULL != kern_desc[i]) {
      NaClDescUnref(kern_desc[i]);
      kern_desc[i] = NULL;
    }
  }
  NaClDescUnref(ndp);
cleanup_leave:
  NaClLog(3, "NaClSysImcSendmsg: returning %d\n", retval);
  return retval;
}

int32_t NaClSysImcRecvmsg(struct NaClAppThread         *natp,
                          int                          d,
                          struct NaClAbiNaClImcMsgHdr  *nanimhp,
                          int                          flags) {
  struct NaClApp                        *nap = natp->nap;
  int32_t                               retval = -NACL_ABI_EINVAL;
  ssize_t                               ssize_retval;
  uintptr_t                             sysaddr;
  size_t                                i;
  struct NaClDesc                       *ndp;
  struct NaClAbiNaClImcMsgHdr           kern_nanimh;
  struct NaClAbiNaClImcMsgIoVec         kern_naiov[NACL_ABI_IMC_IOVEC_MAX];
  struct NaClImcMsgIoVec                kern_iov[NACL_ABI_IMC_IOVEC_MAX];
  int32_t                               usr_desc[NACL_ABI_IMC_USER_DESC_MAX];
  struct NaClImcTypedMsgHdr             recv_hdr;
  struct NaClDesc                       *new_desc[NACL_ABI_IMC_DESC_MAX];
  nacl_abi_size_t                       num_user_desc;
  struct NaClDesc                       *invalid_desc = NULL;
  // yiwen
  int                                   fd;

  NaClLog(3,
          ("Entered NaClSysImcRecvMsg(0x%08"NACL_PRIxPTR", %d,"
           " 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp, d, (uintptr_t) nanimhp);

  /*
   * First, we validate user-supplied message headers before
   * allocating a receive buffer.
   */
  if (!NaClCopyInFromUser(nap, &kern_nanimh, (uintptr_t) nanimhp,
                          sizeof kern_nanimh)) {
    NaClLog(4, "NaClImcMsgHdr not in user address space\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup_leave;
  }
  /* copy before validating */

  if (kern_nanimh.iov_length > NACL_ABI_IMC_IOVEC_MAX) {
    NaClLog(4, "gather/scatter array too large: %"NACL_PRIdNACL_SIZE"\n",
            kern_nanimh.iov_length);
    retval = -NACL_ABI_EINVAL;
    goto cleanup_leave;
  }
  if (kern_nanimh.desc_length > NACL_ABI_IMC_USER_DESC_MAX) {
    NaClLog(4, "handle vector too long: %"NACL_PRIdNACL_SIZE"\n",
            kern_nanimh.desc_length);
    retval = -NACL_ABI_EINVAL;
    goto cleanup_leave;
  }

  if (kern_nanimh.iov_length > 0) {
    /*
     * Copy IOV array into kernel space.  Validate this snapshot and do
     * user->kernel address conversions on this snapshot.
     */
    if (!NaClCopyInFromUser(nap, kern_naiov, (uintptr_t) kern_nanimh.iov,
                            (kern_nanimh.iov_length * sizeof kern_naiov[0]))) {
      NaClLog(4, "gather/scatter array not in user address space\n");
      retval = -NACL_ABI_EFAULT;
      goto cleanup_leave;
    }
    /*
     * Convert every IOV base from user to system address, validate
     * range of bytes are really in user address space.
     */

    for (i = 0; i < kern_nanimh.iov_length; ++i) {
      sysaddr = NaClUserToSysAddrRange(nap,
                                       (uintptr_t) kern_naiov[i].base,
                                       kern_naiov[i].length);
      if (kNaClBadAddress == sysaddr) {
        NaClLog(4, "iov number %"NACL_PRIdS" not entirely in user space\n", i);
        retval = -NACL_ABI_EFAULT;
        goto cleanup_leave;
      }
      kern_iov[i].base = (void *) sysaddr;
      kern_iov[i].length = kern_naiov[i].length;
    }
  }

  if (kern_nanimh.desc_length > 0) {
    sysaddr = NaClUserToSysAddrRange(nap,
                                     (uintptr_t) kern_nanimh.descv,
                                     kern_nanimh.desc_length * sizeof(int32_t));
    if (kNaClBadAddress == sysaddr) {
      retval = -NACL_ABI_EFAULT;
      goto cleanup_leave;
    }
  }

  // yiwen
  fd = fd_cage_table[nap->cage_id][d];
  ndp = NaClGetDesc(nap, fd);
  if (NULL == ndp) {
    NaClLog(4, "receiving descriptor invalid\n");
    retval = -NACL_ABI_EBADF;
    goto cleanup_leave;
  }

  recv_hdr.iov = kern_iov;
  recv_hdr.iov_length = kern_nanimh.iov_length;

  recv_hdr.ndescv = new_desc;
  recv_hdr.ndesc_length = NACL_ARRAY_SIZE(new_desc);
  memset(new_desc, 0, sizeof new_desc);

  recv_hdr.flags = 0;  /* just to make it obvious; IMC will clear it for us */

  /* lock user memory ranges in kern_naiov */
  for (i = 0; i < kern_nanimh.iov_length; ++i) {
    NaClVmIoWillStart(nap,
                      kern_naiov[i].base,
                      kern_naiov[i].base + kern_naiov[i].length - 1);
  }
  ssize_retval = NACL_VTBL(NaClDesc, ndp)->RecvMsg(ndp, &recv_hdr, flags,
      (struct NaClDescQuotaInterface *) nap->reverse_quota_interface);
  /* unlock user memory ranges in kern_naiov */
  for (i = 0; i < kern_nanimh.iov_length; ++i) {
    NaClVmIoHasEnded(nap,
                     kern_naiov[i].base,
                     kern_naiov[i].base + kern_naiov[i].length - 1);
  }
  /*
   * retval is number of user payload bytes received and excludes the
   * header bytes.
   */
  NaClLog(3, "NaClSysImcRecvMsg: RecvMsg() returned %"NACL_PRIdS"\n",
          ssize_retval);
  if (NaClSSizeIsNegErrno(&ssize_retval)) {
    /* negative error numbers all have valid 32-bit representations,
     * so this cast is safe. */
    retval = (int32_t) ssize_retval;
    goto cleanup;
  } else if (ssize_retval > INT32_MAX || ssize_retval < INT32_MIN) {
    retval = -NACL_ABI_EOVERFLOW;
    goto cleanup;
  } else {
    /* cast is safe due to range check above */
    retval = (int32_t) ssize_retval;
  }

  /*
   * NB: recv_hdr.flags may contain NACL_ABI_MESSAGE_TRUNCATED and/or
   * NACL_ABI_HANDLES_TRUNCATED.
   */

  kern_nanimh.flags = recv_hdr.flags;

  /*
   * Now internalize the NaClHandles as NaClDesc objects.
   */
  num_user_desc = recv_hdr.ndesc_length;

  if (kern_nanimh.desc_length < num_user_desc) {
    kern_nanimh.flags |= NACL_ABI_RECVMSG_DESC_TRUNCATED;
    for (i = kern_nanimh.desc_length; i < num_user_desc; ++i) {
      NaClDescUnref(new_desc[i]);
      new_desc[i] = NULL;
    }
    num_user_desc = kern_nanimh.desc_length;
  }

  invalid_desc = (struct NaClDesc *) NaClDescInvalidMake();
  /* prepare to write out to user space the descriptor numbers */
  for (i = 0; i < num_user_desc; ++i) {
    if (invalid_desc == new_desc[i]) {
      usr_desc[i] = kKnownInvalidDescNumber;
      NaClDescUnref(new_desc[i]);
    } else {
      usr_desc[i] = NaClSetAvail(nap, new_desc[i]);
    }
    new_desc[i] = NULL;
  }
  if (0 != num_user_desc &&
      !NaClCopyOutToUser(nap, (uintptr_t) kern_nanimh.descv, usr_desc,
                         num_user_desc * sizeof usr_desc[0])) {
    NaClLog(LOG_FATAL,
            ("NaClSysImcRecvMsg: in/out ptr (descv %"NACL_PRIxPTR
             ") became invalid at copyout?\n"),
            (uintptr_t) kern_nanimh.descv);
  }

  kern_nanimh.desc_length = num_user_desc;
  if (!NaClCopyOutToUser(nap, (uintptr_t) nanimhp, &kern_nanimh,
                         sizeof kern_nanimh)) {
    NaClLog(LOG_FATAL,
            "NaClSysImcRecvMsg: in/out ptr (iov) became"
            " invalid at copyout?\n");
  }
  /* copy out updated desc count, flags */
 cleanup:
  if (retval < 0) {
    for (i = 0; i < NACL_ARRAY_SIZE(new_desc); ++i) {
      if (NULL != new_desc[i]) {
        NaClDescUnref(new_desc[i]);
        new_desc[i] = NULL;
      }
    }
  }
  NaClDescUnref(ndp);
  NaClDescSafeUnref(invalid_desc);
  NaClLog(3, "NaClSysImcRecvMsg: returning %d\n", retval);
cleanup_leave:
  return retval;
}

int32_t NaClSysImcMemObjCreate(struct NaClAppThread  *natp,
                               size_t                size) {
  struct NaClApp        *nap = natp->nap;
  int32_t               retval = -NACL_ABI_EINVAL;
  struct NaClDescImcShm *shmp;
  off_t                 size_as_off;

  NaClLog(3,
          ("Entered NaClSysImcMemObjCreate(0x%08"NACL_PRIxPTR
           " 0x%08"NACL_PRIxS")\n"),
          (uintptr_t) natp, size);

  if (0 != (size & (NACL_MAP_PAGESIZE - 1))) {
    return -NACL_ABI_EINVAL;
  }
  /*
   * TODO(bsy): policy about maximum shm object size should be
   * enforced here.
   */
  size_as_off = (off_t) size;
  if (size_as_off < 0) {
    return -NACL_ABI_EINVAL;
  }

  shmp = NULL;

  shmp = malloc(sizeof *shmp);
  if (NULL == shmp) {
    retval = -NACL_ABI_ENOMEM;
    goto cleanup;
  }

  if (!NaClDescImcShmAllocCtor(shmp, size_as_off, /* executable= */ 0)) {
    retval = -NACL_ABI_ENOMEM;  /* is this reasonable? */
    goto cleanup;
  }

  retval = NaClSetAvail(nap, (struct NaClDesc *) shmp);
  shmp = NULL;

cleanup:
  free(shmp);

  return retval;
}

int32_t NaClSysImcSocketPair(struct NaClAppThread *natp,
                             uint32_t             descs_out) {
  struct NaClApp          *nap = natp->nap;
  int32_t                 usr_pair[2];
  struct NaClDesc         *pair[2];
  int32_t                 retval;

  NaClLog(3,
          ("Entered NaClSysImcSocketPair(0x%08"NACL_PRIxPTR
           " 0x%08"NACL_PRIx32")\n"),
          (uintptr_t) natp, descs_out);

  retval = NaClCommonDescSocketPair(pair);
  if (0 != retval) {
    goto cleanup;
  }

  usr_pair[0] = NaClSetAvail(nap, pair[0]);
  usr_pair[1] = NaClSetAvail(nap, pair[1]);

  if (!NaClCopyOutToUser(nap, (uintptr_t) descs_out, usr_pair,
                         sizeof usr_pair)) {
    NaClSetDesc(nap, usr_pair[0], NULL);
    NaClSetDesc(nap, usr_pair[1], NULL);
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }
  retval = 0;

cleanup:
  return retval;
}

int32_t NaClSysTlsInit(struct NaClAppThread  *natp,
                       uint32_t              thread_ptr) {
  int32_t   retval = -NACL_ABI_EINVAL;
  uintptr_t sys_tls;

  NaClLog(3,
          ("Entered NaClSysTlsInit(0x%08"NACL_PRIxPTR
           ", 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp, (uintptr_t) thread_ptr);

  /* Verify that the address in the app's range and translated from
   * nacl module address to service runtime address - a nop on ARM
   */
  sys_tls = NaClUserToSysAddrRange(natp->nap, thread_ptr, 4);
  NaClLog(4,
          "NaClSysTlsInit: thread_ptr 0x%"NACL_PRIx32
          ", sys_tls 0x%"NACL_PRIxPTR"\n",
          thread_ptr, sys_tls);
  if (kNaClBadAddress == sys_tls) {
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  NaClTlsSetTlsValue1(natp, thread_ptr);
  retval = 0;
cleanup:
  return retval;
}

int32_t NaClSysThreadCreate(struct NaClAppThread *natp,
                            void                 *prog_ctr,
                            uint32_t             stack_ptr,
                            uint32_t             thread_ptr,
                            uint32_t             second_thread_ptr) {
  struct NaClApp *nap = natp->nap;
  int32_t     retval = -NACL_ABI_EINVAL;
  uintptr_t   sys_tls;
  uintptr_t   sys_stack;

  NaClLog(3,
          ("Entered NaClSysThreadCreate(0x%08"NACL_PRIxPTR
           " pc=0x%08"NACL_PRIxPTR", sp=0x%08"NACL_PRIx32", thread_ptr=0x%08"
           NACL_PRIx32")\n"),
          (uintptr_t) natp, (uintptr_t) prog_ctr, stack_ptr, thread_ptr);

  if (!NaClIsValidJumpTarget(nap, (uintptr_t) prog_ctr)) {
    NaClLog(LOG_ERROR, "NaClSysThreadCreate: Bad function pointer\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  /* Align the stack pointer. */
  stack_ptr = ((stack_ptr + NACL_STACK_PAD_BELOW_ALIGN)
               & ~NACL_STACK_ALIGN_MASK) - NACL_STACK_PAD_BELOW_ALIGN
              - NACL_STACK_ARGS_SIZE;

  sys_stack = NaClUserToSysAddr(nap, stack_ptr);
  if (kNaClBadAddress == sys_stack) {
    NaClLog(LOG_ERROR, "bad stack\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }
  sys_tls = NaClUserToSysAddrRange(nap, thread_ptr, 4);
  if (kNaClBadAddress == sys_tls) {
    NaClLog(LOG_ERROR, "bad TLS pointer\n");
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  NaClVmHoleWaitToStartThread(nap);

  retval = NaClCreateAdditionalThread(nap,
                                      (uintptr_t) prog_ctr,
                                      sys_stack,
                                      thread_ptr,
                                      second_thread_ptr);

cleanup:
  return retval;
}

/*
 * This is not used on x86-64 and its functionality is replaced by
 * NaClGetTlsFastPath1 (see nacl_syscall_64.S).
 */
int32_t NaClSysTlsGet(struct NaClAppThread *natp) {
  return NaClTlsGetTlsValue1(natp);
}

int32_t NaClSysSecondTlsSet(struct NaClAppThread *natp,
                            uint32_t             new_value) {
  NaClTlsSetTlsValue2(natp, new_value);
  return 0;
}

/*
 * This is not used on x86-64 and its functionality is replaced by
 * NaClGetTlsFastPath2 (see nacl_syscall_64.S).
 */
int32_t NaClSysSecondTlsGet(struct NaClAppThread *natp) {
  return NaClTlsGetTlsValue2(natp);
}

int NaClSysThreadNice(struct NaClAppThread *natp,
                      const int            nice) {
  /* Note: implementation of nacl_thread_nice is OS dependent. */
  UNREFERENCED_PARAMETER(natp);
  return nacl_thread_nice(nice);
}

int32_t NaClSysMutexCreate(struct NaClAppThread *natp) {
  struct NaClApp       *nap = natp->nap;
  int32_t              retval = -NACL_ABI_EINVAL;
  struct NaClDescMutex *desc;

  NaClLog(3,
          ("Entered NaClSysMutexCreate(0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp);

  desc = malloc(sizeof(*desc));

  if (!desc || !NaClDescMutexCtor(desc)) {
    retval = -NACL_ABI_ENOMEM;
    goto cleanup;
  }

  retval = NaClSetAvail(nap, (struct NaClDesc *) desc);
  desc = NULL;
cleanup:
  free(desc);
  NaClLog(3,
          ("NaClSysMutexCreate(0x%08"NACL_PRIxPTR") = %d\n"),
          (uintptr_t) natp, retval);
  return retval;
}

int32_t NaClSysMutexLock(struct NaClAppThread  *natp,
                         int32_t               mutex_handle) {
  struct NaClApp        *nap = natp->nap;
  int32_t               retval = -NACL_ABI_EINVAL;
  struct NaClDesc       *desc;

  NaClLog(3,
          ("Entered NaClSysMutexLock(0x%08"NACL_PRIxPTR", %d)\n"),
          (uintptr_t) natp, mutex_handle);

  desc = NaClGetDesc(nap, mutex_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->Lock)(desc);
  NaClDescUnref(desc);

cleanup:
  return retval;
}

int32_t NaClSysMutexUnlock(struct NaClAppThread  *natp,
                           int32_t               mutex_handle) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *desc;

  NaClLog(3,
          ("Entered NaClSysMutexUnlock(0x%08"NACL_PRIxPTR", %d)\n"),
          (uintptr_t) natp, mutex_handle);

  desc = NaClGetDesc(nap, mutex_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->Unlock)(desc);
  NaClDescUnref(desc);

cleanup:
  return retval;
}

int32_t NaClSysMutexTrylock(struct NaClAppThread   *natp,
                            int32_t                 mutex_handle) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *desc;

  NaClLog(3,
          ("Entered NaClSysMutexTrylock(0x%08"NACL_PRIxPTR", %d)\n"),
          (uintptr_t) natp, mutex_handle);

  desc = NaClGetDesc(nap, mutex_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->TryLock)(desc);
  NaClDescUnref(desc);

cleanup:
  return retval;
}

int32_t NaClSysCondCreate(struct NaClAppThread *natp) {
  struct NaClApp         *nap = natp->nap;
  int32_t                retval = -NACL_ABI_EINVAL;
  struct NaClDescCondVar *desc;

  NaClLog(3,
          ("Entered NaClSysCondCreate(0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp);

  desc = malloc(sizeof(*desc));

  if (!desc || !NaClDescCondVarCtor(desc)) {
    retval = -NACL_ABI_ENOMEM;
    goto cleanup;
  }

  retval = NaClSetAvail(nap, (struct NaClDesc *)desc);
  desc = NULL;
cleanup:
  free(desc);
  NaClLog(3,
          ("NaClSysCondCreate(0x%08"NACL_PRIxPTR") = %d\n"),
          (uintptr_t) natp, retval);
  return retval;
}

int32_t NaClSysCondWait(struct NaClAppThread *natp,
                        int32_t              cond_handle,
                        int32_t              mutex_handle) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *cv_desc;
  struct NaClDesc *mutex_desc;

  NaClLog(3,
          ("Entered NaClSysCondWait(0x%08"NACL_PRIxPTR", %d, %d)\n"),
          (uintptr_t) natp, cond_handle, mutex_handle);

  cv_desc = NaClGetDesc(nap, cond_handle);

  if (NULL == cv_desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  mutex_desc = NaClGetDesc(nap, mutex_handle);
  if (NULL == mutex_desc) {
    NaClDescUnref(cv_desc);
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) cv_desc->base.vtbl)->
            Wait)(cv_desc, mutex_desc);
  NaClDescUnref(cv_desc);
  NaClDescUnref(mutex_desc);

cleanup:
  return retval;
}

int32_t NaClSysCondSignal(struct NaClAppThread *natp,
                          int32_t              cond_handle) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *desc;

  NaClLog(3,
          ("Entered NaClSysCondSignal(0x%08"NACL_PRIxPTR", %d)\n"),
          (uintptr_t) natp, cond_handle);

  desc = NaClGetDesc(nap, cond_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->Signal)(desc);
  NaClDescUnref(desc);
cleanup:
  return retval;
}

int32_t NaClSysCondBroadcast(struct NaClAppThread  *natp,
                             int32_t               cond_handle) {
  struct NaClApp  *nap = natp->nap;
  struct NaClDesc *desc;
  int32_t         retval = -NACL_ABI_EINVAL;

  NaClLog(3,
          ("Entered NaClSysCondBroadcast(0x%08"NACL_PRIxPTR", %d)\n"),
          (uintptr_t) natp, cond_handle);

  desc = NaClGetDesc(nap, cond_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->Broadcast)(desc);
  NaClDescUnref(desc);

cleanup:
  return retval;
}

int32_t NaClSysCondTimedWaitAbs(struct NaClAppThread     *natp,
                                int32_t                  cond_handle,
                                int32_t                  mutex_handle,
                                struct nacl_abi_timespec *ts) {
  struct NaClApp           *nap = natp->nap;
  int32_t                  retval = -NACL_ABI_EINVAL;
  struct NaClDesc          *cv_desc;
  struct NaClDesc          *mutex_desc;
  struct nacl_abi_timespec trusted_ts;

  NaClLog(3,
          ("Entered NaClSysCondTimedWaitAbs(0x%08"NACL_PRIxPTR
           ", %d, %d, 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp, cond_handle, mutex_handle, (uintptr_t) ts);

  if (!NaClCopyInFromUser(nap, &trusted_ts,
                          (uintptr_t) ts, sizeof trusted_ts)) {
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }
  /* TODO(gregoryd): validate ts - do we have a limit for time to wait? */

  cv_desc = NaClGetDesc(nap, cond_handle);
  if (NULL == cv_desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  mutex_desc = NaClGetDesc(nap, mutex_handle);
  if (NULL == mutex_desc) {
    NaClDescUnref(cv_desc);
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) cv_desc->base.vtbl)->
            TimedWaitAbs)(cv_desc,
                          mutex_desc,
                          &trusted_ts);
  NaClDescUnref(cv_desc);
  NaClDescUnref(mutex_desc);
cleanup:
  return retval;
}

int32_t NaClSysSemCreate(struct NaClAppThread *natp,
                         int32_t              init_value) {
  struct NaClApp           *nap = natp->nap;
  int32_t                  retval = -NACL_ABI_EINVAL;
  struct NaClDescSemaphore *desc;

  NaClLog(3,
          ("Entered NaClSysSemCreate(0x%08"NACL_PRIxPTR
           ", %d)\n"),
          (uintptr_t) natp, init_value);

  desc = malloc(sizeof(*desc));

  if (!desc || !NaClDescSemaphoreCtor(desc, init_value)) {
    retval = -NACL_ABI_ENOMEM;
    goto cleanup;
  }

  retval = NaClSetAvail(nap, (struct NaClDesc *) desc);
  desc = NULL;
cleanup:
  free(desc);
  return retval;
}


int32_t NaClSysSemWait(struct NaClAppThread *natp,
                       int32_t              sem_handle) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *desc;

  NaClLog(3,
          ("Entered NaClSysSemWait(0x%08"NACL_PRIxPTR
           ", %d)\n"),
          (uintptr_t) natp, sem_handle);

  desc = NaClGetDesc(nap, sem_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  /*
   * TODO(gregoryd): we have to decide on the syscall API: do we
   * switch to read/write/ioctl API or do we stay with the more
   * detailed API. Anyway, using a single syscall for waiting on all
   * synchronization objects makes sense.
   */
  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->SemWait)(desc);
  NaClDescUnref(desc);
cleanup:
  return retval;
}

int32_t NaClSysSemPost(struct NaClAppThread *natp,
                       int32_t              sem_handle) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *desc;

  NaClLog(3,
          ("Entered NaClSysSemPost(0x%08"NACL_PRIxPTR
           ", %d)\n"),
          (uintptr_t) natp, sem_handle);

  desc = NaClGetDesc(nap, sem_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->Post)(desc);
  NaClDescUnref(desc);
cleanup:
  return retval;
}

int32_t NaClSysSemGetValue(struct NaClAppThread *natp,
                           int32_t              sem_handle) {
  struct NaClApp  *nap = natp->nap;
  int32_t         retval = -NACL_ABI_EINVAL;
  struct NaClDesc *desc;

  NaClLog(3,
          ("Entered NaClSysSemGetValue(0x%08"NACL_PRIxPTR
           ", %d)\n"),
          (uintptr_t) natp, sem_handle);

  desc = NaClGetDesc(nap, sem_handle);

  if (NULL == desc) {
    retval = -NACL_ABI_EBADF;
    goto cleanup;
  }

  retval = (*((struct NaClDescVtbl const *) desc->base.vtbl)->GetValue)(desc);
  NaClDescUnref(desc);
cleanup:
  return retval;
}

int32_t NaClSysNanosleep(struct NaClAppThread     *natp,
                         struct nacl_abi_timespec *req,
                         struct nacl_abi_timespec *rem) {
  struct NaClApp            *nap = natp->nap;
  struct nacl_abi_timespec  t_sleep;
  struct nacl_abi_timespec  t_rem;
  struct nacl_abi_timespec  *remptr;
  int                       retval = -NACL_ABI_EINVAL;

  NaClLog(3,
          ("Entered NaClSysNanosleep(0x%08"NACL_PRIxPTR
           ", 0x%08"NACL_PRIxPTR", 0x%08"NACL_PRIxPTR"x)\n"),
          (uintptr_t) natp, (uintptr_t) req, (uintptr_t) rem);

  /* do the check before we sleep */
  if (NULL != rem && kNaClBadAddress ==
      NaClUserToSysAddrRange(nap, (uintptr_t) rem, sizeof *rem)) {
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  if (!NaClCopyInFromUser(nap, &t_sleep,
                          (uintptr_t) req, sizeof t_sleep)) {
    retval = -NACL_ABI_EFAULT;
    goto cleanup;
  }

  remptr = (NULL == rem) ? NULL : &t_rem;
  /* NULL != remptr \equiv NULL != rem */

  /*
   * We assume that we do not need to normalize the time request values.
   *
   * If bogus values can cause the underlying OS to get into trouble,
   * then we need more checking here.
   */
  NaClLog(4, "NaClSysNanosleep(time = %"NACL_PRId64".%09"NACL_PRId64" S)\n",
          (int64_t) t_sleep.tv_sec, (int64_t) t_sleep.tv_nsec);
  retval = NaClNanosleep(&t_sleep, remptr);
  NaClLog(4, "NaClNanosleep returned %d\n", retval);

  if (-EINTR == retval && NULL != rem &&
      !NaClCopyOutToUser(nap, (uintptr_t) rem, remptr, sizeof *remptr)) {
    NaClLog(LOG_FATAL, "NaClSysNanosleep: check rem failed at copyout\n");
  }

cleanup:
  NaClLog(4, "nanosleep done.\n");
  return retval;
}

int32_t NaClSysSchedYield(struct NaClAppThread *natp) {
  UNREFERENCED_PARAMETER(natp);

  NaClThreadYield();
  return 0;
}

int32_t NaClSysExceptionHandler(struct NaClAppThread *natp,
                                uint32_t             handler_addr,
                                uint32_t             old_handler) {
  struct NaClApp *nap = natp->nap;
  int32_t rv = -NACL_ABI_EINVAL;

  if (!nap->enable_exception_handling) {
    rv = -NACL_ABI_ENOSYS;
    goto no_lock_exit;
  }
  if (!NaClIsValidJumpTarget(nap, handler_addr)) {
    rv = -NACL_ABI_EFAULT;
    goto no_lock_exit;
  }
  NaClXMutexLock(&nap->exception_mu);

  /*
   * This needs to be done while holding the lock so that we don't
   * start two Windows debug exception handlers.
   */
  if (handler_addr != 0) {
    if (!NaClDebugExceptionHandlerEnsureAttached(nap)) {
      rv = -NACL_ABI_ENOSYS;
      goto unlock_exit;
    }
  }

  if (0 != old_handler &&
      !NaClCopyOutToUser(nap, (uintptr_t) old_handler,
                         &nap->exception_handler,
                         sizeof nap->exception_handler)) {
    rv = -NACL_ABI_EFAULT;
    goto unlock_exit;
  }
  nap->exception_handler = handler_addr;
  rv = 0;
 unlock_exit:
  NaClXMutexUnlock(&nap->exception_mu);
 no_lock_exit:
  return rv;
}

int32_t NaClSysExceptionStack(struct NaClAppThread *natp,
                              uint32_t             stack_addr,
                              uint32_t             stack_size) {
  if (!natp->nap->enable_exception_handling) {
    return -NACL_ABI_ENOSYS;
  }
  if (kNaClBadAddress == NaClUserToSysAddrNullOkay(natp->nap,
                                                   stack_addr + stack_size)) {
    return -NACL_ABI_EINVAL;
  }
  natp->exception_stack = stack_addr + stack_size;
  return 0;
}

int32_t NaClSysExceptionClearFlag(struct NaClAppThread *natp) {
  if (!natp->nap->enable_exception_handling) {
    return -NACL_ABI_ENOSYS;
  }
  natp->exception_flag = 0;
  return 0;
}


int32_t NaClSysTestInfoLeak(struct NaClAppThread *natp) {
#if NACL_ARCH(NACL_BUILD_ARCH) == NACL_x86
  /*
   * Put some interesting bits into the x87 and SSE registers.
   */
  union fxsave {
    char buf[512];
    struct {
      uint16_t fcw;
      uint16_t fsw;
      uint16_t ftw;
      uint16_t fop;
      union {
        struct {
          uint64_t rip;
          uint64_t rdp;
        } x64;
        struct {
          uint32_t fpu_ip;
          uint32_t cs;
          uint32_t fpu_dp;
          uint32_t ds;
        } ia32;
      } bitness;
      uint32_t mxcsr;
      uint32_t mxcsr_mask;
      struct {
        uint8_t st[10];
        uint8_t reserved[6];
      } st_space[8];
      uint32_t xmm_space[64];
    } fxsave;
  };

  static const char tenbytes[10] = "SecretBits";
  static const char manybytes[256] =
      "Highly sensitive information must not be leaked to untrusted code!\n"
      "xyzzy\nplugh\nYou are likely to be eaten by a grue.\n"
      "When in the Course of human events it becomes necessary for one people"
      " to dissolve the political bands which have connected them with ...\n";

# ifdef __GNUC__
  union fxsave u __attribute__((aligned(16)));
# elif NACL_WINDOWS
  __declspec(align(16)) union fxsave u;
# else
#  error Unsupported platform
# endif

  int i;

# ifdef __GNUC__
  __asm__("fxsave %0" : "=m" (u));
# elif NACL_WINDOWS
#  if NACL_BUILD_SUBARCH == 64
  NaClDoFxsave(&u);
#  else
  __asm {
    fxsave u
  };
#  endif
# else
# error Unsupported platform
# endif

  for (i = 0; i < 8; ++i)
    memcpy(&u.fxsave.st_space[i], tenbytes, sizeof(tenbytes));

  memcpy(u.fxsave.xmm_space, manybytes, sizeof(u.fxsave.xmm_space));

  /*
   * Set the MXCSR to an unlikely (but valid) value: all valid bits set.
   * The mask is provided by the hardware to say which bits can be set
   * (all others are reserved).  The untrusted test code (in
   * tests/infoleak/test_infoleak.c) sets MXCSR to zero before
   * making this system call so this value ensures that the test
   * actually verifies the behavior of the syscall return path.
   */
  u.fxsave.mxcsr = u.fxsave.mxcsr_mask;

# ifdef __GNUC__
  __asm__ volatile("fxrstor %0" :: "m" (u));
# elif NACL_WINDOWS
#  if NACL_BUILD_SUBARCH == 64
  NaClDoFxrstor(&u);
#  else
  __asm {
    fxrstor u
  };
#  endif
# else
# error Unsupported platform
# endif

#elif NACL_ARCH(NACL_BUILD_ARCH) == NACL_arm
  /*
   * Put some interesting bits into the VFP registers.
   */

  static const char manybytes[64] =
      "Sensitive information must not be leaked to untrusted code!!!!\n";

  __asm__ volatile("vldm %0, {d0-d7}" :: "r" (manybytes) :
                   "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7");
  __asm__ volatile("fmxr fpscr, %0" :: "r" (0xdeadbeef) : "vfpcc");

#endif

  UNREFERENCED_PARAMETER(natp);

  return -NACL_ABI_ENOSYS;
}

/*
 * This syscall is intended for testing NaCl's support for Breakpad
 * crash reporting inside Chromium.  When
 * http://code.google.com/p/nativeclient/issues/detail?id=579 is
 * addressed, we might put this syscall behind a flag.  Until then,
 * untrusted code can trigger Breakpad-reported crashes inside
 * syscalls, so there is no benefit to restricting this syscall.
 */
int32_t NaClSysTestCrash(struct NaClAppThread *natp, int crash_type) {
  /*
   * Despite being volatile, the Apple system compiler, llvm-gcc, still
   * optimizes the null pointer dereference into an illegal instruction when
   * written as a one-liner. That interferes with tests that expect precisely
   * a SIGSEGV, because they'll see a SIGILL instead.
   */
  volatile int *volatile p = 0;
  UNREFERENCED_PARAMETER(natp);

  switch (crash_type) {
    case NACL_TEST_CRASH_MEMORY:
      *p = 0;
      break;
    case NACL_TEST_CRASH_LOG_FATAL:
      NaClLog(LOG_FATAL, "NaClSysTestCrash: This is a test error\n");
      break;
    case NACL_TEST_CRASH_CHECK_FAILURE:
      CHECK(0);
      break;
  }
  return -NACL_ABI_EINVAL;
}

int32_t NaClSysGetTimeOfDay(struct NaClAppThread      *natp,
                            struct nacl_abi_timeval   *tv,
                            struct nacl_abi_timezone  *tz) {
  int                     retval;
  struct nacl_abi_timeval now;

  UNREFERENCED_PARAMETER(tz);

  NaClLog(3,
          ("Entered NaClSysGetTimeOfDay(%08"NACL_PRIxPTR
           ", 0x%08"NACL_PRIxPTR", 0x%08"NACL_PRIxPTR")\n"),
          (uintptr_t) natp, (uintptr_t) tv, (uintptr_t) tz);

  /*
   * tz is not supported in linux, nor is it supported by glibc, since
   * tzset(3) and the zoneinfo file should be used instead.
   *
   * TODO(bsy) Do we make the zoneinfo directory available to
   * applications?
   */

  retval = NaClGetTimeOfDay(&now);
  if (0 != retval) {
    return retval;
  }
#if !NACL_WINDOWS
  /*
   * Coarsen the time to the same level we get on Windows -
   * 10 microseconds.
   */
  if (!NaClHighResolutionTimerEnabled()) {
    now.nacl_abi_tv_usec = (now.nacl_abi_tv_usec / 10) * 10;
  }
#endif
  CHECK(now.nacl_abi_tv_usec >= 0);
  CHECK(now.nacl_abi_tv_usec < NACL_MICROS_PER_UNIT);
  if (!NaClCopyOutToUser(natp->nap, (uintptr_t) tv, &now, sizeof now)) {
    return -NACL_ABI_EFAULT;
  }
  return 0;
}

static int NaClIsValidClockId(int clk_id) {
  switch (clk_id) {
    case NACL_ABI_CLOCK_REALTIME:
    case NACL_ABI_CLOCK_MONOTONIC:
    case NACL_ABI_CLOCK_PROCESS_CPUTIME_ID:
    case NACL_ABI_CLOCK_THREAD_CPUTIME_ID:
      return 1;
  }
  return 0;
}

int32_t NaClSysClockGetCommon(struct NaClAppThread  *natp,
                              int                   clk_id,
                              uint32_t              ts_addr,
                              int                   (*timefunc)(
                                  nacl_clockid_t            clk_id,
                                  struct nacl_abi_timespec  *tp)) {
  struct NaClApp            *nap = natp->nap;
  int                       retval = -NACL_ABI_EINVAL;
  struct nacl_abi_timespec  out_buf;

  if (!NaClIsValidClockId(clk_id)) {
    goto done;
  }
  retval = (*timefunc)((nacl_clockid_t) clk_id, &out_buf);
  if (0 == retval) {
    if (!NaClCopyOutToUser(nap, (uintptr_t) ts_addr,
                           &out_buf, sizeof out_buf)) {
      retval = -NACL_ABI_EFAULT;
    }
  }
 done:
  return retval;
}

int32_t NaClSysClockGetRes(struct NaClAppThread *natp,
                           int                  clk_id,
                           uint32_t             tsp) {
  return NaClSysClockGetCommon(natp, clk_id, (uintptr_t) tsp,
                                     NaClClockGetRes);
}

int32_t NaClSysClockGetTime(struct NaClAppThread  *natp,
                            int                   clk_id,
                            uint32_t              tsp) {
  return NaClSysClockGetCommon(natp, clk_id, (uintptr_t) tsp,
                                     NaClClockGetTime);
}

// yiwen
int32_t NaClSysPipe(struct NaClAppThread  *natp, uint32_t *pipedes) {
  struct NaClApp *nap = natp->nap;
  int32_t   retval = -NACL_ABI_EINVAL;
  uintptr_t       sysaddr;
  int size;
  int string[2];
  int* string_ptr;
  // int string2[2];
  // int* string2_ptr;

  size = 8;
  string_ptr = string;
  sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) pipedes, size);
  string_ptr = (int*)sysaddr;

  // return two fds to the user
  string_ptr[0] = 8000;
  string_ptr[1] = 8001;

  // fd_cage_table[1][8000] = 8000;
  // fd_cage_table[1][8001] = 8001;
  // fd_cage_table[2][8000] = 8000;
  // fd_cage_table[2][8001] = 8001;

  // we initialize our pipe buffer
  pipe_mutex[0] = 0; // at the initialization, the pipe should be empty, allow write but not read

  // let's try to use the kernel pipes
  /*
  string2_ptr = string2;
  pipe(string2_ptr);

  NaClLog(LOG_WARNING, "[NaClSysPipe] fd_cage_table[1][8001] = %d \n", string2_ptr[1]);
  NaClLog(LOG_WARNING, "[NaClSysPipe] fd_cage_table[2][8000] = %d \n", string2_ptr[0]);
  */
  retval = 0;
  return retval;
}

/* jp */
int32_t NaClSysFork(struct NaClAppThread *natp) {
  struct NaClApp *nap = natp->nap;
  struct NaClThreadContext parent_ctx = natp->user;
  int32_t retval;
  int argc2;
  char **argv2;
  int path_len;

  DPRINTF("[NaClSysFork] NaCl fork starts! \n");
  DPRINTF("[NaClSysFork] fork_num = %d, cage_id = %d\n", fork_num, nap->cage_id);

  if (nap->cage_id >= 1000) {
     DPRINTF("[NaClSysFork] This is the child of fork() \n");
     NaClAppThreadPrintInfo(natp);
     DPRINTF("         natp = 0x%016"NACL_PRIxPTR"\n", (uintptr_t) natp);
     DPRINTF("          nap = 0x%016"NACL_PRIxPTR"\n", (uintptr_t) nap);
     DPRINTF("    usr_entry = 0x%016"NACL_PRIxPTR"\n", natp->user.new_prog_ctr);
     DPRINTF("usr_stack_ptr = 0x%016"NACL_PRIxPTR"\n", natp->user.trusted_stack_ptr);
     return 0;
  }

  fork_num++;

  argc2 = 3 + nap->command_num;
  argv2 = calloc(argc2 + 1, sizeof *argv2);
  argv2[0] = malloc(9);
  strcpy(argv2[0], "NaClMain");
  argv2[1] = malloc(15);
  strcpy(argv2[1], "--library-path");
  argv2[2] = malloc(11);
  strcpy(argv2[2], "/lib/glibc");

  if (nap->binary_path) {
     path_len = strlen(nap->binary_path) + 1;
     argv2[3] = malloc(path_len);
     strcpy(argv2[3], nap->binary_path);
     DPRINTF("[NaClSysFork] binary path: %s \n\n", nap->binary_path);
  }

  if (nap->command_num > 1) {
     path_len = strlen(nap->binary_command) + 1;
     argv2[4] = malloc(path_len);
     strcpy(argv2[4], nap->binary_command);
     DPRINTF("[NaClSysFork] binary command: %s \n\n", nap->binary_command);
  }

  switch (fork_num) {
  case 1:
     NaClLogThreadContext(natp);
     nap->fork_num = fork_num;
     if (!NaClCreateMainForkThread(nap, natp, &parent_ctx, nap0, argc2, argv2, NULL)) {
       DPRINTF("[NaClSysFork] Execv new program failed! \n");
       retval = -1;
       break;
     }
     if (!(nap->child_list = calloc(10, sizeof *nap->child_list)))
       NaClLog(LOG_FATAL, "Failed to allocate memory for nap->child_list\n");
     nap->children_ids[nap->num_children] = nap0->cage_id;
     nap->child_list[nap->num_children] = nap0;
     nap0->parent_id = nap->cage_id;
     nap0->parent = nap;
     nap->num_children++;
     retval = nap0->cage_id;
     DPRINTF("[NaClSysFork] retval = %d \n", retval);
     break;

  case 2:
     /* comment out the break to create this forked thread */
     fork_num = 0;
     retval = 0;
     break;
     NaClLogThreadContext(natp);
     if (!NaClCreateMainForkThread(nap, natp, &parent_ctx, nap0_2, argc2, argv2, NULL)) {
       DPRINTF("[NaClSysFork] Execv new program failed! \n");
       retval = -1;
       break;
     }
     if (!(nap->child_list = calloc(10, sizeof *nap->child_list)))
       NaClLog(LOG_FATAL, "Failed to allocate memory for nap->child_list\n");
     nap->children_ids[nap->num_children] = nap0_2->cage_id;
     nap->child_list[nap->num_children] = nap0_2;
     nap0_2->parent_id = nap->cage_id;
     nap0_2->parent = nap;
     nap->num_children++;
     retval = nap0_2->cage_id;
     DPRINTF("[NaClSysFork] retval = %d \n", retval);
     break;

  default:
     retval = 0;
     break;
  }

  return retval;
}

// yiwen: my implementation for execv() call
// a new cage is created for the new program
// the new program will be running inside the new cage
// the old cage and the runnging main thread inside that cage will be torn down
int32_t NaClSysExecv(struct NaClAppThread  *natp) {
  struct NaClApp *nap = natp->nap;
  int32_t retval = -NACL_ABI_EINVAL;

  int argc2;
  char **argv2;

  NaClLog(LOG_WARNING, "[NaClSysExecv] NaCl execv starts! \n");

  argc2 = 4;
  argv2 = (char**) malloc(4 * sizeof(char*));
  argv2[0] = (char*) malloc(9 * sizeof(char));
  strncpy(argv2[0], "NaClMain", 9);
  argv2[1] = (char*) malloc(15 * sizeof(char));
  strncpy(argv2[1], "--library-path", 15);
  argv2[2] = (char*) malloc(7 * sizeof(char));
  strncpy(argv2[2], "/glibc", 7);
  argv2[3] = (char*) malloc(43 * sizeof(char));
  strncpy(argv2[3], "./test_case/hello_world/hello_world_1.nexe", 43);

  if (!NaClCreateMainThread(nap0,
                            argc2,
                            argv2,
                            NULL)) {
    fprintf(stderr, "creating main thread failed\n");
    NaClLog(LOG_WARNING, "[NaClSysExecv] Execv new program failed! \n");
    retval = -1;
    return retval;
  }

  NaClReportExitStatus(nap, 0);  // need to report the exit status of the old cage, otherwise the main process will hang, waiting for this cage to exit.
  NaClAppThreadTeardown(natp);   // now tear down the old running thread, so that it will not return.

  NaClLog(LOG_WARNING, "[NaClSysExecv] NaCl execv finishes! \n");

  return retval;
}

// yiwen: my implementation for execve(3) call
// a new cage is created for the new program (I am using a pre-allocated cage right now)
// the new program will be running inside the new cage
// the old cage and the runnging main thread inside that cage will be torn down
// on success, this function will not return
// on failure, this function will return -1
int32_t NaClSysExecve(struct NaClAppThread  *natp, void* path, void* argv, void* envp)  {
  struct NaClApp *nap = natp->nap;
  int argc_newcage;
  char **argv_newcage;
  int32_t retval = -1;
  int path_len = 0;
  uintptr_t path_get;
  uintptr_t argv_get;
#ifdef  _DEBUG
  uintptr_t envp_get;
#endif
  char *argv_split;
  int argv_num = 0;
  char **options;
  int option_len = 0;
  int i;

  // convert pointers from addresses within the cage into ones in the whole address space
  // basically just adding the cage memory start address to the offset within the cage
  path_get = NaClUserToSysAddr(nap, (uintptr_t) path);
  argv_get = NaClUserToSysAddr(nap, (uintptr_t) argv);
#ifdef  _DEBUG
  envp_get = NaClUserToSysAddr(nap, (uintptr_t) envp);
#endif

  path_len = strlen((char*) path_get);
  path_len += 1;

  options = (char**) malloc(3 * sizeof(char*));
  argv_split = strtok((char*)argv_get, " ");
  while (argv_split != NULL) {
    option_len = (int) (strlen(argv_split) + 1);
    // printf ("%s \n", argv_split);
    // printf ("%d \n", option_len);
    options[argv_num] = (char*) malloc(option_len * sizeof(char));
    strncpy(options[argv_num], argv_split, option_len - 1);
    options[argv_num][option_len - 1] = '\0';
    argv_num++;
    argv_split = strtok(NULL, " ");
  }

  /*
  printf ("%d \n", argv_num);
  for (i = 0; i < argv_num; i++) {
    printf ("%s \n", options[i]);
  } */

  // setup the arguments needed to start running a new main thread in a pre-allocated new cage
  argc_newcage = 4 + argv_num - 1;
  argv_newcage = (char**) malloc(argc_newcage * sizeof(char*));
  argv_newcage[0] = (char*) malloc(9 * sizeof(char));
  strncpy(argv_newcage[0], "NaClMain", 9);
  argv_newcage[1] = (char*) malloc(15 * sizeof(char));
  strncpy(argv_newcage[1], "--library-path", 15);
  argv_newcage[2] = (char*) malloc(7 * sizeof(char));
  strncpy(argv_newcage[2], "/glibc", 7);
  argv_newcage[3] = (char*) malloc(path_len * sizeof(char));
  strncpy(argv_newcage[3], (char*) path_get, path_len);

  for (i = 1; i < argv_num; i++) {
    // printf ("%d \n", (int) strlen(options[i]));
    argv_newcage[3 + i] = (char*) malloc((strlen(options[i]) + 1) * sizeof(char));
    strncpy(argv_newcage[3 + i], (char*) options[i], strlen(options[i]));
    argv_newcage[3 + i][strlen(options[i])] = '\0';
    // printf ("%s \n", argv_newcage[3 + i]);
  }

  // NaClLog(LOG_WARNING, "[NaClSysExecve] cage id = %d \n", nap->cage_id);
  // NaClLog(LOG_WARNING, "[NaClSysExecve] path = %s \n", (char*) path_get);
  // NaClLog(LOG_WARNING, "[NaClSysExecve] argv = %s \n", (char*) argv_get);
  envp = envp;
#ifdef  _DEBUG
  NaClLog(LOG_WARNING, "[NaClSysExecve] cage id = %d \n", nap->cage_id);
  NaClLog(LOG_WARNING, "[NaClSysExecve] envp = %s \n", (char*) envp_get);
  NaClLog(LOG_WARNING, "[NaClSysExecve] fork_num = %d \n", fork_num);
#endif

  if (fork_num == 1) {
      // need to inherit children info from previous cage
      nap_ready->num_children = nap->num_children;
      for (i = 0; i < nap->num_children; i++) {
          nap_ready->children_ids[i] = nap->children_ids[i];
      }

      // create and start running the main thread in the new cage
      if (!NaClCreateMainThread(nap_ready,
                                argc_newcage,
                                argv_newcage,
                                NULL)) {
        NaClLog(LOG_WARNING, "[NaClSysExecve] creating main thread failed \n");
        retval = -1;
        return retval;
      }
  }

  else if (fork_num == 2) {
      // need to inherit children info from previous cage
      nap_ready_2->num_children = nap->num_children;
      for (i = 0; i < nap->num_children; i++) {
          nap_ready_2->children_ids[i] = nap->children_ids[i];
      }

      // create and start running the main thread in the new cage
      if (!NaClCreateMainThread(nap_ready_2,
                                argc_newcage,
                                argv_newcage,
                                NULL)) {
        NaClLog(LOG_WARNING, "[NaClSysExecve] creating main thread failed \n");
        retval = -1;
        return retval;
      }
  }

  // need to report the exit status of the old cage, otherwise the main process will hang, waiting for this cage to exit.
  if (nap->cage_id == 1) {
     NaClReportExitStatus(nap, 0);
  }
  if (nap->cage_id == 1000) {
     NaClReportExitStatus(nap0, 0);
  }
  if (nap->cage_id == 1001) {
     NaClReportExitStatus(nap0_2, 0);
  }
  NaClAppThreadTeardown(natp);   // now tear down the old running thread, so that it will not return.
  return retval;
}

#define WAIT_ANY -1
#define WAIT_ANY_PG 0

// yiwen:
int32_t NaClSysWaitpid(struct NaClAppThread  *natp, uint32_t pid, uint32_t *stat_loc, uint32_t options) {
        struct NaClApp *nap = natp->nap;
        int *stat_loc_ptr;
        uintptr_t sysaddr;
        int retval;

        DPRINTF("[NaClSysWaitpid] entered waitpid! \n");

        sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t)stat_loc, 4);
        stat_loc_ptr = (int *)sysaddr;
        retval = 0;

        if (nap->num_children > 0) {
                /* seconds between thread switching */
                time_t timeout = 1;
                size_t idx = 0;
                size_t num_children = nap->num_children;
                struct NaClApp *nap_child = nap->child_list[idx];
                struct NaClAppThread *natp_child = nap_child->threads.ptr_array[idx];

                /*
                 * TODO: implement pid == WAIT_ANY_PG (0) and pid < -1 behavior
                 *
                 * -jp
                 */
                if (pid > 0 && pid < num_children) {
                        idx = pid;
                        nap_child = nap->child_list[idx];
                        natp_child = nap_child->threads.ptr_array[idx];
                        retval = NaClThreadJoin(&natp_child->host_thread);
                        if (retval) {
                                NaClLog(LOG_FATAL,
                                        "NaClThreadJoin() failed: %s\n",
                                        strerror(retval));
                                goto out;
                        }
                } else {
                        /* wait for any threads to exit */
                        do {
                                retval = NaClThreadTimedJoin(&natp_child->host_thread, timeout);
                                if (retval && retval != ETIMEDOUT) {
                                        NaClLog(LOG_FATAL,
                                                "NaClThreadTimedJoin() failed: %s\n",
                                                strerror(retval));
                                        goto out;
                                }
                                if (++idx >= num_children)
                                        idx = 0;
                                nap_child = nap->child_list[idx];
                                natp_child = nap_child->threads.ptr_array[idx];
                        } while (retval == ETIMEDOUT);
                        pid = idx - 1;
                }
        }

        DPRINTF("[NaClSysWaitpid] pid = %d \n", pid);
        DPRINTF("[NaClSysWaitpid] options = %d \n", options);
        DPRINTF("[NaClSysWaitpid] retval = %d \n", retval);

out:
        *stat_loc_ptr = retval;
        return retval;
}

int32_t NaClSysWait(struct NaClAppThread  *natp, uint32_t *stat_loc) {
        struct NaClApp *nap = natp->nap;
        int *stat_loc_ptr;
        uintptr_t sysaddr;
        int retval;

        DPRINTF("[NaClSysWait] entered wait! \n");

        sysaddr = NaClUserToSysAddrRange(nap, (uintptr_t) stat_loc, 4);
        stat_loc_ptr = (int *)sysaddr;
        retval = 0;
        if (nap->num_children > 0)
                retval = NaClSysWaitpid(natp, WAIT_ANY, stat_loc, 0);

        DPRINTF("[NaClSysWait] retval = %d \n", retval);

out:
        *stat_loc_ptr = retval;
        return retval;
}
