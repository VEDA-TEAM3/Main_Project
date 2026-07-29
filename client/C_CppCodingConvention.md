# C/C++ Coding Convention

## Formatting

All C/C++ source and header files are formatted by `.clang-format`.

- Indentation: 4 spaces
- Column limit: 120
- Brace style: K&R variant, opening braces stay on the control statement line
- Pointer/reference alignment: type-side, such as `int* value` and `const Type& ref`
- Header extensions: `.h` only
- Source extensions: `.c` and `.cpp` only
- Header guard: `#pragma once` only
- Include order: C system headers, C++ system headers, external/project headers

## Naming

The active `.clang-tidy` configuration enforces the C++ naming rules used by this project.

- Variables: `camelBack`
- Functions: `camelBack`
- Classes: `CamelCase`
- Structs: `CamelCase`
- Macros: `UPPER_CASE`
- Private/protected members: `camelBack_`

C files must use the C naming rules during review:

- Variables/functions: `snake_case`
- Structs: `Capitalized_snake_case`
- Macros: `UPPER_CASE_WITH_UNDERSCORES`

## Constants And Macros

- C++ constants must use `constexpr` or `const`.
- C++ `#define` constants are prohibited.
- C may use `#define` only when there is a clear C-specific reason.

## Comments

Use Doxygen comments for classes and public functions. Avoid comments that only repeat the code.

## Memory

C++ code must not directly call `delete`, `malloc`, or `free`.

- Prefer `std::shared_ptr` for dynamic ownership.
- Use `std::unique_ptr` only when exclusive ownership is specifically justified.
- Qt `QObject` and `QWidget` instances may use `new` only when a parent is passed at construction and the Qt object tree
  clearly owns the instance. This follows Qt's documented parent-child lifetime model.
- Parentless C++ objects must be wrapped by standard smart pointers.

## C Error Handling

C functions returning operation status should follow Linux-kernel-style return values:

- Success: `0`
- Failure: negative error code, such as `-EFAULT`
- Boolean-style functions named `is_*` or `has_*`: true is `1`, false is `0`

This rule must be checked during code review because linters cannot enforce it completely.
