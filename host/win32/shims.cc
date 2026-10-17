#include "win32/shims.hh"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <stdexcept>

#include "adw/core/log.h"
#include "win32/layout.hh"
#include "win32/runtime.hh"

namespace adw::win32 {

const char* conv_name(Conv c) {
  switch (c) {
    case Conv::stdcall_: return "stdcall";
    case Conv::cdecl_: return "cdecl";
    case Conv::varargs: return "varargs";
    case Conv::internal: return "internal";
  }
  return "?";
}

// ---- Call ------------------------------------------------------------------------------------------

MemoryContext& Call::mem() const { return rt.mem(); }
cpu::X86Emulator& Call::cpu() const { return rt.cpu(); }

void Call::ret(uint32_t v) {
  result_ = v;
  rt.cpu().registers().w_eax(v);
}

void Call::ret64(uint64_t v) {
  result_ = uint32_t(v);
  rt.cpu().registers().w_eax(uint32_t(v));
  rt.cpu().registers().w_edx(uint32_t(v >> 32));
}

void Call::set_last_error(uint32_t e) { rt.set_last_error(e); }

// ---- ShimRegistry ------------------------------------------------------------------------------

ShimRegistry::ShimRegistry(MemoryContext& mem) : mem_(mem) {
  for (const ShimSig& s : win32_signatures()) {
    sigs_[std::string(s.dll) + "!" + s.name] = &s;
    dlls_.insert(s.dll);
  }
  // Thunk id 0 is the call_guest sentinel's slot (see Runtime): reserve it.
  by_thunk_.push_back(nullptr);
}

std::string ShimRegistry::normalize_dll(std::string_view dll) {
  // Strip a path, upper-case, and add ".DLL" when there is no extension.
  size_t slash = dll.find_last_of("\\/");
  if (slash != std::string_view::npos) dll = dll.substr(slash + 1);
  std::string s(dll);
  for (char& c : s) c = char(toupper(uint8_t(c)));
  if (s.find('.') == std::string::npos) s += ".DLL";
  return s;
}

void ShimRegistry::impl(std::string_view dll, std::string_view name, ShimFn fn) {
  std::string d = normalize_dll(dll);
  std::string key = d + "!" + std::string(name);
  auto sig = sigs_.find(key);
  if (sig == sigs_.end()) {
    throw std::logic_error("shim " + key + " has no signature: add it to win32/signatures.cc");
  }
  add(d, name, sig->second->conv, sig->second->arg_bytes, std::move(fn));
}

ShimEntry& ShimRegistry::add(std::string_view dll, std::string_view name, Conv conv, uint16_t arg_bytes,
                             ShimFn fn) {
  // "<host>" routines are not in any DLL a guest could import or load.
  bool host = !dll.empty() && dll[0] == '<';
  std::string d = host ? std::string(dll) : normalize_dll(dll);
  std::string key = d + "!" + std::string(name);
  ShimEntry* e;
  auto it = by_key_.find(key);
  if (it != by_key_.end()) {
    e = it->second;
  } else {
    e = &entries_.emplace_back();
    e->dll = d;
    e->name = std::string(name);
    by_key_[key] = e;
  }
  e->conv = conv;
  e->arg_bytes = arg_bytes;
  e->known_signature = true;
  if (fn) e->fn = std::move(fn);
  if (!host) dlls_.insert(d);
  return *e;
}

void ShimRegistry::add_signatures(std::span<const ShimSig> sigs) {
  for (const ShimSig& s : sigs) {
    sigs_[std::string(s.dll) + "!" + s.name] = &s;
    dlls_.insert(s.dll);
  }
}

bool ShimRegistry::known(std::string_view dll, std::string_view name) const {
  std::string key = normalize_dll(dll) + "!" + std::string(name);
  return sigs_.count(key) || by_key_.count(key);
}

bool ShimRegistry::has_dll(std::string_view dll) const { return dlls_.count(normalize_dll(dll)) != 0; }

std::vector<std::string> ShimRegistry::dlls() const { return {dlls_.begin(), dlls_.end()}; }

ShimEntry* ShimRegistry::find(std::string_view dll, std::string_view name) {
  auto it = by_key_.find(normalize_dll(dll) + "!" + std::string(name));
  return it == by_key_.end() ? nullptr : it->second;
}

ShimEntry& ShimRegistry::get(std::string_view dll, std::string_view name) {
  std::string d = normalize_dll(dll);
  std::string key = d + "!" + std::string(name);
  auto it = by_key_.find(key);
  if (it != by_key_.end()) return *it->second;
  auto sig = sigs_.find(key);
  if (sig != sigs_.end()) return add(d, name, sig->second->conv, sig->second->arg_bytes);
  // Unknown signature: it cannot be returned from correctly (the bytes to pop
  // are unknown), so the census lists it and a call to it throws (Runtime).
  ShimEntry& e = entries_.emplace_back();
  e.dll = d;
  e.name = std::string(name);
  e.known_signature = false;
  by_key_[key] = &e;
  dlls_.insert(d);
  return e;
}

ShimEntry* ShimRegistry::by_thunk(uint16_t id) { return id < by_thunk_.size() ? by_thunk_[id] : nullptr; }

uint32_t ShimRegistry::address_of(uint16_t id) { return layout::kThunkBase + uint32_t(id) * layout::kThunkStride; }

int ShimRegistry::thunk_at(uint32_t eip) {
  if (eip < layout::kThunkBase || eip >= layout::kThunkBase + layout::kThunkSize) return -1;
  return int((eip - layout::kThunkBase) / layout::kThunkStride);
}

uint32_t ShimRegistry::thunk_address(ShimEntry& e) {
  if (!e.thunk) {
    if (by_thunk_.size() >= layout::kMaxThunks) throw std::runtime_error("thunk area exhausted");
    e.thunk = uint16_t(by_thunk_.size());
    by_thunk_.push_back(&e);
    uint32_t a = address_of(e.thunk);
    const uint8_t code[8] = {0xCD, 0xFE, uint8_t(e.thunk), uint8_t(e.thunk >> 8), 0xC3, 0xCC, 0xCC, 0xCC};
    mem_.memcpy(a, code, sizeof(code));
  }
  return address_of(e.thunk);
}

uint32_t ShimRegistry::internal_thunk(std::string_view name, ShimFn fn) {
  // Not under a DLL name a guest could import or LoadLibrary.
  ShimEntry& e = entries_.emplace_back();
  e.dll = "<host>";
  e.name = std::string(name);
  e.conv = Conv::internal;
  e.known_signature = true;
  e.fn = std::move(fn);
  by_key_[e.key()] = &e;
  return thunk_address(e);
}

std::vector<const ShimEntry*> ShimRegistry::unimplemented_called() const {
  std::vector<const ShimEntry*> out;
  for (const ShimEntry& e : entries_)
    if (!e.fn && e.calls) out.push_back(&e);
  std::sort(out.begin(), out.end(), [](const ShimEntry* a, const ShimEntry* b) { return a->key() < b->key(); });
  return out;
}

uint64_t ShimRegistry::unimplemented_calls() const {
  uint64_t n = 0;
  for (const ShimEntry& e : entries_)
    if (!e.fn) n += e.calls;
  return n;
}

std::vector<const ShimEntry*> ShimRegistry::unknown_signatures() const {
  std::vector<const ShimEntry*> out;
  for (const ShimEntry& e : entries_)
    if (!e.known_signature) out.push_back(&e);
  std::sort(out.begin(), out.end(), [](const ShimEntry* a, const ShimEntry* b) { return a->key() < b->key(); });
  return out;
}

void ShimRegistry::print_census(std::string_view label) const {
  auto list = unimplemented_called();
  char line[256];
  snprintf(line, sizeof(line), "[census] %.*s: %zu unimplemented API(s) called, %" PRIu64 " call(s)\n",
           int(label.size()), label.data(), list.size(), unimplemented_calls());
  write_stderr(line);
  for (const ShimEntry* e : list) {
    snprintf(line, sizeof(line), "[census]   %s x%" PRIu64 "%s\n", e->key().c_str(), e->calls,
             e->known_signature ? "" : " (unknown signature)");
    write_stderr(line);
  }
  // The host runs no message loop between Module() calls (user32.cc "Known
  // gaps"): a WM_TIMER or a posted message reaches the module only when it
  // pumps its own queue. No module in the corpus sets a timer or posts; a
  // census that shows this line has found one that does.
  std::string loop;
  for (const ShimEntry& e : entries_) {
    if (!e.calls || e.dll != "USER32.DLL") continue;
    if (e.name == "SetTimer" || e.name == "PostMessageA" || e.name == "PostThreadMessageA")
      loop += (loop.empty() ? "" : ", ") + e.name + " x" + std::to_string(e.calls);
  }
  if (!loop.empty()) {
    write_stderr("[census] " + std::string(label) + ": " + loop +
                 " (timers and posted messages reach it only through its own message calls)\n");
  }
  // Bound but never called imports without a signature are harmless in this
  // run, yet a longer one could stop on them: name them all on one line.
  auto unknown = unknown_signatures();
  if (!unknown.empty()) {
    std::string names;
    for (const ShimEntry* e : unknown) names += (names.empty() ? "" : ", ") + e->key();
    write_stderr("[census] " + std::string(label) + ": " + std::to_string(unknown.size()) +
                 " import(s) without a signature (a call stops the module): " + names + "\n");
  }
}

}  // namespace adw::win32
