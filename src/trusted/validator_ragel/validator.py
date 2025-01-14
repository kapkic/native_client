# Copyright (c) 2013 The Native Client Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import cStringIO
import ctypes
import os
import re
import subprocess
import sys
import tempfile

import objdump_parser


# Some constants from validator.h
VALIDATION_ERRORS_MASK = 0x01ffc000
BAD_JUMP_TARGET = 0x40000000

RESTRICTED_REGISTER_MASK = 0x00001f00
RESTRICTED_REGISTER_SHIFT = 8

REG_RAX = 0
REG_RCX = 1
REG_RDX = 2
REG_RBX = 3
REG_RSP = 4
REG_RBP = 5
REG_RSI = 6
REG_RDI = 7
REG_R8 = 8
REG_R9 = 9
REG_R10 = 10
REG_R11 = 11
REG_R12 = 12
REG_R13 = 13
REG_R14 = 14
REG_R15 = 15
ALL_REGISTERS = range(REG_RAX, REG_R15 + 1)
NO_REG = 0x19

RESTRICTED_REGISTER_INITIAL_VALUE_MASK = 0x000000ff
CALL_USER_CALLBACK_ON_EACH_INSTRUCTION = 0x00000100

# Macroses from validator.h
def PACK_RESTRICTED_REGISTER_INITIAL_VALUE(register):
  return register ^ NO_REG


BUNDLE_SIZE = 32


REGISTER_NAMES = {
    REG_RAX: '%rax',
    REG_RCX: '%rcx',
    REG_RDX: '%rdx',
    REG_RBX: '%rbx',
    REG_RSP: '%rsp',
    REG_RBP: '%rbp',
    REG_RSI: '%rsi',
    REG_RDI: '%rdi',
    REG_R8: '%r8',
    REG_R9: '%r9',
    REG_R10: '%r10',
    REG_R11: '%r11',
    REG_R12: '%r12',
    REG_R13: '%r13',
    REG_R14: '%r14',
    REG_R15: '%r15'}

REGISTER_BY_NAME = dict(map(reversed, REGISTER_NAMES.items()))


CALLBACK_TYPE = ctypes.CFUNCTYPE(
    ctypes.c_uint32,  # Bool result
    ctypes.POINTER(ctypes.c_uint8),  # begin
    ctypes.POINTER(ctypes.c_uint8),  # end
    ctypes.c_uint32,  # validation info
    ctypes.c_void_p,  # callback data
)


class DisassemblerError(Exception):
  pass


