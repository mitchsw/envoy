#pragma once

#include "envoy/buffer/buffer.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/grpc/codec.h"
#include "source/common/protobuf/protobuf.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcFieldExtractor {

class HttpBodyUtils {
public:
  static bool parseMessageByFieldPath(Protobuf::io::ZeroCopyInputStream* stream,
                                      const std::vector<const ProtobufWkt::Field*>& field_path,
                                      Protobuf::Message* message);
  static void
  appendHttpBodyEnvelope(Buffer::Instance& output,
                         const std::vector<const ProtobufWkt::Field*>& request_body_field_path,
                         std::string content_type, uint64_t content_length);
};

} // namespace GrpcFieldExtractor
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
