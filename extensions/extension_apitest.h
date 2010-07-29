// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_EXTENSIONS_EXTENSION_APITEST_H_
#define CHROME_BROWSER_EXTENSIONS_EXTENSION_APITEST_H_
#pragma once

#include <deque>
#include <string>

#include "chrome/browser/extensions/extension_browsertest.h"
#include "chrome/common/notification_service.h"

class Extension;

// The general flow of these API tests should work like this:
// (1) Setup initial browser state (e.g. create some bookmarks for the
//     bookmark test)
// (2) Call ASSERT_TRUE(RunExtensionTest(name));
// (3) In your extension code, run your test and call chrome.test.pass or
//     chrome.test.fail
// (4) Verify expected browser state.
// TODO(erikkay): There should also be a way to drive events in these tests.

class ExtensionApiTest : public ExtensionBrowserTest {
 protected:
  // Helper class that observes tests failing or passing. Observation starts
  // when the class is constructed. Get the next result by calling
  // GetNextResult() and message() if GetNextResult() return false. If there
  // are no results, this method will pump the UI message loop until one is
  // received.
  class ResultCatcher : public NotificationObserver {
   public:
    ResultCatcher();

    // Pumps the UI loop until a notification is received that an API test
    // succeeded or failed. Returns true if the test succeeded, false otherwise.
    bool GetNextResult();

    const std::string& message() { return message_; }

   private:
    virtual void Observe(NotificationType type,
                         const NotificationSource& source,
                         const NotificationDetails& details);

    NotificationRegistrar registrar_;

    // A sequential list of pass/fail notifications from the test extension(s).
    std::deque<bool> results_;

    // If it failed, what was the error message?
    std::deque<std::string> messages_;
    std::string message_;
  };

  // Load |extension_name| and wait for pass / fail notification.
  // |extension_name| is a directory in "test/data/extensions/api_test".
  bool RunExtensionTest(const char* extension_name);

  // Load |extension_name|, load page at path |subtest_page| under the
  // extension, and wait for pass / fail notification.  |extension_name|
  // is a directory in "test/data/extensions/api_test".
  bool RunExtensionSubtest(const char* extension_name,
                           const std::string& subtest_page);

  // Test that exactly one extension loaded.  If so, return a pointer to
  // the extension.  If not, return NULL and set message_.
  Extension* GetSingleLoadedExtension();

  // All extensions tested by ExtensionApiTest are in the "api_test" dir.
  virtual void SetUpCommandLine(CommandLine* command_line);

  // If it failed, what was the error message?
  std::string message_;

 private:
  bool RunExtensionTestImpl(const char* extension_name,
                            const std::string& test_page);
};

#endif  // CHROME_BROWSER_EXTENSIONS_EXTENSION_APITEST_H_
