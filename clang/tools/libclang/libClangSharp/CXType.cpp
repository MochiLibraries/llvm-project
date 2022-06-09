// Copyright (c) .NET Foundation and Contributors. All Rights Reserved. Licensed under the MIT License (MIT). See License.md in the repository root for more information.

// Ported from https://github.com/llvm/llvm-project/tree/llvmorg-14.0.0/clang/tools/libclang
// Original source is Copyright (c) the LLVM Project and Contributors. Licensed under the Apache License v2.0 with LLVM Exceptions. See NOTICE.txt in the project root for license information.

#include "libClangSharp/ClangSharp.h"
#include "libClangSharp/CXTranslationUnit.h"
#include "libClangSharp/CXType.h"

using namespace clang;

QualType GetQualType(CXType CT) {
    return QualType::getFromOpaquePtr(CT.data[0]);
}

CXTranslationUnit GetTypeTU(CXType CT) {
    return static_cast<CXTranslationUnit>(CT.data[1]);
}

namespace clang {
namespace cxtype {
}
}
