// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sync/engine/post_commit_message_command.h"

#include <vector>

#include "base/location.h"
#include "chrome/browser/sync/engine/syncer_proto_util.h"
#include "chrome/browser/sync/engine/syncproto.h"
#include "chrome/browser/sync/sessions/sync_session.h"

using std::vector;

namespace browser_sync {

PostCommitMessageCommand::PostCommitMessageCommand() {}
PostCommitMessageCommand::~PostCommitMessageCommand() {}

SyncerError PostCommitMessageCommand::ExecuteImpl(
    sessions::SyncSession* session) {
  if (session->status_controller().commit_ids().empty())
    return SYNCER_OK;  // Nothing to commit.
  ClientToServerResponse response;
  syncable::Directory* dir = session->context()->directory();
  sessions::StatusController* status = session->mutable_status_controller();
  SyncerError result = SyncerProtoUtil::PostClientToServerMessage(
      status->commit_message(), &response, session);
  if (result != SYNCER_OK) {
    // None of our changes got through.  Clear the SYNCING bit which was
    // set to true during BuildCommitCommand, and which may still be true.
    // Not to be confused with IS_UNSYNCED, this bit is used to detect local
    // changes to items that happen during the server Commit operation.
    syncable::WriteTransaction trans(FROM_HERE, syncable::SYNCER, dir);
    const vector<syncable::Id>& commit_ids = status->commit_ids();
    for (size_t i = 0; i < commit_ids.size(); i++) {
      syncable::MutableEntry entry(&trans, syncable::GET_BY_ID, commit_ids[i]);
      entry.Put(syncable::SYNCING, false);
    }
    return result;
  }

  status->set_items_committed();
  status->mutable_commit_response()->CopyFrom(response);
  return SYNCER_OK;
}

}  // namespace browser_sync
