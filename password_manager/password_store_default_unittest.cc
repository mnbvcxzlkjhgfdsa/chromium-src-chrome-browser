// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/basictypes.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/prefs/pref_service.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/testing_profile.h"
#include "components/password_manager/core/browser/password_form_data.h"
#include "components/password_manager/core/browser/password_store_change.h"
#include "components/password_manager/core/browser/password_store_consumer.h"
#include "components/password_manager/core/browser/password_store_default.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_source.h"
#include "content/public/test/mock_notification_observer.h"
#include "content/public/test/test_browser_thread.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using autofill::PasswordForm;
using base::WaitableEvent;
using content::BrowserThread;
using testing::_;
using testing::DoAll;
using testing::ElementsAreArray;
using testing::Pointee;
using testing::Property;
using testing::WithArg;

namespace {

class MockPasswordStoreConsumer : public PasswordStoreConsumer {
 public:
  MOCK_METHOD1(OnGetPasswordStoreResults,
               void(const std::vector<PasswordForm*>&));
};

class MockPasswordStoreObserver : public PasswordStore::Observer {
 public:
  MOCK_METHOD1(OnLoginsChanged,
               void(const PasswordStoreChangeList& changes));
};

}  // anonymous namespace

class PasswordStoreDefaultTest : public testing::Test {
 protected:
  PasswordStoreDefaultTest()
      : ui_thread_(BrowserThread::UI, &message_loop_),
        db_thread_(BrowserThread::DB) {
  }

  virtual void SetUp() {
    ASSERT_TRUE(db_thread_.Start());

    profile_.reset(new TestingProfile());

    login_db_.reset(new LoginDatabase());
    ASSERT_TRUE(login_db_->Init(profile_->GetPath().Append(
        FILE_PATH_LITERAL("login_test"))));
  }

  virtual void TearDown() {
    base::MessageLoop::current()->PostTask(FROM_HERE,
                                           base::MessageLoop::QuitClosure());
    base::MessageLoop::current()->Run();
    db_thread_.Stop();
  }

  base::MessageLoopForUI message_loop_;
  content::TestBrowserThread ui_thread_;
  // PasswordStore, WDS schedule work on this thread.
  content::TestBrowserThread db_thread_;

  scoped_ptr<LoginDatabase> login_db_;
  scoped_ptr<TestingProfile> profile_;
};

ACTION(STLDeleteElements0) {
  STLDeleteContainerPointers(arg0.begin(), arg0.end());
}

ACTION(QuitUIMessageLoop) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  base::MessageLoop::current()->Quit();
}

TEST_F(PasswordStoreDefaultTest, NonASCIIData) {
  scoped_refptr<PasswordStoreDefault> store(new PasswordStoreDefault(
      base::MessageLoopProxy::current(),
      BrowserThread::GetMessageLoopProxyForThread(BrowserThread::DB),
      login_db_.release()));
  store->Init();

  // Some non-ASCII password form data.
  static const PasswordFormData form_data[] = {
    { PasswordForm::SCHEME_HTML,
      "http://foo.example.com",
      "http://foo.example.com/origin",
      "http://foo.example.com/action",
      L"มีสีสัน",
      L"お元気ですか?",
      L"盆栽",
      L"أحب كرة",
      L"£éä국수çà",
      true, false, 1 },
  };

  // Build the expected forms vector and add the forms to the store.
  std::vector<PasswordForm*> expected_forms;
  for (unsigned int i = 0; i < ARRAYSIZE_UNSAFE(form_data); ++i) {
    PasswordForm* form = CreatePasswordFormFromData(form_data[i]);
    expected_forms.push_back(form);
    store->AddLogin(*form);
  }

  // The PasswordStore schedules tasks to run on the DB thread so we schedule
  // yet another task to notify us that it's safe to carry on with the test.
  // The PasswordStore doesn't really understand that it's "done" once the tasks
  // we posted above have completed, so there's no formal notification for that.
  WaitableEvent done(false, false);
  BrowserThread::PostTask(BrowserThread::DB, FROM_HERE,
      base::Bind(&WaitableEvent::Signal, base::Unretained(&done)));
  done.Wait();

  MockPasswordStoreConsumer consumer;

  // Make sure we quit the MessageLoop even if the test fails.
  ON_CALL(consumer, OnGetPasswordStoreResults(_))
      .WillByDefault(QuitUIMessageLoop());

  // We expect to get the same data back, even though it's not all ASCII.
  EXPECT_CALL(consumer,
      OnGetPasswordStoreResults(ContainsAllPasswordForms(expected_forms)))
      .WillOnce(DoAll(WithArg<0>(STLDeleteElements0()), QuitUIMessageLoop()));

  store->GetAutofillableLogins(&consumer);
  base::MessageLoop::current()->Run();

  STLDeleteElements(&expected_forms);

  store->Shutdown();
}

