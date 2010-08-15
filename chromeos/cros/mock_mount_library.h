// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_CROS_MOCK_MOUNT_LIBRARY_H_
#define CHROME_BROWSER_CHROMEOS_CROS_MOCK_MOUNT_LIBRARY_H_
#pragma once

#include <string>

#include "base/observer_list.h"
#include "base/time.h"
#include "cros/chromeos_mount.h"
#include "chrome/browser/chromeos/cros/mount_library.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace chromeos {

class MockMountLibrary : public MountLibrary {
 public:
  MockMountLibrary();
  virtual ~MockMountLibrary();

  MOCK_METHOD1(AddObserver, void(MountLibrary::Observer*));
  MOCK_METHOD1(MountPath, bool(const char*));
  MOCK_METHOD1(RemoveObserver, void(MountLibrary::Observer*));
  MOCK_CONST_METHOD0(disks, const MountLibrary::DiskVector&(void));

  void FireDeviceInsertEvents();
  void FireDeviceRemoveEvents();

 private:
  void AddObserverInternal(MountLibrary::Observer* observer);
  void RemoveObserverInternal(MountLibrary::Observer* observer);
  const MountLibrary::DiskVector& disksInternal() const { return disks_; }


  void UpdateMountStatus(MountEventType evt,
                         const std::string& path);

  ObserverList<MountLibrary::Observer> observers_;

  // The list of disks found.
  MountLibrary::DiskVector disks_;

  DISALLOW_COPY_AND_ASSIGN(MockMountLibrary);
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_CROS_MOCK_MOUNT_LIBRARY_H_
