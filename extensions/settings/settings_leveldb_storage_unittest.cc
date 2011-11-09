// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/settings/settings_storage_unittest.h"

#include "chrome/browser/extensions/settings/settings_leveldb_storage.h"

namespace extensions {

namespace {

SettingsStorage* Param(
    const FilePath& file_path, const std::string& extension_id) {
  return SettingsLeveldbStorage::Create(file_path, extension_id);
}

}  // namespace

INSTANTIATE_TEST_CASE_P(
    SettingsLeveldbStorage,
    SettingsStorageTest,
    testing::Values(&Param));

}  // namespace extensions
