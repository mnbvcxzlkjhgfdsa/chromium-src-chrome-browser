// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/download/chrome_download_manager_delegate.h"

#include "base/callback.h"
#include "base/file_util.h"
#include "base/path_service.h"
#include "base/rand_util.h"
#include "base/stringprintf.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/download/download_extensions.h"
#include "chrome/browser/download/download_file_picker.h"
#include "chrome/browser/download/download_history.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/download/download_safe_browsing_client.h"
#include "chrome/browser/download/download_util.h"
#include "chrome/browser/download/save_package_file_picker.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/prefs/pref_member.h"
#include "chrome/browser/prefs/pref_service.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/safe_browsing/safe_browsing_service.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/extensions/user_script.h"
#include "chrome/common/pref_names.h"
#include "content/browser/download/download_item.h"
#include "content/browser/download/download_manager.h"
#include "content/browser/download/download_status_updater.h"
#include "content/browser/tab_contents/tab_contents.h"
#include "grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"

ChromeDownloadManagerDelegate::ChromeDownloadManagerDelegate(Profile* profile)
    : download_prefs_(new DownloadPrefs(profile->GetPrefs())) {
}

ChromeDownloadManagerDelegate::~ChromeDownloadManagerDelegate() {
}

bool ChromeDownloadManagerDelegate::ShouldStartDownload(int32 download_id) {
  // We create a download item and store it in our download map, and inform the
  // history system of a new download. Since this method can be called while the
  // history service thread is still reading the persistent state, we do not
  // insert the new DownloadItem into 'history_downloads_' or inform our
  // observers at this point. OnCreateDownloadEntryComplete() handles that
  // finalization of the the download creation as a callback from the history
  // thread.
  DownloadItem* download =
      download_manager_->GetActiveDownloadItem(download_id);
  if (!download)
    return false;

#if defined(ENABLE_SAFE_BROWSING)
  // Create a client to verify download URL with safebrowsing.
  // It deletes itself after the callback.
  scoped_refptr<DownloadSBClient> sb_client = new DownloadSBClient(
      download_id, download->url_chain(), download->referrer_url(),
          download_manager_->profile()->GetPrefs()->GetBoolean(
              prefs::kSafeBrowsingEnabled));
  sb_client->CheckDownloadUrl(
      NewCallback(this, &ChromeDownloadManagerDelegate::CheckDownloadUrlDone));
#else
  CheckDownloadUrlDone(download_id, false);
#endif
  return false;
}

void ChromeDownloadManagerDelegate::ChooseDownloadPath(
    TabContents* tab_contents,
    const FilePath& suggested_path,
    void* data) {
  // Deletes itself.
  new DownloadFilePicker(
      download_manager_, tab_contents, suggested_path, data);
}

TabContents* ChromeDownloadManagerDelegate::
    GetAlternativeTabContentsToNotifyForDownload() {
  // Start the download in the last active browser. This is not ideal but better
  // than fully hiding the download from the user.
  Browser* last_active = BrowserList::GetLastActiveWithProfile(
      download_manager_->profile());
  return last_active ? last_active->GetSelectedTabContents() : NULL;
}


bool ChromeDownloadManagerDelegate::ShouldOpenFileBasedOnExtension(
    const FilePath& path) {
  FilePath::StringType extension = path.Extension();
  if (extension.empty())
    return false;
  if (Extension::IsExtension(path))
    return false;
  DCHECK(extension[0] == FilePath::kExtensionSeparator);
  extension.erase(0, 1);
  return download_prefs_->IsAutoOpenEnabledForExtension(extension);
}

bool ChromeDownloadManagerDelegate::GenerateFileHash() {
#if defined(ENABLE_SAFE_BROWSING)
  return download_manager_->profile()->GetPrefs()->GetBoolean(
      prefs::kSafeBrowsingEnabled) &&
          g_browser_process->safe_browsing_service()->DownloadBinHashNeeded();
#else
  return false;
#endif
}

void ChromeDownloadManagerDelegate::GetSaveDir(TabContents* tab_contents,
                                               FilePath* website_save_dir,
                                               FilePath* download_save_dir) {
  Profile* profile =
      Profile::FromBrowserContext(tab_contents->browser_context());
  PrefService* prefs = profile->GetPrefs();

  // Check whether the preference has the preferred directory for saving file.
  // If not, initialize it with default directory.
  if (!prefs->FindPreference(prefs::kSaveFileDefaultDirectory)) {
    DCHECK(prefs->FindPreference(prefs::kDownloadDefaultDirectory));
    FilePath default_save_path = prefs->GetFilePath(
        prefs::kDownloadDefaultDirectory);
    prefs->RegisterFilePathPref(prefs::kSaveFileDefaultDirectory,
                                default_save_path,
                                PrefService::UNSYNCABLE_PREF);
  }

  // Get the directory from preference.
  *website_save_dir = prefs->GetFilePath(prefs::kSaveFileDefaultDirectory);
  DCHECK(!website_save_dir->empty());

  *download_save_dir = prefs->GetFilePath(prefs::kDownloadDefaultDirectory);
}

