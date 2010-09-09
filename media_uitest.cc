// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/basictypes.h"
#include "base/file_path.h"
#include "base/platform_thread.h"
#include "base/string_util.h"
#include "chrome/test/ui/ui_layout_test.h"
#include "chrome/test/ui/ui_test.h"
#include "net/base/net_util.h"

class MediaTest : public UITest {
 protected:
  void PlayMedia(const char* tag, const char* media_file) {
    FilePath test_file(test_data_directory_);
    test_file = test_file.AppendASCII("media/player.html");

    GURL player_gurl = net::FilePathToFileURL(test_file);
    std::string url = StringPrintf("%s?%s=%s",
                                   player_gurl.spec().c_str(),
                                   tag,
                                   media_file);

    NavigateToURL(GURL(url));

    // Allow the media file to be loaded.
    const std::wstring kPlaying = L"PLAYING";
    const std::wstring kFailed = L"FAILED";
    const std::wstring kError = L"ERROR";
    for (int i = 0; i < 10; ++i) {
      PlatformThread::Sleep(sleep_timeout_ms());
      const std::wstring& title = GetActiveTabTitle();
      if (title == kPlaying || title == kFailed ||
          StartsWith(title, kError, true))
        break;
    }

    EXPECT_EQ(kPlaying, GetActiveTabTitle());
  }

  void PlayAudio(const char* url) {
    PlayMedia("audio", url);
  }

  void PlayVideo(const char* url) {
    PlayMedia("video", url);
  }
};

// Crashes, see http://crbug.com/54939
TEST_F(MediaTest, DISABLED_VideoBearTheora) {
  PlayVideo("bear.ogv");
}

TEST_F(MediaTest, VideoBearSilentTheora) {
  PlayVideo("bear_silent.ogv");
}

TEST_F(MediaTest, VideoBearWebm) {
  PlayVideo("bear.webm");
}

TEST_F(MediaTest, VideoBearSilentWebm) {
  PlayVideo("bear_silent.webm");
}

#if defined(GOOGLE_CHROME_BUILD) || defined(USE_PROPRIETARY_CODECS)
TEST_F(MediaTest, VideoBearMp4) {
  PlayVideo("bear.mp4");
}

TEST_F(MediaTest, VideoBearSilentMp4) {
  PlayVideo("bear_silent.mp4");
}
#endif

TEST_F(UILayoutTest, MediaUILayoutTest) {
  static const char* kResources[] = {
    "content",
    "media-file.js",
    "media-fullscreen.js",
    "video-paint-test.js",
    "video-played.js",
    "video-test.js",
  };

  static const char* kMediaTests[] = {
    "video-autoplay.html",
    // "video-loop.html", disabled due to 52887.
    "video-no-autoplay.html",
    // TODO(sergeyu): Add more tests here.
  };

  FilePath test_dir;
  FilePath media_test_dir;
  media_test_dir = media_test_dir.AppendASCII("media");
  InitializeForLayoutTest(test_dir, media_test_dir, kNoHttpPort);

  // Copy resources first.
  for (size_t i = 0; i < arraysize(kResources); ++i)
    AddResourceForLayoutTest(
        test_dir, media_test_dir.AppendASCII(kResources[i]));

  for (size_t i = 0; i < arraysize(kMediaTests); ++i)
    RunLayoutTest(kMediaTests[i], kNoHttpPort);
}
