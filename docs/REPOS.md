# x86Code private repositories

This repo (`designgears/x86code`) holds our own code; Darling and metal2vulkan are git submodules under
`external/` pointing at the forks below. All repos are **private** on GitHub account `designgears`. Each is a full-history copy of its
upstream (not a GitHub fork, since forks of public repos cannot be private). Our work is on branch
**`x86code`** (the default branch); the upstream default branch (`main`/`master`) is mirrored
unchanged. In the local checkouts, `origin` = designgears (SSH), `upstream` = original repo.

| Local path | Private repo | Upstream | Upstream base commit |
|---|---|---|---|
| `.` (this repo) | git@github.com:designgears/x86code.git (`main`) | (ours) | - |
| `external/darling` | git@github.com:designgears/darling.git | darlinghq/darling (`master`) | `60ba801d` |
| `external/darling/src/external/xnu` | git@github.com:designgears/darling-xnu.git | darlinghq/darling-xnu (`main`) | `fa29287a` |
| `external/darling/src/external/darlingserver` | git@github.com:designgears/darlingserver.git | darlinghq/darlingserver (`main`) | `89751e64` |
| `external/darling/src/external/libkqueue` | git@github.com:designgears/darling-libkqueue.git | darlinghq/darling-libkqueue (`master`) | `b0795a2e` |
| `external/darling/src/external/iokitd` | git@github.com:designgears/darling-iokitd.git | darlinghq/darling-iokitd (`master`) | `5549ac4c` |
| `external/darling/src/external/libplatform` | git@github.com:designgears/darling-libplatform.git | darlinghq/darling-libplatform (`master`) | `5a3e5b52` |
| `external/darling/src/external/libpthread` | git@github.com:designgears/darling-libpthread.git | darlinghq/darling-libpthread (`master`) | `f07f265b` |
| `external/darling/src/external/dyld` | git@github.com:designgears/darling-dyld.git | darlinghq/darling-dyld (`main`) | `63f667cf` |
| `external/darling/src/external/libunwind` | git@github.com:designgears/darling-libunwind.git | darlinghq/darling-libunwind (`main`) | `a91da1a0` |
| `external/darling/src/external/objc4` | git@github.com:designgears/darling-objc4.git | darlinghq/darling-objc4 (`master`) | `1a12df76` |
| `external/darling/src/external/sqlite` | git@github.com:designgears/darling-sqlite.git | darlinghq/darling-sqlite (`master`) | `3472e256` |
| `external/metal2vulkan` | git@github.com:designgears/metal2vulkan.git | steelbrain/metal2vulkan (`master`) | `f6a96b7d` |

metal2vulkan is dual-licensed GPL-2.0-or-later OR LGPL-3.0-or-later; LICENSE files are kept.

In the top-level darling `.gitmodules` (branch `x86code`) the nine submodules above point at the
designgears repos (SSH URLs); every other submodule URL was changed from relative `../x.git` to
the absolute `https://github.com/darlinghq/x.git`, because relative URLs would otherwise resolve
against designgears.

## Commits on `x86code`

### darling
- `674c357c` Point submodules at absolute URLs; use designgears private forks for x86Code-modified ones
- `59e024cd` mldr commpage: set kCache64 cache line size bit
- `53e5b414` Update submodules to x86Code branches
- `b6e14cd8` Update xnu: file flags, fsctl, O_SYMLINK reopen, simulator host version