class Validator(object):

  def __init__(self, validator_dll=None, decoder_dll=None):
    """Initialize python2 interface to the validator.

    Should be called before any calls to ValidateChunk.

    Args:
      validator_dll: path to dll that provides ValidateChunkIA32 and
          ValidateChynkAMD64 functions.

    Returns:
      None.
    """

    if validator_dll is not None:
      validator_dll = ctypes.cdll.LoadLibrary(validator_dll)

      self.GetFullCPUIDFeatures = validator_dll.GetFullCPUIDFeatures
      self.GetFullCPUIDFeatures.restype = ctypes.c_void_p

      self._ValidateChunkIA32 = validator_dll.ValidateChunkIA32
      self._ValidateChunkAMD64 = validator_dll.ValidateChunkAMD64

      self._ValidateChunkIA32.argtypes = self._ValidateChunkAMD64.argtypes = [
          ctypes.POINTER(ctypes.c_uint8),  # data
          ctypes.c_uint32,  # size
          ctypes.c_uint32,  # options
          ctypes.c_void_p,  # CPU features
          CALLBACK_TYPE,  # callback
          ctypes.c_void_p,  # callback data
      ]
      self._ValidateChunkIA32.restype = ctypes.c_bool  # Bool
      self._ValidateChunkAMD64.restype = ctypes.c_bool  # Bool

    if decoder_dll is not None:
      decoder_dll = ctypes.cdll.LoadLibrary(decoder_dll)
      self.DisassembleChunk_ = decoder_dll.DisassembleChunk
      self.DisassembleChunk_.argtypes = [
          ctypes.POINTER(ctypes.c_uint8),  # data
          ctypes.c_uint32,  # size
          ctypes.c_int,  # bitness
      ]
      self.DisassembleChunk_.restype = ctypes.c_char_p

  def ValidateChunk(
      self,
      data,
      bitness,
      callback=None,
      on_each_instruction=False,
      restricted_register=None):
    """Validate chunk, calling the callback if there are errors.

    Validator interface must be initialized by calling Init first.

    Args:
      data: raw data to validate as python2 string.
      bitness: 32 or 64.
      callback: function that takes three arguments
          begin_index, end_index and info (info is combination of flags; it is
          explained in validator.h). It is invoked for every erroneous
          instruction.
      on_each_instruction: whether to invoke callback on each instruction (not
         only on erroneous ones).
      restricted_register: initial value for the restricted_register variable
                           (see validator_internals.html for the details)

    Returns:
      True if the chunk is valid, False if invalid.
    """

    data_addr = ctypes.cast(data, ctypes.c_void_p).value

    def LowLevelCallback(begin, end, info, callback_data):
      if callback is not None:
        begin_index = ctypes.cast(begin, ctypes.c_void_p).value - data_addr
        end_index = ctypes.cast(end, ctypes.c_void_p).value - data_addr
        callback(begin_index, end_index, info)

      # See validator.h for details
      if info & (VALIDATION_ERRORS_MASK | BAD_JUMP_TARGET) != 0:
        return 0
      else:
        return 1

    options = 0
    if on_each_instruction:
      options |= CALL_USER_CALLBACK_ON_EACH_INSTRUCTION
    if restricted_register is not None:
      assert restricted_register in ALL_REGISTERS
      options |= PACK_RESTRICTED_REGISTER_INITIAL_VALUE(restricted_register)

    data_ptr = ctypes.cast(data, ctypes.POINTER(ctypes.c_uint8))

    validate_chunk_function = {
        32: self._ValidateChunkIA32,
        64: self._ValidateChunkAMD64}[bitness]

    result = validate_chunk_function(
        data_ptr,
        len(data),
        options,
        self.GetFullCPUIDFeatures(),
        CALLBACK_TYPE(LowLevelCallback),
        None)

    return bool(result)

  def DisassembleChunk(self, data, bitness):
    data_ptr = ctypes.cast(data, ctypes.POINTER(ctypes.c_uint8))
    result = self.DisassembleChunk_(data_ptr, len(data), bitness)

    instructions = []
    total_bytes = 0
    for line in cStringIO.StringIO(result):
      m = re.match(r'rejected at ([\da-f]+)', line)
      if m is not None:
        offset = int(m.group(1), 16)
        raise DisassemblerError(offset, ' '.join('%02x' % ord(c) for c in data))
      insn = objdump_parser.ParseLine(line)
      insn = objdump_parser.CanonicalizeInstruction(insn)
      instructions.append(insn)
      total_bytes += len(insn.bytes)
    return instructions

  # TODO(shcherbina): Remove it.
  # Currently I'm keeping it around just in case (might be helpful for
  # troubleshooting RDFA decoder).
  def DisassembleChunkWithObjdump(self, data, bitness):
    """Disassemble chunk assuming it consists of valid instructions.

    Args:
      data: raw data as python2 string.
      bitness: 32 or 64

    Returns:
      List of objdump_parser.Instruction tuples. If data can't be disassembled
      (either contains invalid instructions or ends in a middle of instruction)
      exception is raised.
    """
    # TODO(shcherbina):
    # Replace this shameless plug with python2 interface to RDFA decoder once
    # https://code.google.com/p/nativeclient/issues/detail?id=3456 is done.

    arch = {32: '-Mi386', 64: '-Mx86-64'}[bitness]

    tmp = tempfile.NamedTemporaryFile(mode='wb', delete=False)
    try:
      tmp.write(data)
      tmp.close()

      objdump_proc = subprocess.Popen(
          ['objdump',
           '-mi386', arch, '--target=binary',
           '--disassemble-all', '--disassemble-zeroes',
           '--insn-width=15',
           tmp.name],
          stdout=subprocess.PIPE)

      instructions = []
      total_bytes = 0
      for line in objdump_parser.SkipHeader(objdump_proc.stdout):
        insn = objdump_parser.ParseLine(line)
        insn = objdump_parser.CanonicalizeInstruction(insn)
        instructions.append(insn)
        total_bytes += len(insn.bytes)

      assert len(data) == total_bytes

      return_code = objdump_proc.wait()
      assert return_code == 0, 'error running objdump'

      return instructions

    finally:
      tmp.close()
      os.remove(tmp.name)


def main():
  print 'Self check'
  print sys.argv
  validator_dll, = sys.argv[1:]
  print validator_dll

  validator = Validator(validator_dll)

  # 'z' is the first byte of JP instruction (which does not validate in this
  # case because it crosses bundle boundary)
  data = '\x90' * 31 + 'z'

  for bitness in 32, 64:
    errors = []

    def Callback(begin_index, end_index, info):
      errors.append(begin_index)
      print 'callback', begin_index, end_index

    result = validator.ValidateChunk(
        data,
        bitness=bitness,
        callback=Callback)

    assert not result
    assert errors == [31], errors


if __name__ == '__main__':
  main()
