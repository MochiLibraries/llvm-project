# LLVM Compiler Infrastructure - Pathogen Fork

This fork adds functionality to libclang to dump the memory layout of records (IE: structs, classes, and unions.) It is currently based on [LLVM 10.0.0](https://github.com/llvm/llvm-project/tree/d32170dbd5b0d54436537b6b75beaf44324e0c28).

Essentially it exposes information provided by `ASTRecordLayout` and `MicrosoftVTableContext`/`ItaniumVTableContext`.
Both sets of information are intended to be ABI-agnostic.

The information provided for record layouts is largely based on the behavior of the `-fdump-record-layouts` switch.

The information provided for vtable layouts is somewhat based on the bahvior of the `-fdump-vtable-layouts` switch, but the implementation of this switch for Itanium and Microsoft ABIs is basically completely separate (the information provided by each isn't even consistent.) The codebase meant to be processed by this fork never has multiple inheritance or virtual bases, so it may be lacking in information in that department.

This fork isn't really ever meant to be merged into libclang proper. It's all contained in one file, the only modification to existing files is to add the new file to cmake. If someone wanted to adapt it to use proper formatting and and API shape to merge with libclang that'd be pretty cool. (However I find it unlikely they'd want to merge the vtable stuff unless it was modified to be more complete.)

Our use case for this fork was for C# via [ClangSharp](https://github.com/microsoft/clangsharp), hence the lack of header file. The C# bindings are not included in this repo, but they can be provided upon request.

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
