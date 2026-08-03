# STREAM + jemalloc (NUMA-aware)

This is the same STREAM C++ benchmark as [`../stream`](../stream) (Copy,
Scale, Add, Triad, decoupled C kernels compiled with OpenMP, C++ driver), but
the `a`/`b`/`c` STREAM arrays are allocated through
[jemalloc](https://jemalloc.net/) (built from the bundled `jemalloc` git
submodule) instead of the platform's default `malloc`/`new`.

**jemalloc itself is used completely unmodified.** No jemalloc source file is
patched, and `stream.cpp` does not call any jemalloc-specific allocation
function either -- `a`, `b`, and `c` are still plain
`std::vector<double>`. Instead:

* jemalloc is linked directly into the binary (see "How this works" below),
  which makes jemalloc's `malloc`/`free`/`calloc`/`realloc`/`posix_memalign`
  and C++ `operator new`/`operator delete` the process-wide allocator,
  transparently including the allocations `std::vector` makes internally.
* jemalloc's own, documented tuning knobs are embedded as its default
  configuration at build time (via `--with-malloc-conf`, see
  [`jemalloc/TUNING.md`](jemalloc/TUNING.md)). No environment variables or
  code changes are required to get NUMA-aware behavior -- **you just build
  and run the binary**, exactly like every other variant in this repository:

  ```sh
  make -f makefiles/Makefile-hw
  ./stream.hw 10000000
  ```

## How this works

### Linking jemalloc without touching its source

jemalloc's public API is unprefixed by default on Linux (see
[`jemalloc/INSTALL.md`](jemalloc/INSTALL.md), `--with-jemalloc-prefix`): a
default `./configure && make` build produces a `libjemalloc.a`/`.so` whose
exported symbols are literally named `malloc`, `free`, `calloc`, `realloc`,
`posix_memalign`, `operator new`, `operator delete`, etc. -- the same names
glibc uses.

The Makefiles in this directory build jemalloc out-of-tree (into
`jemalloc-build.<suffix>/`, never inside the `jemalloc/` submodule checkout
itself) and link `libjemalloc.a` directly into the `stream` binary, ahead of
libc on the link line. Because the executable defines these symbols itself,
they take priority over glibc's own definitions for every allocation in the
process -- including the ones `std::vector<double>` performs internally in
`stream.cpp`. This is the standard, documented way to embed jemalloc into a
program without `LD_PRELOAD` and without changing a single line of
application code.

You can confirm this once the binary is built:

```sh
$ nm -D stream.hw | grep -w malloc
000000000001b750 T malloc          # defined (T) inside the binary itself, not undefined (U)
```

### How NUMA fits in

The system this is meant to run on has NUMA nodes that expose both "fast"
(e.g. local DDR) and "slow" (e.g. CXL-attached or far-socket) memory tiers.
Getting good behavior on such a system is a *layered* problem:

