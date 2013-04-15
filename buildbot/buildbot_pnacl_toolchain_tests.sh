#!/bin/bash
# Copyright (c) 2012 The Native Client Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Run toolchain torture tests and llvm testsuite tests.
# For now, run on linux64, build and run unsandboxed newlib tests
# for all 3 architectures.
# Note: This script builds the toolchain from scratch but does
#       not build the translators and hence the translators
#       are from an older revision, see comment below.

set -o xtrace
set -o nounset
set -o errexit

# NOTE:
# The pexes which are referred to below, and the pexes generated by the
# archived frontend below will be translated with translators from DEPS.
# The motivation is to ensure that newer translators can still handle
# older pexes.

# This hopefully needs to be updated rarely, it contains pexe from
# the sandboxed llc/gold builds
ARCHIVED_PEXE_TRANSLATOR_REV=10489

# The frontend from this rev will generate pexes for the archived frontend
# test. The toolchain downloader expects this information in a specially
# formatted file. We generate that file in this script from this information,
# to keep all our versions in one place
ARCHIVED_TOOLCHAIN_REV=11141

readonly PNACL_BUILD="pnacl/build.sh"
readonly UP_DOWN_LOAD="buildbot/file_up_down_load.sh"
readonly TORTURE_TEST="tools/toolchain_tester/torture_test.py"
readonly LLVM_TESTSUITE="pnacl/scripts/llvm-test.sh"

# build.sh, llvm test suite and torture tests all use this value
export PNACL_CONCURRENCY=${PNACL_CONCURRENCY:-4}

# Change the  toolchain build script (PNACL_BUILD) behavior slightly
# wrt to error logging and mecurial retry delays.
# TODO(robertm): if this special casing is still needed,
#                make this into separate vars
export PNACL_BUILDBOT=true
# Make the toolchain build script (PNACL_BUILD) more verbose.
# This will also prevent bot timeouts which otherwise gets triggered
# by long periods without console output.
export PNACL_VERBOSE=true

# For now this script runs on linux x86-64.
# It is possible to force the PNACL_BUILD to build host binaries with "-m32",
# by uncommenting below:
# export BUILD_ARCH="x86_32"
# export HOST_ARCH="x86_32"
# TODO(pnacl-team): Figure out what to do about this.
# Export this so that the test scripts know where to find the toolchain.
export PNACL_TOOLCHAIN_LABEL=pnacl_linux_x86
# This picks the TC which we just built, even if scons doesn't know
# how to find a 64-bit host toolchain.
readonly SCONS_PICK_TC="pnaclsdk_mode=custom:toolchain/${PNACL_TOOLCHAIN_LABEL}"

# download-old-tc -
# Download the archived frontend toolchain, if we haven't already
download-old-tc() {
  local dst=$1

  if [[ -f "${dst}/${ARCHIVED_TOOLCHAIN_REV}.stamp" ]]; then
    echo "Using existing tarball for archived frontend"
  else
    mkdir -p "${dst}"
    rm -rf "${dst}/*"
    ${UP_DOWN_LOAD} DownloadPnaclToolchains ${ARCHIVED_TOOLCHAIN_REV} \
      ${PNACL_TOOLCHAIN_LABEL} \
      ${dst}/${PNACL_TOOLCHAIN_LABEL}.tgz
    mkdir -p ${dst}/${PNACL_TOOLCHAIN_LABEL}
    tar xz -C ${dst}/${PNACL_TOOLCHAIN_LABEL} \
      -f ${dst}/${PNACL_TOOLCHAIN_LABEL}.tgz
    touch "${dst}/${ARCHIVED_TOOLCHAIN_REV}.stamp"
  fi
}


clobber() {
  echo @@@BUILD_STEP clobber@@@
  rm -rf scons-out
  # Don't clobber toolchain/pnacl_translator; these bots currently don't build
  # it, but they use the DEPSed-in version.
  rm -rf toolchain/pnacl_linux* toolchain/pnacl_mac* toolchain/pnacl_win*
}

handle-error() {
  echo "@@@STEP_FAILURE@@@"
}

ignore-error() {
  echo "@==  IGNORING AN ERROR  ==@"
}

