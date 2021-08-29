//===----------------------------------------------------------------------===//
//
// Pathogen Studios extensions to libclang
// Provides functions for reading the memory and vtable layout of a type
// (Among other things)
//
// Useful references:
// * lib/AST/RecordLayoutBuilder.cpp (Used for -fdump-record-layouts)
// * lib/AST/VTableBuilder.cpp       (Used for -fdump-vtable-layouts)
//
//===----------------------------------------------------------------------===//
// clang-format off

#include "CIndexer.h"
#include "CXCursor.h"
#include "CXSourceLocation.h"
#include "CXString.h"
#include "CXTranslationUnit.h"
#include "CXType.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/AST/VTableBuilder.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Lex/PreprocessingRecord.h"
#include "clang/Sema/Sema.h"

#include "llvm/IR/LLVMContext.h"

// These APIs are technically private to the CodeGen module
#include "../lib/CodeGen/CodeGenModule.h"
#include "../lib/CodeGen/CGCXXABI.h"

#include <limits>
#include <memory>

using namespace clang;
using ABIArgInfo = CodeGen::ABIArgInfo;

#define PATHOGEN_EXPORT extern "C" CINDEX_LINKAGE

// This is incomplete but good enough for our purposes
#define PATHOGEN_FLAGS(ENUM_TYPE) \
    inline ENUM_TYPE operator|(ENUM_TYPE a, ENUM_TYPE b) \
    { \
        return static_cast<ENUM_TYPE>(static_cast<std::underlying_type<ENUM_TYPE>::type>(a) | static_cast<std::underlying_type<ENUM_TYPE>::type>(b)); \
    } \
    \
    inline ENUM_TYPE& operator|=(ENUM_TYPE& a, ENUM_TYPE b) \
    { \
        a = a | b; \
        return a; \
    }

typedef unsigned char interop_bool;

enum class PathogenRecordFieldKind : int32_t
{
    Normal,
    VTablePtr,
    NonVirtualBase,
    VirtualBaseTablePtr, //!< Only appears in Microsoft ABI
    VTorDisp, //!< Only appears in Microsoft ABI
    VirtualBase,
};

struct PathogenRecordField
{
    PathogenRecordFieldKind Kind;
    int64_t Offset;
    PathogenRecordField* NextField;
    CXString Name;

    //! When Kind == Normal, this is the type of the field
    //! When Kind == NonVirtualBase, VTorDisp, or VirtualBase, this is the type of the base
    //! When Kind == VTablePtr, this is void**
    //! When Kind == VirtualBaseTablePtr, this is void*
    CXType Type;

    // Only relevant when Kind == Normal
    CXCursor FieldDeclaration;
    interop_bool IsBitField;

    // Only relevant when IsBitField == true
    unsigned int BitFieldStart;
    unsigned int BitFieldWidth;

    // Only relevant when Kind == NonVirtualBase or VirtialBase
    interop_bool IsPrimaryBase;
};

enum class PathogenVTableEntryKind : int32_t
{
    VCallOffset,
    VBaseOffset,
    OffsetToTop,
    RTTI,
    FunctionPointer,
    CompleteDestructorPointer,
    DeletingDestructorPointer,
    UnusedFunctionPointer,
};

// We verify the enums match manually because we need a stable definition here to reflect on the C# side of things.
#define verify_vtable_entry_kind(PATHOGEN_KIND, CLANG_KIND) static_assert((int)(PathogenVTableEntryKind::PATHOGEN_KIND) == (int)(VTableComponent::CLANG_KIND), #PATHOGEN_KIND " must match " #CLANG_KIND);
verify_vtable_entry_kind(VCallOffset, CK_VCallOffset)
verify_vtable_entry_kind(VBaseOffset, CK_VBaseOffset)
verify_vtable_entry_kind(OffsetToTop, CK_OffsetToTop)
verify_vtable_entry_kind(RTTI, CK_RTTI)
verify_vtable_entry_kind(FunctionPointer, CK_FunctionPointer)
verify_vtable_entry_kind(CompleteDestructorPointer, CK_CompleteDtorPointer)
verify_vtable_entry_kind(DeletingDestructorPointer, CK_DeletingDtorPointer)
verify_vtable_entry_kind(UnusedFunctionPointer, CK_UnusedFunctionPointer)

//TODO: It'd be nice to know which entry of the table corresponds with a vtable pointer in the associated record.
// Unfortunately this is non-trivial to get. For simple inheritance trees with no multi-inheritance this should simply the first entry after the RTTI pointer.
// Clang will dump this with -fdump-vtable-layouts on Itanium platforms. Ctrl+F for "vtable address --" in VTableBuilder.cpp
// This is also hard to model with the way we present record layouts since bases are referenced rather than embedded.
struct PathogenVTableEntry
{
    PathogenVTableEntryKind Kind;

    //! Only relevant when Kind == FunctionPointer, CompleteDestructorPointer, DeletingDestructorPointer, or UnusedFunctionPointer
    CXCursor MethodDeclaration;

    //! Only relevant when Kind == RTTI
    CXCursor RttiType;

    //! Only relevant when Kind == VCallOffset, VBaseOffset, or OffsetToTop
    int64_t Offset;

    PathogenVTableEntry(CXTranslationUnit translationUnit, const VTableComponent& component)
    {
        Kind = (PathogenVTableEntryKind)component.getKind();
        MethodDeclaration = {};
        RttiType = {};
        Offset = 0;
        
        switch (Kind)
        {
            case PathogenVTableEntryKind::VCallOffset:
                Offset = component.getVCallOffset().getQuantity();
                break;
            case PathogenVTableEntryKind::VBaseOffset:
                Offset = component.getVBaseOffset().getQuantity();
                break;
            case PathogenVTableEntryKind::OffsetToTop:
                Offset = component.getOffsetToTop().getQuantity();
                break;
            case PathogenVTableEntryKind::RTTI:
                RttiType = cxcursor::MakeCXCursor(component.getRTTIDecl(), translationUnit);
                break;
            case PathogenVTableEntryKind::FunctionPointer:
            case PathogenVTableEntryKind::CompleteDestructorPointer:
            case PathogenVTableEntryKind::DeletingDestructorPointer:
            case PathogenVTableEntryKind::UnusedFunctionPointer:
                MethodDeclaration = cxcursor::MakeCXCursor(component.getFunctionDecl(), translationUnit);
                break;
        }
    }
};

struct PathogenVTable
{
    int32_t EntryCount;
    PathogenVTableEntry* Entries;
    //! Only relevant on Microsoft ABI
    PathogenVTable* NextVTable;

    PathogenVTable(CXTranslationUnit translationUnit, const VTableLayout& layout)
    {
        NextVTable = nullptr;

        ArrayRef<VTableComponent> components = layout.vtable_components();
        EntryCount = (int32_t)components.size();
        Entries = (PathogenVTableEntry*)malloc(sizeof(PathogenVTableEntry) * EntryCount);

        for (int32_t i = 0; i < EntryCount; i++)
        {
            Entries[i] = PathogenVTableEntry(translationUnit, components[i]);
        }
    }

    ~PathogenVTable()
    {
        free(Entries);
    }
};

struct PathogenRecordLayout
{
    PathogenRecordField* FirstField;
    PathogenVTable* FirstVTable;

    int64_t Size;
    int64_t Alignment;

    // For C++ records only
    interop_bool IsCppRecord;
    int64_t NonVirtualSize;
    int64_t NonVirtualAlignment;

    PathogenRecordField* AddField(PathogenRecordFieldKind kind, int64_t offset, CXString name, CXType type)
    {
        // Find the insertion point for the field
        PathogenRecordField** insertPoint = &FirstField;

        while (*insertPoint != nullptr && (*insertPoint)->Offset <= offset)
        { insertPoint = &((*insertPoint)->NextField); }

        // Insert the new field
        PathogenRecordField* field = new PathogenRecordField();

        field->Kind = kind;
        field->Offset = offset;
        field->Name = name;
        field->Type = type;

        field->NextField = *insertPoint;
        *insertPoint = field;

        return field;
    }

    PathogenRecordField* AddField(PathogenRecordFieldKind kind, int64_t offset, CXTranslationUnit translationUnit, const FieldDecl& field)
    {
        CXType type = cxtype::MakeCXType(field.getType(), translationUnit);
        PathogenRecordField* ret = AddField(kind, offset, cxstring::createDup(field.getName()), type);
        ret->FieldDeclaration = cxcursor::MakeCXCursor(&field, translationUnit);
        return ret;
    }

    PathogenVTable* AddVTableLayout(CXTranslationUnit translationUnit, const VTableLayout& layout)
    {
        // Find insertion point for the new table
        PathogenVTable** insertPoint = &FirstVTable;

        while (*insertPoint != nullptr)
        { insertPoint = &((*insertPoint)->NextVTable); }

        // Insert the new table
        PathogenVTable* vTable = new PathogenVTable(translationUnit, layout);

        vTable->NextVTable = *insertPoint;
        *insertPoint = vTable;

        return vTable;
    }

    ~PathogenRecordLayout()
    {
        // Delete all fields
        for (PathogenRecordField* field = FirstField; field;)
        {
            PathogenRecordField* nextField = field->NextField;
            clang_disposeString(field->Name);
            delete field;
            field = nextField;
        }

        // Delete all VTables
        for (PathogenVTable* vTable = FirstVTable; vTable;)
        {
            PathogenVTable* nextVTable = vTable->NextVTable;
            delete vTable;
            vTable = nextVTable;
        }
    }
};

