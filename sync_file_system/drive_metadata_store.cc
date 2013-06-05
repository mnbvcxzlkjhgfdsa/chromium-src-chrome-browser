// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sync_file_system/drive_metadata_store.h"

#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/callback.h"
#include "base/files/file_path.h"
#include "base/location.h"
#include "base/message_loop/message_loop_proxy.h"
#include "base/sequenced_task_runner.h"
#include "base/stl_util.h"
#include "base/string_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task_runner_util.h"
#include "chrome/browser/sync_file_system/drive/metadata_db_migration_util.h"
#include "chrome/browser/sync_file_system/drive_file_sync_service.h"
#include "chrome/browser/sync_file_system/drive_file_sync_util.h"
#include "chrome/browser/sync_file_system/logger.h"
#include "chrome/browser/sync_file_system/sync_file_system.pb.h"
#include "googleurl/src/gurl.h"
#include "third_party/leveldatabase/src/include/leveldb/db.h"
#include "third_party/leveldatabase/src/include/leveldb/write_batch.h"
#include "webkit/browser/fileapi/file_system_url.h"
#include "webkit/browser/fileapi/syncable/syncable_file_system_util.h"
#include "webkit/common/fileapi/file_system_util.h"

using fileapi::FileSystemURL;

namespace sync_file_system {

typedef DriveMetadataStore::MetadataMap MetadataMap;
typedef DriveMetadataStore::OriginByResourceId OriginByResourceId;
typedef DriveMetadataStore::PathToMetadata PathToMetadata;
typedef DriveMetadataStore::ResourceIdByOrigin ResourceIdByOrigin;

const base::FilePath::CharType DriveMetadataStore::kDatabaseName[] =
    FILE_PATH_LITERAL("DriveMetadata");

namespace {

const char kDatabaseVersionKey[] = "VERSION";
const int64 kCurrentDatabaseVersion = 2;
const char kChangeStampKey[] = "CHANGE_STAMP";
const char kSyncRootDirectoryKey[] = "SYNC_ROOT_DIR";
const char kDriveMetadataKeyPrefix[] = "METADATA: ";
const char kMetadataKeySeparator = ' ';
const char kDriveIncrementalSyncOriginKeyPrefix[] = "ISYNC_ORIGIN: ";
const char kDriveDisabledOriginKeyPrefix[] = "DISABLED_ORIGIN: ";
const size_t kDriveMetadataKeyPrefixLength = arraysize(kDriveMetadataKeyPrefix);

enum OriginSyncType {
  INCREMENTAL_SYNC_ORIGIN,
  DISABLED_ORIGIN
};

std::string RemovePrefix(const std::string& str, const std::string& prefix) {
  if (StartsWithASCII(str, prefix, true))
    return str.substr(prefix.size());
  return str;
}

std::string OriginAndPathToMetadataKey(const GURL& origin,
                                       const base::FilePath& path) {
  return kDriveMetadataKeyPrefix + origin.spec() +
      kMetadataKeySeparator + path.AsUTF8Unsafe();
}

std::string FileSystemURLToMetadataKey(const FileSystemURL& url) {
  return OriginAndPathToMetadataKey(url.origin(), url.path());
}

void MetadataKeyToOriginAndPath(const std::string& metadata_key,
                                GURL* origin,
                                base::FilePath* path) {
  std::string key_body(RemovePrefix(metadata_key, kDriveMetadataKeyPrefix));
  size_t separator_position = key_body.find(kMetadataKeySeparator);
  *origin = GURL(key_body.substr(0, separator_position));
  *path = base::FilePath::FromUTF8Unsafe(
      key_body.substr(separator_position + 1));
}

bool UpdateResourceIdMap(ResourceIdByOrigin* map,
                         OriginByResourceId* reverse_map,
                         const GURL& origin,
                         const std::string& resource_id) {
  ResourceIdByOrigin::iterator found = map->find(origin);
  if (found == map->end())
    return false;
  reverse_map->erase(found->second);
  reverse_map->insert(std::make_pair(resource_id, origin));

  found->second = resource_id;
  return true;
}

}  // namespace

class DriveMetadataDB {
 public:
  typedef DriveMetadataStore::MetadataMap MetadataMap;

