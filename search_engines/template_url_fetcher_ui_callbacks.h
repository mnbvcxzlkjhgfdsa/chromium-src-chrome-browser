// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SEARCH_ENGINES_TEMPLATE_URL_FETCHER_UI_CALLBACKS_H_
#define CHROME_BROWSER_SEARCH_ENGINES_TEMPLATE_URL_FETCHER_UI_CALLBACKS_H_
#pragma once

#include "base/basictypes.h"
#include "chrome/browser/search_engines/template_url_fetcher_callbacks.h"
#include "chrome/common/notification_observer.h"
#include "chrome/common/notification_registrar.h"

class TabContents;

// Callbacks which display UI for the TemplateURLFetcher.
class TemplateURLFetcherUICallbacks : public TemplateURLFetcherCallbacks,
                                      public NotificationObserver {
 public:
  explicit TemplateURLFetcherUICallbacks(TabContents* source);
  virtual ~TemplateURLFetcherUICallbacks();

  // TemplateURLFetcherCallback implementation.
  virtual void ConfirmSetDefaultSearchProvider(
      TemplateURL* template_url,
      TemplateURLModel* template_url_model);
  virtual void ConfirmAddSearchProvider(
      TemplateURL* template_url,
      Profile* profile);

  // NotificationObserver:
  virtual void Observe(NotificationType type,
                       const NotificationSource& source,
                       const NotificationDetails& details);

 private:
  // The TabContents where this request originated. Can be NULL if the
  // originating tab is closed. If NULL, the engine is not added.
  TabContents* source_;

  // Handles registering for our notifications.
  NotificationRegistrar registrar_;

  DISALLOW_COPY_AND_ASSIGN(TemplateURLFetcherUICallbacks);
};

#endif  // CHROME_BROWSER_SEARCH_ENGINES_TEMPLATE_URL_FETCHER_UI_CALLBACKS_H_
