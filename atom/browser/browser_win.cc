// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/browser.h"

#include <atlbase.h>
#include <propkey.h>
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "base/base_paths.h"
#include "base/file_version_info.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/win_util.h"
#include "base/win/registry.h"
#include "base/win/windows_version.h"
#include "atom/browser/ui/win/jump_list.h"
#include "atom/common/atom_version.h"
#include "atom/common/native_mate_converters/callback.h"
#include "atom/common/native_mate_converters/file_path_converter.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "native_mate/dictionary.h"
#include "native_mate/object_template_builder.h"

namespace atom {

using GetJumpListCategoriesCallback = base::Callback<v8::Local<v8::Value>(
  int min_slots, const std::vector<JumpListItem>& removed_items)>;

}  // namespace atom

namespace mate {

using atom::JumpListItem;
using atom::JumpListCategory;
using atom::JumpListResult;

template<>
struct Converter<JumpListItem::Type> {
  static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val,
                     JumpListItem::Type* out) {
    std::string item_type;
    if (!ConvertFromV8(isolate, val, &item_type))
      return false;

    if (item_type == "task")
      *out = JumpListItem::Type::TASK;
    else if (item_type == "separator")
      *out = JumpListItem::Type::SEPARATOR;
    else if (item_type == "file")
      *out = JumpListItem::Type::FILE;
    else
      return false;

    return true;
  }

  static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,
                                   JumpListItem::Type val) {
    std::string item_type;
    switch (val) {
      case JumpListItem::Type::TASK:
        item_type = "task";
        break;

      case JumpListItem::Type::SEPARATOR:
        item_type = "separator";
        break;

      case JumpListItem::Type::FILE:
        item_type = "file";
        break;
    }
    return mate::ConvertToV8(isolate, item_type);
  }
};

template<>
struct Converter<JumpListItem> {
  static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val,
                     JumpListItem* out) {
    mate::Dictionary dict;
    if (!ConvertFromV8(isolate, val, &dict))
      return false;

    if (!dict.Get("type", &(out->type)))
      return false;

    switch (out->type) {
      case JumpListItem::Type::TASK:
        if (!dict.Get("program", &(out->path)) ||
            !dict.Get("title", &(out->title)))
          return false;

        if (dict.Get("iconPath", &(out->icon_path)) &&
            !dict.Get("iconIndex", &(out->icon_index)))
          return false;

        dict.Get("arguments", &(out->arguments));
        dict.Get("description", &(out->description));
        return true;

      case JumpListItem::Type::SEPARATOR:
        return true;

      case JumpListItem::Type::FILE:
        return dict.Get("path", &(out->path));
    }

    assert(false);
    return false;
  }

  static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,
                                   const JumpListItem& val) {
    mate::Dictionary dict = mate::Dictionary::CreateEmpty(isolate);
    dict.Set("type", val.type);

    switch (val.type) {
      case JumpListItem::Type::TASK:
        dict.Set("program", val.path);
        dict.Set("arguments", val.arguments);
        dict.Set("title", val.title);
        dict.Set("iconPath", val.icon_path);
        dict.Set("iconIndex", val.icon_index);
        dict.Set("description", val.description);
        break;

      case JumpListItem::Type::SEPARATOR:
        break;

      case JumpListItem::Type::FILE:
        dict.Set("path", val.path);
        break;
    }
    return dict.GetHandle();
  }
};

