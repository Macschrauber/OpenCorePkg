/** @file
  Copyright (C) 2023, Goldfish64. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "BootManagementInternal.h"

#include <Protocol/DevicePath.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcMiscLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define MBR_SECTOR_SIZE  512

//
// PIWG firmware media device path for Apple legacy interface.
// FwFile(2B0585EB-D8B8-49A9-8B8CE21B01AEF2B7)
//
static CONST UINT8                     AppleLegacyInterfaceMediaDevicePathData[] = {
  0x04, 0x06, 0x14, 0x00, 0xEB, 0x85, 0x05, 0x2B,
  0xB8, 0xD8, 0xA9, 0x49, 0x8B, 0x8C, 0xE2, 0x1B,
  0x01, 0xAE, 0xF2, 0xB7, 0x7F, 0xFF, 0x04, 0x00,
};
static CONST EFI_DEVICE_PATH_PROTOCOL  *AppleLegacyInterfaceMediaDevicePathPath = (EFI_DEVICE_PATH_PROTOCOL *)AppleLegacyInterfaceMediaDevicePathData;

#define MAX_APPLE_LEGACY_DEVICE_PATHS  16

STATIC
BOOLEAN
CheckLegacySignature (
  IN CONST CHAR8  *SignatureStr,
  IN UINT8        *Buffer,
  IN UINT32       BufferSize
  )
{
  UINT32  Offset;

  Offset = 0;

  return FindPattern ((CONST UINT8 *)SignatureStr, NULL, (CONST UINT32)AsciiStrLen (SignatureStr), Buffer, BufferSize, &Offset);
}

STATIC
EFI_STATUS
ScanAppleLegacyInterfacePaths (
  IN OUT EFI_DEVICE_PATH_PROTOCOL  **DevicePaths,
  IN     UINTN                     MaxDevicePaths
  )
{
  EFI_STATUS  Status;
  UINTN       NoHandles;
  EFI_HANDLE  *Handles;
  UINTN       HandleIndex;
  UINTN       PathCount;
  UINTN       PathIndex;
  BOOLEAN     DevicePathExists;

  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;

  MaxDevicePaths--;
  PathCount = 0;

  //
  // Get all LoadedImage protocol handles.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiLoadedImageProtocolGuid,
                  NULL,
                  &NoHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (HandleIndex = 0; HandleIndex < NoHandles && PathCount < MaxDevicePaths; HandleIndex++) {
    Status = gBS->HandleProtocol (
                    Handles[HandleIndex],
                    &gEfiLoadedImageProtocolGuid,
                    (VOID **)&LoadedImage
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    LoadedImage->DeviceHandle,
                    &gEfiDevicePathProtocolGuid,
                    (VOID **)&DevicePath
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Legacy boot interface will be behind a memory range node.
    //
    if (  (DevicePathType (DevicePath) != HARDWARE_DEVICE_PATH)
       || (DevicePathSubType (DevicePath) != HW_MEMMAP_DP))
    {
      continue;
    }

    //
    // Ensure we don't add a duplicate path.
    //
    DevicePathExists = FALSE;
    for (PathIndex = 0; PathIndex < PathCount; PathIndex++) {
      if (DevicePathNodeLength (DevicePath) != DevicePathNodeLength (DevicePaths[PathIndex])) {
        continue;
      }

      if (CompareMem (DevicePath, DevicePaths[PathIndex], DevicePathNodeLength (DevicePath))) {
        DevicePathExists = TRUE;
        break;
      }
    }

    if (DevicePathExists) {
      continue;
    }

    DevicePaths[PathCount++] = AppendDevicePath (DevicePath, AppleLegacyInterfaceMediaDevicePathPath);
  }

  FreePool (Handles);

  DevicePaths[PathCount] = NULL;
  return EFI_SUCCESS;
}

EFI_STATUS
InternalLoadAppleLegacyInterface (
  IN  EFI_HANDLE                ParentImageHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL  *HdDevicePath,
  OUT EFI_HANDLE                *ImageHandle
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *WholeDiskPath;
  EFI_DEVICE_PATH_PROTOCOL  **LegacyDevicePaths;
  CHAR16                    *UnicodeDevicePath;
  UINTN                     Index;

  //
  // Get device path to disk to be booted.
  // TODO: Handle CD booting, device path is not required in that case.
  //
  WholeDiskPath = OcDiskGetDevicePath (HdDevicePath);
  if (WholeDiskPath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DebugPrintDevicePath (DEBUG_INFO, "OCB: Legacy disk device path", WholeDiskPath);

  // TODO: Mark target partition as active on pure MBR and hybrid GPT disks.
  // Macs only boot the active partition.

  //
  // Set BootCampHD variable pointing to target disk.
  //
  Status = gRT->SetVariable (
                  APPLE_BOOT_CAMP_HD_VARIABLE_NAME,
                  &gAppleBootVariableGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                  GetDevicePathSize (WholeDiskPath),
                  WholeDiskPath
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Get list of possible locations for Apple legacy interface and attempt to load.
  //
  LegacyDevicePaths = AllocateZeroPool (sizeof (*LegacyDevicePaths) * MAX_APPLE_LEGACY_DEVICE_PATHS);
  if (LegacyDevicePaths == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = ScanAppleLegacyInterfacePaths (LegacyDevicePaths, MAX_APPLE_LEGACY_DEVICE_PATHS);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; LegacyDevicePaths[Index] != NULL; Index++) {
      Status = gBS->LoadImage (
                      FALSE,
                      ParentImageHandle,
                      LegacyDevicePaths[Index],
                      NULL,
                      0,
                      ImageHandle
                      );
      if (Status != EFI_NOT_FOUND) {
        DEBUG_CODE_BEGIN ();

        UnicodeDevicePath = ConvertDevicePathToText (LegacyDevicePaths[Index], FALSE, FALSE);
        DEBUG ((
          DEBUG_INFO,
          "OCB: Loaded Apple legacy interface at dp %s - %r\n",
          UnicodeDevicePath != NULL ? UnicodeDevicePath : L"<null>",
          Status
          ));
        if (UnicodeDevicePath != NULL) {
          FreePool (UnicodeDevicePath);
        }

        DEBUG_CODE_END ();

        break;
      }
    }
  }

  FreePool (LegacyDevicePaths);

  return Status;
}

OC_LEGACY_OS_TYPE
InternalGetDiskLegacyOsType (
  IN  EFI_HANDLE  DiskHandle,
  IN  BOOLEAN     UseBlockIo2
  )
{
  EFI_STATUS         Status;
  UINT8              *Buffer;
  UINT32             BufferSize;
  OC_LEGACY_OS_TYPE  LegacyOsType;

  OC_DISK_CONTEXT  DiskContext;

  ASSERT (DiskHandle != NULL);

  Status = OcDiskInitializeContext (&DiskContext, DiskHandle, UseBlockIo2);
  if (EFI_ERROR (Status)) {
    return OcLegacyOsTypeNone;
  }

  //
  // Retrieve the first sector of the disk.
  //
  BufferSize = ALIGN_VALUE (MBR_SECTOR_SIZE, DiskContext.BlockSize);
  Buffer     = AllocatePool (BufferSize);
  if (Buffer == NULL) {
    DEBUG ((DEBUG_INFO, "OCBP: Buffer allocation error\n"));
    return OcLegacyOsTypeNone;
  }

  Status = OcDiskRead (
             &DiskContext,
             0,
             BufferSize,
             Buffer
             );

  if (EFI_ERROR (Status)) {
    FreePool (Buffer);
    return OcLegacyOsTypeNone;
  }

  //
  // Validate sector contents and check for known signatures
  // indicating the partition is bootable.
  //
  LegacyOsType = OcLegacyOsTypeNone;
  if (CheckLegacySignature ("BOOTMGR", Buffer, BufferSize)) {
    LegacyOsType = OcLegacyOsTypeWindowsBootmgr;
  } else if (CheckLegacySignature ("NTLDR", Buffer, BufferSize)) {
    LegacyOsType = OcLegacyOsTypeWindowsNtldr;
  }

  FreePool (Buffer);

  return LegacyOsType;
}