### darling-xnu
- `8a27b13` Simulator host: kern.osproductversion reports macOS 26.4 to iOS simulator processes; PROC_PIDPATHINFO returns 0
- `1dcf807` openat O_SYMLINK: reopen regular files and directories with real access instead of O_PATH
- `fd1345e` fsctl/ffsctl: implement the APFS requests the iOS simulator uses (ENOTTY otherwise)
- `988c6f3` File flags: persist chflags flags in an xattr, report them in stat/getattrlist; getattrlist volume-attribute and dir/file packing fixes
- `a340593` IOSurface/memfd: add x86Code memfd syscall 599
- `1d0e910` Code signing: csops and mac_syscall emulation
- `833ad1d` Networking: PF_ROUTE/rtnetlink, socket options, ioctls, EVFILT_SOCK state (ports willi/darling-xnu#7, #8)
- `e95ba1c` Workqueue, ulock/psynch and thread reuse; TSD, sim-compat and signal fixes (ports willi/darling-xnu#26)
- `01198bc` kevent: workloops, cooperative pool and kevent_qos support
- `0324091` Mach memory entries and VM traps (mmap/madvise)
- `96da44b` File system: getattrlist, statfs, fsgetpath, fcntl locks, xattrs, guarded fds
- `e3f6bab` Syscalls and sysctls: misc additions and build wiring

### darlingserver
- `85861be` Semaphores: reference-count dtape semaphores (ports willi/darlingserver#6)
- `d6d86a1` Mach memory entries and VM map support
- `7361305` kqchan: kevent channel fixes for workloops and cooperative pool
- `da5b066` Exceptions and host ports: mach_exc, ux_handler, host/task special ports
- `843c107` Threads, S2C, timers and IPC: thread lifetime/reuse, dying threads, timer calls (ports willi/darlingserver#10)

### darling-libkqueue
- `42ba5e2` EVFILT_SOCK: socket filter driven by libsystem_kernel connect/shutdown hooks (ports willi/darling-libkqueue#5)
- `722099d` Machport/vnode filters, knote locking and sim exports for kevent workloops

### Others (one commit each)
- darling-iokitd `6484b92` IOKit: IOProviderClass matching, IOServiceGetMatchingService, EGL default display for IOSurface
- darling-libplatform `8c8cab3` Export string/memory functions (memset etc.) for the iOS simulator runtime
- darling-libpthread `b53853c` Cooperative thread pool support for Swift concurrency; sim-compat exports
- darling-dyld `c98ef6c` Set ALLOW_LIBRARY_INTERPOSING so newer dyld_sim keeps __interpose tuples
- darling-libunwind `d4b2749` Disable relative lookup tables (clang 23) that bundled ld64 cannot link
- darling-objc4 `9927f0a` Do not pass -DNDEBUG: objc4 always defines OBJC_IS_DEBUG_BUILD=1
- darling-sqlite `999dc2c` sqlite3_serialize (SQLITE_ENABLE_DESERIALIZE) and sqlite3_exec_b for update_dyld_sim_shared_cache
- metal2vulkan `a980409` Translator fixes: global bindings, front_facing bool builtin, no uvec4 units for 16-byte leaves (tests with changed goldens marked `#[ignore]`)

## Clone

```sh
git clone git@github.com:designgears/x86code.git && cd x86code
make submodules     # external/darling (recursive, x86code branches) and external/metal2vulkan
# our Darling submodules are checked out detached at the x86code commits; to work on them:
for d in xnu darlingserver libkqueue iokitd libplatform libpthread dyld libunwind objc4; do
  git -C external/darling/src/external/$d checkout x86code
done
git -C external/darling checkout x86code; git -C external/metal2vulkan checkout x86code
```

## Committing further work

1. In the submodule: commit on `x86code`, `git push origin x86code`.
2. In `external/darling`: `git add src/external/<name> && git commit && git push origin x86code`.
3. In this repo: `git add external/darling` (or `external/metal2vulkan`), commit, push `main`.

After `.gitmodules` changes in an existing checkout run `git submodule sync`.

## Pulling upstream into x86code

```sh
cd <repo>
git remote add upstream https://github.com/darlinghq/<name>.git   # if missing
git fetch upstream
git checkout x86code
git merge upstream/<main|master>        # or rebase, if history is still private-only
git push origin x86code
git push origin upstream/<main|master>:refs/heads/<main|master>   # keep the mirror branch current
```
For the top-level darling, after merging upstream re-check `.gitmodules` (new upstream submodules
come in with relative URLs and must be made absolute) and our submodule pointers.

Pushing needs SSH: the gh OAuth token lacks the `workflow` scope, so HTTPS pushes of upstream
history containing `.github/workflows` are rejected.
