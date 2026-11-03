#include "stdafx.h"
#include "SecurityAttribute.h"
#include <Sddl.h>

#include <cstdlib>

#ifndef SDDL_ALL_APP_PACKAGES
#define SDDL_ALL_APP_PACKAGES TEXT("AC")
#endif

#define LOW_INTEGRITY_SDDL_SACL \
  SDDL_SACL                     \
  SDDL_DELIMINATOR              \
  SDDL_ACE_BEGIN                \
  SDDL_MANDATORY_LABEL          \
  SDDL_SEPERATOR                \
  SDDL_SEPERATOR                \
  SDDL_NO_WRITE_UP              \
  SDDL_SEPERATOR                \
  SDDL_SEPERATOR                \
  SDDL_SEPERATOR                \
  SDDL_ML_LOW                   \
  SDDL_ACE_END

#define LOCAL_SYSTEM_FILE_ACCESS \
  SDDL_ACE_BEGIN                 \
  SDDL_ACCESS_ALLOWED            \
  SDDL_SEPERATOR                 \
  SDDL_SEPERATOR                 \
  SDDL_FILE_ALL                  \
  SDDL_SEPERATOR                 \
  SDDL_SEPERATOR                 \
  SDDL_SEPERATOR                 \
  SDDL_LOCAL_SYSTEM              \
  SDDL_ACE_END

#define EVERYONE_FILE_ACCESS \
  SDDL_ACE_BEGIN             \
  SDDL_ACCESS_ALLOWED        \
  SDDL_SEPERATOR             \
  SDDL_SEPERATOR             \
  SDDL_FILE_ALL              \
  SDDL_SEPERATOR             \
  SDDL_SEPERATOR             \
  SDDL_SEPERATOR             \
  SDDL_EVERYONE              \
  SDDL_ACE_END

#define ALL_APP_PACKAGES_FILE_ACCESS \
  SDDL_ACE_BEGIN                     \
  SDDL_ACCESS_ALLOWED                \
  SDDL_SEPERATOR                     \
  SDDL_SEPERATOR                     \
  SDDL_FILE_ALL                      \
  SDDL_SEPERATOR                     \
  SDDL_SEPERATOR                     \
  SDDL_SEPERATOR                     \
  SDDL_ALL_APP_PACKAGES              \
  SDDL_ACE_END

namespace weasel {

void SecurityAttribute::_Init() {
  // Privileges for UWP and IE protected mode
  // https://stackoverflow.com/questions/39138674/accessing-named-pipe-servers-from-within-ie-epm-bho

  // Fail fast instead of silently falling back to the process-default
  // security descriptor: the low-integrity/UWP grants in this SDDL are
  // load-bearing, and an unchecked failure would leave clients (UWP, IE
  // EPM) rejected by the default DACL with no clue why. There is no logging
  // facility linked into WeaselIPCServer, and WeaselServer.exe is
  // WER-registered for mini dumps (WeaselSetup/imesetup.cpp), so abort() is
  // both the fail-fast and the diagnosable choice here.
  if (!ConvertStringSecurityDescriptorToSecurityDescriptor(
          LOW_INTEGRITY_SDDL_SACL SDDL_DACL SDDL_DELIMINATOR
              LOCAL_SYSTEM_FILE_ACCESS EVERYONE_FILE_ACCESS
                  ALL_APP_PACKAGES_FILE_ACCESS,
          SDDL_REVISION_1, &pd, NULL)) {
    abort();
  }
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor = pd;
  // The security descriptor guards a named pipe; handles are never inherited
  // through it, so bInheritHandle is meaningless and kept FALSE.
  sa.bInheritHandle = FALSE;
}

SECURITY_ATTRIBUTES* SecurityAttribute::get_attr() {
  return &sa;
}
};  // namespace weasel