static bool IsMsLayout(const ASTContext& context)
{
    return context.getTargetInfo().getCXXABI().isMicrosoft();
}

PATHOGEN_EXPORT PathogenRecordLayout* pathogen_GetRecordLayout(CXCursor cursor)
{
    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        return nullptr;
    }

    // Get the record declaration
    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const RecordDecl* record = dyn_cast_or_null<RecordDecl>(declaration);

    // The cursor must be a record declaration
    if (record == nullptr)
    {
        return nullptr;
    }

    // The cursor must have a definition (IE: it can't be a forward-declaration.)
    if (record->getDefinition() == nullptr)
    {
        return nullptr;
    }

    // Get the AST context
    ASTContext& context = cxcursor::getCursorContext(cursor);

    // Get the translation unit
    CXTranslationUnit translationUnit = clang_Cursor_getTranslationUnit(cursor);

    // Get the void* and void** types
    CXType voidPointerType = cxtype::MakeCXType(context.VoidPtrTy, translationUnit);
    CXType voidPointerPointerType = cxtype::MakeCXType(context.getPointerType(context.VoidPtrTy), translationUnit);

    // Get the record layout
    const ASTRecordLayout& layout = context.getASTRecordLayout(record);

    // Get the C++ record if applicable
    const CXXRecordDecl* cxxRecord = dyn_cast<CXXRecordDecl>(record);

    // Create the record layout
    PathogenRecordLayout* ret = new PathogenRecordLayout();
    ret->Size = layout.getSize().getQuantity();
    ret->Alignment = layout.getAlignment().getQuantity();
    
    if (cxxRecord)
    {
        ret->IsCppRecord = true;
        ret->NonVirtualSize = layout.getNonVirtualSize().getQuantity();
        ret->NonVirtualAlignment = layout.getNonVirtualAlignment().getQuantity();
    }

    // C++-specific fields
    if (cxxRecord)
    {
        const CXXRecordDecl* primaryBase = layout.getPrimaryBase();
        bool hasOwnVFPtr = layout.hasOwnVFPtr();
        bool hasOwnVBPtr = layout.hasOwnVBPtr();

        // Add vtable pointer
        if (cxxRecord->isDynamicClass() && !primaryBase && !IsMsLayout(context))
        {
            // Itanium-style VTable pointer
            ret->AddField(PathogenRecordFieldKind::VTablePtr, 0, cxstring::createRef("vtable_pointer"), voidPointerPointerType);
        }
        else if (hasOwnVFPtr)
        {
            // Microsoft C++ ABI VFTable pointer
            ret->AddField(PathogenRecordFieldKind::VTablePtr, 0, cxstring::createRef("vftable_pointer"), voidPointerPointerType);
        }

        // Add non-virtual bases
        for (const CXXBaseSpecifier& base : cxxRecord->bases())
        {
            assert(!base.getType()->isDependentType() && "Cannot layout class with dependent bases.");

            // Ignore virtual bases, they come up later.
            if (base.isVirtual())
            { continue; }

            QualType baseType = base.getType();
            CXType cxType = cxtype::MakeCXType(baseType, translationUnit);
            CXXRecordDecl* baseRecord = baseType->getAsCXXRecordDecl();
            bool isPrimary = baseRecord == primaryBase;
            int64_t offset = layout.getBaseClassOffset(baseRecord).getQuantity();

            PathogenRecordField* field = ret->AddField(PathogenRecordFieldKind::NonVirtualBase, offset, cxstring::createRef(isPrimary ? "primary_base" : "base"), cxType);
            field->IsPrimaryBase = isPrimary;
        }

        // Vbptr - Microsoft C++ ABI
        if (hasOwnVBPtr)
        {
            ret->AddField(PathogenRecordFieldKind::VirtualBaseTablePtr, layout.getVBPtrOffset().getQuantity(), cxstring::createRef("vbtable_pointer"), voidPointerType);
        }
    }

    // Add normal fields
    uint64_t fieldIndex = 0;
    for (RecordDecl::field_iterator it = record->field_begin(), end = record->field_end(); it != end; it++, fieldIndex++)
    {
        const FieldDecl& field = **it;

        uint64_t offsetBits = layout.getFieldOffset(fieldIndex);
        CharUnits offsetChars = context.toCharUnitsFromBits(offsetBits);
        int64_t offset = offsetChars.getQuantity();

        PathogenRecordField* pathogenField = ret->AddField(PathogenRecordFieldKind::Normal, offset, translationUnit, field);

        // If the field is a bitfield, mark it as such.
        // This relies on the fields being offset-sequential since AddField doesn't know about bitfields.
        if (field.isBitField())
        {
            pathogenField->IsBitField = true;
            pathogenField->BitFieldStart = offsetBits - context.toBits(offsetChars);
            pathogenField->BitFieldWidth = field.getBitWidthValue(context);
        }
    }

    // Add virtual bases
    if (cxxRecord)
    {
        const ASTRecordLayout::VBaseOffsetsMapTy& vtorDisps = layout.getVBaseOffsetsMap();
        const CXXRecordDecl* primaryBase = layout.getPrimaryBase();

        for (const CXXBaseSpecifier& base : cxxRecord->vbases())
        {
            assert(base.isVirtual() && "Bases must be virtual.");
            QualType baseType = base.getType();
            CXType baseCxType = cxtype::MakeCXType(baseType, translationUnit);
            const CXXRecordDecl* vbase = baseType->getAsCXXRecordDecl();

            int64_t offset = layout.getVBaseClassOffset(vbase).getQuantity();

            if (vtorDisps.find(vbase)->second.hasVtorDisp())
            {
                ret->AddField(PathogenRecordFieldKind::VTorDisp, offset - 4, cxstring::createRef("vtordisp"), baseCxType);
            }

            bool isPrimary = vbase == primaryBase;
            PathogenRecordField* field = ret->AddField(PathogenRecordFieldKind::VirtualBase, offset, cxstring::createRef(isPrimary ? "primary_virtual_base" : "virtual_base"), baseCxType);
            field->IsPrimaryBase = isPrimary;
        }
    }

    // Add VTable layouts
    if (cxxRecord && cxxRecord->isDynamicClass())
    {
        if (context.getVTableContext()->isMicrosoft())
        {
            MicrosoftVTableContext& vtableContext = *cast<MicrosoftVTableContext>(context.getVTableContext());
            const VPtrInfoVector& offsets = vtableContext.getVFPtrOffsets(cxxRecord);

            for (const std::unique_ptr<VPtrInfo>& offset : offsets)
            {
                const VTableLayout& layout = vtableContext.getVFTableLayout(cxxRecord, offset->FullOffsetInMDC);
                ret->AddVTableLayout(translationUnit, layout);
            }
        }
        else
        {
            ItaniumVTableContext& vtableContext = *cast<ItaniumVTableContext>(context.getVTableContext());
            const VTableLayout& layout = vtableContext.getVTableLayout(cxxRecord);
            ret->AddVTableLayout(translationUnit, layout);
        }
    }

    return ret;
}

PATHOGEN_EXPORT void pathogen_DeleteRecordLayout(PathogenRecordLayout* layout)
{
    delete layout;
}

//-------------------------------------------------------------------------------------------------
// Location helpers
//-------------------------------------------------------------------------------------------------

//! This is essentially the same as clang_Location_isFromMainFile, but it uses SourceManager::isInMainFile instead of SourceManager::isWrittenInMainFile
//! The libclang function suffers from some quirks, namely:
//! * It is possible for the start and end locations for a cursor's extent to have different values.
//! * Cursors which are the result of a macro expansion will be considered to be outside of the main file.
//! These quirks are not good for our usecase of rejecting cursors from included files, so we provide this alternative.
PATHOGEN_EXPORT interop_bool pathogen_Location_isFromMainFile(CXSourceLocation cxLocation)
{
    const SourceLocation location = SourceLocation::getFromRawEncoding(cxLocation.int_data);
    if (location.isInvalid())
    { return false; }

    const SourceManager& sourceManager = *static_cast<const SourceManager*>(cxLocation.ptr_data[0]);
    return sourceManager.isInMainFile(location);
}

//-------------------------------------------------------------------------------------------------
// Operator overload helpers
//-------------------------------------------------------------------------------------------------

enum class PathogenOperatorOverloadKind : int32_t
{
    None,
    New,
    Delete,
    Array_New,
    Array_Delete,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Caret,
    Amp,
    Pipe,
    Tilde,
    Exclaim,
    Equal,
    Less,
    Greater,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    PercentEqual,
    CaretEqual,
    AmpEqual,
    PipeEqual,
    LessLess,
    GreaterGreater,
    LessLessEqual,
    GreaterGreaterEqual,
    EqualEqual,
    ExclaimEqual,
    LessEqual,
    GreaterEqual,
    Spaceship,
    AmpAmp,
    PipePipe,
    PlusPlus,
    MinusMinus,
    Comma,
    ArrowStar,
    Arrow,
    Call,
    Subscript,
    Conditional,
    Coawait,
    Invalid
};

