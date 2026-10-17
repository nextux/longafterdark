// ADVAPI32.DLL — the registry, emulated (docs/ABI.md §2.15.4; API_SURFACE.md §1
// "ADVAPI32.DLL"). Never the host's: the guest sees one tree, held here.
//
// The tree: keys by name (case-insensitive, as Windows 95's), each with its
// values (name, case-insensitive; "" is the default value) of a type and
// bytes. HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE and
// HKEY_USERS are its roots, always open. An opened key is a handle
// 0x000A0000 + 4n naming its path, valid until RegCloseKey; a key opened
// twice gets two handles. The lane seeds what the program's installer wrote
// (registry_seed); what the guest writes goes into the same tree and, once
// a file is attached (registry_attach_file: a guest path, so the Vfs's state
// overlay takes it — memory headless, <state>\<pkg>\… under ADSTATE), into
// that file too, as REGEDIT4 text, read back over the seeds at attach.
//
// Implemented: RegOpenKeyA/ExA, RegCreateKeyA/ExA, RegCloseKey,
// RegQueryValueA/ExA, RegSetValueA/ExA, with Windows 95's results:
// ERROR_SUCCESS, ERROR_FILE_NOT_FOUND (2) for a key or value that is not
// there, ERROR_MORE_DATA (234) with the size needed for a short buffer,
// ERROR_INVALID_HANDLE (6) for a closed or unknown key. REG_SZ data is stored
// with its NUL; RegSetValueA (the default value of a subkey it creates) takes
// REG_SZ only, as Windows does.
//
// Known gaps, deliberately left (Screen Wreckers' DTTY3203.EXE reaches none):
// GetFileSecurityA and SetFileSecurityA (NT file ACLs) answer FALSE with
// ERROR_CALL_NOT_IMPLEMENTED (120), as Windows 95's ADVAPI32 did; there is
// no enumeration, deletion or security on keys.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "adw/core/log.h"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

namespace {

constexpr const char* A = "ADVAPI32.DLL";

// HKEY_CLASSES_ROOT … HKEY_USERS (CURRENT_USER and LOCAL_MACHINE between them).
constexpr uint32_t kRootClasses = 0x80000000, kRootUsers = 0x80000003;
constexpr uint32_t kKeyBase = 0x000A0000, kKeyStep = 4;

std::string upper(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(toupper(uint8_t(c)));
  return o;
}

struct RegValue {
  std::string name;  // as first written
  uint32_t type = REG_SZ;
  std::string data;
};

struct RegKey {
  std::string name;  // as first created
  std::map<std::string, std::unique_ptr<RegKey>> keys;  // by upper-case name
  std::map<std::string, RegValue> values;               // by upper-case name
};

struct RegistryState : RuntimeState {
  RegKey roots[4];
  std::vector<RegKey*> open;  // handle slots (nullptr = closed)
  std::string file;           // guest path of the attached file ("" = none)

  RegistryState() {
    roots[0].name = "HKEY_CLASSES_ROOT";
    roots[1].name = "HKEY_CURRENT_USER";
    roots[2].name = "HKEY_LOCAL_MACHINE";
    roots[3].name = "HKEY_USERS";
  }
  RegKey* root(uint32_t h) {
    if (h >= kRootClasses && h <= kRootUsers) return &roots[h - kRootClasses];
    return nullptr;
  }
  RegKey* key(uint32_t h) {
    if (RegKey* r = root(h)) return r;
    if (h < kKeyBase || (h - kKeyBase) % kKeyStep) return nullptr;
    size_t i = (h - kKeyBase) / kKeyStep;
    return i < open.size() ? open[i] : nullptr;
  }
  uint32_t handle(RegKey* k) {
    for (size_t i = 0; i < open.size(); i++) {
      if (!open[i]) {
        open[i] = k;
        return kKeyBase + uint32_t(i) * kKeyStep;
      }
    }
    open.push_back(k);
    return kKeyBase + uint32_t(open.size() - 1) * kKeyStep;
  }
  // The key at `sub` below `k` ("" = k itself; '\' separates, empty parts skipped).
  RegKey* walk(RegKey* k, std::string_view sub, bool create) {
    size_t i = 0;
    while (k && i <= sub.size()) {
      size_t j = sub.find('\\', i);
      if (j == std::string_view::npos) j = sub.size();
      std::string_view part = sub.substr(i, j - i);
      i = j + 1;
      if (part.empty()) continue;
      auto it = k->keys.find(upper(part));
      if (it == k->keys.end()) {
        if (!create) return nullptr;
        auto n = std::make_unique<RegKey>();
        n->name = std::string(part);
        it = k->keys.emplace(upper(part), std::move(n)).first;
      }
      k = it->second.get();
    }
    return k;
  }
  // "HKEY_LOCAL_MACHINE\SOFTWARE\..." → the key (made when `create`).
  RegKey* by_path(std::string_view path, bool create) {
    size_t s = path.find('\\');
    std::string first = upper(path.substr(0, s));
    for (RegKey& r : roots) {
      if (upper(r.name) == first) return walk(&r, s == std::string_view::npos ? "" : path.substr(s + 1), create);
    }
    return nullptr;
  }
};

RegistryState& reg(Runtime& rt) { return rt.state<RegistryState>(); }

void set_value(RegKey& k, std::string_view name, uint32_t type, std::string data) {
  RegValue& v = k.values[upper(name)];
  if (v.name.empty()) v.name = std::string(name);
  v.type = type;
  v.data = std::move(data);
}

// ---- the attached file: REGEDIT4 text ----------------------------------------------------------

std::string quote(std::string_view s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o.push_back('\\');
    o.push_back(c);
  }
  return o + "\"";
}

