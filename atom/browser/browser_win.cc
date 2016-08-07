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
#include "atom/common/atom_version.h"
#include "atom/common/native_mate_converters/callback.h"
#include "atom/common/native_mate_converters/file_path_converter.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "native_mate/dictionary.h"
#include "native_mate/object_template_builder.h"

namespace atom {

struct JumpListItem {
  enum class Type { TASK, SEPARATOR, FILE };

  Type type;
  // program/file path
  base::FilePath path;
  base::string16 arguments;
  base::string16 title;
  base::string16 description;
  base::FilePath icon_path;
  int icon_index;
};

struct JumpListCategory {
  enum class Type { CUSTOM, FREQUENT, RECENT, TASKS };

  Type type;
  base::string16 name;
  std::vector<JumpListItem> items;
};

using GetJumpListCategoriesCallback = base::Callback<v8::Local<v8::Value>(
  int min_slots, const std::vector<JumpListItem>& removed_items)>;

}  // namespace atom

namespace mate {

using atom::JumpListItem;
using atom::JumpListCategory;

template<>
struct Converter<JumpListItem> {
  static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val,
                     JumpListItem* out) {
    mate::Dictionary dict;
    if (!ConvertFromV8(isolate, val, &dict))
      return false;

    std::string item_type;
    if (!dict.Get("type", &(item_type)))
      return false;

    if (item_type == "task") {
      out->type = JumpListItem::Type::TASK;
    } else if (item_type == "separator") {
      out->type = JumpListItem::Type::SEPARATOR;
      return true;
    } else if (item_type == "file") {
      out->type = JumpListItem::Type::FILE;
    } else {
      return false;
    }

    if (out->type == JumpListItem::Type::TASK) {
      if (!dict.Get("program", &(out->path)) ||
          !dict.Get("title", &(out->title)))
        return false;

      if (dict.Get("iconPath", &(out->icon_path)) &&
          !dict.Get("iconIndex", &(out->icon_index)))
        return false;

      dict.Get("arguments", &(out->arguments));
      dict.Get("description", &(out->description));

      return true;
    } else {  // file item
      return dict.Get("path", &(out->path));
    }
  }

  static v8::Local<v8::Value> ToV8(v8::Isolate* isolate,
                                   const JumpListItem& val) {
    mate::Dictionary dict = mate::Dictionary::CreateEmpty(isolate);
    switch (val.type) {
      case JumpListItem::Type::TASK:
        dict.Set("type", "task");
        dict.Set("program", val.path);
        dict.Set("arguments", val.arguments);
        dict.Set("title", val.title);
        dict.Set("iconPath", val.icon_path);
        dict.Set("iconIndex", val.icon_index);
        dict.Set("description", val.description);
        break;

      case JumpListItem::Type::SEPARATOR:
        dict.Set("type", "separator");
        break;

      case JumpListItem::Type::FILE:
        dict.Set("type", "file");
        dict.Set("path", val.path);
        break;
    }
    return dict.GetHandle();
  }
};

template<>
struct Converter<JumpListCategory> {
  static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val,
                      JumpListCategory* out) {
    mate::Dictionary dict;
    if (!ConvertFromV8(isolate, val, &dict))
      return false;

    if (dict.Get("category", &(out->name)) && out->name.empty())
      return false;

    std::string category_type;
    if (dict.Get("type", &category_type)) {
      if (category_type == "frequent")
        out->type = JumpListCategory::Type::FREQUENT;
      else if (category_type == "recent")
        out->type = JumpListCategory::Type::RECENT;
      else if (category_type == "custom")
        out->type = JumpListCategory::Type::CUSTOM;
      else
        return false;
    } else {
      if (out->name.empty())
        out->type = JumpListCategory::Type::TASKS;
      else
        out->type = JumpListCategory::Type::CUSTOM;
    }

    if ((out->type == JumpListCategory::Type::TASKS) ||
        (out->type == JumpListCategory::Type::CUSTOM))
      if (!dict.Get("items", &(out->items)))
        return false;

    return true;
  }
};

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

void AppendJumpListTask(IObjectCollection* collection,
                        const JumpListItem& item) {
  CComPtr<IShellLink> link;
  if (FAILED(link.CoCreateInstance(CLSID_ShellLink)) ||
      FAILED(link->SetPath(item.path.value().c_str())) ||
      FAILED(link->SetArguments(item.arguments.c_str())) ||
      FAILED(link->SetDescription(item.description.c_str())))
    return;

  if (!item.icon_path.empty() &&
      FAILED(link->SetIconLocation(item.icon_path.value().c_str(),
                                   item.icon_index)))
    return;

  CComQIPtr<IPropertyStore> property_store = link;
  if (!base::win::SetStringValueForPropertyStore(property_store, PKEY_Title,
                                                 item.title.c_str()))
    return;

  collection->AddObject(link);
}

void AppendJumpListSeparator(IObjectCollection* collection) {
  CComPtr<IShellLink> separator;
  if (SUCCEEDED(separator.CoCreateInstance(CLSID_ShellLink))) {
    CComQIPtr<IPropertyStore> property_store = separator;
    if (base::win::SetBooleanValueForPropertyStore(
      property_store, PKEY_AppUserModel_IsDestListSeparator, true)) {
      collection->AddObject(separator);
    }
  }
}