// We verify the enums match manually because we need a stable definition here to reflect on the C# side of things.
#define verify_operator_overload_kind(PATHOGEN_KIND, CLANG_KIND) static_assert((int)(PathogenOperatorOverloadKind::PATHOGEN_KIND) == (int)(CLANG_KIND), #PATHOGEN_KIND " must match " #CLANG_KIND);
verify_operator_overload_kind(None, OO_None)
verify_operator_overload_kind(New, OO_New)
verify_operator_overload_kind(Delete, OO_Delete)
verify_operator_overload_kind(Array_New, OO_Array_New)
verify_operator_overload_kind(Array_Delete, OO_Array_Delete)
verify_operator_overload_kind(Plus, OO_Plus)
verify_operator_overload_kind(Minus, OO_Minus)
verify_operator_overload_kind(Star, OO_Star)
verify_operator_overload_kind(Slash, OO_Slash)
verify_operator_overload_kind(Percent, OO_Percent)
verify_operator_overload_kind(Caret, OO_Caret)
verify_operator_overload_kind(Amp, OO_Amp)
verify_operator_overload_kind(Pipe, OO_Pipe)
verify_operator_overload_kind(Tilde, OO_Tilde)
verify_operator_overload_kind(Exclaim, OO_Exclaim)
verify_operator_overload_kind(Equal, OO_Equal)
verify_operator_overload_kind(Less, OO_Less)
verify_operator_overload_kind(Greater, OO_Greater)
verify_operator_overload_kind(PlusEqual, OO_PlusEqual)
verify_operator_overload_kind(MinusEqual, OO_MinusEqual)
verify_operator_overload_kind(StarEqual, OO_StarEqual)
verify_operator_overload_kind(SlashEqual, OO_SlashEqual)
verify_operator_overload_kind(PercentEqual, OO_PercentEqual)
verify_operator_overload_kind(CaretEqual, OO_CaretEqual)
verify_operator_overload_kind(AmpEqual, OO_AmpEqual)
verify_operator_overload_kind(PipeEqual, OO_PipeEqual)
verify_operator_overload_kind(LessLess, OO_LessLess)
verify_operator_overload_kind(GreaterGreater, OO_GreaterGreater)
verify_operator_overload_kind(LessLessEqual, OO_LessLessEqual)
verify_operator_overload_kind(GreaterGreaterEqual, OO_GreaterGreaterEqual)
verify_operator_overload_kind(EqualEqual, OO_EqualEqual)
verify_operator_overload_kind(ExclaimEqual, OO_ExclaimEqual)
verify_operator_overload_kind(LessEqual, OO_LessEqual)
verify_operator_overload_kind(GreaterEqual, OO_GreaterEqual)
verify_operator_overload_kind(Spaceship, OO_Spaceship)
verify_operator_overload_kind(AmpAmp, OO_AmpAmp)
verify_operator_overload_kind(PipePipe, OO_PipePipe)
verify_operator_overload_kind(PlusPlus, OO_PlusPlus)
verify_operator_overload_kind(MinusMinus, OO_MinusMinus)
verify_operator_overload_kind(Comma, OO_Comma)
verify_operator_overload_kind(ArrowStar, OO_ArrowStar)
verify_operator_overload_kind(Arrow, OO_Arrow)
verify_operator_overload_kind(Call, OO_Call)
verify_operator_overload_kind(Subscript, OO_Subscript)
verify_operator_overload_kind(Conditional, OO_Conditional)
verify_operator_overload_kind(Coawait, OO_Coawait)
verify_operator_overload_kind(Invalid, NUM_OVERLOADED_OPERATORS)

struct PathogenOperatorOverloadInfo
{
    PathogenOperatorOverloadKind Kind;
    const char* Name;
    const char* Spelling;
    interop_bool IsUnary;
    interop_bool IsBinary;
    interop_bool IsMemberOnly;
};

static PathogenOperatorOverloadInfo OperatorInformation[] =
{
    { PathogenOperatorOverloadKind::None, nullptr, nullptr, false, false, false }, // OO_None
#define OVERLOADED_OPERATOR(Name, Spelling, Token, Unary, Binary, MemberOnly) { PathogenOperatorOverloadKind::Name, #Name, Spelling, Unary, Binary, MemberOnly },
#include "clang/Basic/OperatorKinds.def"
    // This entry takes the slot for NUM_OVERLOADED_OPERATORS and is returned when an unexpected operator overload is encountered
    { PathogenOperatorOverloadKind::Invalid, nullptr, nullptr, false, false, false },
};

PATHOGEN_EXPORT PathogenOperatorOverloadInfo* pathogen_getOperatorOverloadInfo(CXCursor cursor)
{
    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        return nullptr;
    }

    // Get the function declaration
    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const FunctionDecl* function = dyn_cast_or_null<FunctionDecl>(declaration);

    // The cursor must be a function declaration
    if (function == nullptr)
    {
        return nullptr;
    }

    // Get the overloaded operator
    OverloadedOperatorKind operatorKind = function->getOverloadedOperator();

    // Ensure the operator kind is within bounds
    if (operatorKind < 0 || operatorKind > NUM_OVERLOADED_OPERATORS)
    {
        // NUM_OVERLOADED_OPERATORS is used for the invalid kind slot.
        operatorKind = NUM_OVERLOADED_OPERATORS;
    }

    // Return the operator information
    return &OperatorInformation[operatorKind];
}

//-------------------------------------------------------------------------------------------------
// Record arg passing kind
//-------------------------------------------------------------------------------------------------

enum class PathogenArgPassingKind : int32_t
{
    CanPassInRegisters,
    CannotPassInRegisters,
    CanNeverPassInRegisters,
    Invalid
};

#define verify_arg_passing_kind(PATHOGEN_KIND, CLANG_KIND) static_assert((int)(PathogenArgPassingKind::PATHOGEN_KIND) == (int)(RecordDecl::CLANG_KIND), #PATHOGEN_KIND " must match " #CLANG_KIND);
verify_arg_passing_kind(CanPassInRegisters, APK_CanPassInRegs)
verify_arg_passing_kind(CannotPassInRegisters, APK_CannotPassInRegs)
verify_arg_passing_kind(CanNeverPassInRegisters, APK_CanNeverPassInRegs)

PATHOGEN_EXPORT PathogenArgPassingKind pathogen_getArgPassingRestrictions(CXCursor cursor)
{
    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        return PathogenArgPassingKind::Invalid;
    }

    // Get the record declaration
    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const RecordDecl* record = dyn_cast_or_null<RecordDecl>(declaration);

    // Return the value
    return (PathogenArgPassingKind)record->getArgPassingRestrictions();
}

//-------------------------------------------------------------------------------------------------
// Computing the constant value an expression of a variable's initializer
//-------------------------------------------------------------------------------------------------

enum class PathogenConstantValueKind : int
{
    Unknown,
    NullPointer,
    UnsignedInteger,
    SignedInteger,
    FloatingPoint,
    String,
};

enum class PathogenStringConstantKind : int
{
    Ascii,
    //! Never actually used. We replace this with the more appropriate UTF equivalent with WideCharBit set instead.
    WideChar,
    Utf8,
    Utf16,
    Utf32,
    //! When combined with one of the UTF values, indicates that the constant was originally a wchar_t string.
    WideCharBit = 1 << 31,
};
PATHOGEN_FLAGS(PathogenStringConstantKind);
static_assert((int)PathogenStringConstantKind::Ascii == StringLiteral::Ascii, "ASCII string kinds must match.");
static_assert((int)PathogenStringConstantKind::WideChar == StringLiteral::Wide, "Wide character string kinds must match.");
static_assert((int)PathogenStringConstantKind::Utf8 == StringLiteral::UTF8, "UTF8 string kinds must match.");
static_assert((int)PathogenStringConstantKind::Utf16 == StringLiteral::UTF16, "UTF16 string kinds must match.");
static_assert((int)PathogenStringConstantKind::Utf32 == StringLiteral::UTF32, "UTF32 string kinds must match.");

struct PathogenConstantString
{
    uint64_t SizeBytes;
    unsigned char FirstByte;
};

struct PathogenConstantValueInfo
{
    bool HasSideEffects;
    bool HasUndefinedBehavior;
    PathogenConstantValueKind Kind;
    //! If Kind is UnsignedInteger, SignedInteger, or FloatingPoint: This is the size of the value in bits
    //! If Kind is String: This is one of PathogenStringConstantKind, potentially with WideCharBit set in the case of wchar_t.
    //! If Kind is Unknown, this is the Clang kind (APValue::ValueKind)
    int SubKind;
    //! The value of the constant
    //! If Kind is NullPointer, this is 0
    //! If Kind is UnsignedInteger, this is zero-extended
    //! If Kind is SignedInteger, this is sign-extended
    //! If Kind is FloatingPoint, this is the floating point value as bits and unused bits are 0
    //! If Kind is String, this is a pointer to a PathogenConstantString representing the string
    uint64_t Value;
};
static_assert(sizeof(PathogenConstantValueInfo::Value) >= sizeof(void*), "PathogenConstantValueInfo::Value must be able to hold a pointer.");