template<>
struct Converter<JumpListCategory::Type> {
  static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val,
                     JumpListCategory::Type* out) {
    std::string category_type;
    if (!ConvertFromV8(isolate, val, &category_type))
      return false;

    if (category_type == "tasks")
      *out = JumpListCategory::Type::TASKS;
    else if (category_type == "frequent")
      *out = JumpListCategory::Type::FREQUENT;
    else if (category_type == "recent")
      *out = JumpListCategory::Type::RECENT;
    else if (category_type == "custom")
      *out = JumpListCategory::Type::CUSTOM;
    else
      return false;

    return true;
  }

  static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,
                                   JumpListCategory::Type val) {
    std::string category_type;
    switch (val) {
      case JumpListCategory::Type::TASKS:
        category_type = "tasks";
        break;

      case JumpListCategory::Type::FREQUENT:
        category_type = "frequent";
        break;

      case JumpListCategory::Type::RECENT:
        category_type = "recent";
        break;

      case JumpListCategory::Type::CUSTOM:
        category_type = "custom";
        break;
    }
    return mate::ConvertToV8(isolate, category_type);
  }
};

template<>
struct Converter<JumpListCategory> {
  static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val,
                     JumpListCategory* out) {
    mate::Dictionary dict;
    if (!ConvertFromV8(isolate, val, &dict))
      return false;

    if (dict.Get("name", &(out->name)) && out->name.empty())
      return false;

    if (!dict.Get("type", &(out->type))) {
      if (out->name.empty())
        out->type = JumpListCategory::Type::TASKS;
      else
        out->type = JumpListCategory::Type::CUSTOM;
    }

    if ((out->type == JumpListCategory::Type::TASKS) ||
        (out->type == JumpListCategory::Type::CUSTOM)) {
      if (!dict.Get("items", &(out->items)))
        return false;
    }

    return true;
  }
};

// static
v8::Local<v8::Value> Converter<JumpListResult>::ToV8(v8::Isolate* isolate,
                                                     JumpListResult val) {
  std::string result_code;
  switch (val) {
    case JumpListResult::SUCCESS:
      result_code = "ok";
      break;

    case JumpListResult::ARGUMENT_ERROR:
      result_code = "argumentError";
      break;

    case JumpListResult::GENERIC_ERROR:
      result_code = "error";
      break;

    case JumpListResult::CUSTOM_CATEGORY_SEPARATOR_ERROR:
      result_code = "invalidSeparatorError";
      break;

    case JumpListResult::MISSING_FILE_TYPE_REGISTRATION_ERROR:
      result_code = "fileTypeRegistrationError";
      break;

    case JumpListResult::CUSTOM_CATEGORY_ACCESS_DENIED_ERROR:
      result_code = "customCategoryAccessDeniedError";
      break;
  }
  return ConvertToV8(isolate, result_code);
}

}  // namespace mate

