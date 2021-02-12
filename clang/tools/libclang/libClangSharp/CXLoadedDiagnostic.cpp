// Copyright (c) Microsoft and Contributors. All rights reserved. Licensed under the University of Illinois/NCSA Open Source License. See LICENSE.txt in the project root for license information.

// Ported from https://github.com/llvm/llvm-project/tree/llvmorg-10.0.0/clang/tools/libclang
// Original source is Copyright (c) the LLVM Project and Contributors. Licensed under the Apache License v2.0 with LLVM Exceptions. See NOTICE.txt in the project root for license information.

#include "libClangSharp/CXLoadedDiagnostic.h"
#include "libClangSharp/CXString.h"

#include <clang/Frontend/SerializedDiagnostics.h>
#include <llvm/ADT/Twine.h>

namespace clang {
    static CXSourceLocation makeLocation(const CXLoadedDiagnostic::Location* DLoc) {
        // The lowest bit of ptr_data[0] is always set to 1 to indicate this
        // is a persistent diagnostic.
        uintptr_t V = (uintptr_t)DLoc;
        V |= 0x1;
        CXSourceLocation Loc = { {  (void*)V, nullptr }, 0 };
        return Loc;
    }
}