//! Tries to compute the constant value of the specified variable declaration or expression
//! Returns false if Clang could not determine the constant value of the specified cursor.
//! Error is never set when this function returns true.
PATHOGEN_EXPORT bool pathogen_ComputeConstantValue(CXCursor cursor, PathogenConstantValueInfo* info, const char** error)
{
    // Get the expression
    const Expr* expression;

    if (clang_isDeclaration(cursor.kind))
    {
        // Get the variable declaration
        const Decl* declaration = cxcursor::getCursorDecl(cursor);
        const VarDecl* variableDeclaration = dyn_cast_or_null<VarDecl>(declaration);

        // The declaration cursor must be a variable declaration
        if (variableDeclaration == nullptr)
        {
            *error = "The cursor is not a variable declaration or expression.";
            return false;
        }

        // If the variable has no initializer, there's no value to get
        if (!variableDeclaration->hasInit())
        { return false; }

        expression = variableDeclaration->getAnyInitializer();
    }
    else if (clang_isExpression(cursor.kind))
    {
        expression = cxcursor::getCursorExpr(cursor);
    }
    else
    {
        *error = "The cursor is not a variable declaration or expression.";
        return false;
    }

    // Try and evaluate the constant
    ASTContext& context = cxcursor::getCursorContext(cursor);
    Expr::EvalResult result;
    bool hasConstantValue = expression->EvaluateAsRValue(result, context);

    if (!hasConstantValue)
    {
        if (result.Diag != nullptr && result.Diag->size() > 0)
        { *error = "EvaluateAsRValue returned diagnostics."; }

        return false;
    }

    memset(info, 0, sizeof(*info));
    info->HasSideEffects = result.HasSideEffects;
    info->HasUndefinedBehavior = result.HasUndefinedBehavior;

    APValue value = result.Val;

    // Default values to unknown, will be replaced by more specific type if possible.
    info->Kind = PathogenConstantValueKind::Unknown;
    info->SubKind = (int)value.getKind();
    info->Value = 0;

    if (value.isInt())
    {
        llvm::APSInt intValue = value.getInt();
        info->Kind = intValue.isSigned() ? PathogenConstantValueKind::SignedInteger : PathogenConstantValueKind::UnsignedInteger;
        info->SubKind = (int)intValue.getBitWidth();
        info->Value = (uint64_t)intValue.getExtValue();
    }
    else if (value.isFloat())
    {
        llvm::APFloat floatValue = value.getFloat();
        info->Kind = PathogenConstantValueKind::FloatingPoint;
        info->SubKind = (int)floatValue.getSizeInBits(floatValue.getSemantics());
        info->Value = floatValue.bitcastToAPInt().getZExtValue();
    }
    else if (value.isNullPointer())
    {
        info->Kind = PathogenConstantValueKind::NullPointer;
        info->SubKind = 0;
        info->Value = 0;
    }
    else if (value.isLValue())
    {
        APValue::LValueBase lValue = value.getLValueBase();

        if (const Expr* lValueExpr = lValue.dyn_cast<const Expr*>())
        {
            if (lValueExpr->getStmtClass() == Stmt::StmtClass::StringLiteralClass)
            {
                const StringLiteral* stringLiteral = (const StringLiteral*)lValueExpr;
                info->Kind = PathogenConstantValueKind::String;
                PathogenStringConstantKind* stringKind = (PathogenStringConstantKind*)&info->SubKind;
                *stringKind = (PathogenStringConstantKind)stringLiteral->getKind();

                if (*stringKind == PathogenStringConstantKind::WideChar)
                {
                    switch (stringLiteral->getCharByteWidth())
                    {
                        case 1:
                            *stringKind = PathogenStringConstantKind::Utf8 | PathogenStringConstantKind::WideCharBit;
                            break;
                        case 2:
                            *stringKind = PathogenStringConstantKind::Utf16 | PathogenStringConstantKind::WideCharBit;
                            break;
                        case 4:
                            *stringKind = PathogenStringConstantKind::Utf32 | PathogenStringConstantKind::WideCharBit;
                            break;
                        default:
                            assert(false && "wchar_t string literal has an unexpected char width.");
                            break;
                    }
                }

                PathogenConstantString* string = (PathogenConstantString*)malloc(sizeof(PathogenConstantString) + stringLiteral->getByteLength() - 1);
                string->SizeBytes = stringLiteral->getByteLength();
                memcpy(&string->FirstByte, stringLiteral->getBytes().data(), string->SizeBytes);
                info->Value = (uint64_t)string;
            }
        }
    }

    return true;
}

//! Cleans up any extra memory allocated for the give constant value info.
PATHOGEN_EXPORT void pathogen_DeletePathogenConstantValueInfo(PathogenConstantValueInfo* info)
{
    if (info && info->Kind == PathogenConstantValueKind::String && info->Value != 0)
    {
        free((void*)info->Value);
        info->Value = 0;
    }
}

//-------------------------------------------------------------------------------------------------
// Macro Information
//-------------------------------------------------------------------------------------------------

enum class PathogenMacroVardicKind : int
{
    None,
    C99,
    Gnu
};

struct PathogenMacroInformation
{
    const char* Name;
    uint64_t NameLength;
    CXSourceLocation Location;
    //! True if this macro was defined at some point but was later undefined.
    interop_bool WasUndefined;
    interop_bool IsFunctionLike;
    //! True if this macro is a built-in.
    //! (IE: __FILE__ or __LINE__. Does not include macros from the "<built-in>" memory buffer.)
    interop_bool IsBuiltInMacro;
    //! True if this macro contains the sequence ", ## __VA_ARGS__"
    interop_bool HasCommaPasting;
    PathogenMacroVardicKind VardicKind;
    int ParameterCount;
    const char** ParameterNames;
    uint64_t* ParameterNameLengths;
};

typedef void (*MacroEnumeratorFunction)(PathogenMacroInformation* macroInfo, void* userData);

PATHOGEN_EXPORT unsigned int pathogen_GetPreprocessorIdentifierCount(CXTranslationUnit translationUnit)
{
    ASTUnit* astUnit = cxtu::getASTUnit(translationUnit);
    const Preprocessor& preprocessor = astUnit->getPreprocessor();
    const IdentifierTable& idTable = preprocessor.getIdentifierTable();
    return idTable.size();
}

PATHOGEN_EXPORT void pathogen_EnumerateMacros(CXTranslationUnit translationUnit, MacroEnumeratorFunction enumerator, void* userData)
{
    ASTUnit* astUnit = cxtu::getASTUnit(translationUnit);
    const Preprocessor& preprocessor = astUnit->getPreprocessor();
    const IdentifierTable& idTable = preprocessor.getIdentifierTable();

    const int stackParameterListCount = 16;
    SmallVector<const char*, stackParameterListCount> parameterNames;
    SmallVector<uint64_t, stackParameterListCount> parameterNameLengths;

    for (auto it = idTable.begin(); it != idTable.end(); it++)
    {
        const MacroDirective* macro = preprocessor.getLocalMacroDirectiveHistory(it->getValue());

        // Skip non-macro preprocessor identifiers
        if (macro == nullptr)
        {
            continue;
        }

        const MacroDirective::DefInfo definition = macro->getDefinition();
        const MacroInfo* macroInfo = definition.getMacroInfo();

        PathogenMacroInformation pathogenInfo;
        pathogenInfo.Name = it->getKey().data();
        pathogenInfo.NameLength = it->getKey().size();
        pathogenInfo.Location = cxloc::translateSourceLocation(astUnit->getASTContext(), definition.getLocation());
        pathogenInfo.WasUndefined = definition.isUndefined();
        pathogenInfo.IsFunctionLike = macroInfo->isFunctionLike();
        pathogenInfo.IsBuiltInMacro = macroInfo->isBuiltinMacro();
        pathogenInfo.HasCommaPasting = macroInfo->hasCommaPasting();
        pathogenInfo.VardicKind = macroInfo->isC99Varargs() ? PathogenMacroVardicKind::C99 : macroInfo->isGNUVarargs() ? PathogenMacroVardicKind::Gnu : PathogenMacroVardicKind::None;
        pathogenInfo.ParameterCount = macroInfo->getNumParams();

        parameterNames.clear();
        parameterNameLengths.clear();
        parameterNames.reserve(pathogenInfo.ParameterCount);
        parameterNameLengths.reserve(pathogenInfo.ParameterCount);

        for (const IdentifierInfo* parameter : macroInfo->params())
        {
            parameterNames.push_back(parameter->getName().data());
            parameterNameLengths.push_back(parameter->getName().size());
        }

        pathogenInfo.ParameterNames = parameterNames.data();
        pathogenInfo.ParameterNameLengths = parameterNameLengths.data();

        enumerator(&pathogenInfo, userData);
    }
}

//-------------------------------------------------------------------------------------------------
// Extended Attribute Information
//-------------------------------------------------------------------------------------------------

PATHOGEN_EXPORT CXString pathogen_GetUuidAttrGuid(CXCursor cursor)
{
    if (!clang_isAttribute(cursor.kind))
    {
        return cxstring::createNull();
    }

    const Attr* attribute = cxcursor::getCursorAttr(cursor);
    const UuidAttr* uuidAttribute = dyn_cast_or_null<UuidAttr>(attribute);

    if (uuidAttribute == nullptr)
    {
        return cxstring::createNull();
    }

    return cxstring::createRef(uuidAttribute->getGuid());
}

//-------------------------------------------------------------------------------------------------
// Class Template Specialization Helpers
//-------------------------------------------------------------------------------------------------

enum class PathogenTemplateSpecializationKind : int
{
    Invalid,
    Undeclared,
    ImplicitInstantiation,
    ExplicitSpecialization,
    ExplicitInstantiationDeclaration,
    ExplicitInstantiationDefinition
};

static_assert((int)PathogenTemplateSpecializationKind::Undeclared == (TSK_Undeclared + 1), "Undeclared template specialization kinds must match.");
static_assert((int)PathogenTemplateSpecializationKind::ImplicitInstantiation == (TSK_ImplicitInstantiation + 1), "Implicit template specialization kinds must match.");
static_assert((int)PathogenTemplateSpecializationKind::ExplicitSpecialization == (TSK_ExplicitSpecialization + 1), "Excplicit specialization template specialization kinds must match.");
static_assert((int)PathogenTemplateSpecializationKind::ExplicitInstantiationDeclaration == (TSK_ExplicitInstantiationDeclaration + 1), "Explicit declaration template specialization kinds must match.");
static_assert((int)PathogenTemplateSpecializationKind::ExplicitInstantiationDefinition == (TSK_ExplicitInstantiationDefinition + 1), "Explicit definition template specialization kinds must match.");