namespace atom {

namespace {

const wchar_t kAppUserModelIDFormat[] = L"electron.app.$1";

BOOL CALLBACK WindowsEnumerationHandler(HWND hwnd, LPARAM param) {
  DWORD target_process_id = *reinterpret_cast<DWORD*>(param);
  DWORD process_id = 0;

  GetWindowThreadProcessId(hwnd, &process_id);
  if (process_id == target_process_id) {
    SetFocus(hwnd);
    return FALSE;
  }

  return TRUE;
}

}  // namespace

void Browser::Focus() {
  // On Windows we just focus on the first window found for this process.
  DWORD pid = GetCurrentProcessId();
  EnumWindows(&WindowsEnumerationHandler, reinterpret_cast<LPARAM>(&pid));
}

void Browser::AddRecentDocument(const base::FilePath& path) {
  if (base::win::GetVersion() < base::win::VERSION_WIN7)
    return;

  CComPtr<IShellItem> item;
  HRESULT hr = SHCreateItemFromParsingName(
      path.value().c_str(), NULL, IID_PPV_ARGS(&item));
  if (SUCCEEDED(hr)) {
    SHARDAPPIDINFO info;
    info.psi = item;
    info.pszAppID = GetAppUserModelID();
    SHAddToRecentDocs(SHARD_APPIDINFO, &info);
  }
}

void Browser::ClearRecentDocuments() {
  CComPtr<IApplicationDestinations> destinations;
  if (FAILED(destinations.CoCreateInstance(CLSID_ApplicationDestinations,
                                           NULL, CLSCTX_INPROC_SERVER)))
    return;
  if (FAILED(destinations->SetAppID(GetAppUserModelID())))
    return;
  destinations->RemoveAllDestinations();
}

void Browser::SetAppUserModelID(const base::string16& name) {
  app_user_model_id_ = name;
  SetCurrentProcessExplicitAppUserModelID(app_user_model_id_.c_str());
}

bool Browser::SetUserTasks(const std::vector<UserTask>& tasks) {
  CComPtr<ICustomDestinationList> destinations;
  if (FAILED(destinations.CoCreateInstance(CLSID_DestinationList)))
    return false;
  if (FAILED(destinations->SetAppID(GetAppUserModelID())))
    return false;

  // Start a transaction that updates the JumpList of this application.
  UINT max_slots;
  CComPtr<IObjectArray> removed;
  if (FAILED(destinations->BeginList(&max_slots, IID_PPV_ARGS(&removed))))
    return false;

  CComPtr<IObjectCollection> collection;
  if (FAILED(collection.CoCreateInstance(CLSID_EnumerableObjectCollection)))
    return false;

  for (auto& task : tasks) {
    CComPtr<IShellLink> link;
    if (FAILED(link.CoCreateInstance(CLSID_ShellLink)) ||
        FAILED(link->SetPath(task.program.value().c_str())) ||
        FAILED(link->SetArguments(task.arguments.c_str())) ||
        FAILED(link->SetDescription(task.description.c_str())))
      return false;

    if (!task.icon_path.empty() &&
        FAILED(link->SetIconLocation(task.icon_path.value().c_str(),
                                     task.icon_index)))
      return false;

    CComQIPtr<IPropertyStore> property_store = link;
    if (!base::win::SetStringValueForPropertyStore(property_store, PKEY_Title,
                                                   task.title.c_str()))
      return false;

    if (FAILED(collection->AddObject(link)))
      return false;
  }

  // When the list is empty "AddUserTasks" could fail, so we don't check return
  // value for it.
  CComQIPtr<IObjectArray> task_array = collection;
  destinations->AddUserTasks(task_array);
  return SUCCEEDED(destinations->CommitList());
}

JumpListResult Browser::SetJumpList(v8::Local<v8::Value> val,
                                    mate::Arguments* args) {
  // TODO(enlight): Maybe move this to the BrowserThread::FILE thread?
  bool delete_jump_list = val->IsNull();
  GetJumpListCategoriesCallback get_categories;
  if (!delete_jump_list &&
      !mate::ConvertFromV8(args->isolate(), val, &get_categories)) {
    args->ThrowError("Argument must be null or a function");
    return JumpListResult::ARGUMENT_ERROR;
  }

  JumpList jump_list(GetAppUserModelID());

  if (delete_jump_list) {
    return jump_list.Delete()
      ? JumpListResult::SUCCESS
      : JumpListResult::GENERIC_ERROR;
  }

  // Start a transaction that updates the JumpList of this application.
  int min_items;
  std::vector<JumpListItem> removed_items;
  if (!jump_list.Begin(&min_items, &removed_items))
    return JumpListResult::GENERIC_ERROR;

  auto result = JumpListResult::SUCCESS;
  // Let the app generate the list of categories to add to the Jump List.
  auto categories_val = get_categories.Run(min_items, removed_items);
  std::vector<JumpListCategory> categories;
  if (mate::ConvertFromV8(args->isolate(), categories_val, &categories)) {
    result = jump_list.AppendCategories(categories);
    if (!jump_list.Commit()) {
      LOG(ERROR) << "Failed to commit changes to custom Jump List.";
      // It's more useful to return the earlier error code that might give
      // some indication as to why the transaction actually failed.
      if (result == JumpListResult::SUCCESS)
        result = JumpListResult::GENERIC_ERROR;
    }
  } else {
    jump_list.Abort();
    args->ThrowError("Callback failed to return a valid category array.");
    result = JumpListResult::ARGUMENT_ERROR;
  }
  return result;
}

bool Browser::RemoveAsDefaultProtocolClient(const std::string& protocol) {
  if (protocol.empty())
    return false;

  base::FilePath path;
  if (!PathService::Get(base::FILE_EXE, &path)) {
    LOG(ERROR) << "Error getting app exe path";
    return false;
  }

  // Main Registry Key
  HKEY root = HKEY_CURRENT_USER;
  std::string keyPathStr = "Software\\Classes\\" + protocol;
  std::wstring keyPath = std::wstring(keyPathStr.begin(), keyPathStr.end());

  // Command Key
  std::string cmdPathStr = keyPathStr + "\\shell\\open\\command";
  std::wstring cmdPath = std::wstring(cmdPathStr.begin(), cmdPathStr.end());

  base::win::RegKey key;
  base::win::RegKey commandKey;
  if (FAILED(key.Open(root, keyPath.c_str(), KEY_ALL_ACCESS)))
    // Key doesn't even exist, we can confirm that it is not set
    return true;

  if (FAILED(commandKey.Open(root, cmdPath.c_str(), KEY_ALL_ACCESS)))
    // Key doesn't even exist, we can confirm that it is not set
    return true;

  std::wstring keyVal;
  if (FAILED(commandKey.ReadValue(L"", &keyVal)))
    // Default value not set, we can confirm that it is not set
    return true;

  std::wstring exePath(path.value());
  std::wstring exe = L"\"" + exePath + L"\" \"%1\"";
  if (keyVal == exe) {
    // Let's kill the key
    if (FAILED(key.DeleteKey(L"shell")))
      return false;

    return true;
  } else {
    return true;
  }
}

bool Browser::SetAsDefaultProtocolClient(const std::string& protocol) {
  // HKEY_CLASSES_ROOT
  //    $PROTOCOL
  //       (Default) = "URL:$NAME"
  //       URL Protocol = ""
  //       shell
  //          open
  //             command
  //                (Default) = "$COMMAND" "%1"
  //
  // However, the "HKEY_CLASSES_ROOT" key can only be written by the
  // Administrator user. So, we instead write to "HKEY_CURRENT_USER\
  // Software\Classes", which is inherited by "HKEY_CLASSES_ROOT"
  // anyway, and can be written by unprivileged users.

  if (protocol.empty())
    return false;

  base::FilePath path;
  if (!PathService::Get(base::FILE_EXE, &path)) {
    LOG(ERROR) << "Error getting app exe path";
    return false;
  }

  // Main Registry Key
  HKEY root = HKEY_CURRENT_USER;
  std::string keyPathStr = "Software\\Classes\\" + protocol;
  std::wstring keyPath = std::wstring(keyPathStr.begin(), keyPathStr.end());
  std::string urlDeclStr = "URL:" + protocol;
  std::wstring urlDecl = std::wstring(urlDeclStr.begin(), urlDeclStr.end());

  // Command Key
  std::string cmdPathStr = keyPathStr + "\\shell\\open\\command";
  std::wstring cmdPath = std::wstring(cmdPathStr.begin(), cmdPathStr.end());

  // Executable Path
  std::wstring exePath(path.value());
  std::wstring exe = L"\"" + exePath + L"\" \"%1\"";

  // Write information to registry
  base::win::RegKey key(root, keyPath.c_str(), KEY_ALL_ACCESS);
  if (FAILED(key.WriteValue(L"URL Protocol", L"")) ||
      FAILED(key.WriteValue(L"", urlDecl.c_str())))
    return false;

  base::win::RegKey commandKey(root, cmdPath.c_str(), KEY_ALL_ACCESS);
  if (FAILED(commandKey.WriteValue(L"", exe.c_str())))
    return false;

  return true;
}

bool Browser::IsDefaultProtocolClient(const std::string& protocol) {
  if (protocol.empty())
    return false;

  base::FilePath path;
  if (!PathService::Get(base::FILE_EXE, &path)) {
    LOG(ERROR) << "Error getting app exe path";
    return false;
  }

  // Main Registry Key
  HKEY root = HKEY_CURRENT_USER;
  std::string keyPathStr = "Software\\Classes\\" + protocol;
  std::wstring keyPath = std::wstring(keyPathStr.begin(), keyPathStr.end());

  // Command Key
  std::string cmdPathStr = keyPathStr + "\\shell\\open\\command";
  std::wstring cmdPath = std::wstring(cmdPathStr.begin(), cmdPathStr.end());

  base::win::RegKey key;
  base::win::RegKey commandKey;
  if (FAILED(key.Open(root, keyPath.c_str(), KEY_ALL_ACCESS)))
    // Key doesn't exist, we can confirm that it is not set
    return false;

  if (FAILED(commandKey.Open(root, cmdPath.c_str(), KEY_ALL_ACCESS)))
    // Key doesn't exist, we can confirm that it is not set
    return false;

  std::wstring keyVal;
  if (FAILED(commandKey.ReadValue(L"", &keyVal)))
    // Default value not set, we can confirm that it is not set
    return false;

  std::wstring exePath(path.value());
  std::wstring exe = L"\"" + exePath + L"\" \"%1\"";
  if (keyVal == exe) {
    // Default value is the same as current file path
    return true;
  } else {
    return false;
  }
}

bool Browser::SetBadgeCount(int count) {
  return false;
}

void Browser::SetLoginItemSettings(LoginItemSettings settings) {
  std::wstring keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  base::win::RegKey key(HKEY_CURRENT_USER, keyPath.c_str(), KEY_ALL_ACCESS);

  if (settings.open_at_login) {
    base::FilePath path;
    if (PathService::Get(base::FILE_EXE, &path)) {
      std::wstring exePath(path.value());
      key.WriteValue(GetAppUserModelID(), exePath.c_str());
    }
  } else {
    key.DeleteValue(GetAppUserModelID());
  }
}

Browser::LoginItemSettings Browser::GetLoginItemSettings() {
  LoginItemSettings settings;
  std::wstring keyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  base::win::RegKey key(HKEY_CURRENT_USER, keyPath.c_str(), KEY_ALL_ACCESS);
  std::wstring keyVal;

  if (!FAILED(key.ReadValue(GetAppUserModelID(), &keyVal))) {
    base::FilePath path;
    if (PathService::Get(base::FILE_EXE, &path)) {
      std::wstring exePath(path.value());
      settings.open_at_login = keyVal == exePath;
    }
  }

  return settings;
}


PCWSTR Browser::GetAppUserModelID() {
  if (app_user_model_id_.empty()) {
    SetAppUserModelID(base::ReplaceStringPlaceholders(
        kAppUserModelIDFormat, base::UTF8ToUTF16(GetName()), nullptr));
  }

  return app_user_model_id_.c_str();
}

std::string Browser::GetExecutableFileVersion() const {
  base::FilePath path;
  if (PathService::Get(base::FILE_EXE, &path)) {
    std::unique_ptr<FileVersionInfo> version_info(
        FileVersionInfo::CreateFileVersionInfo(path));
    return base::UTF16ToUTF8(version_info->product_version());
  }

  return ATOM_VERSION_STRING;
}

std::string Browser::GetExecutableFileProductName() const {
  base::FilePath path;
  if (PathService::Get(base::FILE_EXE, &path)) {
    std::unique_ptr<FileVersionInfo> version_info(
        FileVersionInfo::CreateFileVersionInfo(path));
    return base::UTF16ToUTF8(version_info->product_name());
  }

  return ATOM_PRODUCT_NAME;
}

}  // namespace atom