void AppendJumpListFile(IObjectCollection* collection,
                        const JumpListItem& item) {
  CComPtr<IShellItem> file;
  if (SUCCEEDED(SHCreateItemFromParsingName(
        item.path.value().c_str(), NULL, IID_PPV_ARGS(&file)))) {
    collection->AddObject(file);
  }
}

void AppendJumpListCategory(ICustomDestinationList* destinations,
                            const JumpListCategory& category) {
  if (category.items.empty())
    return;

  CComPtr<IObjectCollection> collection;
  if (FAILED(collection.CoCreateInstance(CLSID_EnumerableObjectCollection)))
    return;

  for (const auto& item : category.items) {
    switch (item.type) {
      case JumpListItem::Type::TASK:
        AppendJumpListTask(collection, item);
        break;

      case JumpListItem::Type::SEPARATOR:
        if (category.type == JumpListCategory::Type::TASKS) {
          AppendJumpListSeparator(collection);
        } else {
          LOG(ERROR) << "Can't append separator to Jump List category '"
                     << category.name << "'. Separators only allowed in "
                     << "'Tasks' Jump List category.";
        }
        break;

      case JumpListItem::Type::FILE:
        AppendJumpListFile(collection, item);
        break;
    }
  }

  CComQIPtr<IObjectArray> items = collection;

  if (category.type == JumpListCategory::Type::TASKS) {
    destinations->AddUserTasks(items);
  } else {
    auto hr = destinations->AppendCategory(category.name.c_str(), items);
    if (FAILED(hr)) {
      if (hr == 0x80040F03) {
        LOG(ERROR) << "Failed to append category '" << category.name
                   << "' to Jump List due to missing file type registration.";
      } else if (hr == E_ACCESSDENIED) {
        LOG(ERROR) << "Failed to append category '" << category.name
                   << "' to Jump List due to privacy settings.";
      } else {
        LOG(ERROR) << "Failed to append category '" << category.name << "'";
      }
    }
  }
}

bool GetShellItemFileName(IShellItem* shell_item, base::FilePath* file_name) {
  DCHECK(file_name);

  wchar_t* file_name_buffer = nullptr;
  if (SUCCEEDED(shell_item->GetDisplayName(SIGDN_FILESYSPATH,
                                           &file_name_buffer))) {
    *file_name = base::FilePath(file_name_buffer);
    ::CoTaskMemFree(file_name_buffer);
    return true;
  }
  return false;
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

void Browser::SetJumpList(v8::Local<v8::Value> val, mate::Arguments* args) {
  // TODO(enlight): Maybe move this to the BrowserThread::FILE thread?
  bool delete_jump_list = val->IsNull();
  GetJumpListCategoriesCallback get_categories;
  if (!delete_jump_list &&
      !mate::ConvertFromV8(args->isolate(), val, &get_categories)) {
    args->ThrowError("Argument must be null or a function");
  }

  CComPtr<ICustomDestinationList> destinations;
  if (FAILED(destinations.CoCreateInstance(CLSID_DestinationList)))
    return;

  if (delete_jump_list) {
    destinations->DeleteList(GetAppUserModelID());
    return;
  }

  if (FAILED(destinations->SetAppID(GetAppUserModelID())))
    return;

  // Start a transaction that updates the JumpList of this application.
  UINT min_slots;
  CComPtr<IObjectArray> removed;
  if (FAILED(destinations->BeginList(&min_slots, IID_PPV_ARGS(&removed))))
    return;

  // Convert the removed items COM array to a std::vector.
  std::vector<JumpListItem> removed_items;
  UINT removed_count;
  if (SUCCEEDED(removed->GetCount(&removed_count) && (removed_count > 0))) {
    removed_items.reserve(removed_count);
    JumpListItem item;
    IShellItem* shell_item;
    IShellLink* shell_link;
    for (UINT i = 0; i < removed_count; ++i) {
      if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(&shell_item)))) {
        item.type = JumpListItem::Type::FILE;
        GetShellItemFileName(shell_item, &item.path);
      } else if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(&shell_link)))) {
        item.type = JumpListItem::Type::TASK;
        // TODO(enlight): populate the rest of the fields
      }
    }
  }

  auto retval = get_categories.Run(min_slots, removed_items);
  std::vector<JumpListCategory> categories;
  if (mate::ConvertFromV8(args->isolate(), retval, &categories)) {
    for (const auto& category : categories) {
      switch (category.type) {
        case JumpListCategory::Type::TASKS:
        case JumpListCategory::Type::CUSTOM:
          AppendJumpListCategory(destinations, category);
          break;

        case JumpListCategory::Type::RECENT:
          destinations->AppendKnownCategory(KDC_RECENT);
          break;

        case JumpListCategory::Type::FREQUENT:
          destinations->AppendKnownCategory(KDC_FREQUENT);
          break;
      }
    }
    if (FAILED(destinations->CommitList()))
      LOG(ERROR) << "Failed to commit changes to custom Jump List.";
  } else {
    destinations->AbortList();
  }
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
