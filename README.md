# LLVM Compiler Infrastructure - Pathogen Fork

This fork extends the functionality of libclang. Amonth other things, it primarily allows inspecting the memory and vtable layout of records (structs, classes, and unions.) It is currently based on [LLVM 10.0.0](https://github.com/llvm/llvm-project/tree/d32170dbd5b0d54436537b6b75beaf44324e0c28).

All functionality provided by this fork can be found in [`PathogenExtensions.cpp`](clang/tools/libclang/PathogenExtensions.cpp).

Essentially it exposes information provided by `ASTRecordLayout` and `MicrosoftVTableContext`/`ItaniumVTableContext`.
Both sets of information are intended to be ABI-agnostic.

The information provided for record layouts is largely based on the behavior of the `-fdump-record-layouts` switch.

The information provided for vtable layouts is somewhat based on the bahvior of the `-fdump-vtable-layouts` switch, but the implementation of this switch for Itanium and Microsoft ABIs is basically completely separate (the information provided by each isn't even consistent.) The codebase meant to be processed by this fork never has multiple inheritance or virtual bases, so it may be lacking in information in that department.

This fork also provides:

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
