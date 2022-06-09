libClangSharp
===============================================================================

This directory contains a (slightly modified) copy if libClangSharp. It is based on [624eed156699bda250dc4e3f25224548113f4cdd](https://github.com/dotnet/ClangSharp/commit/624eed156699bda250dc4e3f25224548113f4cdd).

We build it in with the rest of Clang to avoid issues with having multiple copies of Clang's inline functions and to simplify our native dependencies. The version of ClangSharp used must still match libClangSharp here.

Changes made:

* Removed use of nested namespace definitions (These are a C++17 feature, libclang is not built as C++17.)
* Fixed up includes
* Removed duplicate private libclang definitions
* Manually wrote `ClangSharp_export.h` to undo [ClangSharp#247](https://github.com/dotnet/ClangSharp/pull/247)
* Disabled `-Wunused-variable` in `ClangSharp.cpp`
* Modified `clangsharp_getVersion` to indicate it's for the ClangSharp.Pathogen fork.

## License

This directory contains files adapted from ClangSharp, which is licensed under the terms found in [LICENSE.md](LICENSE.md).