struct PathogenTemplateInstantiationMetrics
{
    uint64_t TotalSpecializationsCount;
    uint64_t PartialSpecializationsCount;
    uint64_t SuccessfulInstantiationsCount;
    uint64_t FailedInstantiationsCount;
};

typedef void (*SpecializedClassTemplateEnumeratorFunction)(PathogenTemplateSpecializationKind specializationKind, CXCursor classTemplate, void* userData);

PATHOGEN_EXPORT PathogenTemplateSpecializationKind pathogen_GetSpecializationKind(CXCursor cursor)
{
    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        return PathogenTemplateSpecializationKind::Invalid;
    }

    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const ClassTemplateSpecializationDecl* classTemplateSpecialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(declaration);

    // Declaration must be a class template specialization
    if (classTemplateSpecialization == nullptr)
    {
        return PathogenTemplateSpecializationKind::Invalid;
    }

    return (PathogenTemplateSpecializationKind)(classTemplateSpecialization->getSpecializationKind() + 1);
}

//! Initializes the specified specialized class template declaration.
//! \returns true if the template was initialized (or was already initialized), false if an error ocurred
PATHOGEN_EXPORT interop_bool pathogen_InstantiateSpecializedClassTemplate(CXCursor cursor)
{
    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        return false;
    }

    // libclang tries to present an immutable view, but this is obviously mutating things so we have to drop the const
    // (Evil? Maybe. Problematic? Hopefully not...)
    Decl* declaration = const_cast<Decl*>(cxcursor::getCursorDecl(cursor));
    ClassTemplateSpecializationDecl* classTemplateSpecialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(declaration);

    // Declaration must be a class template specialization
    if (classTemplateSpecialization == nullptr)
    {
        return false;
    }

    // If the class tempalte is already specialized there's nothing to do
    if (classTemplateSpecialization->getSpecializationKind() != TSK_Undeclared)
    {
        return true;
    }

    // Implicitly instantiate the class template
    ASTUnit* unit = cxcursor::getCursorASTUnit(cursor);
    Sema& sema = unit->getSema();
    SourceLocation sourceLocation = classTemplateSpecialization->getSourceRange().getBegin();
    return !sema.InstantiateClassTemplateSpecialization(sourceLocation, classTemplateSpecialization, TSK_ImplicitInstantiation);
}

//! Finds all specialized class templates referenced in the translation unit and implicitly instantiates them
PATHOGEN_EXPORT PathogenTemplateInstantiationMetrics pathogen_InstantiateAllFullySpecializedClassTemplates(CXTranslationUnit translationUnit)
{
    ASTUnit* unit = cxtu::getASTUnit(translationUnit);
    ASTContext& context = unit->getASTContext();
    Sema& semanticModel = unit->getSema();

    PathogenTemplateInstantiationMetrics metrics = {};

    // Enumerate all types present in the entire translation unit and look for any record types which point to uninstantiated template specializations
    for (Type* type : context.getTypes())
    {
        if (type->getTypeClass() != Type::TypeClass::Record)
        {
            continue;
        }

        const RecordType* recordType = dyn_cast_or_null<RecordType>(type);
        if (recordType == nullptr)
        {
            continue;
        }

        RecordDecl* record = recordType->getDecl();
        ClassTemplateSpecializationDecl* classTemplateSpecialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);

        if (classTemplateSpecialization == nullptr)
        {
            continue;
        }

        // Skip specializations of templates which are never defined
        if (!classTemplateSpecialization->getSpecializedTemplate()->getTemplatedDecl()->hasDefinition())
        {
            continue;
        }

        if (classTemplateSpecialization->getKind() == Decl::Kind::ClassTemplatePartialSpecialization)
        {
            metrics.PartialSpecializationsCount++;
            continue;
        }

        metrics.TotalSpecializationsCount++;

        if (classTemplateSpecialization->getSpecializationKind() != TSK_Undeclared)
        {
            continue;
        }

        // Since this implicit instantiation isn't actually present in source, we just attribute it to the template definition.
        // (Pretty sure this is only used for diagnostics, so it seemingly doesn't matter a ton where it goes.)
        SourceLocation sourceLocation = classTemplateSpecialization->getSourceRange().getBegin();
        if (semanticModel.InstantiateClassTemplateSpecialization(sourceLocation, classTemplateSpecialization, TSK_ImplicitInstantiation))
        {
            // InstantiateClassTemplateSpecialization returns true on failure.
            metrics.FailedInstantiationsCount++;
        }
        else
        {
            metrics.SuccessfulInstantiationsCount++;
        }
    }

    return metrics;
}

//! Enumerates all specialized templates present in the translation unit
//! Note that this also includes uninstantiated templates too.
PATHOGEN_EXPORT void pathogen_EnumerateAllSpecializedClassTemplates(CXTranslationUnit translationUnit, SpecializedClassTemplateEnumeratorFunction enumerator, void* userData)
{
    ASTUnit* unit = cxtu::getASTUnit(translationUnit);
    ASTContext& context = unit->getASTContext();

    // Enumerate all types present in the entire translation unit and look for any record types which point to uninstantiated template specializations
    for (Type* type : context.getTypes())
    {
        if (type->getTypeClass() != Type::TypeClass::Record)
        {
            continue;
        }

        const RecordType* recordType = dyn_cast_or_null<RecordType>(type);
        if (recordType == nullptr)
        {
            continue;
        }

        RecordDecl* record = recordType->getDecl();
        ClassTemplateSpecializationDecl* classTemplateSpecialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(record);

        if (classTemplateSpecialization == nullptr)
        {
            continue;
        }

        PathogenTemplateSpecializationKind specializationKind = (PathogenTemplateSpecializationKind)(classTemplateSpecialization->getSpecializationKind() + 1);
        CXCursor cursor = cxcursor::MakeCXCursor(classTemplateSpecialization, translationUnit);
        enumerator(specializationKind, cursor, userData);
    }
}

//-------------------------------------------------------------------------------------------------
// Pretty print type with a variable name
//-------------------------------------------------------------------------------------------------

PATHOGEN_EXPORT CXString pathogen_getTypeSpellingWithPlaceholder(CXType type, const char* placeholder, size_t placeholderByteCount)
{
    QualType qualifiedType = QualType::getFromOpaquePtr(type.data[0]);

    if (qualifiedType.isNull())
    {
        return cxstring::createEmpty();
    }

    CXTranslationUnit translationUnit = static_cast<CXTranslationUnit>(type.data[1]);
    SmallString<64> resultStorage;
    llvm::raw_svector_ostream resultOutput(resultStorage);
    PrintingPolicy printingPolicy(cxtu::getASTUnit(translationUnit)->getASTContext().getLangOpts());
    
    qualifiedType.print(resultOutput, printingPolicy, std::string(placeholder, placeholderByteCount));

    return cxstring::createDup(resultOutput.str());
}

//-------------------------------------------------------------------------------------------------
// Enumerate child declarations directly
//-------------------------------------------------------------------------------------------------
// libclang normally only enumerates cursors for declarations defined in source, this enumerats all of them regardless.

PATHOGEN_EXPORT CXCursor pathogen_BeginEnumerateDeclarationsRaw(CXCursor cursor)
{
    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        return clang_getNullCursor();
    }

    // Get the declaration context
    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const DeclContext* declContext = dyn_cast_or_null<DeclContext>(declaration);

    // The cursor must be a declaration context
    if (declContext == nullptr || declContext->decls_empty())
    {
        return clang_getNullCursor();
    }

    // Get the start of the declaration list
    const Decl* child = *declContext->decls_begin();
    CXTranslationUnit translationUnit = cxcursor::getCursorTU(cursor);
    return cxcursor::MakeCXCursor(child, translationUnit);
}

PATHOGEN_EXPORT CXCursor pathogen_EnumerateDeclarationsRawMoveNext(CXCursor cursor)
{
    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        return clang_getNullCursor();
    }

    // Get the next sibling declaration
    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const Decl* sibling = declaration->getNextDeclInContext();

    if (sibling == nullptr)
    {
        return clang_getNullCursor();
    }

    CXTranslationUnit translationUnit = cxcursor::getCursorTU(cursor);
    return cxcursor::MakeCXCursor(sibling, translationUnit, SourceRange(), /* FirstInDeclGroup */ false);
}

//-------------------------------------------------------------------------------------------------
// Function declaration/type callability
//-------------------------------------------------------------------------------------------------

CXStringSet* pathogen_CreateSingleDiagnosticStringSet(const char* diagnostic)
{
    std::vector<std::string> diagnostics;
    diagnostics.push_back(diagnostic);
    return cxstring::createSet(diagnostics);
}