void dump(const RegKey& k, const std::string& path, std::string& out) {
  if (!k.values.empty()) {
    out += "\r\n[" + path + "]\r\n";
    for (const auto& [u, v] : k.values) {
      out += v.name.empty() ? "@" : quote(v.name);
      out += "=";
      if (v.type == REG_SZ && !v.data.empty() && v.data.back() == '\0' &&
          v.data.find('\0') == v.data.size() - 1) {
        out += quote(std::string_view(v.data).substr(0, v.data.size() - 1));
      } else if (v.type == REG_DWORD && v.data.size() == 4) {
        uint32_t d;
        memcpy(&d, v.data.data(), 4);
        char b[24];
        snprintf(b, sizeof(b), "dword:%08x", d);
        out += b;
      } else {
        char b[16];
        snprintf(b, sizeof(b), "hex(%x):", v.type);
        out += b;
        for (size_t i = 0; i < v.data.size(); i++) {
          snprintf(b, sizeof(b), "%s%02x", i ? "," : "", uint8_t(v.data[i]));
          out += b;
        }
      }
      out += "\r\n";
    }
  }
  for (const auto& [u, c] : k.keys) dump(*c, path + "\\" + c->name, out);
}

// Writes the whole tree's written state. Seeds included: the file is the
// registry as the guest left it.
void save(Runtime& rt) {
  RegistryState& s = reg(rt);
  if (s.file.empty()) return;
  std::string out = "REGEDIT4\r\n";
  for (const RegKey& r : s.roots) dump(r, r.name, out);
  uint32_t err = 0;
  if (!rt.vfs().write_file(s.file, out, &err)) log("registry: cannot write %s (error %u)", s.file.c_str(), err);
}

// A quoted string at text[i] (i at the opening quote); i ends past the closing one.
bool unquote(const std::string& text, size_t& i, std::string& out) {
  if (i >= text.size() || text[i] != '"') return false;
  for (i++; i < text.size(); i++) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      out.push_back(text[++i]);
    } else if (text[i] == '"') {
      i++;
      return true;
    } else {
      out.push_back(text[i]);
    }
  }
  return false;
}

void load(Runtime& rt, const std::string& text) {
  RegistryState& s = reg(rt);
  RegKey* k = nullptr;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line == "REGEDIT4") continue;
    if (line.front() == '[' && line.back() == ']') {
      k = s.by_path(line.substr(1, line.size() - 2), true);
      continue;
    }
    if (!k) continue;
    std::string name;
    size_t i = 0;
    if (line[0] == '@') {
      i = 1;
    } else if (!unquote(line, i, name)) {
      continue;
    }
    if (i >= line.size() || line[i] != '=') continue;
    i++;
    std::string rest = line.substr(i);
    if (!rest.empty() && rest[0] == '"') {
      std::string v;
      size_t j = 0;
      if (unquote(rest, j, v)) set_value(*k, name, REG_SZ, v + std::string(1, '\0'));
    } else if (rest.compare(0, 6, "dword:") == 0) {
      uint32_t d = uint32_t(strtoul(rest.c_str() + 6, nullptr, 16));
      set_value(*k, name, REG_DWORD, std::string(reinterpret_cast<const char*>(&d), 4));
    } else if (rest.compare(0, 4, "hex(") == 0) {
      size_t close = rest.find("):");
      if (close == std::string::npos) continue;
      uint32_t type = uint32_t(strtoul(rest.c_str() + 4, nullptr, 16));
      std::string bytes;
      for (size_t j = close + 2; j + 2 <= rest.size(); j += 3)
        bytes.push_back(char(strtoul(rest.substr(j, 2).c_str(), nullptr, 16)));
      set_value(*k, name, type, bytes);
    }
  }
}

