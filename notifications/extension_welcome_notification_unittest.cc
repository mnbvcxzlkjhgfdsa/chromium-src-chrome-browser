// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/notifications/extension_welcome_notification.h"

#include <string>

#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/prefs/pref_service.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/test_simple_task_runner.h"
#include "base/thread_task_runner_handle.h"
#include "chrome/browser/notifications/notification.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/testing_pref_service_syncable.h"
#include "chrome/test/base/testing_profile.h"
#include "components/user_prefs/pref_registry_syncable.h"
#include "sync/api/sync_error_factory_mock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/message_center/fake_message_center.h"
#include "ui/message_center/notification.h"

const char kChromeNowExtensionID[] = "pafkbggdmjlpgkdkcbjmhmfcdpncadgh";

class MockMessageCenter : public message_center::FakeMessageCenter {
 public:
  MockMessageCenter()
      : add_notification_calls_(0),
        remove_notification_calls_(0),
        notifications_with_shown_as_popup_(0) {};

  int add_notification_calls() { return add_notification_calls_; }
  int remove_notification_calls() { return remove_notification_calls_; }
  int notifications_with_shown_as_popup() {
    return notifications_with_shown_as_popup_;
  }

  // message_center::FakeMessageCenter Overrides
  virtual bool HasNotification(const std::string& id) OVERRIDE {
    return last_notification.get() && (last_notification->id() == id);
  }

  virtual void AddNotification(
      scoped_ptr<message_center::Notification> notification) OVERRIDE {
    EXPECT_FALSE(last_notification.get());
    last_notification.swap(notification);
    add_notification_calls_++;
    if (last_notification->shown_as_popup())
      notifications_with_shown_as_popup_++;
  }

  virtual void RemoveNotification(const std::string& id,
                                  bool by_user) OVERRIDE {
    EXPECT_TRUE(last_notification.get());
    last_notification.reset();
    remove_notification_calls_++;
  }

  void CloseCurrentNotification() {
    EXPECT_TRUE(last_notification.get());
    last_notification->delegate()->Close(true);
    RemoveNotification(last_notification->id(), true);
  }

 private:
  scoped_ptr<message_center::Notification> last_notification;
  int add_notification_calls_;
  int remove_notification_calls_;
  int notifications_with_shown_as_popup_;
};

class WelcomeNotificationDelegate
    : public ExtensionWelcomeNotification::Delegate {
public:
  WelcomeNotificationDelegate()
      : start_time_(base::Time::Now()),
        message_center_(new MockMessageCenter()) {}

  // ExtensionWelcomeNotification::Delegate
  virtual message_center::MessageCenter* GetMessageCenter() OVERRIDE {
    return message_center_.get();
  }

  virtual base::Time GetCurrentTime() OVERRIDE {
    return start_time_ + elapsed_time_;
  }

  virtual void PostTask(
      const tracked_objects::Location& from_here,
      const base::Closure& task) OVERRIDE {
    EXPECT_TRUE(pending_task_.is_null());
    pending_task_ = task;
  }

  // WelcomeNotificationDelegate
  MockMessageCenter* message_center() { return message_center_.get(); }

  base::Time GetStartTime() { return start_time_; }

  void SetElapsedTime(base::TimeDelta elapsed_time) {
    elapsed_time_ = elapsed_time;
  }

  void RunPendingTask() {
    base::Closure task_to_run = pending_task_;
    pending_task_.Reset();
    task_to_run.Run();
  }

private:
  base::Time start_time_;
  base::TimeDelta elapsed_time_;
  scoped_ptr<MockMessageCenter> message_center_;
  base::Closure pending_task_;
};

class TestSyncProcessor : public syncer::SyncChangeProcessor {
  virtual syncer::SyncError ProcessSyncChanges(
      const tracked_objects::Location& from_here,
      const syncer::SyncChangeList& change_list) OVERRIDE {
    return syncer::SyncError();
  }

  virtual syncer::SyncDataList GetAllSyncData(syncer::ModelType type)
      const OVERRIDE {
    return syncer::SyncDataList();
  }
};

class ExtensionWelcomeNotificationTest : public testing::Test {
 protected:
  ExtensionWelcomeNotificationTest() {
    scoped_refptr<user_prefs::PrefRegistrySyncable> pref_registry(
        new user_prefs::PrefRegistrySyncable());
    ExtensionWelcomeNotification::RegisterProfilePrefs(pref_registry.get());
  }

  virtual void SetUp() {
    task_runner_ = new base::TestSimpleTaskRunner();
    thread_task_runner_handle_.reset(
        new base::ThreadTaskRunnerHandle(task_runner_));
    profile_.reset(new TestingProfile());
    delegate_ = new WelcomeNotificationDelegate();
    welcome_notification_ = ExtensionWelcomeNotification::Create(
        kChromeNowExtensionID, profile_.get(), delegate_);
  }

  virtual void TearDown() {
    delegate_ = NULL;
    welcome_notification_.reset();
    profile_.reset();
    thread_task_runner_handle_.reset();
    task_runner_ = NULL;
  }