#### Support for running arm sbtc tests on this bot, since we have
# less coverage on the main waterfall now:
# http://code.google.com/p/nativeclient/issues/detail?id=2581
readonly SCONS_COMMON="./scons --verbose bitcode=1 -j${PNACL_CONCURRENCY}"
readonly SCONS_COMMON_SLOW="./scons --verbose bitcode=1 -j2"

build-sbtc-prerequisites() {
  local platform=$1
  ${SCONS_COMMON} ${SCONS_PICK_TC} platform=${platform} \
    sel_ldr sel_universal irt_core
}


scons-tests-pic() {
  local platform=$1

  echo "@@@BUILD_STEP scons-tests-pic [${platform}]@@@"
  local extra="--mode=opt-host,nacl -k \
               nacl_pic=1  pnacl_generate_pexe=0"
  ${SCONS_COMMON} ${SCONS_PICK_TC} ${extra} \
    platform=${platform} smoke_tests || handle-error
}


scons-tests-translator() {
  local platform=$1

  echo "@@@BUILD_STEP scons-sb-trans [${platform}] [prereq]@@@"
  build-sbtc-prerequisites ${platform}

  local flags="--mode=opt-host,nacl use_sandboxed_translator=1 \
               platform=${platform} -k"
  local targets="small_tests medium_tests large_tests"

  # ROUND 1: regular builds
  # generate pexes with full parallelism
  echo "@@@BUILD_STEP scons-sb-trans-pexe [${platform}] [${targets}]@@@"
  ${SCONS_COMMON} ${SCONS_PICK_TC} ${flags} ${targets} \
      translate_in_build_step=0 do_not_run_tests=1 || handle-error

  # translate pexes
  echo "@@@BUILD_STEP scons-sb-trans-trans [${platform}] [${targets}]@@@"
  if [[ ${platform} = arm ]] ; then
      # For ARM we use less parallelism to avoid mysterious QEMU crashes.
      # We also force a timeout for translation only.
      export QEMU_PREFIX_HOOK="timeout 120"
      # Run sb translation twice in case we failed to translate some of the
      # pexes.  If there was an error in the first run this shouldn't
      # trigger a buildbot error.  Only the second run can make the bot red.
      ${SCONS_COMMON_SLOW} ${SCONS_PICK_TC} ${flags} ${targets} \
          do_not_run_tests=1 || ignore-error
      ${SCONS_COMMON_SLOW} ${SCONS_PICK_TC} ${flags} ${targets} \
          do_not_run_tests=1 || handle-error
      # Do not use the prefix hook for running actual tests as
      # it will break some of them due to exit code sign inversion.
      unset QEMU_PREFIX_HOOK
  else
      ${SCONS_COMMON} ${SCONS_PICK_TC} ${flags} ${targets} \
          do_not_run_tests=1 || handle-error
  fi
  # finally run the tests
  echo "@@@BUILD_STEP scons-sb-trans-run [${platform}] [${targets}]@@@"
  ${SCONS_COMMON_SLOW} ${SCONS_PICK_TC} ${flags} ${targets} || handle-error

  # ROUND 2: builds with "fast translation"
  flags="${flags} translate_fast=1"
  echo "@@@BUILD_STEP scons-sb-trans-pexe [fast] [${platform}] [${targets}]@@@"
  ${SCONS_COMMON} ${SCONS_PICK_TC} ${flags} ${targets} \
      translate_in_build_step=0 do_not_run_tests=1 || handle-error

  echo "@@@BUILD_STEP scons-sb-trans-trans [fast] [${platform}] [${targets}]@@@"
  if [[ ${platform} = arm ]] ; then
      # For ARM we use less parallelism to avoid mysterious QEMU crashes.
      # We also force a timeout for translation only.
      export QEMU_PREFIX_HOOK="timeout 120"
      # Run sb translation twice in case we failed to translate some of the
      # pexes.  If there was an error in the first run this shouldn't
      # trigger a buildbot error.  Only the second run can make the bot red.
      ${SCONS_COMMON_SLOW} ${SCONS_PICK_TC} ${flags} ${targets} \
          do_not_run_tests=1 || ignore-error
      ${SCONS_COMMON_SLOW} ${SCONS_PICK_TC} ${flags} ${targets} \
          do_not_run_tests=1 || handle-error
      # Do not use the prefix hook for running actual tests as
      # it will break some of them due to exit code sign inversion.
      unset QEMU_PREFIX_HOOK
  else
      ${SCONS_COMMON} ${SCONS_PICK_TC} ${flags} ${targets} \
          do_not_run_tests=1 || handle-error
  fi
  echo "@@@BUILD_STEP scons-sb-trans-run [fast] [${platform}] [${targets}]@@@"
  ${SCONS_COMMON_SLOW} ${SCONS_PICK_TC} ${flags} ${targets} || handle-error
}

