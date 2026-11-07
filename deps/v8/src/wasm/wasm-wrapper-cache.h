// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_WASM_WASM_WRAPPER_CACHE_H_
#define V8_WASM_WASM_WRAPPER_CACHE_H_

#if !V8_ENABLE_WEBASSEMBLY
#error This header should only be included if WebAssembly is enabled.
#endif  // !V8_ENABLE_WEBASSEMBLY

#include <unordered_map>

#include "src/base/platform/mutex.h"
#include "src/logging/counters.h"
#include "src/wasm/module-instantiate.h"
#include "include/v8.h"  // For replay.
#include "src/wasm/wasm-code-manager.h"

namespace v8::internal::wasm {

class WasmCode;
class WasmWrapperHandle;

// Takes a cache's ordered lock, for the caches which asked for one. Ordering
// the mutex itself would order every stack walk, including the walks Replay's
// own CDP handling does with events disallowed, where there is no order to
// record; see WasmImportWrapperCache's constructor.
class V8_NODISCARD WasmWrapperCacheOrderedScope {
 public:
  explicit WasmWrapperCacheOrderedScope(int lock) : lock_(lock) {
    if (lock_) v8::recordreplay::OrderedLock(lock_);
  }
  ~WasmWrapperCacheOrderedScope() {
    if (lock_) v8::recordreplay::OrderedUnlock(lock_);
  }

 private:
  const int lock_;
};

// Implements a cache for import wrappers.
template <typename CacheKey>
class WasmWrapperCache {
 public:
  // Helper class to modify the cache under a lock.
  class V8_NODISCARD ModificationScope {
   public:
    explicit ModificationScope(WasmWrapperCache<CacheKey>* cache)
        : cache_(cache),
          ordered_(cache->record_replay_ordered_lock_),
          guard_(&cache->mutex_) {}

    WasmCode* AddWrapper(
        WasmCompilationResult result, WasmCode::Kind kind,
        uint64_t signature_hash,
        std::shared_ptr<wasm::WasmWrapperHandle> wrapper_handle);

   private:
    WasmWrapperCache* const cache_;
    // Declared before guard_ so the recorded order is taken first.
    WasmWrapperCacheOrderedScope ordered_;
    base::MutexGuard guard_;
  };

  WasmWrapperCache() : code_allocator_{&counter_updates_} {}
  ~WasmWrapperCache() = default;

  void Free(std::vector<WasmCode*>& wrappers);

  V8_EXPORT_PRIVATE std::shared_ptr<WasmWrapperHandle> GetCompiled(
      Isolate* isolate, CacheKey cache_key);

  WasmCode* Lookup(Address pc) const;

  void LogForIsolate(Isolate* isolate);

  size_t EstimateCurrentMemoryConsumption() const;

  // Call this from time to time (after creating / tiering up import wrappers)
  // to publish delayed counter updates in the given isolate.
  void PublishCounterUpdates(Isolate* isolate) {
    counter_updates_.Publish(isolate);
  }

 protected:
  virtual std::pair<WasmCompilationResult, WasmCode::Kind> CompileWrapper(
      Isolate* isolate, const CacheKey& cache_key) = 0;

  std::shared_ptr<WasmWrapperHandle> CacheCompiledWrapper(
      Isolate* isolate, WasmCompilationResult, WasmCode::Kind, CacheKey,
      std::shared_ptr<WasmWrapperHandle>);

  WasmCodeAllocator code_allocator_;
  mutable base::Mutex mutex_;
  // Orders the acquisitions that compile or publish a wrapper, for a cache
  // whose constructor creates a lock. Zero leaves the cache unordered, which
  // is what the export and stack wrapper caches are. Not taken by Lookup(),
  // which every stack walk reaches.
  int record_replay_ordered_lock_ = 0;
  std::unordered_map<CacheKey, std::weak_ptr<WasmWrapperHandle>,
                     typename CacheKey::Hash>
      entry_map_;

  // Lookup support. The map key is the instruction start address.
  std::map<Address, WasmCode*> codes_;

  // Since the cache is not owned by a single isolate, we store histogram
  // samples and publish them in the next best isolate.
  DelayedCounterUpdates counter_updates_;

  friend class WasmWrapperHandle;
};

class WasmWrapperHandle {
 public:
  WasmWrapperHandle(Address addr, uint64_t signature_hash);
  WasmWrapperHandle(WasmCode* code, Address addr, uint64_t signature_hash);
  WasmWrapperHandle(const WasmWrapperHandle&) = delete;
  WasmWrapperHandle& operator=(const WasmWrapperHandle&) = delete;

  ~WasmWrapperHandle();

  WasmCodePointer code_pointer() const { return code_pointer_; }
  const WasmCode* code() const { return code_.load(std::memory_order_acquire); }
  bool has_code() const {
    return code_.load(std::memory_order_acquire) != nullptr;
  }

  void set_code(WasmCode* code);

 private:
  const WasmCodePointer code_pointer_;
  std::atomic<WasmCode*> code_ = nullptr;
};

}  //  namespace v8::internal::wasm

#endif  // V8_WASM_WASM_WRAPPER_CACHE_H_
