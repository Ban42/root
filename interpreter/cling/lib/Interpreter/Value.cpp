//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Axel Naumann <axel@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "cling/Interpreter/Value.h"

#include "EnterUserCodeRAII.h"

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/Utils/AST.h"
#include "cling/Utils/Casting.h"
#include "cling/Utils/Output.h"
#include "cling/Utils/UTF8.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/CanonicalType.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Type.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Overload.h"
#include "clang/Sema/Sema.h"

#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_os_ostream.h"

#include <cstring>

namespace {

  ///\brief The allocation starts with this layout; it is followed by the
  ///  value's object at m_Payload. This class does not inherit from
  ///  llvm::RefCountedBase because deallocation cannot use this type but must
  ///  free the character array.

  class AllocatedValue {
  public:
    typedef void (*DtorFunc_t)(void*);

  private:
    ///\brief The reference count - once 0, this object will be deallocated.
    mutable unsigned m_RefCnt;

    ///\brief The destructor function.
    DtorFunc_t m_DtorFunc;

    ///\brief The size of the allocation (for arrays)
    unsigned long m_AllocSize;

    ///\brief The number of elements in the array
    unsigned long m_NElements;

    ///\brief The start of the allocation.
    char m_Payload[1];

    static const unsigned char kCanaryUnconstructedObject[8];

    ///\brief Return whether the contained object has been constructed,
    /// or rather, whether the canary has been changed.
    bool IsAlive() const
    {
      // If the canary values are still there
      return (std::memcmp(getPayload(), kCanaryUnconstructedObject,
                          sizeof(kCanaryUnconstructedObject)) != 0);
    }

    ///\brief Initialize the storage management part of the allocated object.
    ///  The allocator is referencing it, thus initialize m_RefCnt with 1.
    ///\param [in] dtorFunc - the function to be called before deallocation.
    AllocatedValue(void* dtorFunc, size_t allocSize, size_t nElements):
      m_RefCnt(1),
      m_DtorFunc(cling::utils::VoidToFunctionPtr<DtorFunc_t>(dtorFunc)),
      m_AllocSize(allocSize), m_NElements(nElements)
    {}

  public:
    ///\brief Allocate the memory needed by the AllocatedValue managing
    /// an object of payloadSize bytes, and return the address of the
    /// payload object.
    static char* CreatePayload(unsigned payloadSize, void* dtorFunc,
                               size_t nElements) {
      if (payloadSize < sizeof(kCanaryUnconstructedObject))
        payloadSize = sizeof(kCanaryUnconstructedObject);
      char* alloc = new char[AllocatedValue::getPayloadOffset() + payloadSize];
      AllocatedValue* allocVal
        = new (alloc) AllocatedValue(dtorFunc, payloadSize, nElements);
      std::memcpy(allocVal->getPayload(), kCanaryUnconstructedObject,
                  sizeof(kCanaryUnconstructedObject));
      return allocVal->getPayload();
    }

    const char* getPayload() const { return m_Payload; }
    char* getPayload() { return m_Payload; }

    static unsigned getPayloadOffset() {
      static const AllocatedValue Dummy(0,0,0);
      return Dummy.m_Payload - (const char*)&Dummy;
    }

    static AllocatedValue* getFromPayload(void* payload) {
      return
        reinterpret_cast<AllocatedValue*>((char*)payload - getPayloadOffset());
    }

    void Retain() { ++m_RefCnt; }

    ///\brief This object must be allocated as a char array. Deallocate it as
    ///   such.
    void Release() {
      assert (m_RefCnt > 0 && "Reference count is already zero.");
      if (--m_RefCnt == 0) {
        if (m_DtorFunc && IsAlive()) {
          assert(m_NElements && "No elements!");
          char* Payload = getPayload();
          const auto Skip = m_AllocSize / m_NElements;
          while (m_NElements-- != 0)
            (*m_DtorFunc)(Payload + m_NElements * Skip);
        }
        delete [] (char*)this;
      }
    }
  };

  const unsigned char AllocatedValue::kCanaryUnconstructedObject[8]
    = {0x4c, 0x37, 0xad, 0x8f, 0x2d, 0x23, 0x95, 0x91};
}