  void StartPreferenceSyncing() {
    PrefServiceSyncable::FromProfile(profile_.get())
        ->GetSyncableService(syncer::PREFERENCES)
        ->MergeDataAndStartSyncing(
              syncer::PREFERENCES,
              syncer::SyncDataList(),
              scoped_ptr<syncer::SyncChangeProcessor>(new TestSyncProcessor),
              scoped_ptr<syncer::SyncErrorFactory>(
                  new syncer::SyncErrorFactoryMock()));
  }

  void ShowChromeNowNotification() {
    ShowNotification(
        "ChromeNowNotification",
        message_center::NotifierId(message_center::NotifierId::APPLICATION,
                                   kChromeNowExtensionID));
  }

  void ShowRegularNotification() {
    ShowNotification(
        "RegularNotification",
        message_center::NotifierId(message_center::NotifierId::APPLICATION,
                                   "aaaabbbbccccddddeeeeffffggghhhhi"));
  }

  void FlushMessageLoop() { delegate_->RunPendingTask(); }

  MockMessageCenter* message_center() { return delegate_->message_center(); }
  base::TestSimpleTaskRunner* task_runner() { return task_runner_.get(); }
  base::Time GetStartTime() { return delegate_->GetStartTime(); }
  void SetElapsedTime(base::TimeDelta elapsed_time) {
    delegate_->SetElapsedTime(elapsed_time);
  }
  bool GetBooleanPref(const char* path) {
    return profile_->GetPrefs()->GetBoolean(path);
  }
  void SetBooleanPref(const char* path, bool value) {
    profile_->GetPrefs()->SetBoolean(path, value);
  }
  int64 GetInt64Pref(const char* path) {
    return profile_->GetPrefs()->GetInt64(path);
  }
  void SetInt64Pref(const char* path, int64 value) {
    profile_->GetPrefs()->SetInt64(path, value);
  }

 private:
  class TestNotificationDelegate : public NotificationDelegate {
   public:
    explicit TestNotificationDelegate(const std::string& id) : id_(id) {}

    // Overridden from NotificationDelegate:
    virtual void Display() OVERRIDE {}
    virtual void Error() OVERRIDE {}
    virtual void Close(bool by_user) OVERRIDE {}
    virtual void Click() OVERRIDE {}
    virtual void ButtonClick(int index) OVERRIDE {}

    virtual std::string id() const OVERRIDE { return id_; }

    virtual content::RenderViewHost* GetRenderViewHost() const OVERRIDE {
      return NULL;
    }

   private:
    virtual ~TestNotificationDelegate() {}

    const std::string id_;

    DISALLOW_COPY_AND_ASSIGN(TestNotificationDelegate);
  };

  void ShowNotification(std::string notification_id,
                        const message_center::NotifierId& notifier_id) {
    message_center::RichNotificationData rich_notification_data;
    rich_notification_data.priority = 0;
    Notification notification(message_center::NOTIFICATION_TYPE_BASE_FORMAT,
                              GURL("http://tests.url"),
                              base::UTF8ToUTF16("Title"),
                              base::UTF8ToUTF16("Body"),
                              gfx::Image(),
                              blink::WebTextDirectionDefault,
                              notifier_id,
                              base::UTF8ToUTF16("Source"),
                              base::UTF8ToUTF16(notification_id),
                              rich_notification_data,
                              new TestNotificationDelegate("TestNotification"));
    welcome_notification_->ShowWelcomeNotificationIfNecessary(notification);
  }

  scoped_refptr<base::TestSimpleTaskRunner> task_runner_;
  scoped_ptr<base::ThreadTaskRunnerHandle> thread_task_runner_handle_;
  scoped_ptr<TestingProfile> profile_;
  // Weak Ref owned by welcome_notification_
  WelcomeNotificationDelegate* delegate_;
  scoped_ptr<ExtensionWelcomeNotification> welcome_notification_;
};