void ChromeDownloadManagerDelegate::ChooseSavePath(
    const base::WeakPtr<SavePackage>& save_package,
    const FilePath& suggested_path,
    bool can_save_as_complete) {
  // Deletes itself.
  new SavePackageFilePicker(
      save_package, suggested_path, can_save_as_complete,
      download_prefs_.get());
}

void ChromeDownloadManagerDelegate::DownloadProgressUpdated() {
  if (!g_browser_process->download_status_updater())
    return;

  float progress = 0;
  int download_count = 0;
  bool progress_known =
      g_browser_process->download_status_updater()->GetProgress(
          &progress, &download_count);
  download_util::UpdateAppIconDownloadProgress(
      download_count, progress_known, progress);
}

void ChromeDownloadManagerDelegate::CheckDownloadUrlDone(
    int32 download_id, bool is_dangerous_url) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  DownloadItem* download =
      download_manager_->GetActiveDownloadItem(download_id);
  if (!download)
    return;

  if (is_dangerous_url)
    download->MarkUrlDangerous();

  download_manager_->download_history()->CheckVisitedReferrerBefore(
      download_id,
      download->referrer_url(),
      NewCallback(this,
          &ChromeDownloadManagerDelegate::CheckVisitedReferrerBeforeDone));
}

void ChromeDownloadManagerDelegate::CheckVisitedReferrerBeforeDone(
    int32 download_id,
    bool visited_referrer_before) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  DownloadItem* download =
      download_manager_->GetActiveDownloadItem(download_id);
  if (!download)
    return;

  // Check whether this download is for an extension install or not.
  // Allow extensions to be explicitly saved.
  DownloadStateInfo state = download->state_info();
  if (!state.prompt_user_for_save_location) {
    if (UserScript::IsURLUserScript(download->GetURL(),
        download->mime_type()) ||
        (download->mime_type() == Extension::kMimeType)) {
      state.is_extension_install = true;
    }
  }

  if (state.force_file_name.empty()) {
    FilePath generated_name;
    download_util::GenerateFileNameFromRequest(*download,
                                               &generated_name);

    // Freeze the user's preference for showing a Save As dialog.  We're going
    // to bounce around a bunch of threads and we don't want to worry about race
    // conditions where the user changes this pref out from under us.
    if (download_prefs_->PromptForDownload()) {
      // But ignore the user's preference for the following scenarios:
      // 1) Extension installation. Note that we only care here about the case
      //    where an extension is installed, not when one is downloaded with
      //    "save as...".
      // 2) Filetypes marked "always open." If the user just wants this file
      //    opened, don't bother asking where to keep it.
      if (!state.is_extension_install &&
          !ShouldOpenFileBasedOnExtension(generated_name))
        state.prompt_user_for_save_location = true;
    }
    if (download_prefs_->IsDownloadPathManaged()) {
      state.prompt_user_for_save_location = false;
    }

    // Determine the proper path for a download, by either one of the following:
    // 1) using the default download directory.
    // 2) prompting the user.
    if (state.prompt_user_for_save_location &&
        !download_manager_->last_download_path().empty()) {
      state.suggested_path = download_manager_->last_download_path();
    } else {
      state.suggested_path = download_prefs_->download_path();
    }
    state.suggested_path = state.suggested_path.Append(generated_name);
  } else {
    state.suggested_path = state.force_file_name;
  }

  if (!state.prompt_user_for_save_location && state.force_file_name.empty()) {
    state.is_dangerous_file =
        IsDangerousFile(*download, state, visited_referrer_before);
  }

  // We need to move over to the download thread because we don't want to stat
  // the suggested path on the UI thread.
  // We can only access preferences on the UI thread, so check the download path
  // now and pass the value to the FILE thread.
  BrowserThread::PostTask(
      BrowserThread::FILE, FROM_HERE,
      NewRunnableMethod(
          this,
          &ChromeDownloadManagerDelegate::CheckIfSuggestedPathExists,
          download->id(),
          state,
          download_prefs_->download_path()));
}