CXStringSet* pathogen_IsFunctionTypeCallable(CXTranslationUnit translationUnit, const FunctionProtoType* functionType)
{
    bool isCallable = true;
    ASTUnit* astUnit = cxtu::getASTUnit(translationUnit);
    Sema& semanticModel = astUnit->getSema();

    // We don't have a sensible location to use within this function, so we just use the start of the main file.
    // Generally the source location is just passed through to the diagnoser we defined below and we don't use it.
    // However, in certain situations it can be passed into other parts of Clang (such as the implicit template initializer) and they expect it to be valid.
    SourceLocation sourceLocation = astUnit->getStartOfMainFileID();

    // This captures the diagnostics for incomplete types in some situations
    // (RequireCompleteType will emit informational diagnostics to highlight forward declarations and such. We unfortunately can't disable this.)
    class Diagnoser : public Sema::TypeDiagnoser
    {
        private:
            PrintingPolicy printingPolicy;
        public:
            bool IsHandlingParameters = false;
            std::vector<std::string> Diagnostics;
            bool DiagnosticWasReceived = false;

            Diagnoser(ASTUnit* astUnit)
                : printingPolicy(astUnit->getASTContext().getLangOpts())
            {
            }

        private:
            void emitDiagnostic(QualType type)
            {
                DiagnosticWasReceived = true;
                SmallString<64> diagnosticStorage;
                llvm::raw_svector_ostream diagnostic(diagnosticStorage);

                if (!IsHandlingParameters)
                {
                    diagnostic << "Return type '";
                }
                else
                {
                    diagnostic << "Argument type '";
                }

                type.print(diagnostic, printingPolicy);
                diagnostic << "' is incomplete.";

                Diagnostics.push_back((std::string)diagnostic.str());
            }

        public:
            void diagnose(Sema& semanticModel, SourceLocation sourceLocation, QualType type) override
            {
                emitDiagnostic(type);
            }

            void ensureDiagnosticEmitted(QualType type)
            {
                if (!DiagnosticWasReceived)
                {
                    emitDiagnostic(type);
                }

                DiagnosticWasReceived = false;
            }
    } diagnoser(astUnit);

    // Check the return type
    // When a function call happens in Clang, this is validated by Sema::CheckCallReturnType.
    // We don't use it directly because it requires a CallExpr, which we obviously don't have, but the implementation is simple so we duplicat its logic below.
    {
        QualType returnType = functionType->getReturnType();

        // Void is always allowed for return types despite being incomplete
        if (returnType->isVoidType())
        { }
        // Complete types are always allowed for return types
        else if (!returnType->isIncompleteType())
        { }
        // Require the type to be complete
        // This gives the semantic model a final chance to complete the type for things like implicitly instantiated tempaltes
        // (RequireCompleteType returns true upon failure.)
        else if (semanticModel.RequireCompleteType(sourceLocation, returnType, diagnoser))
        {
            diagnoser.ensureDiagnosticEmitted(returnType);
            isCallable = false;
        }
    }

    // Check the parameter types
    diagnoser.IsHandlingParameters = true;
    for (const QualType parameterType : functionType->param_types())
    {
        diagnoser.DiagnosticWasReceived = false;

        // There's not a single place where Clang handles checking whether a type is complete for an argument because the source of the argument value is usually what goes bang well before
        // the function call expression is even processed. There are some cases where this doesn't happen though, and Sema::RequireCompleteType is what handles those cases.
        // (It's also probably handles things like variable declaration expressions too, but I did not check.)
        if (semanticModel.RequireCompleteType(sourceLocation, parameterType, diagnoser))
        {
            diagnoser.ensureDiagnosticEmitted(parameterType);
            isCallable = false;
        }
    }

    // If we're callable there should not be diagnositcs.
    // If we're not there should be.
    if (isCallable)
    {
        assert(diagnoser.Diagnostics.size() == 0);
    }
    else
    {
        assert(diagnoser.Diagnostics.size() > 0);
    }

    return isCallable ? nullptr : cxstring::createSet(diagnoser.Diagnostics);
}

CXStringSet* pathogen_IsFunctionCallable(CXTranslationUnit translationUnit, const FunctionDecl* function)
{
    const FunctionProtoType* functionType = function->getType()->getAs<FunctionProtoType>();

    if (functionType == nullptr)
    {
        assert(false && "FunctionDecl should be a FunctionProtoType");
        return pathogen_CreateSingleDiagnosticStringSet("The specified function is not a FunctionProtoType.");
    }
    
    return pathogen_IsFunctionTypeCallable(translationUnit, functionType);
}

PATHOGEN_EXPORT CXStringSet* pathogen_IsFunctionCallable(CXCursor cursor)
{
    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const FunctionDecl* functionDeclaration = dyn_cast_or_null<FunctionDecl>(declaration);

    if (functionDeclaration == nullptr)
    {
        assert(false && "The specified cursor must refer to a FunctionDecl.");
        return pathogen_CreateSingleDiagnosticStringSet("The specified cursor is not a FunctionDecl.");
    }

    return pathogen_IsFunctionCallable(cxcursor::getCursorTU(cursor), functionDeclaration);
}

PATHOGEN_EXPORT CXStringSet* pathogen_IsFunctionTypeCallable(CXType type)
{
    QualType qualifiedType = QualType::getFromOpaquePtr(type.data[0]);

    if (qualifiedType.isNull())
    {
        assert(false && "The type is null.");
        return pathogen_CreateSingleDiagnosticStringSet("The specified type is null.");
    }

    const FunctionProtoType* functionType = qualifiedType->getAs<FunctionProtoType>();

    if (functionType == nullptr)
    {
        assert(false && "The specified type must refer to a FunctionProtoType.");
        return pathogen_CreateSingleDiagnosticStringSet("The specified type is not a FunctionProtoType.");
    }

    return pathogen_IsFunctionTypeCallable(static_cast<CXTranslationUnit>(type.data[1]), functionType);
}

//-------------------------------------------------------------------------------------------------
// Code Generation
//-------------------------------------------------------------------------------------------------

struct PathogenCodeGenerator
{
    llvm::LLVMContext* LlvmContext;
    CodeGenerator* CodeGenerator;
};

PATHOGEN_EXPORT void pathogen_CreateCodeGenerator(CXTranslationUnit translationUnit, PathogenCodeGenerator* codeGenerator)
{
    assert(codeGenerator->LlvmContext == nullptr);
    assert(codeGenerator->CodeGenerator == nullptr);

    ASTUnit* astUnit = cxtu::getASTUnit(translationUnit);
    ASTContext& astContext = astUnit->getASTContext();
    const CompilerInvocation& invocation = astUnit->getCompilerInvocation();

    codeGenerator->LlvmContext = new llvm::LLVMContext();
    codeGenerator->CodeGenerator = CreateLLVMCodeGen
    (
        astUnit->getDiagnostics(),
        "ClangSharp.Pathogen",
        invocation.getHeaderSearchOpts(),
        invocation.getPreprocessorOpts(),
        invocation.getCodeGenOpts(),
        *codeGenerator->LlvmContext
    );
    codeGenerator->CodeGenerator->Initialize(astContext);
}

PATHOGEN_EXPORT void pathogen_DisposeCodeGenerator(PathogenCodeGenerator* codeGenerator)
{
    delete codeGenerator->CodeGenerator;
    delete codeGenerator->LlvmContext;

    codeGenerator->CodeGenerator = nullptr;
    codeGenerator->LlvmContext = nullptr;
}

enum class PathogenLlvmCallingConventionKind : uint8_t
{
    C = 0,
    Fast = 8,
    Cold = 9,
    GHC = 10,
    HiPE = 11,
    WebKit_JS = 12,
    AnyReg = 13,
    PreserveMost = 14,
    PreserveAll = 15,
    Swift = 16,
    CXX_FAST_TLS = 17,
    Tail = 18,
    CFGuard_Check = 19,
    FirstTargetCC = 64,
    X86_StdCall = 64,
    X86_FastCall = 65,
    ARM_APCS = 66,
    ARM_AAPCS = 67,
    ARM_AAPCS_VFP = 68,
    MSP430_INTR = 69,
    X86_ThisCall = 70,
    PTX_Kernel = 71,
    PTX_Device = 72,
    SPIR_FUNC = 75,
    SPIR_KERNEL = 76,
    Intel_OCL_BI = 77,
    X86_64_SysV = 78,
    Win64 = 79,
    X86_VectorCall = 80,
    HHVM = 81,
    HHVM_C = 82,
    X86_INTR = 83,
    AVR_INTR = 84,
    AVR_SIGNAL = 85,
    AVR_BUILTIN = 86,
    AMDGPU_VS = 87,
    AMDGPU_GS = 88,
    AMDGPU_PS = 89,
    AMDGPU_CS = 90,
    AMDGPU_KERNEL = 91,
    X86_RegCall = 92,
    AMDGPU_HS = 93,
    MSP430_BUILTIN = 94,
    AMDGPU_LS = 95,
    AMDGPU_ES = 96,
    AArch64_VectorCall = 97,
    AArch64_SVE_VectorCall = 98,
    WASM_EmscriptenInvoke = 99
};
#define verify_llvm_calling_convention_kind(KIND) static_assert((int)(PathogenLlvmCallingConventionKind::KIND) == ((int)llvm::CallingConv::KIND), "LLVM " #KIND " must match Pathogen " #KIND);
verify_llvm_calling_convention_kind(C);
verify_llvm_calling_convention_kind(Fast);
verify_llvm_calling_convention_kind(Cold);
verify_llvm_calling_convention_kind(GHC);
verify_llvm_calling_convention_kind(HiPE);
verify_llvm_calling_convention_kind(WebKit_JS);
verify_llvm_calling_convention_kind(AnyReg);
verify_llvm_calling_convention_kind(PreserveMost);
verify_llvm_calling_convention_kind(PreserveAll);
verify_llvm_calling_convention_kind(Swift);
verify_llvm_calling_convention_kind(CXX_FAST_TLS);
verify_llvm_calling_convention_kind(Tail);
verify_llvm_calling_convention_kind(CFGuard_Check);
verify_llvm_calling_convention_kind(FirstTargetCC);
verify_llvm_calling_convention_kind(X86_StdCall);
verify_llvm_calling_convention_kind(X86_FastCall);
verify_llvm_calling_convention_kind(ARM_APCS);
verify_llvm_calling_convention_kind(ARM_AAPCS);
verify_llvm_calling_convention_kind(ARM_AAPCS_VFP);
verify_llvm_calling_convention_kind(MSP430_INTR);
verify_llvm_calling_convention_kind(X86_ThisCall);
verify_llvm_calling_convention_kind(PTX_Kernel);
verify_llvm_calling_convention_kind(PTX_Device);
verify_llvm_calling_convention_kind(SPIR_FUNC);
verify_llvm_calling_convention_kind(SPIR_KERNEL);
verify_llvm_calling_convention_kind(Intel_OCL_BI);
verify_llvm_calling_convention_kind(X86_64_SysV);
verify_llvm_calling_convention_kind(Win64);
verify_llvm_calling_convention_kind(X86_VectorCall);
verify_llvm_calling_convention_kind(HHVM);
verify_llvm_calling_convention_kind(HHVM_C);
verify_llvm_calling_convention_kind(X86_INTR);
verify_llvm_calling_convention_kind(AVR_INTR);
verify_llvm_calling_convention_kind(AVR_SIGNAL);
verify_llvm_calling_convention_kind(AVR_BUILTIN);
verify_llvm_calling_convention_kind(AMDGPU_VS);
verify_llvm_calling_convention_kind(AMDGPU_GS);
verify_llvm_calling_convention_kind(AMDGPU_PS);
verify_llvm_calling_convention_kind(AMDGPU_CS);
verify_llvm_calling_convention_kind(AMDGPU_KERNEL);
verify_llvm_calling_convention_kind(X86_RegCall);
verify_llvm_calling_convention_kind(AMDGPU_HS);
verify_llvm_calling_convention_kind(MSP430_BUILTIN);
verify_llvm_calling_convention_kind(AMDGPU_LS);
verify_llvm_calling_convention_kind(AMDGPU_ES);
verify_llvm_calling_convention_kind(AArch64_VectorCall);
verify_llvm_calling_convention_kind(AArch64_SVE_VectorCall);
verify_llvm_calling_convention_kind(WASM_EmscriptenInvoke);