scons-tests-x86-64-zero-based-sandbox() {
  echo "@@@BUILD_STEP hello_world (x86-64 zero-based sandbox)@@@"
  local flags="--mode=opt-host,nacl platform=x86-64 \
               x86_64_zero_based_sandbox=1"
  ${SCONS_COMMON} ${SCONS_PICK_TC} ${flags} "run_hello_world_test"
}

# This test is a bitcode stability test, which builds pexes for all the tests
# using an old version of the toolchain frontend, and then translates those
# pexes using the current version of the translator. It's simpler than using
# archived pexes, because archived pexes for old scons tests may not match the
# current scons tests (e.g. if the expected output changes or if a new test
# is added). The only thing that would break this approach is if a new test
# is added that is incompatible with the old frontend. For this case there
# simply needs to be a mechanism to disable that test (which could be as simple
# as using disable_tests here on the scons command line).
# Note: If this test is manually interrupted or killed during the run, the
# toolchain install might end up missing or replaced with the old one.
# To fix, copy the current one from toolchains/current_tc or blow it away
# and re-run gclient runhooks.
archived-frontend-test() {
  local arch=$1
  # Build the IRT with the latest toolchain before building user
  # pexes with the archived toolchain.
  echo "@@@BUILD_STEP archived_frontend [${arch}]\
        rev ${ARCHIVED_TOOLCHAIN_REV} BUILD IRT@@@"
  ${SCONS_COMMON} ${SCONS_PICK_TC} --mode=opt-host,nacl platform=${arch} \
    irt_core || handle-error


  echo "@@@BUILD_STEP archived_frontend [${arch}]\
        rev ${ARCHIVED_TOOLCHAIN_REV} BUILD@@@"
  local targets="small_tests medium_tests large_tests"
  local flags="--mode=opt-host,nacl platform=${arch} \
               translate_in_build_step=0 skip_trusted_tests=1 \
               skip_nonstable_bitcode=1"

  rm -rf scons-out/nacl-${arch}*

  # Get the archived frontend.
  # If the correct cached frontend is in place, the hash will match and the
  # download will be a no-op. Otherwise the downloader will fix it.
  download-old-tc toolchain/archived_tc

  # Save the current toolchain.
  mkdir -p toolchain/current_tc
  rm -rf toolchain/current_tc/*
  mv toolchain/${PNACL_TOOLCHAIN_LABEL} \
    toolchain/current_tc/${PNACL_TOOLCHAIN_LABEL}

  # Link the old frontend into place. If we just use pnaclsdk_mode to select a
  # different toolchain, SCons will attempt to rebuild the IRT.
  ln -s archived_tc/${PNACL_TOOLCHAIN_LABEL} toolchain/${PNACL_TOOLCHAIN_LABEL}

  # Build the pexes with the old frontend
  ${SCONS_COMMON} ${SCONS_PICK_TC} \
    do_not_run_tests=1 ${flags} ${targets} || handle-error
  # The pexes for fast translation tests are identical but scons uses a
  # different directory.
  cp -a scons-out/nacl-${arch}-pnacl-pexe-clang \
    scons-out/nacl-${arch}-pnacl-fast-pexe-clang

  # Put the current toolchain back in place.
  rm toolchain/${PNACL_TOOLCHAIN_LABEL}
  mv toolchain/current_tc/${PNACL_TOOLCHAIN_LABEL} \
    toolchain/${PNACL_TOOLCHAIN_LABEL}

  # Translate them with the new translator, and run the tests
  echo "@@@BUILD_STEP archived_frontend [${arch}]\
        rev ${ARCHIVED_TOOLCHAIN_REV} RUN@@@"
  ${SCONS_COMMON} ${SCONS_PICK_TC} \
    ${flags} ${targets} built_elsewhere=1 || handle-error
  # Also test the fast-translation option
  echo "@@@BUILD_STEP archived_frontend [${arch} translate-fast]\
        rev ${ARCHIVED_TOOLCHAIN_REV} RUN@@@"
  ${SCONS_COMMON} ${SCONS_PICK_TC} ${flags} translate_fast=1 built_elsewhere=1 \
    ${targets} || handle-error
}

archived-pexe-translator-test() {
  local arch=$1
  echo "@@@BUILD_STEP archived_pexe_translator \
        $arch rev ${ARCHIVED_PEXE_TRANSLATOR_REV} @@@"
  local dir="$(pwd)/pexe_archive"
  local tarball="${dir}/pexes.tar.bz2"
  local measure_cmd="/usr/bin/time -v"
  local sb_translator="${measure_cmd} \
                       toolchain/pnacl_translator/bin/pnacl-translate"
  rm -rf ${dir}
  mkdir -p ${dir}

  ${UP_DOWN_LOAD} DownloadArchivedPexesTranslator \
      ${ARCHIVED_PEXE_TRANSLATOR_REV} ${tarball}
  tar jxf ${tarball} --directory ${dir}

  # Note, the archive provides both unstripped (ext="") and stripped
  #       (ext=".strip-all") versions of the pexes ("ld", "llc").
  #       We do not want to strip them here as the "translator"
  #       package and the toolchain package maybe out of sync and
  #       strip does more than just stripping, it also upgrades
  #       bitcode versions if there was a format change.
  #       We only run with ext="" to save time, but if any bugs show up
  #       we can switch to ext=".strip-all" and diagnose the bugs.
  local ld_ext=""
  local llc_ext=""
  # Pexes are arch specific.
  case ${arch} in
    arm)
      ld_ext=".armv7.pexe"
      llc_ext=".armv7.pexe"
      ;;
    x86-32)
      ld_ext=".i686.pexe"
      llc_ext=".i686.pexe"
      ;;
    x86-64)
      ld_ext=".x86_64.pexe"
      llc_ext=".i686.pexe" # One llc pexe handles both x86-32 and x86-64.
      ;;
    *) echo "unknown arch!" && handle-error ;;
  esac

  # Note, that the arch flag has two functions:
  # 1) it selects the target arch for the translator
  # 2) combined with --pnacl-sb it selects the host arch for the
  #    sandboxed translators
  local flags="-arch ${arch} --pnacl-sb --pnacl-driver-verbose"
  if [[ ${arch} = arm ]] ; then
      # We need to enable qemu magic for arm
      flags="${flags} --pnacl-use-emulator"
  fi
  local fast_trans_flags="${flags} -translate-fast"

  # Driver flags for overriding *just* the LLC and LD from
  # the translator, to test that the LLC and LD generated
  # from archived pexes may work.  Note that this does not override the
  # libaries or the driver that are part of the translator,
  # so it is not a full override and will not work if the interface
  # has changed.
  local override_flags="\
    --pnacl-driver-set-LLC_SB=${dir}/llc-${arch}.nexe \
    --pnacl-driver-set-LD_SB=${dir}/ld-${arch}.nexe"
  local fast_override_flags="\
    --pnacl-driver-set-LLC_SB=${dir}/llc-${arch}.fast_trans.nexe \
    --pnacl-driver-set-LD_SB=${dir}/ld-${arch}.fast_trans.nexe"

  echo "=== Translating the archived translator."
  echo "=== Compiling Old Gold (normal mode) ==="
  ${sb_translator} ${flags} ${dir}/ld${ld_ext} \
      -o ${dir}/ld-${arch}.nexe
  echo "=== Compiling Old Gold (fast mode) ==="
  ${sb_translator} ${fast_trans_flags} ${dir}/ld${ld_ext} \
      -o ${dir}/ld-${arch}.fast_trans.nexe

  # Yikes: This takes about 17min on arm with qemu
  echo "=== Compiling Old LLC (normal mode) ==="
  ${sb_translator} ${flags} ${dir}/llc${llc_ext} \
      -o ${dir}/llc-${arch}.nexe
  echo "=== Compiling Old LLC (fast mode) ==="
  ${sb_translator} ${fast_trans_flags} ${dir}/llc${llc_ext} \
      -o ${dir}/llc-${arch}.fast_trans.nexe

  ls -l ${dir}
  file ${dir}/*

  # The llc.pexe compile finishes with -translate-fast, but the result
  # has bugs on x86-64, so we cannot test it below.
  # Known error is: assertion "InChain.getValueType() == MVT::Other &&
  # "Not a chain"" failed:
  # file "../llvm/lib/CodeGen/SelectionDAG/SelectionDAGISel.cpp", line 1927,
  # function: llvm::SDValue HandleMergeInputChains(
  #    SmallVectorImpl<llvm::SDNode *> &, llvm::SelectionDAG *)
  # To test, run with the original fast_override_flags:
  if [[ ${arch} = x86-64 ]]; then
    fast_override_flags="\
      --pnacl-driver-set-LD_SB=${dir}/ld-${arch}.fast_trans.nexe"
  fi
  echo "=== Running the translated archived translator to test."
  ${sb_translator} ${flags} ${override_flags} ${dir}/ld${ld_ext} \
      -o ${dir}/ld-${arch}.2.nexe
  ${sb_translator} ${flags} ${fast_override_flags} ${dir}/ld${ld_ext} \
      -o ${dir}/ld-${arch}.3.nexe

  # TODO(robertm): Ideally we would compare the result of translation like so
  # ${dir}/ld-${arch}.2.nexe == ${dir}/ld-${arch}.3.nexe
  # but this requires the translator to be deterministic which is not
  # quite true at the moment - probably due to due hashing inside of
  # llc based on pointer values.
}


tc-test-bot() {
  local archset="$1"
  clobber

  # Only build MIPS stuff on mips bots
  if [[ ${archset} == "mips" ]]; then
    export PNACL_BUILD_MIPS=true
    # Don't run any of the tests yet
    echo "MIPS bot: Only running build, and not tests"
    archset=
  fi

  echo "@@@BUILD_STEP show-config@@@"
  ${PNACL_BUILD} show-config

  # Build the un-sandboxed toolchain
  echo "@@@BUILD_STEP compile_toolchain@@@"
  ${PNACL_BUILD} clean
  HOST_ARCH=x86_32 ${PNACL_BUILD} all
  # Make 64-bit versions of the build tools such as fpcmp (used for llvm
  # test suite and for some reason it matters that they match the build machine)
  ${PNACL_BUILD} llvm-configure
  PNACL_MAKE_OPTS=BUILD_DIRS_ONLY=1 ${PNACL_BUILD} llvm-make

  # run the torture tests. the "trybot" phases take care of prerequisites
  # for both test sets
  for arch in ${archset}; do
    echo "@@@BUILD_STEP torture_tests $arch @@@"
    ${TORTURE_TEST} pnacl ${arch} --bot --verbose \
      --concurrency=${PNACL_CONCURRENCY} || handle-error
  done

  for arch in ${archset}; do
    echo "@@@BUILD_STEP llvm-test-suite $arch @@@"
    ${LLVM_TESTSUITE} testsuite-prereq ${arch}
    ${LLVM_TESTSUITE} testsuite-clean

    { ${LLVM_TESTSUITE} testsuite-configure ${arch} &&
        ${LLVM_TESTSUITE} testsuite-run ${arch} &&
        ${LLVM_TESTSUITE} testsuite-report ${arch} -v -c
    } || handle-error

    scons-tests-pic ${arch}

    archived-frontend-test ${arch}

    # Note: we do not build the sandboxed translator on this bot
    # because this would add another 20min to the build time.
    # The upshot of this is that we are using the sandboxed
    # toolchain which is currently deps'ed in.
    # There is a small upside here: we will notice that bitcode has
    # changed in a way that is incompatible with older translators.
    # TODO(pnacl-team): rethink this.
    # Note: the tests which use sandboxed translation are at the end,
    # because they can sometimes hang on arm, causing buildbot to kill the
    # script without running any more tests.
    scons-tests-translator ${arch}

    archived-pexe-translator-test ${arch}

    if [[ ${arch} = x86-64 ]] ; then
      scons-tests-x86-64-zero-based-sandbox
    fi

  done
}


if [ $# = 0 ]; then
  # NOTE: this is used for manual testing only
  tc-test-bot "x86-64 x86-32 arm"
else
  "$@"
fi