TEST_F(PasswordStoreDefaultTest, Notifications) {
  scoped_refptr<PasswordStoreDefault> store(new PasswordStoreDefault(
      base::MessageLoopProxy::current(),
      BrowserThread::GetMessageLoopProxyForThread(BrowserThread::DB),
      login_db_.release()));
  store->Init();

  PasswordFormData form_data =
  { PasswordForm::SCHEME_HTML,
    "http://bar.example.com",
    "http://bar.example.com/origin",
    "http://bar.example.com/action",
    L"submit_element",
    L"username_element",
    L"password_element",
    L"username_value",
    L"password_value",
    true, false, 1 };
  scoped_ptr<PasswordForm> form(CreatePasswordFormFromData(form_data));

  MockPasswordStoreObserver observer;
  store->AddObserver(&observer);

  const PasswordStoreChange expected_add_changes[] = {
    PasswordStoreChange(PasswordStoreChange::ADD, *form),
  };

  EXPECT_CALL(
      observer,
      OnLoginsChanged(ElementsAreArray(expected_add_changes)));

  // Adding a login should trigger a notification.
  store->AddLogin(*form);

  // The PasswordStore schedules tasks to run on the DB thread so we schedule
  // yet another task to notify us that it's safe to carry on with the test.
  WaitableEvent done(false, false);
  BrowserThread::PostTask(BrowserThread::DB, FROM_HERE,
      base::Bind(&WaitableEvent::Signal, base::Unretained(&done)));
  done.Wait();

  // Notification happens on the same thread that we added the observer on,
  // so run message loop to get the notification
  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::MessageLoop::QuitClosure());
  base::MessageLoop::current()->Run();

  // Change the password.
  form->password_value = base::ASCIIToUTF16("a different password");

  const PasswordStoreChange expected_update_changes[] = {
    PasswordStoreChange(PasswordStoreChange::UPDATE, *form),
  };

  EXPECT_CALL(
      observer,
      OnLoginsChanged(ElementsAreArray(expected_update_changes)));

  // Updating the login with the new password should trigger a notification.
  store->UpdateLogin(*form);

  // Wait for PasswordStore to send the notification.
  BrowserThread::PostTask(BrowserThread::DB, FROM_HERE,
      base::Bind(&WaitableEvent::Signal, base::Unretained(&done)));
  done.Wait();

  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::MessageLoop::QuitClosure());
  base::MessageLoop::current()->Run();

  const PasswordStoreChange expected_delete_changes[] = {
    PasswordStoreChange(PasswordStoreChange::REMOVE, *form),
  };

  EXPECT_CALL(
      observer,
      OnLoginsChanged(ElementsAreArray(expected_delete_changes)));

  // Deleting the login should trigger a notification.
  store->RemoveLogin(*form);

  // Wait for PasswordStore to send the notification.
  BrowserThread::PostTask(BrowserThread::DB, FROM_HERE,
      base::Bind(&WaitableEvent::Signal, base::Unretained(&done)));
  done.Wait();

  base::MessageLoop::current()->PostTask(
      FROM_HERE, base::MessageLoop::QuitClosure());
  base::MessageLoop::current()->Run();

  store->RemoveObserver(&observer);
  store->Shutdown();
}