namespace cling {

  Value::Value(const Value& other):
    m_Storage(other.m_Storage), m_NeedsManagedAlloc(other.m_NeedsManagedAlloc),
    m_TypeKind(other.m_TypeKind),
    m_Type(other.m_Type), m_Interpreter(other.m_Interpreter) {
    if (other.needsManagedAllocation())
      AllocatedValue::getFromPayload(m_Storage.m_Ptr)->Retain();
  }

  static Value::TypeKind getCorrespondingTypeKind(clang::QualType QT) {
    using namespace clang;

    if (QT->isVoidType())
      return Value::kVoid;

    if (const auto *ET = dyn_cast<EnumType>(QT.getTypePtr()))
      QT = ET->getDecl()->getIntegerType();

    if (!QT->isBuiltinType() || QT->castAs<BuiltinType>()->isNullPtrType())
      return Value::kPtrOrObjTy;

    switch(QT->getAs<BuiltinType>()->getKind()) {
    default:
#ifndef NDEBUG
      QT->dump();
#endif // NDEBUG
      assert(false && "Type not supported");
      return Value::kInvalid;
#define X(type, name) \
      case BuiltinType::name: return Value::k##name;
      CLING_VALUE_BUILTIN_TYPES
#undef X
    }
  }

  Value::Value(clang::QualType clangTy, Interpreter& Interp):
    m_TypeKind(getCorrespondingTypeKind(clangTy)),
    m_Type(clangTy.getAsOpaquePtr()), // FIXME: What if clangTy is freed?
    m_Interpreter(&Interp) {
    if (m_TypeKind == Value::kPtrOrObjTy) {
      clang::QualType Canon = clangTy.getCanonicalType();
      if (Canon->isPointerType() || Canon->isObjectType() ||
          Canon->isReferenceType())
        if (Canon->isRecordType() || Canon->isConstantArrayType() ||
            Canon->isMemberPointerType())
          m_NeedsManagedAlloc = true;
    }

    if (needsManagedAllocation())
      ManagedAllocate();
  }

  Value& Value::operator =(const Value& other) {
    // Release old value.
    if (needsManagedAllocation())
      AllocatedValue::getFromPayload(m_Storage.m_Ptr)->Release();

    // Retain new one.
    m_Type = other.m_Type;
    m_Storage = other.m_Storage;
    m_NeedsManagedAlloc = other.m_NeedsManagedAlloc;
    m_TypeKind = other.m_TypeKind;
    m_Interpreter = other.m_Interpreter;
    if (needsManagedAllocation())
      AllocatedValue::getFromPayload(m_Storage.m_Ptr)->Retain();
    return *this;
  }

  Value& Value::operator =(Value&& other) {
    // Release old value.
    if (needsManagedAllocation())
      AllocatedValue::getFromPayload(m_Storage.m_Ptr)->Release();

    // Move new one.
    m_Type = other.m_Type;
    m_Storage = other.m_Storage;
    m_NeedsManagedAlloc = other.m_NeedsManagedAlloc;
    m_TypeKind = other.m_TypeKind;
    m_Interpreter = other.m_Interpreter;
    // Invalidate other so it will not release.
    other.m_NeedsManagedAlloc = false;
    other.m_TypeKind = kInvalid;

    return *this;
  }

  Value::~Value() {
    if (needsManagedAllocation())
      AllocatedValue::getFromPayload(m_Storage.m_Ptr)->Release();
  }

  clang::QualType Value::getType() const {
    return clang::QualType::getFromOpaquePtr(m_Type);
  }

  clang::ASTContext& Value::getASTContext() const {
    return m_Interpreter->getCI()->getASTContext();
  }

  static size_t GetNumberOfElements(clang::QualType QT) {
    if (const clang::ConstantArrayType* ArrTy
        = llvm::dyn_cast<clang::ConstantArrayType>(QT.getTypePtr())) {
      llvm::APInt arrSize(sizeof(size_t)*8, 1);
      do {
        arrSize *= ArrTy->getSize();
        ArrTy = llvm::dyn_cast<clang::ConstantArrayType>(ArrTy->getElementType()
                                                         .getTypePtr());
      } while (ArrTy);
      return static_cast<size_t>(arrSize.getZExtValue());
    }
    return 1;
  }

