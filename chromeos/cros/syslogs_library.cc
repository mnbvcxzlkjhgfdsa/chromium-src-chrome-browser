// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/cros/syslogs_library.h"

#include "base/command_line.h"
#include "base/file_util.h"
#include "base/string_util.h"
#include "chrome/browser/chromeos/cros/cros_library.h"
#include "chrome/common/chrome_switches.h"
#include "content/browser/browser_thread.h"

namespace chromeos {

namespace {
const char kSysLogsScript[] =
    "/usr/share/userfeedback/scripts/sysinfo_script_runner";
const char kBzip2Command[] =
    "/bin/bzip2";
const char kMultilineQuote[] = "\"\"\"";
const char kNewLineChars[] = "\r\n";
const char kInvalidLogEntry[] = "<invalid characters in log entry>";
const char kEmptyLogEntry[] = "<no value>";

// Reads a key from the input string erasing the read values + delimiters read
// from the initial string
std::string ReadKey(std::string* data) {
  size_t equal_sign = data->find("=");
  if (equal_sign == std::string::npos)
    return std::string("");
  std::string key = data->substr(0, equal_sign);
  data->erase(0, equal_sign);
  if (data->size() > 0) {
    // erase the equal to sign also
    data->erase(0,1);
    return key;
  }
  return std::string();
}

// Reads a value from the input string; erasing the read values from
// the initial string; detects if the value is multiline and reads
// accordingly
std::string ReadValue(std::string* data) {
  // Trim the leading spaces and tabs. In order to use a multi-line
  // value, you have to place the multi-line quote on the same line as
  // the equal sign.
  //
  // Why not use TrimWhitespace? Consider the following input:
  //
  // KEY1=
  // KEY2=VALUE
  //
  // If we use TrimWhitespace, we will incorrectly trim the new line
  // and assume that KEY1's value is "KEY2=VALUE" rather than empty.
  TrimString(*data, " \t", data);

  // If multiline value
  if (StartsWithASCII(*data, std::string(kMultilineQuote), false)) {
    data->erase(0, strlen(kMultilineQuote));
    size_t next_multi = data->find(kMultilineQuote);
    if (next_multi == std::string::npos) {
      // Error condition, clear data to stop further processing
      data->erase();
      return std::string();
    }
    std::string value = data->substr(0, next_multi);
    data->erase(0, next_multi + 3);
    return value;
  } else { // single line value
    size_t endl_pos = data->find_first_of(kNewLineChars);
    // if we don't find a new line, we just return the rest of the data
    std::string value = data->substr(0, endl_pos);
    data->erase(0, endl_pos);
    return value;
  }
}

// Returns a map of system log keys and values.
//
// Parameters:
// temp_filename: This is an out parameter that holds the name of a file in
//                /tmp that contains the system logs in a KEY=VALUE format.
//                If this parameter is NULL, system logs are not retained on
//                the filesystem after this call completes.
// context:       This is an in parameter specifying what context should be
//                passed to the syslog collection script; currently valid
//                values are "sysinfo" or "feedback"; in case of an invalid
//                value, the script will currently default to "sysinfo"

LogDictionaryType* GetSystemLogs(FilePath* zip_file_name,
                                         const std::string& context) {
  // Create the temp file, logs will go here
  FilePath temp_filename;

  if (!file_util::CreateTemporaryFile(&temp_filename))
    return NULL;

  std::string cmd = std::string(kSysLogsScript) + " " + context + " >> " +
      temp_filename.value();

  // Ignore the return value - if the script execution didn't work
  // stderr won't go into the output file anyway.
  if (system(cmd.c_str()) == -1)
    LOG(WARNING) << "Command " << cmd << " failed to run";

  // Compress the logs file if requested.
  if (zip_file_name) {
    cmd = std::string(kBzip2Command) + " -c " + temp_filename.value() + " > " +
        zip_file_name->value();
    if (system(cmd.c_str()) == -1)
      LOG(WARNING) << "Command " << cmd << " failed to run";
  }
  // Read logs from the temp file
  std::string data;
  bool read_success = file_util::ReadFileToString(temp_filename,
                                                  &data);
  // if we were using an internal temp file, the user does not need the
  // logs to stay past the ReadFile call - delete the file
  file_util::Delete(temp_filename, false);

  if (!read_success)
    return NULL;

  // Parse the return data into a dictionary
  LogDictionaryType* logs = new LogDictionaryType();
  while (data.length() > 0) {
    std::string key = ReadKey(&data);
    TrimWhitespaceASCII(key, TRIM_ALL, &key);
    if (!key.empty()) {
      std::string value = ReadValue(&data);
      if (IsStringUTF8(value)) {
        TrimWhitespaceASCII(value, TRIM_ALL, &value);
        if (value.empty())
          (*logs)[key] = kEmptyLogEntry;
        else
          (*logs)[key] = value;
      } else {
        LOG(WARNING) << "Invalid characters in system log entry: " << key;
        (*logs)[key] = kInvalidLogEntry;
      }
    } else {
      // no more keys, we're done
      break;
    }
  }

  return logs;
}

} // anonymous namespace

const char kContextFeedback[] = "feedback";
const char kContextSysInfo[] = "sysinfo";
const char kContextNetwork[] = "network";

class SyslogsLibraryImpl : public SyslogsLibrary {
 public:
  SyslogsLibraryImpl() {}
  virtual ~SyslogsLibraryImpl() {}