void ChromeDownloadManagerDelegate::CheckIfSuggestedPathExists(
    int32 download_id,
    DownloadStateInfo state,
    const FilePath& default_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::FILE));

  // Make sure the default download directory exists.
  // TODO(phajdan.jr): only create the directory when we're sure the user
  // is going to save there and not to another directory of his choice.
  file_util::CreateDirectory(default_path);

  // Check writability of the suggested path. If we can't write to it, default
  // to the user's "My Documents" directory. We'll prompt them in this case.
  FilePath dir = state.suggested_path.DirName();
  FilePath filename = state.suggested_path.BaseName();
  if (!file_util::PathIsWritable(dir)) {
    VLOG(1) << "Unable to write to directory \"" << dir.value() << "\"";
    state.prompt_user_for_save_location = true;
    PathService::Get(chrome::DIR_USER_DOCUMENTS, &state.suggested_path);
    state.suggested_path = state.suggested_path.Append(filename);
  }

  // If the download is deemed dangerous, we'll use a temporary name for it.
  if (state.IsDangerous()) {
    state.target_name = FilePath(state.suggested_path).BaseName();
    // Create a temporary file to hold the file until the user approves its
    // download.
    FilePath::StringType file_name;
    FilePath path;
#if defined(OS_WIN)
    string16 unconfirmed_prefix =
        l10n_util::GetStringUTF16(IDS_DOWNLOAD_UNCONFIRMED_PREFIX);
#else
    std::string unconfirmed_prefix =
        l10n_util::GetStringUTF8(IDS_DOWNLOAD_UNCONFIRMED_PREFIX);
#endif

    while (path.empty()) {
      base::SStringPrintf(
          &file_name,
          unconfirmed_prefix.append(
              FILE_PATH_LITERAL(" %d.crdownload")).c_str(),
          base::RandInt(0, 100000));
      path = dir.Append(file_name);
      if (file_util::PathExists(path))
        path = FilePath();
    }
    state.suggested_path = path;
  } else {
    // Do not add the path uniquifier if we are saving to a specific path as in
    // the drag-out case.
    if (state.force_file_name.empty()) {
      state.path_uniquifier = download_util::GetUniquePathNumberWithCrDownload(
          state.suggested_path);
    }
    // We know the final path, build it if necessary.
    if (state.path_uniquifier > 0) {
      download_util::AppendNumberToPath(&(state.suggested_path),
                                        state.path_uniquifier);
      // Setting path_uniquifier to 0 to make sure we don't try to unique it
      // later on.
      state.path_uniquifier = 0;
    } else if (state.path_uniquifier == -1) {
      // We failed to find a unique path.  We have to prompt the user.
      VLOG(1) << "Unable to find a unique path for suggested path \""
              << state.suggested_path.value() << "\"";
      state.prompt_user_for_save_location = true;
    }
  }

  // Create an empty file at the suggested path so that we don't allocate the
  // same "non-existant" path to multiple downloads.
  // See: http://code.google.com/p/chromium/issues/detail?id=3662
  if (!state.prompt_user_for_save_location &&
      state.force_file_name.empty()) {
    if (state.IsDangerous())
      file_util::WriteFile(state.suggested_path, "", 0);
    else
      file_util::WriteFile(download_util::GetCrDownloadPath(
          state.suggested_path), "", 0);
  }

  BrowserThread::PostTask(
      BrowserThread::UI, FROM_HERE,
      NewRunnableMethod(
          this,
          &ChromeDownloadManagerDelegate::OnPathExistenceAvailable,
          download_id,
          state));
}

void ChromeDownloadManagerDelegate::OnPathExistenceAvailable(
    int32 download_id,
    const DownloadStateInfo& new_state) {
  DownloadItem* download =
      download_manager_->GetActiveDownloadItem(download_id);
  if (!download)
    return;
  download->SetFileCheckResults(new_state);
  download_manager_->RestartDownload(download_id);
}

// TODO(phajdan.jr): This is apparently not being exercised in tests.
bool ChromeDownloadManagerDelegate::IsDangerousFile(
    const DownloadItem& download,
    const DownloadStateInfo& state,
    bool visited_referrer_before) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  bool auto_open = ShouldOpenFileBasedOnExtension(state.suggested_path);
  download_util::DownloadDangerLevel danger_level =
      download_util::GetFileDangerLevel(state.suggested_path.BaseName());

  if (danger_level == download_util::Dangerous)
    return !(auto_open && state.has_user_gesture);

  if (danger_level == download_util::AllowOnUserGesture &&
      (!state.has_user_gesture || !visited_referrer_before))
    return true;

  if (state.is_extension_install) {
    // Extensions that are not from the gallery are considered dangerous.
    ExtensionService* service =
        download_manager_->profile()->GetExtensionService();
    if (!service || !service->IsDownloadFromGallery(download.GetURL(),
                                                    download.referrer_url()))
      return true;
  }
  return false;
}