1. **The kernel** decides, per page, which physical NUMA node backs it, and
   (on kernels with
   [NUMA balancing](https://www.kernel.org/doc/html/latest/admin-guide/sysctl/kernel.html#numa-balancing)
   and [memory tiering](https://www.kernel.org/doc/html/latest/mm/multigen_lru.html)
   support, i.e. Linux 5.15+) automatically **migrates pages between tiers**
   over time: hot pages get promoted toward the fast tier, cold pages get
   demoted toward the slow tier when the fast tier is under pressure. This
   machinery works on ordinary anonymous/mmap'd memory and doesn't care which
   userspace allocator requested it -- as long as nothing in userspace
   defeats it (e.g. by hard-pinning memory to one specific node with `mbind`
   or `numactl --membind`).
2. **jemalloc's job** is to not get in the kernel's way, and to make its own
   *first-touch* placement decisions be NUMA-friendly to begin with, so the
   kernel has less remedial work to do. That's exactly what the embedded
   `malloc_conf` in this variant's Makefiles does:

   | Option | Effect |
   |---|---|
   | `percpu_arena:percpu` | Dynamically associates each jemalloc arena with the CPU currently running it (see [`jemalloc/TUNING.md`](jemalloc/TUNING.md), `opt.percpu_arena`). Combined with Linux's default "local" NUMA memory policy, this means an arena's *first-touch* allocations land in memory local to whichever CPU (and therefore whichever NUMA node) is actually using them, instead of being pinned to whatever node the allocating thread happened to start on. |
   | `background_thread:true` | Lets jemalloc's own background threads purge/decay unused memory asynchronously, instead of doing it synchronously on the allocation hot path. |
   | `metadata_thp:auto` | Lets jemalloc use transparent huge pages for its own metadata when it's profitable, reducing TLB pressure for large allocations like the STREAM arrays. |

   jemalloc never calls `mbind()`/`set_mempolicy()` to pin memory to a
   specific node, and it never disables NUMA balancing -- so nothing it does
   can block the kernel from migrating STREAM's array pages between the fast
   and slow tiers as their access pattern (and therefore "hotness") changes.

Put together: jemalloc auto-detects and follows the CPU/NUMA topology at the
*allocation* level (`percpu_arena`), and the kernel's own NUMA
balancing/tiering handles the *migration* level. This is why no jemalloc
source changes, and no `stream.cpp` allocator-selection logic, are needed:
running the binary is enough for jemalloc to "automatically pick up the NUMA
configuration" and let pages move between fast/slow nodes as usual.

### Runtime diagnostics

At start-up, `stream.cpp` prints a short, best-effort summary (via
jemalloc's public [`mallctl(3)`](https://jemalloc.net/jemalloc.3.html#mallctl_namespace)
introspection API and libnuma) so you can confirm on real hardware that
everything is wired up as expected, e.g.:

```
NUMA: 4 node(s) visible to this process
NUMA: allowed memory nodes: 0 1 2 3
jemalloc opt.percpu_arena: percpu
jemalloc opt.metadata_thp: auto
jemalloc opt.background_thread: true
jemalloc opt.narenas: 8
jemalloc arenas.narenas: 8
```

This is purely diagnostic (read-only `mallctl`/libnuma calls); it does not
change how memory is allocated.

## Usage

```sh
<binary_name> <number_of_elements>
```

E.g.,

```sh
make -f makefiles/Makefile-hw
./stream.hw 10000000
```

The first `make` invocation will also bootstrap and build jemalloc itself
(running `jemalloc/autogen.sh` once to generate `configure`, then
`configure && make` out-of-tree); this only needs network/tooling available
locally (autoconf/automake/libtool), no external dependencies are fetched
over the network.

## Compilation

### Using Native Hardware

```sh
make -f makefiles/Makefile-hw
# output: stream.hw
```

This is the variant meant to be run on the real NUMA (fast/slow memory)
system. `WITH_LIBNUMA` defaults to `1` here (see "Build-time knobs" below).

### Compilations for Specific Architectures

The following Makefiles are mostly identical to `Makefile-hw`, except the
default `CC`/`CXX` are cross compilers, `CFLAGS_KERNEL` carries
architecture-specific `-march` flags, and jemalloc's own `configure` is
additionally passed `--host=<triple>` so that jemalloc itself is
cross-compiled for the target instead of the build machine:

```sh
make -f makefiles/Makefile-rv64gc
# output: stream.rv64gc
make -f makefiles/Makefile-rvv
# output: stream.rvv
make -f makefiles/Makefile-arm
# output: stream.arm
make -f makefiles/Makefile-armsve
# output: stream.armsve
```

`WITH_LIBNUMA` defaults to `0` for these cross variants, since typical cross
sysroots do not ship `libnuma-dev` for the target; pass `WITH_LIBNUMA=1` if
your sysroot does have it (this only affects the diagnostic printout, not
jemalloc's own NUMA behavior, which relies solely on `percpu_arena` and the
kernel, not on libnuma).

### Compiling with gem5 ROI Annotations

This requires setting those two environmental variables: `M5_BUILD_PATH` and
`M5OPS_HEADER_PATH`, where `M5_BUILD_PATH` is the path to the m5 build for
the corresponding ISA (see the example below), and `M5OPS_HEADER_PATH` is
the path to the `m5ops.h` header file (also see the example below).

The following is an example of compiling an arm64 binary with gem5 ROI
annotations on an x86\_64 machine,

```sh
# prerequisite steps
cd <gem5_dir>/util/m5
scons arm64.CROSS_COMPILE=aarch64-linux-gnu- build/arm64/out/m5
# stream compilation steps
cd <stream_dir>
make -f makefiles/Makefile-arm M5OPS_HEADER_PATH=<gem5_dir>/include/ M5_BUILD_PATH=<gem5_dir>/util/m5/build/arm64/
```

### Build-time knobs

All of the following can be overridden on the `make` command line, e.g.
`make -f makefiles/Makefile-hw JEMALLOC_MALLOC_CONF=narenas:1`:

| Variable | Default (native) | Meaning |
|---|---|---|
| `JEMALLOC_MALLOC_CONF` | `percpu_arena:percpu,background_thread:true,metadata_thp:auto` | Embedded as jemalloc's default `malloc_conf` via `--with-malloc-conf`. See [`jemalloc/TUNING.md`](jemalloc/TUNING.md) for other knobs (e.g. `dirty_decay_ms`, `narenas`). |
| `JEMALLOC_HOST` | *(unset, native only)* | Autotools `--host` triple, only present in the cross-compilation Makefiles. |
| `WITH_LIBNUMA` | `1` on `Makefile-hw`/`Makefile-hw+sve`, `0` on cross variants | Whether to compile in the libnuma-based topology printout in `stream.cpp` and link `-lnuma`. |
| `JEMALLOC_CONFIGURE_FLAGS` | empty (native) / `--host=...` (cross) | Extra flags forwarded to jemalloc's `./configure`. |

Since `malloc_conf` is only a *default* (see
[`jemalloc/INSTALL.md`](jemalloc/INSTALL.md), `--with-malloc-conf`), it can
still be overridden without rebuilding, at run time, via the `MALLOC_CONF`
environment variable, e.g.:

```sh
MALLOC_CONF="percpu_arena:percpu,narenas:4" ./stream.hw 10000000
```

## Verifying NUMA behavior on real hardware

None of this can be meaningfully exercised on a machine without multiple
NUMA nodes, so it is intentionally not exercised as part of building this
repository. On the target system, useful things to check:

* `numactl --hardware` -- confirms the fast/slow node layout the kernel sees.
* `cat /sys/kernel/mm/numa/demotion_enabled` -- whether kernel memory-tiering
  demotion (moving cold pages from a fast/top-tier node to a slower one) is
  enabled. `echo 1 | sudo tee /sys/kernel/mm/numa/demotion_enabled` to turn
  it on if it's off.
* `sysctl kernel.numa_balancing` -- whether automatic NUMA balancing (which
  also drives promotion of hot pages back toward faster nodes) is enabled.
* `numastat -p $(pidof stream.hw)` or `cat /proc/<pid>/numa_maps` while the
  benchmark is running (e.g. run it with a very large array size / add a
  sleep) -- shows which nodes the STREAM arrays' pages actually landed on,
  and (across repeated samples) whether they migrate over time.
* The `mallctl`-based printout described above -- confirms jemalloc actually
  picked up `percpu_arena`/`background_thread` as configured.

## Troubleshooting

* **`./configure: not found` / autogen.sh fails**: make sure `autoconf`,
  `automake`, and `libtool` are installed; jemalloc ships without a
  pre-generated `configure` script and `autogen.sh` bootstraps it.
* **Link errors about `pthread_*`/`dlopen`/`sqrt` when linking
  `libjemalloc.a`**: these come from jemalloc itself (threading, and, on
  some libc/toolchain versions, `dl`/`m`); the Makefiles already add
  `-lpthread -ldl -lm` after the archive on the link line, which is required
  for static linking to resolve them.
* **`malloc_conf` looks ignored**: `--with-malloc-conf` only sets the
  *default*; both `/etc/malloc.conf` (if it exists as a symlink) and the
  `MALLOC_CONF` environment variable are applied afterward and can override
  it. Also double-check with the `opt.*` printout at start-up.
