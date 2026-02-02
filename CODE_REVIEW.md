# Code Review Report: nix-scheduler-hook

## Overview

This document presents a comprehensive code review of the **nix-scheduler-hook** project - a build hook for Nix that dispatches builds through job schedulers (Slurm, Slurm-native, and PBS).

**Review Date:** February 2, 2026  
**Codebase Version:** 0.5.0

---

## Executive Summary

The codebase is generally well-structured and functional, implementing support for three job schedulers. The code demonstrates good use of modern C++ features (C++23) and integrates well with the Nix ecosystem. However, there are several areas that could benefit from improvement in terms of code quality, safety, maintainability, and documentation.

---

## Findings

### 1. Code Quality Issues

#### 1.1 Missing Header Guards Consistency
**Severity:** Low  
**Location:** `src/logging.hh`, `src/sched_util.hh`

The files `logging.hh` and `sched_util.hh` lack the `#pragma once` directive that is used in other header files, though they are included in ways that may prevent multiple inclusion issues.

**Recommendation:** Add `#pragma once` to all header files for consistency.

#### 1.2 Static Variables in Functions
**Severity:** Medium  
**Location:** `src/slurm.cpp:25-40`

```cpp
static std::shared_ptr<RestClient::Connection> getConn()
{
    static bool init = false;
    static std::shared_ptr<RestClient::Connection> conn;
    // ...
}
```

Using static local variables for connection management can lead to issues in multi-threaded contexts and makes testing more difficult.

**Recommendation:** Consider using a connection manager class or passing connections as parameters.

#### 1.3 Static Variable in Scheduler Base Class
**Severity:** Medium  
**Location:** `src/scheduler.hh:56-57`

```cpp
static auto ssh = sshStoreConfig->createSSHMaster(false);
sshMaster = &ssh;
```

The static variable `ssh` will persist across multiple scheduler instances, which could lead to unexpected behavior if multiple schedulers are created.

**Recommendation:** Redesign to avoid static variables or use proper lifetime management.

#### 1.4 Unused Variable Warning
**Severity:** Low  
**Location:** `src/logging.hh:15`

```cpp
static std::string currentHookLine;
```

The variable `currentHookLine` is declared but never used.

**Recommendation:** Remove unused variable.

---

### 2. Error Handling

#### 2.1 Silent Exception Swallowing
**Severity:** Medium  
**Location:** `src/settings.cpp:24-25`

```cpp
} catch (nix::SystemError &) {
}
```

Exceptions are caught and silently ignored when reading configuration files.

**Recommendation:** At minimum, log a debug message when configuration files cannot be read.

#### 2.2 Inconsistent Error Handling in Destructors
**Severity:** Medium  
**Location:** `src/scheduler.hh:23-35`, `src/slurm.cpp:179-189`, `src/slurm-native.cpp:145-155`

Destructors catch exceptions and print error messages but have inconsistent patterns. Some check for empty job IDs, others don't.

**Recommendation:** Standardize error handling in destructors across all scheduler implementations.

---

### 3. Memory and Resource Safety

#### 3.1 Raw Pointer Usage in PBS
**Severity:** Medium  
**Location:** `src/pbs.cpp:46-62`

```cpp
static struct attropl *new_attropl()
{
    return new attropl{nullptr, nullptr, nullptr, nullptr, SET};
}
```

Manual memory management with raw pointers and custom free functions is error-prone.

**Recommendation:** Consider using smart pointers with custom deleters:
```cpp
using attropl_ptr = std::unique_ptr<attropl, decltype(&free_attropl)>;
```

#### 3.2 Potential Resource Leak
**Severity:** Low  
**Location:** `src/pbs.cpp:83-92`

If `genScript()` throws after `mkstemp()` creates the file but before `createdScript` is set to true, the temporary file will not be cleaned up.

**Recommendation:** Use RAII wrapper for the temporary file.

#### 3.3 Fixed-Size Buffer
**Severity:** Low  
**Location:** `src/pbs.hh:40`

```cpp
char scriptName[MAXPATHLEN + 1];
```

Using a fixed-size buffer for paths can be fragile.

**Recommendation:** Consider using `std::string` or `std::filesystem::path`.

---

### 4. Thread Safety Concerns

#### 4.1 Non-Atomic Access to Shared State
**Severity:** Medium  
**Location:** `src/main.cpp:40`

```cpp
static std::string currentLoad;
```

`currentLoad` is a static string that could be accessed from multiple threads, but string operations are not atomic.

**Recommendation:** Either protect with mutex or use thread-local storage if appropriate.

#### 4.2 Scheduler State Thread Safety
**Severity:** Low  
**Location:** `src/scheduler.hh:91-101`

While `cmdOutInit` and `submitCalled` are atomic, other member variables are accessed without synchronization.

**Recommendation:** Document thread safety guarantees or add necessary synchronization.

---

### 5. API and Design Issues

#### 5.1 Hardcoded API Version
**Severity:** Medium  
**Location:** `src/slurm.cpp:75, 92, 122, 141, 183`

