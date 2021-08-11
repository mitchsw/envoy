#pragma once

#include "envoy/api/api.h"
#include "envoy/buffer/buffer.h"
#include "envoy/extensions/filters/http/grpc_field_extractor/v3/config.pb.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/logger.h"
#include "source/common/grpc/codec.h"
#include "source/common/protobuf/protobuf.h"
#include "source/extensions/filters/http/grpc_field_extractor/extractor_input_stream_impl.h"

#include "google/api/http.pb.h"
#include "grpc_transcoding/path_matcher.h"
#include "grpc_transcoding/request_message_translator.h"
#include "grpc_transcoding/extractor.h"
#include "grpc_transcoding/type_helper.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcFieldExtractor {

struct FieldExtraction {
  // The name of the object to be added to the per-request filterState.
  std::string extraction_name;
  // The path to the field to be extracted. This field path can be traversed
  // from an incoming request proto.
  std::vector<const ProtobufWkt::Field*> field_path;
};

struct MethodExtractions {
  // The field that should be extracted from the request of a specific method.
  std::vector<FieldExtraction> field_extractions;
};

/**
 * Global configuration for the gRPC field extractor filter. Factory for the Extractor interface.
 */
class ExtractorConfig : public Logger::Loggable<Logger::Id::config>,
                        public Router::RouteSpecificFilterConfig {

public:
  /**
   * constructor that loads protobuf descriptors from the file specified in the JSON config.
   * and construct a path matcher for HTTP path bindings.
   */
  ExtractorConfig(
      const envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor&
          proto_config,
      Api::Api& api);

  /**
   * Create an instance of Extractor interface based on incoming request.
   * @param headers headers received from decoder.
   * @param request_input a ZeroCopyInputStream reading from downstream request body.
   * @param response_input a ExtractorInputStream reading from upstream response body.
   * @param extractor output parameter for the instance of Extractor interface.
   * @return status whether the Extractor instance are successfully created or not. If the method
   *         is not found, status with Code::NOT_FOUND is returned. If the method is found, but
   * fields cannot be resolved, status with Code::INVALID_ARGUMENT is returned.
   */
  ProtobufUtil::Status
  maybeCreateExtractor(const Http::RequestHeaderMap& headers,
                   Protobuf::io::ZeroCopyInputStream& request_input,
                   std::unique_ptr<google::grpc::transcoding::Extractor>& extractor) const;

  bool disabled() const { return disabled_; }

  envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor::
      RequestValidationOptions request_validation_options_{};

private:
  /**
   * Convert method descriptor to RequestInfo that needed for transcoding library
   */
  ProtobufUtil::Status methodToRequestInfo(const MethodInfoSharedPtr& method_info,
                                           google::grpc::transcoding::RequestInfo* info) const;

  void addFileDescriptor(const Protobuf::FileDescriptorProto& file);
  
  
  ProtobufUtil::Status resolveField(const Protobuf::Descriptor* descriptor,
                                    const std::string& field_path_str,
                                    std::vector<const ProtobufWkt::Field*>* field_path,
                                    bool* is_http_body);

  Protobuf::DescriptorPool descriptor_pool_;
  // Map keyed by full method path, e.g. "/service/method"
  absl::flat_hash_map<std::string, MethodExtractions> method_extractions_;
  bool disabled_;
};

using ExtractorConfigSharedPtr = std::shared_ptr<ExtractorConfig>;

/**
 * The filter instance for gRPC JSON extractor.
 */
// TODO: only need decoder filter. (see buffer filter as neat example)
class ExtractorFilter : public Http::StreamFilter, public Logger::Loggable<Logger::Id::http2> {
public:
  ExtractorFilter(ExtractorConfig& config);

  // Http::StreamDecoderFilter
  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus decodeTrailers(Http::RequestTrailerMap& trailers) override;
  void setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) override;

  // Http::StreamEncoderFilter
  Http::FilterHeadersStatus encode100ContinueHeaders(Http::ResponseHeaderMap&) override {
    return Http::FilterHeadersStatus::Continue;
  }
  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;
  Http::FilterTrailersStatus encodeTrailers(Http::ResponseTrailerMap& trailers) override;
  Http::FilterMetadataStatus encodeMetadata(Http::MetadataMap&) override {
    return Http::FilterMetadataStatus::Continue;
  }
  void setEncoderFilterCallbacks(Http::StreamEncoderFilterCallbacks& callbacks) override;

  // Http::StreamFilterBase
  void onDestroy() override {}

private:
  bool checkIfExtractorFailed(const std::string& details);
  bool readToBuffer(Protobuf::io::ZeroCopyInputStream& stream, Buffer::Instance& data);
  void maybeSendHttpBodyRequestMessage();
  /**
   * Builds response from HttpBody protobuf.
   * Returns true if at least one gRPC frame has processed.
   */
  bool buildResponseFromHttpBodyOutput(Http::ResponseHeaderMap& response_headers,
                                       Buffer::Instance& data);
  bool maybeConvertGrpcStatus(Grpc::Status::GrpcStatus grpc_status,
                              Http::ResponseHeaderOrTrailerMap& trailers);
  bool hasHttpBodyAsOutputType();
  void doTrailers(Http::ResponseHeaderOrTrailerMap& headers_or_trailers);
  void initPerRouteConfig();

  // Helpers for flow control.
  bool decoderBufferLimitReached(uint64_t buffer_length);
  bool encoderBufferLimitReached(uint64_t buffer_length);

  ExtractorConfig& config_;
  const ExtractorConfig* per_route_config_{};
  std::unique_ptr<google::grpc::transcoding::Extractor> extractor_;
  ExtractorInputStreamImpl request_in_;
  ExtractorInputStreamImpl response_in_;
  Http::StreamDecoderFilterCallbacks* decoder_callbacks_{};
  Http::StreamEncoderFilterCallbacks* encoder_callbacks_{};
  Http::ResponseHeaderMap* response_headers_{};
  Grpc::Decoder decoder_;

  // Data of the initial request message, initialized from query arguments, path, etc.
  Buffer::OwnedImpl initial_request_data_;
  Buffer::OwnedImpl request_data_;
  bool first_request_sent_{false};
  std::string content_type_;

  bool error_{false};
  bool has_body_{false};
  bool http_body_response_headers_set_{false};
};

} // namespace GrpcFieldExtractor
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
