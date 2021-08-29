# LLVM Compiler Infrastructure - Pathogen Fork

This fork extends the functionality of libclang. It is currently based on [LLVM 12.0.1](https://github.com/llvm/llvm-project/commit/fed41342a82f5a3a9201819a82bf7a48313e296b).

This fork exists primarily to support [Biohazrd](https://github.com/InfectedLibraries/Biohazrd) via [ClangSharp.Pathogen](https://github.com/InfectedLibraries/ClangSharp.Pathogen). The API is generally designed to simplify calling from C# and little-to-no effort has been put in to making the API consistent.

All functionality provided by this fork can be found in [`PathogenExtensions.cpp`](clang/tools/libclang/PathogenExtensions.cpp).

This fork also includes a (slightly modified) copy of libClangSharp, see [clang/tools/libclang/libClangSharp](clang/tools/libclang/libClangSharp/Readme.md) for details.

The primary APIs provided by this fork are:

* `pathogen_GetRecordLayout`
  * Allows inspecting the memory and vtable layout of records (structs, classes, and unions.)
  * Exposes information provided by `ASTRecordLayout` and `MicrosoftVTableContext`/`ItaniumVTableContext` in an ABI-agnostic manner.
  * The information provided for record layouts is largely based on the behavior of the `-fdump-record-layouts` switch.
  * The information provided for vtable layouts is somewhat based on the bahvior of the `-fdump-vtable-layouts` switch, but the implementation of this switch for Itanium and Microsoft ABIs is basically completely separate (the information provided by each isn't even consistent.)
  * Biohazrd does not currently process records with multiple inheritance or virtual bases, so information in this department has never been fully utilized and may be lacking.
* `pathogen_Location_isFromMainFile`
  * A variant of `clang_Location_isFromMainFile` that uses `SourceManager::isInMainFile` instead of `SourceManager::isWrittenInMainFile`.
  * In particular this function will consider cursors created from a macro expansion in the main file to be in the main file.
* `pathogen_getOperatorOverloadInfo`
  * Provides information about operator overloads (and whether a given function is an operator overload.)
* `pathogen_getArgPassingRestrictions`
  * Returns whether the given type is able to be passed in registers for by-value arguments (or return values.)
  * Note that the underlying method for this function (`RecordDecl::getArgPassingRestrictions`) only cares about C++ restrictions, it does not consider size-related restrictions.
* `pathogen_ComputeConstantValue`
  * Tries to compute the constant value of an expression or a variable's initializer and returns the constant value.
* `pathogen_EnumerateMacros`
  * Enumerates macros from the preprocessor and provides information about them.
* `pathogen_GetUuidAttrGuid`
  * Gets the GUID used for a COM interface from a [`__declspec(uuid)` attribute](https://docs.microsoft.com/en-us/cpp/cpp/uuid-cpp?view=msvc-160).
* `pathogen_GetSpecializationKind`
  * Gets the kind of specialization for a `ClassTemplateSpecializationDecl`.
* `pathogen_InstantiateSpecializedClassTemplate`
  * Initializes the specified specialized class template declaration.
* `pathogen_InstantiateAllFullySpecializedClassTemplates`
  * Instantiates all fully-specializaed class templates in the translation unit.
* `pathogen_EnumerateAllSpecializedClassTemplates`
  * Enumerates all specialized class templates in the translation unit.
* `pathogen_getTypeSpellingWithPlaceholder`
  * Similar to `clang_getTypeSpelling`, except it allows specifying the placeholder used to pretty-print the type. (Makes it easy to print a type like `int [10]` as `int someParameterName[10]`.)
* `pathogen_BeginEnumerateDeclarationsRaw` / `pathogen_EnumerateDeclarationsRawMoveNext`
  * Enumerates child declarations from a declaration context without any filtering.
  * In particular, this is useful for enumerating the members of an implicitly-instantiated template specialization.
* `pathogen_IsFunctionCallable` / `pathogen_IsFunctionTypeCallable`
  * Performs checks similar to what Clang does internally to determine if the required types are complete in order to call a function.
  * As a side-effect, will implicitly instantiate any templates required to perform the call.
  * `nullptr` is returned when the function is callable. Otherwise a set of one or more strings describing why the function cannot be called is returned.
  * Due to an unavoidable side-effect of how these functions work, unecessary informational diagnostics may be attached to the translation unit.
  * (This is because the diagnostics for an incomplete type involved in a function call are split between two areas within Clang. We can handle the error half but not the informational half.)
* `pathogen_GetArrangedFunction`
  * Queries Clang's code generator to determine how a function call is arranged.

This fork was never really intended to be merged into libclang proper. The API shape doesn't match exactly what libclang provides, and it only exists to support [ClangSharp.Pathogen](https://github.com/InfectedLibraries/ClangSharp.Pathogen) (and as such are accessed via C# bindings, hence the lack of a header file.)

---------------

# The LLVM Compiler Infrastructure

[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/llvm/llvm-project/badge)](https://securityscorecards.dev/viewer/?uri=github.com/llvm/llvm-project)
[![OpenSSF Best Practices](https://www.bestpractices.dev/projects/8273/badge)](https://www.bestpractices.dev/projects/8273)
[![libc++](https://github.com/llvm/llvm-project/actions/workflows/libcxx-build-and-test.yaml/badge.svg?branch=main&event=schedule)](https://github.com/llvm/llvm-project/actions/workflows/libcxx-build-and-test.yaml?query=event%3Aschedule)

Welcome to the LLVM project!

This repository contains the source code for LLVM, a toolkit for the
construction of highly optimized compilers, optimizers, and run-time
environments.

The LLVM project has multiple components. The core of the project is
itself called "LLVM". This contains all of the tools, libraries, and header
files needed to process intermediate representations and convert them into
object files. Tools include an assembler, disassembler, bitcode analyzer, and
bitcode optimizer.

C-like languages use the [Clang](http://clang.llvm.org/) frontend. This
component compiles C, C++, Objective-C, and Objective-C++ code into LLVM bitcode
-- and from there into object files, using LLVM.

Other components include:
the [libc++ C++ standard library](https://libcxx.llvm.org),
the [LLD linker](https://lld.llvm.org), and more.

## Getting the Source Code and Building LLVM

Consult the
[Getting Started with LLVM](https://llvm.org/docs/GettingStarted.html#getting-the-source-code-and-building-llvm)
page for information on building and running LLVM.

For information on how to contribute to the LLVM project, please take a look at
the [Contributing to LLVM](https://llvm.org/docs/Contributing.html) guide.

## Getting in touch

Join the [LLVM Discourse forums](https://discourse.llvm.org/), [Discord
chat](https://discord.gg/xS7Z362),
[LLVM Office Hours](https://llvm.org/docs/GettingInvolved.html#office-hours) or
[Regular sync-ups](https://llvm.org/docs/GettingInvolved.html#online-sync-ups).

The LLVM project has adopted a [code of conduct](https://llvm.org/docs/CodeOfConduct.html) for
participants to all modes of communication within the project.
