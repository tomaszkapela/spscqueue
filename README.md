# SPSC queue

A bounded, nonblocking, lock-free, header-only C++20 queue for exactly one
producer thread and one consumer thread: `spsc::SpscQueue<T, Layout>`, in
`<spsc/spsc_queue.hpp>`.

The implementation uses only the standard library. GoogleTest is used by the
tests.

```cpp
#include <spsc/spsc_queue.hpp>

spsc::SpscQueue<int> queue(3);              // Exactly three usable places.
bool inserted = queue.try_emplace(42);      // false when full
std::optional<int> value = queue.try_pop(); // empty when the queue is empty
```

## Build

Requires CMake 3.20 or newer, and a C++20 compiler whose library provides
`std::hardware_destructive_interference_size`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

CMake options:

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | `OFF` builds no tests and needs no GoogleTest. |
| `SPSC_WARNINGS_AS_ERRORS` | `ON` | Adds `-Werror`; test targets always use `-Wall -Wextra -Wpedantic`. |
| `SPSC_SANITIZER` | `none` | `address` (ASan + UBSan, fail fast) or `thread` (TSan). |
| `SPSC_CLANG_TIDY` | `OFF` | Runs clang-tidy on the tests while building; see [Static analysis](#static-analysis). |

CMake uses an installed GoogleTest package when it finds one. Otherwise it
downloads the pinned 1.14.0 release.

### Use from another CMake project

Fetch the repository at configure time and link `spsc::queue`:

```cmake
include(FetchContent)
FetchContent_Declare(spscqueue
  GIT_REPOSITORY https://github.com/tomaszkapela/spscqueue.git
  GIT_TAG main) # or a commit hash, for a reproducible build
FetchContent_MakeAvailable(spscqueue)
target_link_libraries(app PRIVATE spsc::queue)
```

Tests, the sanitizer and clang-tidy options, and the format targets are defined
only when this is the top-level project, so a consumer gets only `spsc::queue`.

## Run the tests

```sh
ctest --test-dir build --output-on-failure
```

The suite contains:

- `spsc_core_tests`: single-threaded contract, element-lifetime, exception and
  batch tests, seeded random operations checked against `std::deque`, plus
  compile-time checks (`tests/core_compile_time_test.cpp`) of the
  `queue_element` concept and of the operations' constraints and exception
  specifications.
- `spsc_core_concurrency_tests`: two-thread transfers of sequence-numbered
  packets, one at a time and in batches, at capacity one and irregular
  capacities, for both slot layouts.

Tests compile as C++20 without extensions. A CTest timeout turns a hang into a
failure.

Each sanitizer needs its own build tree; for TSan, use `thread` in place of
`address`:

```sh
cmake -S . -B build-asan -DSPSC_SANITIZER=address -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

With Valgrind installed, an uninstrumented build can also run under Memcheck,
which adds detection of uninitialized reads:

```sh
ctest --test-dir build -T memcheck
```

Valgrind's race detectors (Helgrind, DRD) don't model C++ atomics and report
false races on this queue; use TSan for races.

## Formatting

`.clang-format` defines the style. When clang-format is on `PATH`, or given with
`-DCLANG_FORMAT=/path/to/clang-format`, top-level builds provide two targets:

```sh
cmake --build build --target format        # rewrite sources in place
cmake --build build --target format-check  # fail on any deviation
```

Output can differ between releases: the sources were formatted with clang-format
23.1, and 21.1.8, which CI installs, leaves them unchanged. To check staged C++
changes on every commit, enable the bundled hook once per clone:

```sh
git config core.hooksPath .githooks
```

## Static analysis

With `-DSPSC_CLANG_TIDY=ON`, clang-tidy runs on the tests as they compile, and
so on the header they include:

```sh
cmake -S . -B build-tidy -DSPSC_CLANG_TIDY=ON
cmake --build build-tidy -j
```

With `SPSC_WARNINGS_AS_ERRORS` on (the default), any finding fails the build.
Deliberate exceptions carry a `NOLINT` comment with the reason. Pass
`-DCLANG_TIDY=/path/to/clang-tidy` to pin a binary.

## Assumptions and known limitations

**Roles.** Exactly one producer thread calls the insertion operations and
exactly one consumer thread calls the removal operations. A role may move to
another thread only through external synchronization. Recursive calls on the
same side, for example from an element constructor, are unsupported. Construct
the queue before workers start, and destroy it only after every call has
returned. The queue cannot be copied or moved.

**Capacity.** Capacity is fixed at construction and equals the number of usable
places. Zero throws `std::invalid_argument`. Storage holds N + 1 slots. A slot
array larger than `PTRDIFF_MAX` bytes throws `std::length_error` before
allocation. Allocation failure throws `std::bad_alloc`. The queue allocates only
in its constructor; element operations may allocate.

**Element types.** `T` must satisfy `spsc::queue_element`: a cv-unqualified,
non-array object type with nonthrowing destruction and
`std::is_nothrow_move_constructible_v<T>`. It needs no default constructor or
assignment. Copy-only types qualify when their copy constructor is nonthrowing
and they declare no move constructor, since rvalues then bind to the copy. Types
with a throwing or deleted move cannot be queued; queue a `std::unique_ptr<T>`
instead.

**Exceptions.**
- If an element constructor throws in `try_emplace` or `try_push`, nothing is
  published and the queue is unchanged.
- A rejected insertion constructs nothing and does not move from its argument.
  Argument expressions are still evaluated by the caller before the call.
- `try_pop`, `try_push_range` and `consume` are `noexcept`. They accept only
  what can't throw: the element's move and destructor, the batch's element
  construction, and the visitor.

**Progress.** Operations never wait, retry, or lock. The queue is lock-free
wherever `std::atomic<std::size_t>` is, which includes x86-64 and aarch64; the
tests check this at compile time. The header doesn't require it: where atomics
use a lock, the queue stays correct, just not lock-free. Element constructors
and destructors run inside queue operations, so they can block or allocate, and
no wait-free or real-time guarantee is claimed. A caller that polls an empty or
full queue should back off (for example yield) rather than spin on it.

**Cache-line size.** Cache separation uses the compiler's
`std::hardware_destructive_interference_size` as-is. It is a compile-time
constant, not a runtime probe of the CPU, and compilers choose it conservatively
(GCC and Clang both report 256 on aarch64, where lines are usually 64 bytes).
Because the value depends on `-mtune`/`-mcpu`, GCC warns about its use in
headers; the `spsc::queue` CMake target passes `-Wno-interference-size` on GCC,
and every translation unit sharing a queue type must use the same tuning flags.
To pin the value, add `--param destructive-interference-size=64`.

**Evidence.** The tests and sanitizer runs support the memory-order reasoning
below but do not prove it for every weak-memory execution. They were run on
aarch64, whose memory model allows reorderings that x86 never performs.

## API

### `SpscQueue<T, Layout = SlotLayout::compact>`

| Operation | Role | Result |
| --- | --- | --- |
| `SpscQueue(capacity)` | before workers start | Allocates N + 1 slots. |
| `capacity()` | any | Usable places. |
| `try_emplace(args...)` | producer | `bool`: `false` when observed full. |
| `try_push(const T&)`, `try_push(T&&)` | producer | `bool`: `false` when observed full. |
| `try_push_range(r)` | producer | Pushes the elements of the range `r` until full, published together. Returns an iterator to the first element not pushed: the end of `r` when all went in. Requires nonthrowing construction, and range and iterator operations that don't throw. |
| `available_to_push()` | producer | At least this many pushes will succeed. |
| `try_pop()` | consumer | `std::optional<T>`: empty when observed empty. |
| `consume(f, max)` | consumer | Calls `f(T&&)` on up to `max` elements in their slots, frees them together, and returns how many. Requires a nonthrowing `f`. |
| `available_to_pop()` | consumer | At least this many pops will succeed. |

`Layout` is `SlotLayout::compact` (slots at the element's natural alignment) or
`SlotLayout::cache_isolated` (each slot on its own cache line); see [Memory
layout](#design-notes).

A `false` or empty result describes what the caller observed, not a lasting
state: the other side may change it immediately. That's also why there's no
`size()`. Instead, each side gets a count that is exact or too small, never too
large, because the other side can only add free places (for the producer) or
elements (for the consumer). Called from any other thread, the counts are
estimates between 0 and `capacity()`. There's also no front pointer, which would
let the caller hold a reference into a slot the producer reuses. `consume` hands
out a reference that's valid only during the call to `f`.

### Batch example

`try_push_range` and `consume` move many elements per call and publish them with
one index store; [Design notes](#design-notes) explains why both require that
nothing throws. Here the producer moves a burst of orders in, and the consumer
moves out everything that's ready:

```cpp
// Producer thread: pushes until full; rest is the first order not pushed.
std::vector<Order> orders = next_burst();
const auto rest = queue.try_push_range(std::ranges::subrange(
    std::make_move_iterator(orders.begin()), std::make_move_iterator(orders.end())));
// In C++23: queue.try_push_range(orders | std::views::as_rvalue)

// Consumer thread: reserving first keeps push_back from reallocating, which
// could throw, inside the nonthrowing visitor.
const std::size_t ready = queue.available_to_pop();
batch.reserve(batch.size() + ready);
queue.consume([&](Order&& order) noexcept { batch.push_back(std::move(order)); }, ready);
```

## Design notes

**Two publication handoffs.** The producer constructs the element, then
publishes the write index with release. The consumer acquires that index before
it touches the slot. In the other direction, the consumer destroys the element,
then publishes the read index with release. The producer acquires it before
reusing the slot. The spare (N + 1th) slot distinguishes full from empty and
prevents either side from lapping the other. Each side is the only writer of its
own index, so it reads that index with a relaxed load. No sequentially
consistent operation is needed, because neither side ever has to order its own
store with a load of the other side's index.

**Cached indices.** Each side keeps a conservative copy of the other side's
index and rereads the shared atomic only when the copy says full or empty. A
stale copy can only understate the available space or data, never overstate it,
because the other side's index only moves forward. So in the usual case an
operation doesn't read the other side's index at all, and that cache line stays
with its owner.

**Memory layout.** Producer state, consumer state, and immutable metadata are
each separated by the destructive-interference size in both layouts. The
metadata is read by both sides but never written after construction, so it is
shared without coherence traffic. `compact` slots use the element's natural
alignment. `cache_isolated` slots are aligned and padded to the interference
size, so adjacent elements never share a line. That costs memory and locality,
so it is not always faster. Both layouts keep any stronger alignment the element
type requires.

**Element lifetime.** Slots are raw `std::byte` storage, so the queue needs no
default constructor and constructs nothing in empty slots. Elements start their
lifetime with `std::construct_at` and end it with `std::destroy_at`. The
consumer reaches a live element through `std::launder`, and the queue's
destructor destroys any elements still queued.

**Nonthrowing extraction.** `try_pop()` move-constructs the result, destroys the
slot element, then publishes the freed space. Requiring a nonthrowing move keeps
this a straight line. Throwing copies could be supported with a strong
guarantee: if copying the element out throws, the consumer doesn't publish, and
the element stays queued. But the slot may be freed only after the value has
reached the caller, so the return would have to build it straight in the
caller's storage, with the commit in a guard that runs afterwards. That was
judged not worth it for rare element types.

**Batches.** `try_push_range` and `consume` publish once per call: one release
store for a whole batch. The cached indices can't do that on their own: they
save loads of the other side's index, not stores of one's own. As in single
operations, a batch reloads the other side's index only when its cached copy
runs out, which a long batch can do more than once. Both operations require that
nothing can throw:
- If construction could throw halfway through a batch, the queue would have to
  either publish part of the batch and throw, so the caller can't learn how many
  went in, or destroy the constructed elements, losing values moved in from the
  source. The same goes for the iterator's own operations, but this can't be
  checked, because few iterators declare them `noexcept`: not even
  `std::move_iterator`. Since `try_push_range` is `noexcept`, an exception from
  one terminates the program.
- If `f` could throw, the queue couldn't know whether `f` had already moved the
  value out. Keeping the element could deliver a moved-from value again;
  dropping it could lose the value.

Requiring `noexcept` removes the question, just as a nonthrowing move does for
`try_pop()`. `try_push_range` returns where it stopped rather than a count, so
the caller can continue from there. For a temporary range it returns
`std::ranges::dangling` instead, unless the range is borrowed, like `std::span`
or `std::ranges::subrange`, whose iterators don't point into the range object
itself. Returning the position also makes input ranges safe: when a
`std::ranges::istream_view` reads one value too many, the returned iterator
still holds it.

## License

MIT; see [LICENSE](LICENSE).