enum class PathogenClangCallingConventionKind : uint8_t
{
    C,
    X86StdCall,
    X86FastCall,
    X86ThisCall,
    X86VectorCall,
    X86Pascal,
    Win64,
    X86_64SysV,
    X86RegCall,
    AAPCS,
    AAPCS_VFP,
    IntelOclBicc,
    SpirFunction,
    OpenCLKernel,
    Swift,
    PreserveMost,
    PreserveAll,
    AArch64VectorCall,
};
#define verify_clang_calling_convention_kind(PATHOGEN_KIND, CLANG_KIND) static_assert((int)(PathogenClangCallingConventionKind::PATHOGEN_KIND) == ((int)clang::CLANG_KIND), "Clang " #CLANG_KIND " must match Pathogen " #PATHOGEN_KIND);
verify_clang_calling_convention_kind(C, CC_C);
verify_clang_calling_convention_kind(X86StdCall, CC_X86StdCall);
verify_clang_calling_convention_kind(X86FastCall, CC_X86FastCall);
verify_clang_calling_convention_kind(X86ThisCall, CC_X86ThisCall);
verify_clang_calling_convention_kind(X86VectorCall, CC_X86VectorCall);
verify_clang_calling_convention_kind(X86Pascal, CC_X86Pascal);
verify_clang_calling_convention_kind(Win64, CC_Win64);
verify_clang_calling_convention_kind(X86_64SysV, CC_X86_64SysV);
verify_clang_calling_convention_kind(X86RegCall, CC_X86RegCall);
verify_clang_calling_convention_kind(AAPCS, CC_AAPCS);
verify_clang_calling_convention_kind(AAPCS_VFP, CC_AAPCS_VFP);
verify_clang_calling_convention_kind(IntelOclBicc, CC_IntelOclBicc);
verify_clang_calling_convention_kind(SpirFunction, CC_SpirFunction);
verify_clang_calling_convention_kind(OpenCLKernel, CC_OpenCLKernel);
verify_clang_calling_convention_kind(Swift, CC_Swift);
verify_clang_calling_convention_kind(PreserveMost, CC_PreserveMost);
verify_clang_calling_convention_kind(PreserveAll, CC_PreserveAll);
verify_clang_calling_convention_kind(AArch64VectorCall, CC_AArch64VectorCall);

enum class PathogenArrangedFunctionFlags : uint16_t
{
    None = 0,
    IsInstanceMethod = 1,
    IsChainCall = 2,
    IsNoReturn = 4,
    IsReturnsRetained = 8,
    IsNoCallerSavedRegs = 16,
    HasRegParm = 32,
    IsNoCfCheck = 64,
    IsVariadic = 128,
    UsesInAlloca = 256,
    HasExtendedParameterInfo = 512,
};
PATHOGEN_FLAGS(PathogenArrangedFunctionFlags);

enum class PathogenArgumentKind : uint8_t
{
    Direct,
    Extend,
    Indirect,
    IndirectAliased,
    Ignore,
    Expand,
    CoerceAndExpand,
    InAlloca
};
#define verify_argument_kind(KIND) static_assert((int)(PathogenArgumentKind::KIND) == ((int)ABIArgInfo::KIND), "Clang argument kind " #KIND " must match Pathogen kind " #KIND);
verify_argument_kind(Direct);
verify_argument_kind(Extend);
verify_argument_kind(Indirect);
verify_argument_kind(IndirectAliased);
verify_argument_kind(Ignore);
verify_argument_kind(Expand);
verify_argument_kind(CoerceAndExpand);
verify_argument_kind(InAlloca);
static_assert(ABIArgInfo::KindFirst == ABIArgInfo::Direct, "Direct must be the final ABI argument kind.");
static_assert(ABIArgInfo::KindLast == ABIArgInfo::InAlloca, "InAlloca must be the final ABI argument kind.");

enum class PathogenArgumentFlags : uint16_t
{
    None = 0,
    // Requires Kind = Direct, Extend, or CoerceAndExpand
    HasCoerceToTypeType = 1,
    // Requires Kind = Direct, Extend, Indirect, or Expand
    HasPaddingType = 2,
    // Requires Kind = CoerceAndExpand
    HasUnpaddedCoerceAndExpandType = 4,
    // Applies to any kind
    PaddingInRegister = 8,
    // Requires Kind = InAlloca
    IsInAllocaSRet = 16,
    // Requires Kind = Indirect
    IsIndirectByVal = 32,
    // Requires Kind = Indirect
    IsIndirectRealign = 64,
    // Requires Kind = Indirect
    IsSRetAfterThis = 128,
    // Requires Kind = Direct, Extend, or Indirect
    IsInRegister = 256,
    // Requires Kind = Direct
    CanBeFlattened = 512,
    // Requires Kind = Extend
    IsSignExtended = 1024
};
PATHOGEN_FLAGS(PathogenArgumentFlags);

struct PathogenArgumentInfo
{
    CXType Type;
    PathogenArgumentKind Kind;
    // Not exposing ABIArgInfo::TypeData
    // ABIArgInfo::PaddingType and UnpaddedCoerceAndExpandType are exposed as on/off flags for now until we find a use for them.

    PathogenArgumentFlags Flags;

    // For Kind = Direct or Extend: DirectOffset
    // For Kind = Inidrect or IndirectAliased: IndirectAlignment
    // For Kind = InAlloca: AllocaFieldIndex
    uint32_t Extra;

    // For Kind = IndirectAliased: IndirectAddrSpace
    uint32_t Extra2;
};

struct PathogenArrangedFunction
{
    PathogenLlvmCallingConventionKind CallingConvention;
    PathogenLlvmCallingConventionKind EffectiveCallingConvention;
    PathogenClangCallingConventionKind AstCallingConvention;
    PathogenArrangedFunctionFlags Flags;
    uint32_t RequiredArgumentCount;
    uint32_t ArgumentsPassedInRegisterCount;
    uint32_t ArgumentCount;
    PathogenArgumentInfo ReturnInfo;
};

static void pathogen_CreateArgumentInfo(CXTranslationUnit translationUnit, CanQualType type, const ABIArgInfo& info, PathogenArgumentInfo* output)
{
    output->Type = cxtype::MakeCXType(type, translationUnit);
    output->Kind = (PathogenArgumentKind)info.getKind();
    output->Flags = PathogenArgumentFlags::None;
    output->Extra = 0;

    if (info.canHaveCoerceToType() && info.getCoerceToType() != nullptr)
    {
        output->Flags |= PathogenArgumentFlags::HasCoerceToTypeType;
    }

    if (info.getPaddingType() != nullptr)
    {
        output->Flags |= PathogenArgumentFlags::HasPaddingType;
    }

    if ((info.isDirect() || info.isExtend() || info.isIndirect()) && info.getInReg())
    {
        output->Flags |= PathogenArgumentFlags::IsInRegister;
    }

    switch (info.getKind())
    {
        case ABIArgInfo::Direct:
            if (info.getCanBeFlattened())
            {
                output->Flags |= PathogenArgumentFlags::CanBeFlattened;
            }

            output->Extra = info.getDirectOffset();
            break;
        case ABIArgInfo::Extend:
            if (info.isSignExt())
            {
                output->Flags |= PathogenArgumentFlags::IsSignExtended;
            }
            
            output->Extra = info.getDirectOffset();
            break;
        case ABIArgInfo::Indirect:
            if (info.getIndirectByVal())
            {
                output->Flags |= PathogenArgumentFlags::IsIndirectByVal;
            }

            if (info.getIndirectRealign())
            {
                output->Flags |= PathogenArgumentFlags::IsIndirectRealign;
            }

            if (info.isSRetAfterThis())
            {
                output->Flags |= PathogenArgumentFlags::IsSRetAfterThis;
            }

            output->Extra = info.getIndirectAlign().getQuantity();
            break;
        case ABIArgInfo::IndirectAliased:
            if (info.getIndirectRealign())
            {
                output->Flags |= PathogenArgumentFlags::IsIndirectRealign;
            }

            output->Extra = info.getIndirectAlign().getQuantity();
            output->Extra2 = info.getIndirectAddrSpace();
            break;
        case ABIArgInfo::Ignore:
        case ABIArgInfo::Expand:
            break;
        case ABIArgInfo::CoerceAndExpand:
            if (info.getUnpaddedCoerceAndExpandType())
            {
                output->Flags |= PathogenArgumentFlags::HasUnpaddedCoerceAndExpandType;
            }
            break;
        case ABIArgInfo::InAlloca:
            if (info.getInAllocaSRet())
            {
                output->Flags |= PathogenArgumentFlags::IsInAllocaSRet;
            }

            output->Extra = info.getInAllocaFieldIndex();
            break;
    }
}

