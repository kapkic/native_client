/*
 * Copyright (c) 2013 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "native_client/src/untrusted/nacl/nacl_irt.h"

int mkdir(const char *path, mode_t mode) {
  if (__libnacl_irt_dev_filename.mkdir == NULL) {
    __libnacl_irt_filename_init();
    if (__libnacl_irt_dev_filename.mkdir == NULL) {
      errno = ENOSYS;
      return -1;
    }
  }

  int error = __libnacl_irt_dev_filename.mkdir(path, mode);
  if (error) {
    errno = error;
    return -1;
  }

  return 0;
}
