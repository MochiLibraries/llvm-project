libClangSharp
===============================================================================

This directory contains a (slightly modified) copy if libClangSharp. It is based on 2126c1f2a407e29fe90be5e603da861c3932c34f.

We build it in with the rest of Clang to avoid issues with having multiple copies of Clang's inline functions and to simplify our native dependencies. The version of ClangSharp used must still match libClangSharp here.

Changes made:

* Removed use of nested namespace definitions (These are a C++17 feature, libclang is not built as C++17.)
* Fixed up includes
* Removed duplicate private libclang definitions

## License

This directory contains files adapted from ClangSharp, which is licensed under the following terms:

```
University of Illinois/NCSA Open Source License
Copyright (c) Microsoft and Contributors
All rights reserved.

Developed by: Microsoft and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal with the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimers.
Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimers in the documentation and/or other materials provided with the distribution.
Neither the names of Microsoft, nor the names of its contributors may be used to endorse or promote products derived from this Software without specific prior written permission.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS WITH THE SOFTWARE.
```
