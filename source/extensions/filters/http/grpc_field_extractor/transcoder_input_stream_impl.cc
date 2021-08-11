#include "source/extensions/filters/http/grpc_field_extractor/transcoder_input_stream_impl.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcFieldExtractor {

int64_t TranscoderInputStreamImpl::BytesAvailable() const { return buffer_->length() - position_; }

bool TranscoderInputStreamImpl::Finished() const { return finished_; }

uint64_t TranscoderInputStreamImpl::bytesStored() const { return buffer_->length(); }

} // namespace GrpcFieldExtractor
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
