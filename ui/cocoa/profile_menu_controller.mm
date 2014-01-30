// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "chrome/browser/ui/cocoa/profile_menu_controller.h"

#include "base/mac/scoped_nsobject.h"
#include "base/strings/sys_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/avatar_menu.h"
#include "chrome/browser/profiles/avatar_menu_observer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_info_cache.h"
#include "chrome/browser/profiles/profile_info_interface.h"
#include "chrome/browser/profiles/profile_info_util.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_metrics.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "chrome/browser/ui/cocoa/last_active_browser_cocoa.h"
#include "grit/generated_resources.h"
#include "ui/base/l10n/l10n_util_mac.h"
#include "ui/gfx/image/image.h"

@interface ProfileMenuController (Private)
- (void)initializeMenu;
@end

namespace ProfileMenuControllerInternal {

class Observer : public chrome::BrowserListObserver,
                 public AvatarMenuObserver {
 public:
  Observer(ProfileMenuController* controller) : controller_(controller) {
    BrowserList::AddObserver(this);
  }

  virtual ~Observer() {
    BrowserList::RemoveObserver(this);
  }

  // chrome::BrowserListObserver:
  virtual void OnBrowserAdded(Browser* browser) OVERRIDE {}
  virtual void OnBrowserRemoved(Browser* browser) OVERRIDE {
    [controller_ activeBrowserChangedTo:chrome::GetLastActiveBrowser()];
  }
  virtual void OnBrowserSetLastActive(Browser* browser) OVERRIDE {
    [controller_ activeBrowserChangedTo:browser];
  }

  // AvatarMenuObserver:
  virtual void OnAvatarMenuChanged(AvatarMenu* menu) OVERRIDE {
    [controller_ rebuildMenu];
  }

 private:
  ProfileMenuController* controller_;  // Weak; owns this.
};

}  // namespace ProfileMenuControllerInternal

////////////////////////////////////////////////////////////////////////////////

@implementation ProfileMenuController

- (id)initWithMainMenuItem:(NSMenuItem*)item {
  if ((self = [super init])) {
    mainMenuItem_ = item;

    base::scoped_nsobject<NSMenu> menu([[NSMenu alloc] initWithTitle:
            l10n_util::GetNSStringWithFixup(IDS_PROFILES_OPTIONS_GROUP_NAME)]);
    [mainMenuItem_ setSubmenu:menu];

    // This object will be constructed as part of nib loading, which happens
    // before the message loop starts and g_browser_process is available.
    // Schedule this on the loop to do work when the browser is ready.
    [self performSelector:@selector(initializeMenu)
               withObject:nil
               afterDelay:0];
  }
  return self;
}

- (IBAction)switchToProfileFromMenu:(id)sender {
  menu_->SwitchToProfile([sender tag], false);
  ProfileMetrics::LogProfileSwitchUser(ProfileMetrics::SWITCH_PROFILE_MENU);
}

- (IBAction)switchToProfileFromDock:(id)sender {
  // Explicitly bring to the foreground when taking action from the dock.
  [NSApp activateIgnoringOtherApps:YES];
  menu_->SwitchToProfile([sender tag], false);
  ProfileMetrics::LogProfileSwitchUser(ProfileMetrics::SWITCH_PROFILE_DOCK);
}

- (IBAction)editProfile:(id)sender {
  menu_->EditProfile(menu_->GetActiveProfileIndex());
}

- (IBAction)newProfile:(id)sender {
  menu_->AddNewProfile(ProfileMetrics::ADD_NEW_USER_MENU);
}

- (BOOL)insertItemsIntoMenu:(NSMenu*)menu
                   atOffset:(NSInteger)offset
                   fromDock:(BOOL)dock {
  if (!menu_ || !menu_->ShouldShowAvatarMenu())
    return NO;

  if (dock) {
    NSString* headerName =
        l10n_util::GetNSStringWithFixup(IDS_PROFILES_OPTIONS_GROUP_NAME);
    base::scoped_nsobject<NSMenuItem> header(
        [[NSMenuItem alloc] initWithTitle:headerName
                                   action:NULL
                            keyEquivalent:@""]);
    [header setEnabled:NO];
    [menu insertItem:header atIndex:offset++];
  }

  for (size_t i = 0; i < menu_->GetNumberOfItems(); ++i) {
    const AvatarMenu::Item& itemData = menu_->GetItemAt(i);
    NSString* name = base::SysUTF16ToNSString(itemData.name);
    SEL action = dock ? @selector(switchToProfileFromDock:)
                      : @selector(switchToProfileFromMenu:);
    NSMenuItem* item = [self createItemWithTitle:name
                                          action:action];
    [item setTag:itemData.menu_index];
    if (dock) {
      [item setIndentationLevel:1];
    } else {
      gfx::Image itemIcon = itemData.icon;
      // The image might be too large and need to be resized (i.e. if this is
      // a signed-in user using the GAIA profile photo).
      if (itemIcon.Width() > profiles::kAvatarIconWidth ||
          itemIcon.Height() > profiles::kAvatarIconHeight) {
        itemIcon = profiles::GetAvatarIconForWebUI(itemIcon, true);
      }
      DCHECK(itemIcon.Width() <= profiles::kAvatarIconWidth);
      DCHECK(itemIcon.Height() <= profiles::kAvatarIconHeight);
      [item setImage:itemIcon.ToNSImage()];
      [item setState:itemData.active ? NSOnState : NSOffState];
    }
    [menu insertItem:item atIndex:i + offset];
  }

  return YES;
}

