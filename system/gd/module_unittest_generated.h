// automatically generated by the FlatBuffers compiler, do not modify

#ifndef FLATBUFFERS_GENERATED_MODULEUNITTEST_BLUETOOTH_H_
#define FLATBUFFERS_GENERATED_MODULEUNITTEST_BLUETOOTH_H_

#include "flatbuffers/flatbuffers.h"

namespace bluetooth {

struct ModuleUnitTestData;
struct ModuleUnitTestDataBuilder;

struct ModuleUnitTestData FLATBUFFERS_FINAL_CLASS : private flatbuffers::Table {
  typedef ModuleUnitTestDataBuilder Builder;
  enum FlatBuffersVTableOffset FLATBUFFERS_VTABLE_UNDERLYING_TYPE { VT_TITLE = 4 };
  const flatbuffers::String* title() const {
    return GetPointer<const flatbuffers::String*>(VT_TITLE);
  }
  bool Verify(flatbuffers::Verifier& verifier) const {
    return VerifyTableStart(verifier) && VerifyOffset(verifier, VT_TITLE) && verifier.VerifyString(title()) &&
           verifier.EndTable();
  }
};

struct ModuleUnitTestDataBuilder {
  typedef ModuleUnitTestData Table;
  flatbuffers::FlatBufferBuilder& fbb_;
  flatbuffers::uoffset_t start_;
  void add_title(flatbuffers::Offset<flatbuffers::String> title) {
    fbb_.AddOffset(ModuleUnitTestData::VT_TITLE, title);
  }
  explicit ModuleUnitTestDataBuilder(flatbuffers::FlatBufferBuilder& _fbb) : fbb_(_fbb) {
    start_ = fbb_.StartTable();
  }
  ModuleUnitTestDataBuilder& operator=(const ModuleUnitTestDataBuilder&);
  flatbuffers::Offset<ModuleUnitTestData> Finish() {
    const auto end = fbb_.EndTable(start_);
    auto o = flatbuffers::Offset<ModuleUnitTestData>(end);
    return o;
  }
};

inline flatbuffers::Offset<ModuleUnitTestData> CreateModuleUnitTestData(
    flatbuffers::FlatBufferBuilder& _fbb, flatbuffers::Offset<flatbuffers::String> title = 0) {
  ModuleUnitTestDataBuilder builder_(_fbb);
  builder_.add_title(title);
  return builder_.Finish();
}

inline flatbuffers::Offset<ModuleUnitTestData> CreateModuleUnitTestDataDirect(
    flatbuffers::FlatBufferBuilder& _fbb, const char* title = nullptr) {
  auto title__ = title ? _fbb.CreateString(title) : 0;
  return bluetooth::CreateModuleUnitTestData(_fbb, title__);
}

inline const bluetooth::ModuleUnitTestData* GetModuleUnitTestData(const void* buf) {
  return flatbuffers::GetRoot<bluetooth::ModuleUnitTestData>(buf);
}

inline const bluetooth::ModuleUnitTestData* GetSizePrefixedModuleUnitTestData(const void* buf) {
  return flatbuffers::GetSizePrefixedRoot<bluetooth::ModuleUnitTestData>(buf);
}

inline bool VerifyModuleUnitTestDataBuffer(flatbuffers::Verifier& verifier) {
  return verifier.VerifyBuffer<bluetooth::ModuleUnitTestData>(nullptr);
}

inline bool VerifySizePrefixedModuleUnitTestDataBuffer(flatbuffers::Verifier& verifier) {
  return verifier.VerifySizePrefixedBuffer<bluetooth::ModuleUnitTestData>(nullptr);
}

inline void FinishModuleUnitTestDataBuffer(
    flatbuffers::FlatBufferBuilder& fbb, flatbuffers::Offset<bluetooth::ModuleUnitTestData> root) {
  fbb.Finish(root);
}

inline void FinishSizePrefixedModuleUnitTestDataBuffer(
    flatbuffers::FlatBufferBuilder& fbb, flatbuffers::Offset<bluetooth::ModuleUnitTestData> root) {
  fbb.FinishSizePrefixed(root);
}

}  // namespace bluetooth

#endif  // FLATBUFFERS_GENERATED_MODULEUNITTEST_BLUETOOTH_H_