  void Value::ManagedAllocate() {
    assert(needsManagedAllocation() && "Does not need managed allocation");
    void* dtorFunc = 0;
    clang::QualType DtorType = getType();
    // For arrays we destruct the elements.
    if (const clang::ConstantArrayType* ArrTy
        = llvm::dyn_cast<clang::ConstantArrayType>(DtorType.getTypePtr())) {
      DtorType = ArrTy->getElementType();
    }
    if (const clang::RecordType* RTy = DtorType->getAs<clang::RecordType>()) {
      LockCompilationDuringUserCodeExecutionRAII LCDUCER(*m_Interpreter);
      dtorFunc = m_Interpreter->compileDtorCallFor(RTy->getDecl());
    }

    const clang::ASTContext& ctx = getASTContext();
    unsigned payloadSize = ctx.getTypeSizeInChars(getType()).getQuantity();
    m_Storage.m_Ptr = AllocatedValue::CreatePayload(payloadSize, dtorFunc,
                                                GetNumberOfElements(getType()));
  }

  void Value::AssertTypeMismatch(const char* Type) const {
#ifndef NDEBUG
    assert(isBuiltinType() && "Must be a builtin!");
    const clang::BuiltinType *BT = getType()->castAs<clang::BuiltinType>();
    clang::PrintingPolicy Policy = getASTContext().getPrintingPolicy();
#endif // NDEBUG
    assert(BT->getName(Policy) == Type);
  }

  static clang::QualType getCorrespondingBuiltin(clang::ASTContext &C,
                                                 clang::BuiltinType::Kind K) {
    using namespace clang;
    switch(K) {
    default:
      assert(false && "Type not supported");
      return {};
#define BUILTIN_TYPE(Id, SingletonId) \
      case BuiltinType::Id: return C.SingletonId;
#include "clang/AST/BuiltinTypes.def"
    }
  }

#define X(type, name)                                                   \
  template <> Value Value::Create(Interpreter& Interp, type val) {      \
    clang::ASTContext &C = Interp.getCI()->getASTContext();             \
    clang::BuiltinType::Kind K = clang::BuiltinType::name;              \
    Value res = Value(getCorrespondingBuiltin(C, K), Interp);           \
    res.set##name(val);                                                 \
    return res;                                                         \
  }                                                                     \

  CLING_VALUE_BUILTIN_TYPES

#undef X

  void Value::AssertOnUnsupportedTypeCast() const {
    assert("unsupported type in Value, cannot cast!" && 0);
  }

  namespace valuePrinterInternal {
    std::string printTypeInternal(const Value& V);
    std::string printValueInternal(const Value& V);
  } // end namespace valuePrinterInternal

  void Value::print(llvm::raw_ostream& Out, bool Escape) const {
    // Save the default type string representation so output can occur as one
    // operation (calling printValueInternal below may write to stderr).
    const std::string Type = valuePrinterInternal::printTypeInternal(*this);

    // Get the value string representation, by printValue() method overloading
    const std::string Val
      = cling::valuePrinterInternal::printValueInternal(*this);
    if (Escape) {
      const char* Data = Val.data();
      const size_t N = Val.size();
      switch (N ? Data[0] : 0) {
        case 'u': case 'U': case 'L':
          if (N < 3 || Data[1] != '\"')
            break;
          LLVM_FALLTHROUGH;
        case '\"':
          // Unicode string, encoded as Utf-8
          if (N > 2 && Data[N-1] == '\"') {
            // Drop the terminating " so Utf-8 errors can be detected ("\xeA")
            Out << Type << ' ';
            utils::utf8::EscapeSequence().encode(Data, N-1, Out) << "\"\n";
            return;
          }
          LLVM_FALLTHROUGH;
        default:
          break;
      }
    }
    Out << Type << ' ' << Val << '\n';
  }

  void Value::dump(bool Escape) const {
    print(cling::outs(), Escape);
  }
} // end namespace cling