// RegQueryValue(Ex)A's copy-out: *pcb in (the buffer's size) and out (the
// data's size); ERROR_MORE_DATA when the buffer is too short.
uint32_t copy_out(Call& c, const RegValue& v, uint32_t ptype, uint32_t data, uint32_t pcb) {
  if (ptype) c.mem().write_u32l(ptype, v.type);
  uint32_t need = uint32_t(v.data.size());
  uint32_t r = ERROR_SUCCESS;
  if (data) {
    uint32_t cap = pcb ? c.mem().read_u32l(pcb) : 0;
    if (cap < need) r = ERROR_MORE_DATA;
    else if (need) c.mem().memcpy(data, v.data.data(), need);
  }
  if (pcb) c.mem().write_u32l(pcb, need);
  return r;
}

// The default value of `sub` below `k` (RegQueryValueA): Windows 95 answers
// an empty string for a key that exists without one.
uint32_t query_default(Call& c, RegKey* k, uint32_t data, uint32_t pcb) {
  RegValue empty;
  empty.data = std::string(1, '\0');
  auto it = k->values.find("");
  return copy_out(c, it == k->values.end() ? empty : it->second, 0, data, pcb);
}

}  // namespace

void registry_seed(Runtime& rt, std::string_view key_path, std::string_view name, std::string_view value) {
  RegKey* k = reg(rt).by_path(key_path, true);
  if (!k) throw std::invalid_argument("registry_seed: no root in " + std::string(key_path));
  set_value(*k, name, REG_SZ, std::string(value) + std::string(1, '\0'));
}

void registry_attach_file(Runtime& rt, std::string_view guest_path) {
  RegistryState& s = reg(rt);
  s.file = std::string(guest_path);
  std::string text;
  if (rt.vfs().read_file(s.file, &text)) {
    load(rt, text);
    trace("registry", "read %s (%zu bytes)", s.file.c_str(), text.size());
  }
}

bool registry_get(Runtime& rt, std::string_view key_path, std::string_view name, std::string* value) {
  RegKey* k = reg(rt).by_path(key_path, false);
  if (!k) return false;
  auto it = k->values.find(upper(name));
  if (it == k->values.end()) return false;
  if (value) {
    *value = it->second.data;
    if (it->second.type == REG_SZ && !value->empty() && value->back() == '\0') value->pop_back();
  }
  return true;
}

