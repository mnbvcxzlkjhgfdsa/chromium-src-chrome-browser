// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_BROWSING_DATA_BROWSING_DATA_LOCAL_STORAGE_HELPER_H_
#define CHROME_BROWSER_BROWSING_DATA_BROWSING_DATA_LOCAL_STORAGE_HELPER_H_

#include <list>
#include <set>
#include <vector>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/files/file_path.h"
#include "base/memory/ref_counted.h"
#include "base/synchronization/lock.h"
#include "base/time/time.h"
#include "chrome/common/url_constants.h"
#include "content/public/browser/dom_storage_context.h"
#include "url/gurl.h"

class Profile;

// This class fetches local storage information and provides a
// means to delete the data associated with an origin.
class BrowsingDataLocalStorageHelper
    : public base::RefCounted<BrowsingDataLocalStorageHelper> {
 public:
  // Contains detailed information about local storage.
  struct LocalStorageInfo {
    LocalStorageInfo(
        const GURL& origin_url,
        int64 size,
        base::Time last_modified);
    ~LocalStorageInfo();

    GURL origin_url;
    int64 size;
    base::Time last_modified;
  };

  explicit BrowsingDataLocalStorageHelper(Profile* profile);

  // Starts the fetching process, which will notify its completion via
  // callback. This must be called only in the UI thread.
  virtual void StartFetching(
      const base::Callback<void(const std::list<LocalStorageInfo>&)>& callback);

  // Deletes the local storage for the |origin|.
  virtual void DeleteOrigin(const GURL& origin);

 protected:
  friend class base::RefCounted<BrowsingDataLocalStorageHelper>;
  virtual ~BrowsingDataLocalStorageHelper();

  void CallCompletionCallback();

  content::DOMStorageContext* dom_storage_context_;  // Owned by the profile
  base::Callback<void(const std::list<LocalStorageInfo>&)> completion_callback_;
  bool is_fetching_;
  std::list<LocalStorageInfo> local_storage_info_;

 private:
  void GetUsageInfoCallback(
      const std::vector<dom_storage::LocalStorageUsageInfo>& infos);

  DISALLOW_COPY_AND_ASSIGN(BrowsingDataLocalStorageHelper);
};

// This class is a thin wrapper around BrowsingDataLocalStorageHelper that does
// not fetch its information from the local storage tracker, but gets them
// passed as a parameter during construction.
class CannedBrowsingDataLocalStorageHelper
    : public BrowsingDataLocalStorageHelper {
 public:
  explicit CannedBrowsingDataLocalStorageHelper(Profile* profile);

  // Return a copy of the local storage helper. Only one consumer can use the
  // StartFetching method at a time, so we need to create a copy of the helper
  // every time we instantiate a cookies tree model for it.
  CannedBrowsingDataLocalStorageHelper* Clone();

  // Add a local storage to the set of canned local storages that is returned
  // by this helper.
  void AddLocalStorage(const GURL& origin);

  // Clear the list of canned local storages.
  void Reset();

  // True if no local storages are currently stored.
  bool empty() const;

  // Returns the number of local storages currently stored.
  size_t GetLocalStorageCount() const;

  // Returns the set of origins that use local storage.
  const std::set<GURL>& GetLocalStorageInfo() const;

  // BrowsingDataLocalStorageHelper implementation.
  virtual void StartFetching(
      const base::Callback<void(const std::list<LocalStorageInfo>&)>& callback)
          OVERRIDE;

 private:
  virtual ~CannedBrowsingDataLocalStorageHelper();

  // Convert the pending local storage info to local storage info objects.
  void ConvertPendingInfo();

  std::set<GURL> pending_local_storage_info_;

  Profile* profile_;

  DISALLOW_COPY_AND_ASSIGN(CannedBrowsingDataLocalStorageHelper);
};

#endif  // CHROME_BROWSER_BROWSING_DATA_BROWSING_DATA_LOCAL_STORAGE_HELPER_H_
