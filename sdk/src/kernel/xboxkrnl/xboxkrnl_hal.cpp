/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

void HalReturnToFirmware_entry(u32 routine) {
  // void
  // IN FIRMWARE_REENTRY  Routine

  // Routine must be 1 'HalRebootRoutine'
  assert_true(routine == 1);

  // guwange-nx (2026-10-22): the title ends as XamLoaderLaunchTitle / XamLoaderTerminateTitle end it (its guest
  // threads leave at their next kernel call, then the app quits through its normal exit). exit(0) here returned a Switch
  // NRO to hbloader with the game's guest, GPU and audio threads still running (peggle-nx's finding: hbmenu then
  // crashes on heap pages they alias); the app's exit asks the system to end the process.
  REXKRNL_ERROR("Game requested shutdown via HalReturnToFirmware");
  REX_KERNEL_STATE()->TerminateTitle();
  rex::system::XThread::CheckTitleTermination();  // this thread leaves too (does not return)
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__HalReturnToFirmware, rex::kernel::xboxkrnl::HalReturnToFirmware_entry)

REX_EXPORT_STUB(__imp__HalGetCurrentAVPack);
REX_EXPORT_STUB(__imp__HalGpioControl);
REX_EXPORT_STUB(__imp__HalOpenCloseODDTray);
REX_EXPORT_STUB(__imp__HalReadWritePCISpace);
REX_EXPORT_STUB(__imp__HalRegisterPowerDownNotification);
REX_EXPORT_STUB(__imp__HalRegisterSMCNotification);
REX_EXPORT_STUB(__imp__HalSendSMCMessage);
REX_EXPORT_STUB(__imp__HalSetAudioEnable);
REX_EXPORT_STUB(__imp__HalIsExecutingPowerDownDpc);
REX_EXPORT_STUB(__imp__HalGetPowerUpCause);
REX_EXPORT_STUB(__imp__HalRegisterPowerDownCallback);
REX_EXPORT_STUB(__imp__HalRegisterBackgroundModeTransitionCallback);
REX_EXPORT_STUB(__imp__HalClampUnclampOutputDACs);
REX_EXPORT_STUB(__imp__HalPowerDownToBackgroundMode);
REX_EXPORT_STUB(__imp__HalNotifyAddRemoveBackgroundTask);
REX_EXPORT_STUB(__imp__HalCallBackgroundModeNotificationRoutines);
REX_EXPORT_STUB(__imp__HalGetMemoryInformation);
REX_EXPORT_STUB(__imp__HalNotifyBackgroundModeTransitionComplete);
REX_EXPORT_STUB(__imp__HalFinalizePowerLossRecovery);
REX_EXPORT_STUB(__imp__HalSetPowerLossRecovery);
REX_EXPORT_STUB(__imp__HalRegisterXamPowerDownCallback);
REX_EXPORT_STUB(__imp__HalRegisterHdDvdRomNotification);
REX_EXPORT_STUB(__imp__HalGetNotedArgonErrors);
REX_EXPORT_STUB(__imp__HalReadArgonEeprom);
REX_EXPORT_STUB(__imp__HalWriteArgonEeprom);
REX_EXPORT_STUB(__imp__HalConfigureVeDevice);
