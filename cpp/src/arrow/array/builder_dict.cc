// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/array/builder_dict.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/status.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/hashing.h"
#include "arrow/util/logging.h"

namespace arrow {

using internal::checked_cast;

// ----------------------------------------------------------------------
// DictionaryBuilder

template <typename T>
class DictionaryBuilder<T>::MemoTableImpl
    : public internal::HashTraits<T>::MemoTableType {
 public:
  using MemoTableType = typename internal::HashTraits<T>::MemoTableType;
  using MemoTableType::MemoTableType;
};

template <typename T>
DictionaryBuilder<T>::~DictionaryBuilder() {}

template <typename T>
DictionaryBuilder<T>::DictionaryBuilder(const std::shared_ptr<DataType>& type,
                                        MemoryPool* pool)
    : ArrayBuilder(type, pool), byte_width_(-1), values_builder_(pool) {
  DCHECK_EQ(T::type_id, type->id()) << "inconsistent type passed to DictionaryBuilder";
}

DictionaryBuilder<NullType>::DictionaryBuilder(const std::shared_ptr<DataType>& type,
                                               MemoryPool* pool)
    : ArrayBuilder(type, pool), values_builder_(pool) {
  DCHECK_EQ(Type::NA, type->id()) << "inconsistent type passed to DictionaryBuilder";
}

template <>
DictionaryBuilder<FixedSizeBinaryType>::DictionaryBuilder(
    const std::shared_ptr<DataType>& type, MemoryPool* pool)
    : ArrayBuilder(type, pool),
      byte_width_(checked_cast<const FixedSizeBinaryType&>(*type).byte_width()) {}

template <typename T>
void DictionaryBuilder<T>::Reset() {
  ArrayBuilder::Reset();
  values_builder_.Reset();
  memo_table_.reset();
  delta_offset_ = 0;
}

template <typename T>
Status DictionaryBuilder<T>::Resize(int64_t capacity) {
  RETURN_NOT_OK(CheckCapacity(capacity, capacity_));
  capacity = std::max(capacity, kMinBuilderCapacity);

  if (capacity_ == 0) {
    // Initialize hash table
    // XXX should we let the user pass additional size heuristics?
    memo_table_.reset(new MemoTableImpl(0));
    delta_offset_ = 0;
  }
  RETURN_NOT_OK(values_builder_.Resize(capacity));
  return ArrayBuilder::Resize(capacity);
}

Status DictionaryBuilder<NullType>::Resize(int64_t capacity) {
  RETURN_NOT_OK(CheckCapacity(capacity, capacity_));
  capacity = std::max(capacity, kMinBuilderCapacity);

  RETURN_NOT_OK(values_builder_.Resize(capacity));
  return ArrayBuilder::Resize(capacity);
}

template <typename T>
Status DictionaryBuilder<T>::Append(const Scalar& value) {
  RETURN_NOT_OK(Reserve(1));

  auto memo_index = memo_table_->GetOrInsert(value);
  RETURN_NOT_OK(values_builder_.Append(memo_index));

  return Status::OK();
}

template <typename T>
Status DictionaryBuilder<T>::AppendNull() {
  return values_builder_.AppendNull();
}

Status DictionaryBuilder<NullType>::AppendNull() { return values_builder_.AppendNull(); }

template <typename T>
Status DictionaryBuilder<T>::AppendArray(const Array& array) {
  const auto& numeric_array = checked_cast<const NumericArray<T>&>(array);
  for (int64_t i = 0; i < array.length(); i++) {
    if (array.IsNull(i)) {
      RETURN_NOT_OK(AppendNull());
    } else {
      RETURN_NOT_OK(Append(numeric_array.Value(i)));
    }
  }
  return Status::OK();
}

Status DictionaryBuilder<NullType>::AppendArray(const Array& array) {
  for (int64_t i = 0; i < array.length(); i++) {
    RETURN_NOT_OK(AppendNull());
  }
  return Status::OK();
}

template <typename T>
Status DictionaryBuilder<T>::FinishInternal(std::shared_ptr<ArrayData>* out) {
  // Finalize indices array
  RETURN_NOT_OK(values_builder_.FinishInternal(out));

  // Generate dictionary array from hash table contents
  std::shared_ptr<Array> dictionary;
  std::shared_ptr<ArrayData> dictionary_data;

  RETURN_NOT_OK(internal::DictionaryTraits<T>::GetDictionaryArrayData(
      pool_, type_, *memo_table_, delta_offset_, &dictionary_data));
  dictionary = MakeArray(dictionary_data);

  // Set type of array data to the right dictionary type
  (*out)->type = std::make_shared<DictionaryType>((*out)->type, dictionary);

  // Update internals for further uses of this DictionaryBuilder
  delta_offset_ = memo_table_->size();
  values_builder_.Reset();

  return Status::OK();
}

Status DictionaryBuilder<NullType>::FinishInternal(std::shared_ptr<ArrayData>* out) {
  std::shared_ptr<Array> dictionary = std::make_shared<NullArray>(0);

  RETURN_NOT_OK(values_builder_.FinishInternal(out));
  (*out)->type = std::make_shared<DictionaryType>((*out)->type, dictionary);

  return Status::OK();
}

//
// StringType and BinaryType specializations
//

#define BINARY_DICTIONARY_SPECIALIZATIONS(Type)                            \
                                                                           \
  template <>                                                              \
  Status DictionaryBuilder<Type>::AppendArray(const Array& array) {        \
    using ArrayType = typename TypeTraits<Type>::ArrayType;                \
    const ArrayType& binary_array = checked_cast<const ArrayType&>(array); \
    for (int64_t i = 0; i < array.length(); i++) {                         \
      if (array.IsNull(i)) {                                               \
        RETURN_NOT_OK(AppendNull());                                       \
      } else {                                                             \
        RETURN_NOT_OK(Append(binary_array.GetView(i)));                    \
      }                                                                    \
    }                                                                      \
    return Status::OK();                                                   \
  }

BINARY_DICTIONARY_SPECIALIZATIONS(StringType);
BINARY_DICTIONARY_SPECIALIZATIONS(BinaryType);

template <>
Status DictionaryBuilder<FixedSizeBinaryType>::AppendArray(const Array& array) {
  if (!type_->Equals(*array.type())) {
    return Status::Invalid("Cannot append FixedSizeBinary array with non-matching type");
  }

  const auto& typed_array = checked_cast<const FixedSizeBinaryArray&>(array);
  for (int64_t i = 0; i < array.length(); i++) {
    if (array.IsNull(i)) {
      RETURN_NOT_OK(AppendNull());
    } else {
      RETURN_NOT_OK(Append(typed_array.GetValue(i)));
    }
  }
  return Status::OK();
}

template class DictionaryBuilder<UInt8Type>;
template class DictionaryBuilder<UInt16Type>;
template class DictionaryBuilder<UInt32Type>;
template class DictionaryBuilder<UInt64Type>;
template class DictionaryBuilder<Int8Type>;
template class DictionaryBuilder<Int16Type>;
template class DictionaryBuilder<Int32Type>;
template class DictionaryBuilder<Int64Type>;
template class DictionaryBuilder<Date32Type>;
template class DictionaryBuilder<Date64Type>;
template class DictionaryBuilder<Time32Type>;
template class DictionaryBuilder<Time64Type>;
template class DictionaryBuilder<TimestampType>;
template class DictionaryBuilder<FloatType>;
template class DictionaryBuilder<DoubleType>;
template class DictionaryBuilder<FixedSizeBinaryType>;
template class DictionaryBuilder<BinaryType>;
template class DictionaryBuilder<StringType>;

}  // namespace arrow