// Show a regular notification. Expect that WelcomeNotification will
// not show a welcome notification.
TEST_F(ExtensionWelcomeNotificationTest, FirstRunShowRegularNotification) {
  StartPreferenceSyncing();
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowRegularNotification();

  EXPECT_TRUE(message_center()->add_notification_calls() == 0);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Show a Chrome Now notification. Expect that WelcomeNotification will
// show a welcome notification.
TEST_F(ExtensionWelcomeNotificationTest, FirstRunChromeNowNotification) {
  StartPreferenceSyncing();
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowChromeNowNotification();

  EXPECT_TRUE(message_center()->add_notification_calls() == 1);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Show a Chrome Now notification that was already shown before.
TEST_F(ExtensionWelcomeNotificationTest, ShowWelcomeNotificationAgain) {
  StartPreferenceSyncing();
  SetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp, true);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowChromeNowNotification();

  EXPECT_TRUE(message_center()->add_notification_calls() == 1);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 1);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Don't show a welcome notification if it was previously dismissed
TEST_F(ExtensionWelcomeNotificationTest,
       WelcomeNotificationPreviouslyDismissed) {
  StartPreferenceSyncing();
  SetBooleanPref(prefs::kWelcomeNotificationDismissed, true);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowChromeNowNotification();

  EXPECT_TRUE(message_center()->add_notification_calls() == 0);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Show a Chrome Now notification and dismiss it.
// Expect welcome toast dismissed to be true.
TEST_F(ExtensionWelcomeNotificationTest, DismissWelcomeNotification) {
  StartPreferenceSyncing();
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowChromeNowNotification();
  message_center()->CloseCurrentNotification();
  FlushMessageLoop();

  EXPECT_TRUE(message_center()->add_notification_calls() == 1);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 1);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Show a Chrome Now notification and dismiss it via a synced preference change.
// Expect welcome toast dismissed to be true.
TEST_F(ExtensionWelcomeNotificationTest, SyncedDismissalWelcomeNotification) {
  StartPreferenceSyncing();
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowChromeNowNotification();
  SetBooleanPref(prefs::kWelcomeNotificationDismissed, true);

  EXPECT_TRUE(message_center()->add_notification_calls() == 1);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 1);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Simulate a delayed preference sync when the welcome notification was
// previously dismissed.
TEST_F(ExtensionWelcomeNotificationTest,
       DelayedPreferenceSyncPreviouslyDismissed) {
  // Show a notification while the preference system is not syncing.
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowChromeNowNotification();

  EXPECT_TRUE(message_center()->add_notification_calls() == 0);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  // Now start the preference syncing with a previously dismissed welcome.
  SetBooleanPref(prefs::kWelcomeNotificationDismissed, true);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  StartPreferenceSyncing();

  EXPECT_TRUE(message_center()->add_notification_calls() == 0);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Simulate a delayed preference sync when the welcome notification was
// never shown.
TEST_F(ExtensionWelcomeNotificationTest, DelayedPreferenceSyncNeverShown) {
  // Show a notification while the preference system is not syncing.
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  ShowChromeNowNotification();

  EXPECT_TRUE(message_center()->add_notification_calls() == 0);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  // Now start the preference syncing with the default preference values.
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));

  StartPreferenceSyncing();

  EXPECT_TRUE(message_center()->add_notification_calls() == 1);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
}

// Simulate the passage of time when the welcome notification
// automatically dismisses.
TEST_F(ExtensionWelcomeNotificationTest, TimeExpiredNotification) {
  StartPreferenceSyncing();
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
  EXPECT_TRUE(
      GetInt64Pref(prefs::kWelcomeNotificationExpirationTimestamp) == 0);
  EXPECT_TRUE(task_runner()->GetPendingTasks().empty());

  ShowChromeNowNotification();

  base::TimeDelta requested_show_time =
      base::TimeDelta::FromDays(
          ExtensionWelcomeNotification::kRequestedShowTimeDays);

  EXPECT_TRUE(task_runner()->GetPendingTasks().size() == 1);
  EXPECT_TRUE(task_runner()->NextPendingTaskDelay() == requested_show_time);

  EXPECT_TRUE(message_center()->add_notification_calls() == 1);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
  EXPECT_TRUE(
      GetInt64Pref(prefs::kWelcomeNotificationExpirationTimestamp) ==
          (GetStartTime() + requested_show_time).ToInternalValue());

  SetElapsedTime(requested_show_time);
  task_runner()->RunPendingTasks();

  EXPECT_TRUE(task_runner()->GetPendingTasks().empty());
  EXPECT_TRUE(message_center()->add_notification_calls() == 1);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 1);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
  EXPECT_TRUE(
      GetInt64Pref(prefs::kWelcomeNotificationExpirationTimestamp) ==
          (GetStartTime() + requested_show_time).ToInternalValue());
}

// Simulate the passage of time after Chrome is closed and the welcome
// notification expiration elapses.
TEST_F(ExtensionWelcomeNotificationTest, NotificationPreviouslyExpired) {
  StartPreferenceSyncing();
  SetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp, true);
  SetInt64Pref(prefs::kWelcomeNotificationExpirationTimestamp, 1);
  EXPECT_FALSE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
  EXPECT_TRUE(
      GetInt64Pref(prefs::kWelcomeNotificationExpirationTimestamp) == 1);
  EXPECT_TRUE(task_runner()->GetPendingTasks().empty());

  base::TimeDelta requested_show_time =
      base::TimeDelta::FromDays(
          ExtensionWelcomeNotification::kRequestedShowTimeDays);
  SetElapsedTime(requested_show_time);
  ShowChromeNowNotification();

  EXPECT_TRUE(task_runner()->GetPendingTasks().empty());
  EXPECT_TRUE(message_center()->add_notification_calls() == 0);
  EXPECT_TRUE(message_center()->remove_notification_calls() == 0);
  EXPECT_TRUE(message_center()->notifications_with_shown_as_popup() == 0);
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationDismissed));
  EXPECT_TRUE(GetBooleanPref(prefs::kWelcomeNotificationPreviouslyPoppedUp));
  EXPECT_TRUE(
      GetInt64Pref(prefs::kWelcomeNotificationExpirationTimestamp) == 1);
}