The Slurm API version `v0.0.43` is hardcoded in multiple places.

**Recommendation:** Define the API version as a constant or configuration option:
```cpp
constexpr std::string_view SLURM_API_VERSION = "v0.0.43";
```

#### 5.2 Magic Numbers
**Severity:** Low  
**Location:** Throughout the codebase

Several magic numbers are used without explanation:
- `src/main.cpp:323`: `15 * 60` (upload lock timeout)
- `src/slurm.cpp:108`: `1s`, `2s`, `4s` (backoff limits)

**Recommendation:** Define named constants for these values.

---

### 6. Documentation

#### 6.1 Missing Function Documentation
**Severity:** Low  
**Location:** Throughout the codebase

Most functions lack documentation comments explaining their purpose, parameters, and return values.

**Recommendation:** Add documentation comments, especially for public interfaces:
```cpp
/**
 * @brief Submits a derivation for building
 * @param drvPath Path to the derivation to build
 * @throws SlurmAPIError if submission fails
 */
void submit(nix::StorePath drvPath) override;
```

#### 6.2 Missing Class Documentation
**Severity:** Low  
**Location:** `src/scheduler.hh`

The `Scheduler` base class and its interface could benefit from more comprehensive documentation.

---

### 7. Configuration and Settings

#### 7.1 Inconsistent Setting Description
**Severity:** Low  
**Location:** `src/settings.hh:15-20`

The job-scheduler setting description mentions only 'slurm' and 'pbs' but 'slurm-native' is also available.

```cpp
nix::Setting <std::string> jobScheduler {
    this,
    "slurm",
    "job-scheduler",
    "Which job scheduler to use, available choices are 'slurm' and 'pbs'."
};
```

**Recommendation:** Update description to include 'slurm-native'.

---

### 8. Build System

#### 8.1 Typo in Meson Build File
**Severity:** Low  
**Location:** `src/meson.build:9`

```meson
rectclient_proj = cmake.subproject('restclient-cpp')
```

The variable name `rectclient_proj` appears to be a typo (should be `restclient_proj`).

**Recommendation:** Fix the typo for consistency.

---

### 9. Security Considerations

#### 9.1 JWT Token in Configuration
**Severity:** Medium  
**Location:** `src/settings.hh:85-90`

JWT tokens stored in configuration files may have insufficient protection. The code does not validate or warn about file permissions.

**Recommendation:** Add documentation about secure storage of JWT tokens and consider validating configuration file permissions.

#### 9.2 Temporary File Security
**Severity:** Low  
**Location:** `src/pbs.cpp:83-85`

The temporary script file is created with `mkstemp()` which is secure, but the directory location uses `std::filesystem::temp_directory_path()` which may be shared.

**Recommendation:** Consider using `slurm-state-dir` equivalent for PBS or document security implications.

---

### 10. Code Style

#### 10.1 Inconsistent Namespace Usage
**Severity:** Low  
**Location:** Throughout the codebase

Some files use `using namespace nlohmann;` or `using namespace std::chrono_literals;` at file scope, while others use qualified names.

**Recommendation:** Establish consistent guidelines for namespace usage.

#### 10.2 GNU Extension Usage
**Severity:** Low  
**Location:** `src/main.cpp:6`, `src/scheduler.hh:6`, `src/pbs.cpp:7`

```cpp
#include <ext/stdio_filebuf.h>
```

The code uses GNU-specific `__gnu_cxx::stdio_filebuf` extension which limits portability.

**Recommendation:** Document this requirement or consider portable alternatives.

---

## Positive Observations

1. **Modern C++ Usage**: The code effectively uses C++23 features and follows modern C++ practices in many areas.

2. **Polymorphic Scheduler Design**: The `Scheduler` base class provides a clean abstraction for different job scheduler implementations.

3. **Comprehensive Test Suite**: The `tests.nix` file contains thorough integration tests covering multiple scenarios.

4. **Error Recovery**: The fallback mechanism to the normal build hook is a thoughtful design choice.

5. **NixOS Integration**: Excellent integration with the Nix ecosystem and NixOS module system.

---

## Summary of Recommendations

| Priority | Category | Count |
|----------|----------|-------|
| High     | -        | 0     |
| Medium   | Safety, Design | 6 |
| Low      | Style, Documentation | 12 |

### Top 5 Recommendations

1. **Standardize error handling** across all scheduler implementations
2. **Replace raw pointer management** in PBS with smart pointers
3. **Extract magic numbers** to named constants
4. **Add documentation** to public interfaces
5. **Fix configuration description** for job-scheduler setting

---

## Conclusion

The nix-scheduler-hook codebase is functional and well-integrated with the Nix ecosystem. The identified issues are primarily related to code maintainability, documentation, and some potential edge cases in error handling. None of the issues represent critical security vulnerabilities or functionality blockers.

The project would benefit most from improved documentation and standardization of patterns across the different scheduler implementations.