void register_advapi32(ShimRegistry& r) {
  // (hKey, lpSubKey, phkResult)
  auto open = [](Call& c, uint32_t hkey, uint32_t sub, uint32_t phk, bool create, uint32_t disposition) {
    RegistryState& s = reg(c.rt);
    RegKey* base = s.key(hkey);
    if (!base) return c.ret(ERROR_INVALID_HANDLE);
    std::string name = read_cstr(c.mem(), sub);
    RegKey* k = s.walk(base, name, false);
    bool made = false;
    if (!k && create) {
      k = s.walk(base, name, true);
      made = true;
    }
    trace("registry", "%s 0x%X \\ \"%s\" -> %s", create ? "create" : "open", hkey, name.c_str(),
          k ? (made ? "made" : "found") : "not found");
    if (!k) return c.ret(ERROR_FILE_NOT_FOUND);
    if (made) save(c.rt);
    c.mem().write_u32l(phk, s.handle(k));
    if (disposition) c.mem().write_u32l(disposition, made ? REG_CREATED_NEW_KEY : REG_OPENED_EXISTING_KEY);
    c.ret(ERROR_SUCCESS);
  };
  r.impl(A, "RegOpenKeyA", [open](Call& c) { open(c, c.arg(0), c.arg(1), c.arg(2), false, 0); });
  // (hKey, lpSubKey, ulOptions, samDesired, phkResult)
  r.impl(A, "RegOpenKeyExA", [open](Call& c) { open(c, c.arg(0), c.arg(1), c.arg(4), false, 0); });
  r.impl(A, "RegCreateKeyA", [open](Call& c) { open(c, c.arg(0), c.arg(1), c.arg(2), true, 0); });
  // (hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition)
  r.impl(A, "RegCreateKeyExA", [open](Call& c) { open(c, c.arg(0), c.arg(1), c.arg(7), true, c.arg(8)); });
  r.impl(A, "RegCloseKey", [](Call& c) {
    RegistryState& s = reg(c.rt);
    uint32_t h = c.arg(0);
    if (s.root(h)) return c.ret(ERROR_SUCCESS);
    if (!s.key(h)) return c.ret(ERROR_INVALID_HANDLE);
    s.open[(h - kKeyBase) / kKeyStep] = nullptr;
    c.ret(ERROR_SUCCESS);
  });
  // (hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
  r.impl(A, "RegQueryValueExA", [](Call& c) {
    RegistryState& s = reg(c.rt);
    RegKey* k = s.key(c.arg(0));
    if (!k) return c.ret(ERROR_INVALID_HANDLE);
    std::string name = c.str(1);
    auto it = k->values.find(upper(name));
    trace("registry", "query \"%s\" in %s: %s", name.c_str(), k->name.c_str(),
          it == k->values.end() ? "not found" : it->second.data.c_str());
    if (it == k->values.end()) return c.ret(ERROR_FILE_NOT_FOUND);
    c.ret(copy_out(c, it->second, c.arg(3), c.arg(4), c.arg(5)));
  });
  // (hKey, lpSubKey, lpValue, lpcbValue): the default value of the subkey.
  r.impl(A, "RegQueryValueA", [](Call& c) {
    RegistryState& s = reg(c.rt);
    RegKey* base = s.key(c.arg(0));
    if (!base) return c.ret(ERROR_INVALID_HANDLE);
    RegKey* k = s.walk(base, c.str(1), false);
    if (!k) return c.ret(ERROR_FILE_NOT_FOUND);
    c.ret(query_default(c, k, c.arg(2), c.arg(3)));
  });
  // (hKey, lpValueName, Reserved, dwType, lpData, cbData)
  r.impl(A, "RegSetValueExA", [](Call& c) {
    RegistryState& s = reg(c.rt);
    RegKey* k = s.key(c.arg(0));
    if (!k) return c.ret(ERROR_INVALID_HANDLE);
    uint32_t type = c.arg(3), cb = c.arg(5);
    std::string data = c.arg(4) ? c.mem().read(c.arg(4), cb) : std::string();
    // A REG_SZ keeps its terminator (Windows adds one when cbData leaves it out).
    if (type == REG_SZ && (data.empty() || data.back() != '\0')) data.push_back('\0');
    set_value(*k, c.str(1), type, data);
    trace("registry", "set \"%s\" in %s (type %u, %u bytes)", c.str(1).c_str(), k->name.c_str(), type, cb);
    save(c.rt);
    c.ret(ERROR_SUCCESS);
  });
  // (hKey, lpSubKey, dwType, lpData, cbData): REG_SZ as the subkey's default value.
  r.impl(A, "RegSetValueA", [](Call& c) {
    RegistryState& s = reg(c.rt);
    RegKey* base = s.key(c.arg(0));
    if (!base) return c.ret(ERROR_INVALID_HANDLE);
    if (c.arg(2) != REG_SZ) return c.ret(ERROR_INVALID_PARAMETER);
    RegKey* k = s.walk(base, c.str(1), true);
    set_value(*k, "", REG_SZ, read_cstr(c.mem(), c.arg(3)) + std::string(1, '\0'));
    save(c.rt);
    c.ret(ERROR_SUCCESS);
  });
  // NT file security: Windows 95's ADVAPI32 had none.
  for (const char* n : {"GetFileSecurityA", "SetFileSecurityA"}) {
    r.impl(A, n, [](Call& c) {
      c.set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
      c.ret(0);
    });
  }
}

}  // namespace adw::win32
