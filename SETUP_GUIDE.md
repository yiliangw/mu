# Cluster Setup Guide for mu

This guide documents all modifications required to build and run mu on nsl cluster (node1-3) with **GCC 15** and **RoCE**.

The original project targets GCC 7 + native InfiniBand. Our cluster runs GCC 15 and uses RoCE, so both compiler compatibility and RDMA transport changes were needed.

---

## 1. Install Conan v1

The project uses Conan v1 API (`python_requires`, `conanbuildinfo.cmake`). Conan v2 is incompatible.

```bash
pip3 install --break-system-packages "conan<2.0.0"
conan profile new default --detect
conan profile update settings.compiler.libcxx=libstdc++11 default
```

## 2. Register GCC 15 with Conan

- **`~/.conan/settings.yml`** — Add `"15"` to the GCC version list (under `compiler > gcc > version`)
- **`conan/profiles/gcc-release.profile`** — Change `compiler.version=7` to `compiler.version=15`

## 3. GCC 15 Compiler Compatibility Fixes

GCC 15 enforces stricter C++17 compliance and introduces new warnings treated as errors.

### 3a. Suppress new warnings

**`conan/exports/compiler-options/compileroptions.py`**
- Add `-Wno-template-body` to both C and CXX flags (new GCC 15 warning)
- Replace `-Wnoexcept` with `-Wno-noexcept` in CXX flags (flag was renamed)

### 3b. Add missing standard library headers

GCC 15 no longer transitively includes many standard headers. Add explicit includes:

| File | Missing Include(s) |
|------|-------------------|
| `shared/src/pointer-wrapper.hpp` | `<stdexcept>` |
| `crash-consensus/src/log/log-helpers.hpp` | `<cstdint>` |
| `crash-consensus/src/log/log-config.hpp` | `<cstddef>`, `<sys/types.h>` |
| `crash-consensus/src/log/log-iterators.cpp` | `<cstdint>`, `<stdexcept>` |
| `crash-consensus/src/log/log.hpp` | `<cstdint>`, `<stdexcept>` |
| `crash-consensus/src/fixed-size-majority.hpp` | `<stdexcept>` |
| `neb/src/sync/mem-pool.hpp` | `<cstdint>`, `<stdexcept>` |
| `neb/src/async/mem-pool.hpp` | `<cstdint>`, `<stdexcept>` |

### 3c. Fix deprecated/removed APIs

- **`crypto/src/sign/sodium.cpp`** — Add `noexcept` to lambda passed as C function pointer (GCC 15 enforces noexcept matching)
- **`crash-consensus/src/consensus.cpp`** — Replace all `bind2nd(std::plus<uintptr_t>(), val)` with `[val](uintptr_t x) { return x + val; }` (3 occurrences). `bind2nd` was removed in C++17.

### 3d. Fix multiple-definition linker error

- **`crash-consensus/experiments/timers.h`** — Change `unsigned long long g_timerfreq;` to `static unsigned long long g_timerfreq;`. GCC 15 defaults to `-fno-common`, making tentative definitions an error.

## 4. RoCE (RDMA over Ethernet) Support

The original code assumes InfiniBand (LID-based addressing). RoCE requires GID-based addressing with GRH (Global Route Header).

### 4a. Device & port layer (`ctrl/src/`)

**`device.hpp`** — Add to `ResolvedPort`:
- `bool portIsRoCE()`, `union ibv_gid portGID()`, `int portGIDIndex()` accessors
- Private members: `bool is_roce`, `union ibv_gid port_gid`, `int gid_index`

**`device.cpp`** — In `bindTo()`:
- Remove the check that throws on non-InfiniBand link layer
- After binding, detect RoCE via `port_attr.link_layer == IBV_LINK_LAYER_ETHERNET`
- Query GID table (indices 0-15), prefer RoCEv2 (IPv4-mapped GIDs)

**`block.hpp` / `block.cpp`** — Add `isRoCE()`, `gid()`, `gidIndex()` delegating to `ResolvedPort`

### 4b. Connection layer (`conn/src/`)

**`rc.hpp`** — In `RemoteConnection`:
- Add `union ibv_gid gid` and `bool is_roce` fields
- Add constructor accepting GID/RoCE params
- Extend `serialize()` / `fromStr()` to include GID bytes and RoCE flag (backward compatible)

**`rc.cpp`** — In `connect()`:
- If RoCE: set `ah_attr.is_global = 1`, populate GRH (`dgid`, `sgid_index`, `hop_limit`), use `IBV_MTU_1024`
- If InfiniBand: keep original LID-based addressing with `IBV_MTU_4096`
- In `remoteInfo()`: include GID and RoCE flag

### 4c. Device selection fix (`crash-consensus/src/consensus.cpp`)

- Original code picks the **last** RDMA device. On nodes with multiple devices (e.g., node3: mlx5_0 active, mlx5_1 down), this fails.
- Changed to iterate devices and pick the **first** one with an active port.

---

## 5. Build

```bash
cd /path/to/mu

# Build components in dependency order
for pkg in shared extern memstore crypto; do
  (cd $pkg && conan create . --profile conan/profiles/gcc-release.profile)
done

cd ctrl && conan create . --profile ../conan/profiles/gcc-release.profile && cd ..
cd conn && conan create . --profile ../conan/profiles/gcc-release.profile --build=dory-ctrl && cd ..
cd crash-consensus && conan create . --profile ../conan/profiles/gcc-release.profile \
  --build=dory-ctrl --build=dory-connection && cd ..

# Build static/shared library
cd crash-consensus/libgen && bash export.sh gcc-release && cd ../..

# Build demo binaries
cd crash-consensus/demo/without_conan
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build .
```

## 6. Run (3-node consensus test)

Requires memcached running on one node (default port 9999).

```bash
# From the repo root:
bash run_test.sh
```

See `run_test.sh` for details or the `[Syntony]` section in `README.md`.
