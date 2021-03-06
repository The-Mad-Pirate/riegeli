// Copyright 2017 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "riegeli/chunk_encoding/chunk_decoder.h"

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <utility>

#include "absl/base/optimization.h"
#include "absl/strings/str_cat.h"
#include "google/protobuf/message_lite.h"
#include "riegeli/base/base.h"
#include "riegeli/base/chain.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/chain_backward_writer.h"
#include "riegeli/bytes/chain_reader.h"
#include "riegeli/bytes/limiting_reader.h"
#include "riegeli/bytes/message_parse.h"
#include "riegeli/bytes/reader.h"
#include "riegeli/bytes/reader_utils.h"
#include "riegeli/chunk_encoding/chunk.h"
#include "riegeli/chunk_encoding/constants.h"
#include "riegeli/chunk_encoding/simple_decoder.h"
#include "riegeli/chunk_encoding/transpose_decoder.h"

namespace riegeli {

void ChunkDecoder::Done() { recoverable_ = false; }

void ChunkDecoder::Reset() {
  MarkHealthy();
  limits_.clear();
  values_reader_ = ChainReader<Chain>(Chain());
  index_ = 0;
  recoverable_ = false;
}

bool ChunkDecoder::Reset(const Chunk& chunk) {
  Reset();
  ChainReader<> data_reader(&chunk.data);
  if (ABSL_PREDICT_FALSE(chunk.header.num_records() > limits_.max_size())) {
    return Fail("Too many records");
  }
  if (ABSL_PREDICT_FALSE(chunk.header.decoded_data_size() >
                         record_scratch_.max_size())) {
    return Fail("Too large chunk");
  }
  Chain values;
  if (ABSL_PREDICT_FALSE(!Parse(chunk.header, &data_reader, &values))) {
    limits_.clear();  // Ensure that index() == num_records().
    return false;
  }
  RIEGELI_ASSERT_EQ(limits_.size(), chunk.header.num_records())
      << "Wrong number of record end positions";
  RIEGELI_ASSERT_EQ(limits_.empty() ? size_t{0} : limits_.back(), values.size())
      << "Wrong last record end position";
  if (chunk.header.num_records() == 0) {
    RIEGELI_ASSERT_EQ(values.size(), 0u) << "Wrong decoded data size";
  } else if (field_projection_.includes_all()) {
    RIEGELI_ASSERT_EQ(values.size(), chunk.header.decoded_data_size())
        << "Wrong decoded data size";
  } else {
    RIEGELI_ASSERT_LE(values.size(), chunk.header.decoded_data_size())
        << "Wrong decoded data size";
  }
  values_reader_ = ChainReader<Chain>(std::move(values));
  return true;
}

bool ChunkDecoder::Parse(const ChunkHeader& header, Reader* src, Chain* dest) {
  switch (header.chunk_type()) {
    case ChunkType::kFileSignature:
      if (ABSL_PREDICT_FALSE(header.data_size() != 0)) {
        return Fail(absl::StrCat(
            "Invalid file signature chunk: data size is not zero: ",
            header.data_size()));
      }
      if (ABSL_PREDICT_FALSE(header.num_records() != 0)) {
        return Fail(absl::StrCat(
            "Invalid file signature chunk: number of records is not zero: ",
            header.num_records()));
      }
      if (ABSL_PREDICT_FALSE(header.decoded_data_size() != 0)) {
        return Fail(absl::StrCat(
            "Invalid file signature chunk: decoded data size is not zero: ",
            header.decoded_data_size()));
      }
      return true;
    case ChunkType::kFileMetadata:
      if (ABSL_PREDICT_FALSE(header.num_records() != 0)) {
        return Fail(absl::StrCat(
            "Invalid file metadata chunk: number of records is not zero: ",
            header.num_records()));
      }
      return true;
    case ChunkType::kPadding:
      if (ABSL_PREDICT_FALSE(header.num_records() != 0)) {
        return Fail(absl::StrCat(
            "Invalid padding chunk: number of records is not zero: ",
            header.num_records()));
      }
      if (ABSL_PREDICT_FALSE(header.decoded_data_size() != 0)) {
        return Fail(absl::StrCat(
            "Invalid padding chunk: decoded data size is not zero: ",
            header.decoded_data_size()));
      }
      return true;
    case ChunkType::kSimple: {
      SimpleDecoder simple_decoder;
      if (ABSL_PREDICT_FALSE(!simple_decoder.Reset(src, header.num_records(),
                                                   header.decoded_data_size(),
                                                   &limits_))) {
        return Fail("Invalid simple chunk", simple_decoder);
      }
      dest->Clear();
      if (ABSL_PREDICT_FALSE(!simple_decoder.reader()->Read(
              dest, IntCast<size_t>(header.decoded_data_size())))) {
        return Fail("Reading record values failed", *simple_decoder.reader());
      }
      if (ABSL_PREDICT_FALSE(!simple_decoder.VerifyEndAndClose())) {
        return Fail(simple_decoder);
      }
      if (ABSL_PREDICT_FALSE(!src->VerifyEndAndClose())) {
        return Fail("Invalid simple chunk", *src);
      }
      return true;
    }
    case ChunkType::kTransposed: {
      TransposeDecoder transpose_decoder;
      dest->Clear();
      ChainBackwardWriter<> dest_writer(
          dest,
          ChainBackwardWriterBase::Options().set_size_hint(
              field_projection_.includes_all() ? header.decoded_data_size()
                                               : uint64_t{0}));
      const bool ok = transpose_decoder.Reset(
          src, header.num_records(), header.decoded_data_size(),
          field_projection_, &dest_writer, &limits_);
      if (ABSL_PREDICT_FALSE(!dest_writer.Close())) return Fail(dest_writer);
      if (ABSL_PREDICT_FALSE(!ok)) {
        return Fail("Invalid transposed chunk", transpose_decoder);
      }
      if (ABSL_PREDICT_FALSE(!src->VerifyEndAndClose())) {
        return Fail("Invalid transposed chunk", *src);
      }
      return true;
    }
  }
  if (header.num_records() == 0) {
    // Ignore chunks with no records, even if the type is unknown.
    return true;
  }
  return Fail(absl::StrCat("Unknown chunk type: ",
                           static_cast<uint64_t>(header.chunk_type())));
}

bool ChunkDecoder::ReadRecord(google::protobuf::MessageLite* record) {
  if (ABSL_PREDICT_FALSE(index() == num_records() || !healthy())) return false;
  const size_t start = IntCast<size_t>(values_reader_.pos());
  const size_t limit = limits_[IntCast<size_t>(index_)];
  RIEGELI_ASSERT_LE(start, limit)
      << "Failed invariant of ChunkDecoder: record end positions not sorted";
  std::string error_message;
  if (ABSL_PREDICT_FALSE(!ParseFromReader(
          record, LimitingReader<>(&values_reader_, limit), &error_message))) {
    if (!values_reader_.Seek(limit)) {
      RIEGELI_ASSERT_UNREACHABLE()
          << "Seeking record values failed: " << values_reader_.message();
    }
    recoverable_ = true;
    return Fail(error_message);
  }
  ++index_;
  return true;
}

bool ChunkDecoder::Recover() {
  if (!recoverable_) return false;
  RIEGELI_ASSERT(!healthy()) << "Failed invariant of ChunkDecoder: "
                                "recovery applicable but ChunkDecoder healthy";
  recoverable_ = false;
  MarkNotFailed();
  ++index_;
  return true;
}

}  // namespace riegeli
