#ifdef __cplusplus
#define EXTERN_C extern "C"
#else
#define EXTERN_C
#endif

#ifdef _MSC_VER
// We always export functions on Windows as this library
// isn't meant to be consumed by other native code
#define CLANGSHARP_LINKAGE EXTERN_C __declspec(dllexport)
#else
// Not necessary outside MSVC
#define CLANGSHARP_LINKAGE EXTERN_C
#endif
