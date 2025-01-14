/* Copyright (c) 2013, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

/* minidump_exception_ps3.h: A definition of exception codes for
 * PS3 */


#ifndef GOOGLE_BREAKPAD_COMMON_MINIDUMP_EXCEPTION_PS3_H__
#define GOOGLE_BREAKPAD_COMMON_MINIDUMP_EXCEPTION_PS3_H__

#include <stddef.h>

#include "native_client/google_breakpad/common/breakpad_types.h"

typedef enum {
  MD_EXCEPTION_CODE_PS3_UNKNOWN = 0,
  MD_EXCEPTION_CODE_PS3_TRAP_EXCEP = 1,
  MD_EXCEPTION_CODE_PS3_PRIV_INSTR = 2,
  MD_EXCEPTION_CODE_PS3_ILLEGAL_INSTR = 3,
  MD_EXCEPTION_CODE_PS3_INSTR_STORAGE = 4,
  MD_EXCEPTION_CODE_PS3_INSTR_SEGMENT = 5,
  MD_EXCEPTION_CODE_PS3_DATA_STORAGE = 6,
  MD_EXCEPTION_CODE_PS3_DATA_SEGMENT = 7,
  MD_EXCEPTION_CODE_PS3_FLOAT_POINT = 8,
  MD_EXCEPTION_CODE_PS3_DABR_MATCH = 9,
  MD_EXCEPTION_CODE_PS3_ALIGN_EXCEP = 10,
  MD_EXCEPTION_CODE_PS3_MEMORY_ACCESS = 11,
  MD_EXCEPTION_CODE_PS3_COPRO_ALIGN = 12,
  MD_EXCEPTION_CODE_PS3_COPRO_INVALID_COM = 13,
  MD_EXCEPTION_CODE_PS3_COPRO_ERR = 14,
  MD_EXCEPTION_CODE_PS3_COPRO_FIR = 15,
  MD_EXCEPTION_CODE_PS3_COPRO_DATA_SEGMENT = 16,
  MD_EXCEPTION_CODE_PS3_COPRO_DATA_STORAGE = 17,
  MD_EXCEPTION_CODE_PS3_COPRO_STOP_INSTR = 18,
  MD_EXCEPTION_CODE_PS3_COPRO_HALT_INSTR = 19,
  MD_EXCEPTION_CODE_PS3_COPRO_HALTINST_UNKNOWN = 20,
  MD_EXCEPTION_CODE_PS3_COPRO_MEMORY_ACCESS = 21,
  MD_EXCEPTION_CODE_PS3_GRAPHIC = 22
} MDExceptionCodePS3;

#endif /* GOOGLE_BREAKPAD_COMMON_MINIDUMP_EXCEPTION_PS3_H__ */