  DriveMetadataDB(const base::FilePath& base_dir,
                  base::SequencedTaskRunner* task_runner);
  ~DriveMetadataDB();

  SyncStatusCode Initialize(bool* created);
  SyncStatusCode ReadContents(DriveMetadataDBContents* contents);

  SyncStatusCode MigrateDatabaseIfNeeded();

  SyncStatusCode WriteToDB(leveldb::WriteBatch* batch) {
    return LevelDBStatusToSyncStatusCode(db_->Write(
        leveldb::WriteOptions(), batch));
  }

 private:
  friend class DriveMetadataStore;

  bool CalledOnValidThread() const {
    return task_runner_->RunsTasksOnCurrentThread();
  }

  scoped_refptr<base::SequencedTaskRunner> task_runner_;

  std::string db_path_;
  scoped_ptr<leveldb::DB> db_;

  DISALLOW_COPY_AND_ASSIGN(DriveMetadataDB);
};

struct DriveMetadataDBContents {
  int64 largest_changestamp;
  DriveMetadataStore::MetadataMap metadata_map;
  std::string sync_root_directory_resource_id;
  ResourceIdByOrigin incremental_sync_origins;
  ResourceIdByOrigin disabled_origins;
};

namespace {

SyncStatusCode InitializeDBOnFileThread(DriveMetadataDB* db,
                                        DriveMetadataDBContents* contents,
                                        bool* created) {
  DCHECK(db);
  DCHECK(contents);
  DCHECK(created);

  contents->largest_changestamp = 0;
  contents->metadata_map.clear();
  contents->incremental_sync_origins.clear();
  contents->disabled_origins.clear();

  *created = false;
  SyncStatusCode status = db->Initialize(created);
  if (status != SYNC_STATUS_OK)
    return status;

  if (!*created) {
    status = db->MigrateDatabaseIfNeeded();
    if (status != SYNC_STATUS_OK) {
      util::Log(logging::LOG_WARNING,
                FROM_HERE,
                "Failed to migrate DriveMetadataStore to latest version.");
      return status;
    }
  }

  return db->ReadContents(contents);
}

// Returns a key string for the given origin.
// For example, when |origin| is "http://www.example.com" and |sync_type| is
// BATCH_SYNC_ORIGIN, returns "BSYNC_ORIGIN: http://www.example.com".
std::string CreateKeyForOriginRoot(const GURL& origin,
                                   OriginSyncType sync_type) {
  DCHECK(origin.is_valid());
  switch (sync_type) {
    case INCREMENTAL_SYNC_ORIGIN:
      return kDriveIncrementalSyncOriginKeyPrefix + origin.spec();
    case DISABLED_ORIGIN:
      return kDriveDisabledOriginKeyPrefix + origin.spec();
  }
  NOTREACHED();
  return std::string();
}

void AddOriginsToVector(std::vector<GURL>* all_origins,
                        const ResourceIdByOrigin& resource_map) {
  for (ResourceIdByOrigin::const_iterator itr = resource_map.begin();
       itr != resource_map.end();
       ++itr) {
    all_origins->push_back(itr->first);
  }
}

void InsertReverseMap(const ResourceIdByOrigin& forward_map,
                      OriginByResourceId* backward_map) {
  for (ResourceIdByOrigin::const_iterator itr = forward_map.begin();
       itr != forward_map.end(); ++itr)
    backward_map->insert(std::make_pair(itr->second, itr->first));
}

bool EraseIfExists(ResourceIdByOrigin* map,
                   const GURL& origin,
                   std::string* resource_id) {
  ResourceIdByOrigin::iterator found = map->find(origin);
  if (found == map->end())
    return false;
  *resource_id = found->second;
  map->erase(found);
  return true;
}

void AppendMetadataDeletionToBatch(const MetadataMap& metadata_map,
                                   const GURL& origin,
                                   leveldb::WriteBatch* batch) {
  MetadataMap::const_iterator found = metadata_map.find(origin);
  if (found == metadata_map.end())
    return;

  for (PathToMetadata::const_iterator itr = found->second.begin();
       itr != found->second.end(); ++itr)
    batch->Delete(OriginAndPathToMetadataKey(origin, itr->first));
}

}  // namespace

DriveMetadataStore::DriveMetadataStore(
    const base::FilePath& base_dir,
    base::SequencedTaskRunner* file_task_runner)
    : file_task_runner_(file_task_runner),
      db_(new DriveMetadataDB(base_dir, file_task_runner)),
      db_status_(SYNC_STATUS_UNKNOWN),
      largest_changestamp_(0) {
  DCHECK(file_task_runner);
}

DriveMetadataStore::~DriveMetadataStore() {
  DCHECK(CalledOnValidThread());
  file_task_runner_->DeleteSoon(FROM_HERE, db_.release());
}

void DriveMetadataStore::Initialize(const InitializationCallback& callback) {
  DCHECK(CalledOnValidThread());
  DriveMetadataDBContents* contents = new DriveMetadataDBContents;

  bool* created = new bool(false);
  base::PostTaskAndReplyWithResult(
      file_task_runner_.get(),
      FROM_HERE,
      base::Bind(InitializeDBOnFileThread, db_.get(), contents, created),
      base::Bind(&DriveMetadataStore::DidInitialize,
                 AsWeakPtr(),
                 callback,
                 base::Owned(contents),
                 base::Owned(created)));
}

void DriveMetadataStore::DidInitialize(const InitializationCallback& callback,
                                       DriveMetadataDBContents* contents,
                                       bool* created,
                                       SyncStatusCode status) {
  DCHECK(CalledOnValidThread());
  DCHECK(contents);

  db_status_ = status;
  if (status != SYNC_STATUS_OK) {
    callback.Run(status, false);
    return;
  }

  largest_changestamp_ = contents->largest_changestamp;
  metadata_map_.swap(contents->metadata_map);
  sync_root_directory_resource_id_ = contents->sync_root_directory_resource_id;
  incremental_sync_origins_.swap(contents->incremental_sync_origins);
  disabled_origins_.swap(contents->disabled_origins);
  // |largest_changestamp_| is set to 0 for a fresh empty database.

  origin_by_resource_id_.clear();
  InsertReverseMap(incremental_sync_origins_, &origin_by_resource_id_);
  InsertReverseMap(disabled_origins_, &origin_by_resource_id_);

  callback.Run(status, *created);
}

leveldb::DB* DriveMetadataStore::GetDBInstanceForTesting() {
  return db_->db_.get();
}

void DriveMetadataStore::SetLargestChangeStamp(
    int64 largest_changestamp,
    const SyncStatusCallback& callback) {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(SYNC_STATUS_OK, db_status_);
  largest_changestamp_ = largest_changestamp;

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Put(kChangeStampKey, base::Int64ToString(largest_changestamp));
  return WriteToDB(batch.Pass(), callback);
}

int64 DriveMetadataStore::GetLargestChangeStamp() const {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(SYNC_STATUS_OK, db_status_);
  return largest_changestamp_;
}

void DriveMetadataStore::UpdateEntry(
    const FileSystemURL& url,
    const DriveMetadata& metadata,
    const SyncStatusCallback& callback) {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(SYNC_STATUS_OK, db_status_);
  DCHECK(!metadata.conflicted() || !metadata.to_be_fetched());

  std::pair<PathToMetadata::iterator, bool> result =
      metadata_map_[url.origin()].insert(std::make_pair(url.path(), metadata));
  if (!result.second)
    result.first->second = metadata;

  std::string value;
  if (!IsDriveAPIEnabled()) {
    DriveMetadata metadata_in_db(metadata);
    metadata_in_db.set_resource_id(
        drive::RemoveWapiIdPrefix(metadata.resource_id()));
    bool success = metadata_in_db.SerializeToString(&value);
    DCHECK(success);
  } else {
    bool success = metadata.SerializeToString(&value);
    DCHECK(success);
  }

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Put(FileSystemURLToMetadataKey(url), value);
  WriteToDB(batch.Pass(), callback);
}

void DriveMetadataStore::DeleteEntry(
    const FileSystemURL& url,
    const SyncStatusCallback& callback) {
  DCHECK(CalledOnValidThread());
  MetadataMap::iterator found = metadata_map_.find(url.origin());
  if (found == metadata_map_.end()) {
    base::MessageLoopProxy::current()->PostTask(
        FROM_HERE,
        base::Bind(callback, SYNC_DATABASE_ERROR_NOT_FOUND));
    return;
  }

  if (found->second.erase(url.path()) == 1) {
    scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
    batch->Delete(FileSystemURLToMetadataKey(url));
    WriteToDB(batch.Pass(), callback);
    return;
  }

  base::MessageLoopProxy::current()->PostTask(
      FROM_HERE,
      base::Bind(callback, SYNC_DATABASE_ERROR_NOT_FOUND));
}

SyncStatusCode DriveMetadataStore::ReadEntry(const FileSystemURL& url,
                                             DriveMetadata* metadata) const {
  DCHECK(CalledOnValidThread());
  DCHECK(metadata);

  MetadataMap::const_iterator found_origin = metadata_map_.find(url.origin());
  if (found_origin == metadata_map_.end())
    return SYNC_DATABASE_ERROR_NOT_FOUND;

  PathToMetadata::const_iterator found = found_origin->second.find(url.path());
  if (found == found_origin->second.end())
    return SYNC_DATABASE_ERROR_NOT_FOUND;

  *metadata = found->second;
  return SYNC_STATUS_OK;
}

void DriveMetadataStore::AddIncrementalSyncOrigin(
    const GURL& origin,
    const std::string& resource_id) {
  DCHECK(CalledOnValidThread());
  DCHECK(!IsIncrementalSyncOrigin(origin));
  DCHECK(!IsOriginDisabled(origin));
  DCHECK_EQ(SYNC_STATUS_OK, db_status_);

  incremental_sync_origins_.insert(std::make_pair(origin, resource_id));
  origin_by_resource_id_.insert(std::make_pair(resource_id, origin));

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Delete(CreateKeyForOriginRoot(origin, DISABLED_ORIGIN));
  batch->Put(CreateKeyForOriginRoot(origin, INCREMENTAL_SYNC_ORIGIN),
             drive::RemoveWapiIdPrefix(resource_id));
  WriteToDB(batch.Pass(),
            base::Bind(&DriveMetadataStore::UpdateDBStatus, AsWeakPtr()));
}

void DriveMetadataStore::SetSyncRootDirectory(const std::string& resource_id) {
  DCHECK(CalledOnValidThread());

  sync_root_directory_resource_id_ = resource_id;

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Put(kSyncRootDirectoryKey, drive::RemoveWapiIdPrefix(resource_id));
  return WriteToDB(batch.Pass(),
                   base::Bind(&DriveMetadataStore::UpdateDBStatus,
                              AsWeakPtr()));
}

void DriveMetadataStore::SetOriginRootDirectory(
    const GURL& origin,
    const std::string& resource_id) {
  DCHECK(CalledOnValidThread());
  DCHECK(IsKnownOrigin(origin));

  OriginSyncType sync_type;
  if (UpdateResourceIdMap(
      &incremental_sync_origins_, &origin_by_resource_id_,
      origin, resource_id)) {
    sync_type = INCREMENTAL_SYNC_ORIGIN;
  } else if (UpdateResourceIdMap(&disabled_origins_, &origin_by_resource_id_,
                                 origin, resource_id)) {
    sync_type = DISABLED_ORIGIN;
  } else {
    return;
  }

  std::string key = CreateKeyForOriginRoot(origin, sync_type);
  DCHECK(!key.empty());

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Put(key, drive::RemoveWapiIdPrefix(resource_id));
  WriteToDB(batch.Pass(),
            base::Bind(&DriveMetadataStore::UpdateDBStatus, AsWeakPtr()));
}

bool DriveMetadataStore::IsKnownOrigin(const GURL& origin) const {
  DCHECK(CalledOnValidThread());
  return IsIncrementalSyncOrigin(origin) || IsOriginDisabled(origin);
}

bool DriveMetadataStore::IsIncrementalSyncOrigin(const GURL& origin) const {
  DCHECK(CalledOnValidThread());
  return ContainsKey(incremental_sync_origins_, origin);
}

bool DriveMetadataStore::IsOriginDisabled(const GURL& origin) const {
  DCHECK(CalledOnValidThread());
  return ContainsKey(disabled_origins_, origin);
}

void DriveMetadataStore::EnableOrigin(
    const GURL& origin,
    const SyncStatusCallback& callback) {
  DCHECK(CalledOnValidThread());

  std::map<GURL, std::string>::iterator found = disabled_origins_.find(origin);
  if (found == disabled_origins_.end()) {
    // |origin| has not been registered yet.
    return;
  }
  std::string resource_id = found->second;
  disabled_origins_.erase(found);

  // |origin| goes back to DriveFileSyncService::pending_batch_sync_origins_
  // only and is not stored in drive_metadata_store.
  found = incremental_sync_origins_.find(origin);
  if (found != incremental_sync_origins_.end())
    incremental_sync_origins_.erase(found);

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Delete(CreateKeyForOriginRoot(origin, INCREMENTAL_SYNC_ORIGIN));
  batch->Delete(CreateKeyForOriginRoot(origin, DISABLED_ORIGIN));
  WriteToDB(batch.Pass(), callback);
}

void DriveMetadataStore::DisableOrigin(
    const GURL& origin,
    const SyncStatusCallback& callback) {
  DCHECK(CalledOnValidThread());

  std::string resource_id;
  if (!EraseIfExists(&incremental_sync_origins_, origin, &resource_id))
    return;
  disabled_origins_[origin] = resource_id;

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Delete(CreateKeyForOriginRoot(origin, INCREMENTAL_SYNC_ORIGIN));
  batch->Put(CreateKeyForOriginRoot(origin, DISABLED_ORIGIN),
             drive::RemoveWapiIdPrefix(resource_id));
  AppendMetadataDeletionToBatch(metadata_map_, origin, batch.get());
  metadata_map_.erase(origin);

  WriteToDB(batch.Pass(), callback);
}

void DriveMetadataStore::RemoveOrigin(
    const GURL& origin,
    const SyncStatusCallback& callback) {
  DCHECK(CalledOnValidThread());

  std::string resource_id;
  if (!EraseIfExists(&incremental_sync_origins_, origin, &resource_id) &&
      !EraseIfExists(&disabled_origins_, origin, &resource_id))
    return;
  origin_by_resource_id_.erase(resource_id);

  scoped_ptr<leveldb::WriteBatch> batch(new leveldb::WriteBatch);
  batch->Delete(CreateKeyForOriginRoot(origin, INCREMENTAL_SYNC_ORIGIN));
  batch->Delete(CreateKeyForOriginRoot(origin, DISABLED_ORIGIN));
  AppendMetadataDeletionToBatch(metadata_map_, origin, batch.get());
  metadata_map_.erase(origin);

  WriteToDB(batch.Pass(), callback);
}

void DriveMetadataStore::DidUpdateOrigin(
    const SyncStatusCallback& callback,
    SyncStatusCode status) {
  UpdateDBStatus(status);
  callback.Run(status);
}

void DriveMetadataStore::WriteToDB(scoped_ptr<leveldb::WriteBatch> batch,
                                   const SyncStatusCallback& callback) {
  base::PostTaskAndReplyWithResult(
      file_task_runner_, FROM_HERE,
      base::Bind(&DriveMetadataDB::WriteToDB,
                 base::Unretained(db_.get()), base::Owned(batch.release())),
      base::Bind(&DriveMetadataStore::UpdateDBStatusAndInvokeCallback,
                 AsWeakPtr(), callback));
}

void DriveMetadataStore::UpdateDBStatus(SyncStatusCode status) {
  DCHECK(CalledOnValidThread());
  if (db_status_ != SYNC_STATUS_OK &&
      db_status_ != SYNC_DATABASE_ERROR_NOT_FOUND) {
    // TODO(tzik): Handle database corruption. http://crbug.com/153709
    db_status_ = status;
    util::Log(logging::LOG_WARNING,
              FROM_HERE,
              "DriveMetadataStore turned to wrong state: %s",
              SyncStatusCodeToString(status).c_str());
    return;
  }
  db_status_ = SYNC_STATUS_OK;
}

void DriveMetadataStore::UpdateDBStatusAndInvokeCallback(
    const SyncStatusCallback& callback,
    SyncStatusCode status) {
  UpdateDBStatus(status);
  callback.Run(status);
}

SyncStatusCode DriveMetadataStore::GetConflictURLs(
    fileapi::FileSystemURLSet* urls) const {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(SYNC_STATUS_OK, db_status_);

  urls->clear();
  for (MetadataMap::const_iterator origin_itr = metadata_map_.begin();
       origin_itr != metadata_map_.end();
       ++origin_itr) {
    for (PathToMetadata::const_iterator itr = origin_itr->second.begin();
         itr != origin_itr->second.end();
         ++itr) {
      if (itr->second.conflicted()) {
        urls->insert(CreateSyncableFileSystemURL(
            origin_itr->first, itr->first));
      }
    }
  }
  return SYNC_STATUS_OK;
}

SyncStatusCode DriveMetadataStore::GetToBeFetchedFiles(
    URLAndDriveMetadataList* list) const {
  DCHECK(CalledOnValidThread());
  DCHECK_EQ(SYNC_STATUS_OK, db_status_);

  list->clear();
  for (MetadataMap::const_iterator origin_itr = metadata_map_.begin();
       origin_itr != metadata_map_.end();
       ++origin_itr) {
    for (PathToMetadata::const_iterator itr = origin_itr->second.begin();
         itr != origin_itr->second.end();
         ++itr) {
      if (itr->second.to_be_fetched()) {
        FileSystemURL url = CreateSyncableFileSystemURL(
            origin_itr->first, itr->first);
        list->push_back(std::make_pair(url, itr->second));
      }
    }
  }
  return SYNC_STATUS_OK;
}

std::string DriveMetadataStore::GetResourceIdForOrigin(
    const GURL& origin) const {
  DCHECK(CalledOnValidThread());

  // If we don't have valid root directory (this could be reset even after
  // initialized) just return empty string, as the origin directories
  // in the root directory must have become invalid now too.
  if (sync_root_directory().empty())
    return std::string();

  ResourceIdByOrigin::const_iterator found =
      incremental_sync_origins_.find(origin);
  if (found != incremental_sync_origins_.end())
    return found->second;

  found = disabled_origins_.find(origin);
  if (found != disabled_origins_.end())
    return found->second;

  return std::string();
}

void DriveMetadataStore::GetAllOrigins(std::vector<GURL>* origins) {
  DCHECK(CalledOnValidThread());
  DCHECK(origins);
  origins->clear();
  origins->reserve(incremental_sync_origins_.size() +
                   disabled_origins_.size());
  AddOriginsToVector(origins, incremental_sync_origins_);
  AddOriginsToVector(origins, disabled_origins_);
}

bool DriveMetadataStore::GetOriginByOriginRootDirectoryId(
    const std::string& resource_id,
    GURL* origin) {
  DCHECK(CalledOnValidThread());
  DCHECK(origin);

  OriginByResourceId::iterator found = origin_by_resource_id_.find(resource_id);
  if (found == origin_by_resource_id_.end())
    return false;
  *origin = found->second;
  return true;
}

////////////////////////////////////////////////////////////////////////////////

DriveMetadataDB::DriveMetadataDB(const base::FilePath& base_dir,
                                 base::SequencedTaskRunner* task_runner)
    : task_runner_(task_runner),
      db_path_(fileapi::FilePathToString(
          base_dir.Append(DriveMetadataStore::kDatabaseName))) {
}

DriveMetadataDB::~DriveMetadataDB() {
  DCHECK(CalledOnValidThread());
}

SyncStatusCode DriveMetadataDB::Initialize(bool* created) {
  DCHECK(CalledOnValidThread());
  DCHECK(!db_.get());
  DCHECK(created);

  leveldb::Options options;
  options.create_if_missing = true;
  leveldb::DB* db = NULL;
  leveldb::Status status = leveldb::DB::Open(options, db_path_, &db);
  // TODO(tzik): Handle database corruption. http://crbug.com/153709
  if (!status.ok()) {
    delete db;
    return LevelDBStatusToSyncStatusCode(status);
  }

  scoped_ptr<leveldb::Iterator> itr(db->NewIterator(leveldb::ReadOptions()));
  itr->SeekToFirst();
  *created = !itr->Valid();

  if (*created) {
    status = db->Put(leveldb::WriteOptions(),
                     kDatabaseVersionKey,
                     base::Int64ToString(kCurrentDatabaseVersion));
    if (!status.ok()) {
      delete db;
      return LevelDBStatusToSyncStatusCode(status);
    }
  }

  db_.reset(db);
  return LevelDBStatusToSyncStatusCode(status);
}

SyncStatusCode DriveMetadataDB::ReadContents(
    DriveMetadataDBContents* contents) {
  DCHECK(CalledOnValidThread());
  DCHECK(db_.get());
  DCHECK(contents);

  contents->largest_changestamp = 0;
  contents->metadata_map.clear();
  contents->incremental_sync_origins.clear();

  scoped_ptr<leveldb::Iterator> itr(db_->NewIterator(leveldb::ReadOptions()));
  for (itr->SeekToFirst(); itr->Valid(); itr->Next()) {
    std::string key = itr->key().ToString();
    if (key == kChangeStampKey) {
      bool success = base::StringToInt64(itr->value().ToString(),
                                         &contents->largest_changestamp);
      DCHECK(success);
      continue;
    }

    if (key == kSyncRootDirectoryKey) {
      std::string resource_id = itr->value().ToString();
      if (!IsDriveAPIEnabled())
        resource_id = drive::AddWapiFolderPrefix(resource_id);
      contents->sync_root_directory_resource_id = resource_id;
      continue;
    }

    if (StartsWithASCII(key, kDriveMetadataKeyPrefix, true)) {
      GURL origin;
      base::FilePath path;
      MetadataKeyToOriginAndPath(key, &origin, &path);

      DriveMetadata metadata;
      bool success = metadata.ParseFromString(itr->value().ToString());
      DCHECK(success);

      if (!IsDriveAPIEnabled()) {
        metadata.set_resource_id(
            drive::AddWapiIdPrefix(metadata.resource_id(), metadata.type()));
      }

      success = contents->metadata_map[origin].insert(
          std::make_pair(path, metadata)).second;
      DCHECK(success);
      continue;
    }

    if (StartsWithASCII(key, kDriveIncrementalSyncOriginKeyPrefix, true)) {
      GURL origin(RemovePrefix(key, kDriveIncrementalSyncOriginKeyPrefix));
      DCHECK(origin.is_valid());

      std::string origin_resource_id = IsDriveAPIEnabled()
          ? itr->value().ToString()
          : drive::AddWapiFolderPrefix(itr->value().ToString());

      DCHECK(!ContainsKey(contents->incremental_sync_origins, origin));
      contents->incremental_sync_origins[origin] = origin_resource_id;
      continue;
    }

    if (StartsWithASCII(key, kDriveDisabledOriginKeyPrefix, true)) {
      GURL origin(RemovePrefix(key, kDriveDisabledOriginKeyPrefix));
      DCHECK(origin.is_valid());

      std::string origin_resource_id = IsDriveAPIEnabled()
          ? itr->value().ToString()
          : drive::AddWapiFolderPrefix(itr->value().ToString());

      DCHECK(!ContainsKey(contents->disabled_origins, origin));
      contents->disabled_origins[origin] = origin_resource_id;
      continue;
    }
  }

  return SYNC_STATUS_OK;
}

SyncStatusCode DriveMetadataDB::MigrateDatabaseIfNeeded() {
  scoped_ptr<leveldb::Iterator> itr(db_->NewIterator(leveldb::ReadOptions()));
  itr->Seek(kDatabaseVersionKey);

  int64 database_version = 0;
  if (itr->Valid() && itr->key().ToString() == kDatabaseVersionKey) {
    bool success = base::StringToInt64(itr->value().ToString(),
                                       &database_version);
    if (!success)
      return SYNC_DATABASE_ERROR_FAILED;
    if (database_version > kCurrentDatabaseVersion)
      return SYNC_DATABASE_ERROR_FAILED;
    if (database_version == kCurrentDatabaseVersion)
      return SYNC_STATUS_OK;
  }

  switch (database_version) {
    case 0:
      drive::MigrateDatabaseFromV0ToV1(db_.get());
      // fall-through
    case 1:
      drive::MigrateDatabaseFromV1ToV2(db_.get());
      return SYNC_STATUS_OK;
  }
  return SYNC_DATABASE_ERROR_FAILED;
}

}  // namespace sync_file_system