- (BOOL)validateMenuItem:(NSMenuItem*)menuItem {
  // In guest mode, chrome://settings isn't available, so disallow creating
  // or editing a profile.
  Profile* activeProfile = ProfileManager::GetLastUsedProfile();
  if (activeProfile->IsGuestSession()) {
    return [menuItem action] != @selector(newProfile:) &&
           [menuItem action] != @selector(editProfile:);
  }

  const AvatarMenu::Item& itemData = menu_->GetItemAt(
      menu_->GetActiveProfileIndex());
  if ([menuItem action] == @selector(switchToProfileFromDock:) ||
      [menuItem action] == @selector(switchToProfileFromMenu:)) {
    if (!itemData.managed)
      return YES;

    return [menuItem tag] == static_cast<NSInteger>(itemData.menu_index);
  }

  if ([menuItem action] == @selector(newProfile:))
    return !itemData.managed;

  return YES;
}

// Private /////////////////////////////////////////////////////////////////////

- (NSMenu*)menu {
  return [mainMenuItem_ submenu];
}

- (void)initializeMenu {
  observer_.reset(new ProfileMenuControllerInternal::Observer(self));
  menu_.reset(new AvatarMenu(
      &g_browser_process->profile_manager()->GetProfileInfoCache(),
      observer_.get(),
      NULL));
  menu_->RebuildMenu();

  [[self menu] addItem:[NSMenuItem separatorItem]];

  NSMenuItem* item = [self createItemWithTitle:
          l10n_util::GetNSStringWithFixup(IDS_PROFILES_CUSTOMIZE_PROFILE)
                                        action:@selector(editProfile:)];
  [[self menu] addItem:item];

  [[self menu] addItem:[NSMenuItem separatorItem]];
  item = [self createItemWithTitle:l10n_util::GetNSStringWithFixup(
                                       IDS_PROFILES_CREATE_NEW_PROFILE_OPTION)
                            action:@selector(newProfile:)];
  [[self menu] addItem:item];

  [self rebuildMenu];
}

// Notifies the controller that the active browser has changed and that the
// menu item and menu need to be updated to reflect that.
- (void)activeBrowserChangedTo:(Browser*)browser {
  // Tell the menu that the browser has changed.
  menu_->ActiveBrowserChanged(browser);

  // If |browser| is NULL, it may be because the current profile was deleted
  // and there are no other loaded profiles. In this case, calling
  // |menu_->GetActiveProfileIndex()| may result in a profile being loaded,
  // which is inappropriate to do on the UI thread.
  //
  // An early return provides the desired behavior:
  //   a) If the profile was deleted, the menu would have been rebuilt and no
  //      profile will have a check mark.
  //   b) If the profile was not deleted, but there is no active browser, then
  //      the previous profile will remain checked.
  if (!browser)
    return;

  // In guest mode, there is no active menu item.
  size_t activeProfileIndex = browser->profile()->IsGuestSession() ?
      std::string::npos : menu_->GetActiveProfileIndex();

  // Update the state for the menu items.
  for (size_t i = 0; i < menu_->GetNumberOfItems(); ++i) {
    size_t tag = menu_->GetItemAt(i).menu_index;
    [[[self menu] itemWithTag:tag]
        setState:activeProfileIndex == tag ? NSOnState : NSOffState];
  }
}

- (void)rebuildMenu {
  NSMenu* menu = [self menu];

  for (NSMenuItem* item = [menu itemAtIndex:0];
       ![item isSeparatorItem];
       item = [menu itemAtIndex:0]) {
    [menu removeItemAtIndex:0];
  }

  BOOL hasContent = [self insertItemsIntoMenu:menu atOffset:0 fromDock:NO];

  [mainMenuItem_ setHidden:!hasContent];
}

- (NSMenuItem*)createItemWithTitle:(NSString*)title action:(SEL)sel {
  base::scoped_nsobject<NSMenuItem> item(
      [[NSMenuItem alloc] initWithTitle:title action:sel keyEquivalent:@""]);
  [item setTarget:self];
  return [item.release() autorelease];
}

@end