  virtual Handle RequestSyslogs(
      bool compress_logs,
      Context context,
      CancelableRequestConsumerBase* consumer,
      ReadCompleteCallback* callback);

  // Reads system logs, compresses content if requested.
  // Called from FILE thread.
  void ReadSyslogs(
      scoped_refptr<CancelableRequest<ReadCompleteCallback> > request,
      bool compress_logs,
      Context context);

  void LoadCompressedLogs(const FilePath& zip_file,
                          std::string* zip_content);

 private:
  const char* GetContextString(Context context);

  DISALLOW_COPY_AND_ASSIGN(SyslogsLibraryImpl);
};

class SyslogsLibraryStubImpl : public SyslogsLibrary {
 public:
  SyslogsLibraryStubImpl() {}
  virtual ~SyslogsLibraryStubImpl() {}

  virtual Handle RequestSyslogs(bool compress_logs,
                                Context context,
                                CancelableRequestConsumerBase* consumer,
                                ReadCompleteCallback* callback) {
    if (callback)
      callback->Run(Tuple2<LogDictionaryType*, std::string*>(NULL , NULL));

    return 0;
  }
};

// static
SyslogsLibrary* SyslogsLibrary::GetImpl(bool stub) {
  if (stub)
    return new SyslogsLibraryStubImpl();
  else
    return new SyslogsLibraryImpl();
}


CancelableRequestProvider::Handle SyslogsLibraryImpl::RequestSyslogs(
    bool compress_logs,
    Context context,
    CancelableRequestConsumerBase* consumer,
    ReadCompleteCallback* callback) {
  // Register the callback request.
  scoped_refptr<CancelableRequest<ReadCompleteCallback> > request(
         new CancelableRequest<ReadCompleteCallback>(callback));
  AddRequest(request, consumer);

  // Schedule a task on the FILE thread which will then trigger a request
  // callback on the calling thread (e.g. UI) when complete.
  BrowserThread::PostTask(
      BrowserThread::FILE, FROM_HERE,
      NewRunnableMethod(
          this, &SyslogsLibraryImpl::ReadSyslogs, request,
          compress_logs, context));

  return request->handle();
}

// Called from FILE thread.
void SyslogsLibraryImpl::ReadSyslogs(
    scoped_refptr<CancelableRequest<ReadCompleteCallback> > request,
    bool compress_logs,
    Context context) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::FILE));

  if (request->canceled())
    return;

  if (compress_logs && !CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kCompressSystemFeedback))
    compress_logs = false;

  // Create temp file.
  FilePath zip_file;
  if (compress_logs && !file_util::CreateTemporaryFile(&zip_file)) {
    LOG(ERROR) << "Cannot create temp file";
    compress_logs = false;
  }

  LogDictionaryType* logs = NULL;
  if (CrosLibrary::Get()->EnsureLoaded())
    logs = chromeos::GetSystemLogs(
        compress_logs ? &zip_file : NULL,
        GetContextString(context));

  std::string* zip_content = NULL;
  if (compress_logs) {
    // Load compressed logs.
    zip_content = new std::string();
    LoadCompressedLogs(zip_file, zip_content);
    file_util::Delete(zip_file, false);
  }

  // Will call the callback on the calling thread.
  request->ForwardResult(Tuple2<LogDictionaryType*,
                                std::string*>(logs, zip_content));
}


void SyslogsLibraryImpl::LoadCompressedLogs(const FilePath& zip_file,
                                            std::string* zip_content) {
  DCHECK(zip_content);
  if (!file_util::ReadFileToString(zip_file, zip_content)) {
    LOG(ERROR) << "Cannot read compressed logs file from " <<
        zip_file.value().c_str();
  }
}

const char* SyslogsLibraryImpl::GetContextString(Context context) {
  switch (context) {
    case(SYSLOGS_FEEDBACK):
      return kContextFeedback;
    case(SYSLOGS_SYSINFO):
      return kContextSysInfo;
    case(SYSLOGS_NETWORK):
      return kContextNetwork;
    case(SYSLOGS_DEFAULT):
      return kContextSysInfo;
    default:
      NOTREACHED();
      return "";
  }
}

}  // namespace chromeos

// Allows InvokeLater without adding refcounting. SyslogsLibraryImpl is a
// Singleton and won't be deleted until it's last InvokeLater is run.
DISABLE_RUNNABLE_METHOD_REFCOUNT(chromeos::SyslogsLibraryImpl);
