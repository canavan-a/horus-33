# C++ Standards — capture-eye

Guiding question: **"What would Go do?"** Prefer plain data, explicit errors,
small free functions, and obvious control flow over C++ ceremony.

Target: C++23.

## Errors

- Return `std::expected<T, E>` instead of throwing. Define one project error
  type and alias it:
  ```cpp
  enum class Error { ... };
  template <typename T> using Result = std::expected<T, Error>;
  ```
- Exceptions are reserved for truly unrecoverable states (allocation failure,
  invariant violation). Don't use them for control flow.
- Never discard a `Result`. Mark returns `[[nodiscard]]`.
- Errors carry context: prefer a struct with a code + message over a bare enum
  once messages start mattering.

## Types and data

- Public `struct`s with public data. No getter/setter boilerplate around a value
  that's just a value.
- Avoid inheritance. Use it only when it's genuinely cleaner than the
  alternative (composition, `std::variant`, a function pointer / `std::function`
  member). No deep hierarchies; no protected data.
- Prefer `std::variant` + `std::visit` for closed sets of cases, concepts or
  templates for open ones.
- Make invalid states unrepresentable: `std::optional` over sentinel values,
  strong typedefs over bare `int` for units (pixels, ms, IDs).

## Ownership and memory

- Smart pointers over raw `new`/`delete`. `unique_ptr` by default; `shared_ptr`
  only when ownership is genuinely shared.
- Raw pointers and references are **non-owning views**. Passing one means "you
  don't free this."
- Prefer values and `std::span` / `std::string_view` for parameters. Never store
  a view whose owner may outlive it — document lifetimes when it's not obvious.
- No manual memory management in application code. RAII for every resource
  (files, sockets, device handles, GPU buffers).

## Control flow

- Ranges and algorithms over hand-written `for` loops.
- Lambdas for local behavior; dependency injection for swappable behavior (pass
  the dependency in, don't reach for a singleton).
- Early return over nested `if`. Keep functions short enough to read at once.

## Const and immutability

- `const` by default for locals, parameters, and methods. Mutability is the
  exception you justify.
- `constexpr` wherever the compiler will take it.
- No global mutable state. Configuration is passed in, not read from a global.

## Naming and layout

- `snake_case` for functions/variables, `PascalCase` for types, `SCREAMING_CASE`
  for constants. (Pick and keep — consistency beats the specific choice.)
- One concept per header. No `utils.h` dumping ground.
- Use namespaces; never `using namespace` at file scope in a header.
- Include what you use.

## Concurrency

- Prefer message passing over shared mutable state (Go again).
- If sharing is unavoidable, the mutex lives next to the data it guards, and the
  lock is always RAII (`std::scoped_lock`).
- No detached threads. Every thread has an owner that joins it.

## Dependencies

- **Do not vendor libraries.** No copied-in source trees, no git submodules of
  third-party code, no checked-in prebuilt binaries.
- All dependencies come from the Nix dev shell (`flake.nix`). Entering the shell
  is the only setup step; a clean checkout must build with nothing else
  installed.
- Adding a dependency means adding it to the flake, not to the source tree. If
  it isn't in nixpkgs, package it in the flake — still not vendored into the
  repo.
- The build system finds deps through the environment the shell provides
  (`pkg-config` / `find_package`). No hardcoded paths, no `FetchContent`, no
  downloads at build time.
- `flake.lock` is committed; it is the source of truth for what version you're
  building against.

## Build and tooling

- Warnings: `-Wall -Wextra -Wpedantic`, warnings-as-errors in CI.
- Sanitizers (ASan/UBSan) enabled in debug builds.
- `clang-format` and `clang-tidy` configs checked into the repo; formatting is
  not a review topic.

## Testing

- Every non-trivial function is testable without hardware — keep I/O at the
  edges, logic in pure functions.
- Tests live alongside the code they cover.

## Comments

- Comment *why*, not *what*. If the *what* needs a comment, rename things.
- Document non-obvious lifetimes, units, and invariants.