static PathogenArrangedFunction* pathogen_CreateArrangedFunction(CXTranslationUnit translationUnit, const CodeGen::CGFunctionInfo& function)
{
    ArrayRef<CodeGen::CGFunctionInfoArgInfo> arguments = function.arguments();

    // Allocate the Pathogen representation of the arranged function
    PathogenArrangedFunction* result = (PathogenArrangedFunction*)malloc(sizeof(PathogenArrangedFunction) + (sizeof(PathogenArgumentInfo) * arguments.size()));
    PathogenArgumentInfo* resultArguments = (PathogenArgumentInfo*)&result[1];

    // Populate the result
    result->CallingConvention = (PathogenLlvmCallingConventionKind)function.getCallingConvention();
    result->EffectiveCallingConvention = (PathogenLlvmCallingConventionKind)function.getEffectiveCallingConvention();
    result->AstCallingConvention = (PathogenClangCallingConventionKind)function.getASTCallingConvention();
    result->RequiredArgumentCount = function.getNumRequiredArgs();
    result->ArgumentsPassedInRegisterCount = function.getRegParm();
    result->ArgumentCount = (uint32_t)arguments.size();
    pathogen_CreateArgumentInfo(translationUnit, function.getReturnType(), function.getReturnInfo(), &result->ReturnInfo);

    for (size_t i = 0; i < arguments.size(); i++)
    {
        pathogen_CreateArgumentInfo(translationUnit, arguments[i].type, arguments[i].info, &resultArguments[i]);
    }

    // Populate the function flags
    result->Flags = PathogenArrangedFunctionFlags::None;

    if (function.isInstanceMethod())
    { result->Flags |= PathogenArrangedFunctionFlags::IsInstanceMethod; }

    if (function.isChainCall())
    { result->Flags |= PathogenArrangedFunctionFlags::IsChainCall; }

    if (function.isNoReturn())
    { result->Flags |= PathogenArrangedFunctionFlags::IsNoReturn; }

    if (function.isReturnsRetained())
    { result->Flags |= PathogenArrangedFunctionFlags::IsReturnsRetained; }

    if (function.isNoCallerSavedRegs())
    { result->Flags |= PathogenArrangedFunctionFlags::IsNoCallerSavedRegs; }

    if (function.getHasRegParm())
    { result->Flags |= PathogenArrangedFunctionFlags::HasRegParm; }

    if (function.isNoCfCheck())
    { result->Flags |= PathogenArrangedFunctionFlags::IsNoCfCheck; }

    if (function.isVariadic())
    { result->Flags |= PathogenArrangedFunctionFlags::IsVariadic; }

    if (function.usesInAlloca())
    { result->Flags |= PathogenArrangedFunctionFlags::UsesInAlloca; }

    if (function.getExtParameterInfos().size() > 0)
    { result->Flags |= PathogenArrangedFunctionFlags::HasExtendedParameterInfo; }

    return result;
}

PATHOGEN_EXPORT PathogenArrangedFunction* pathogen_GetArrangedFunction(PathogenCodeGenerator* codeGenerator, CXCursor cursor)
{
    CXTranslationUnit translationUnit = clang_Cursor_getTranslationUnit(cursor);

    // The cursor must be a declaration
    if (!clang_isDeclaration(cursor.kind))
    {
        assert(false && "The cursor must be a declaration.");
        return nullptr;
    }

    // Get the function declaration
    const Decl* declaration = cxcursor::getCursorDecl(cursor);
    const FunctionDecl* functionDeclaration = dyn_cast_or_null<FunctionDecl>(declaration);
    const CXXConstructorDecl* constructorDeclaration = dyn_cast_or_null<CXXConstructorDecl>(declaration);
    const CXXDestructorDecl* destructorDeclaration = dyn_cast_or_null<CXXDestructorDecl>(declaration);

    // Build the global declaration
    GlobalDecl globalDeclaration;

    if (constructorDeclaration != nullptr)
    {
        assert(functionDeclaration != nullptr); // Sanity check that constructors are functions
        globalDeclaration = GlobalDecl(constructorDeclaration, Ctor_Complete); //TODO: Allow changing constructor type
    }
    else if (destructorDeclaration != nullptr)
    {
        assert(functionDeclaration != nullptr); // Sanity check taht destructors are functions
        globalDeclaration = GlobalDecl(destructorDeclaration, Dtor_Complete); //TODO: Allow changing destructor type
    }
    else if (functionDeclaration != nullptr)
    {
        globalDeclaration = GlobalDecl(functionDeclaration);
    }
    else
    {
        assert(false && "The cursor must be a function declaration.");
        return nullptr;
    }

    // Arrange the function
    const CodeGen::CGFunctionInfo& function = codeGenerator->CodeGenerator->CGM().getTypes().arrangeGlobalDeclaration(globalDeclaration);
    return pathogen_CreateArrangedFunction(translationUnit, function);
}

PATHOGEN_EXPORT PathogenArrangedFunction* pathogen_GetArrangedFunctionPointer(PathogenCodeGenerator* codeGenerator, CXType type)
{
    QualType qualifiedType = QualType::getFromOpaquePtr(type.data[0]);

    if (qualifiedType.isNull())
    {
        assert(false && "The type is null.");
        return nullptr;
    }

    CXTranslationUnit translationUnit = static_cast<CXTranslationUnit>(type.data[1]);

    // Get the function pointer type
    assert(isa<FunctionType>(qualifiedType) && "The type must be a function type");
    const FunctionProtoType* functionType = qualifiedType->getAs<FunctionProtoType>();

    if (functionType == nullptr)
    {
        assert(false && "Only FunctionProtoType function types are supported.");
        return nullptr;
    }

    CanQualType canQualFunctionType = functionType->getCanonicalTypeUnqualified();
    assert(isa<FunctionProtoType>(canQualFunctionType));

    // Arrange the function
    const CodeGen::CGFunctionInfo& function = codeGenerator->CodeGenerator->CGM().getTypes().arrangeFreeFunctionType(canQualFunctionType.castAs<FunctionProtoType>());
    return pathogen_CreateArrangedFunction(translationUnit, function);
}

PATHOGEN_EXPORT void pathogen_DisposeArrangedFunction(PathogenArrangedFunction* function)
{
    free(function);
}

//-------------------------------------------------------------------------------------------------
// Interop Verification
//-------------------------------------------------------------------------------------------------

struct PathogenTypeSizes
{
    int PathogenTypeSizes;
    int PathogenRecordLayout;
    int PathogenRecordField;
    int PathogenVTable;
    int PathogenVTableEntry;
    int PathogenOperatorOverloadInfo;
    int PathogenConstantString;
    int PathogenConstantValueInfo;
    int PathogenMacroInformation;
    int PathogenTemplateInstantiationMetrics;
    int PathogenCodeGenerator;
    int PathogenArgumentInfo;
    int PathogenArrangedFunction;
};

//! Returns true if the sizes were populated, false if sizes->PathogenTypeSizes was invalid.
//! sizes->PathogenTypeSizes must be set to sizeof(PathogenTypeSizes)
PATHOGEN_EXPORT interop_bool pathogen_GetTypeSizes(PathogenTypeSizes* sizes)
{
    // Can't populate if the destination struct is the wrong size.
    if (sizes->PathogenTypeSizes != sizeof(PathogenTypeSizes))
    {
        return false;
    }

    sizes->PathogenRecordLayout = sizeof(PathogenRecordLayout);
    sizes->PathogenRecordField = sizeof(PathogenRecordField);
    sizes->PathogenVTable = sizeof(PathogenVTable);
    sizes->PathogenVTableEntry = sizeof(PathogenVTableEntry);
    sizes->PathogenOperatorOverloadInfo = sizeof(PathogenOperatorOverloadInfo);
    sizes->PathogenConstantString = sizeof(PathogenConstantString);
    sizes->PathogenConstantValueInfo = sizeof(PathogenConstantValueInfo);
    sizes->PathogenMacroInformation = sizeof(PathogenMacroInformation);
    sizes->PathogenTemplateInstantiationMetrics = sizeof(PathogenTemplateInstantiationMetrics);
    sizes->PathogenCodeGenerator = sizeof(PathogenCodeGenerator);
    sizes->PathogenArgumentInfo = sizeof(PathogenArgumentInfo);
    sizes->PathogenArrangedFunction = sizeof(PathogenArrangedFunction);
    return true;
}
