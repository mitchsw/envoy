#include "source/extensions/filters/http/grpc_field_extractor/json_extractor_filter.h"

#include <memory>
#include <unordered_set>

#include "envoy/common/exception.h"
#include "envoy/extensions/filters/http/grpc_field_extractor/v3/config.pb.h"
#include "envoy/http/filter.h"

#include "source/common/common/assert.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/common/utility.h"
#include "source/common/grpc/common.h"
#include "source/common/http/headers.h"
#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h"
#include "source/common/protobuf/utility.h"
#include "source/common/runtime/runtime_features.h"
#include "source/extensions/filters/http/grpc_field_extractor/http_body_utils.h"

#include "google/api/annotations.pb.h"
#include "google/api/http.pb.h"
#include "google/api/httpbody.pb.h"

using Envoy::Protobuf::FileDescriptorSet;
using Envoy::Protobuf::io::ZeroCopyInputStream;
using Envoy::ProtobufUtil::Status;
using Envoy::ProtobufUtil::StatusCode;
using google::api::HttpRule;
using google::grpc::extractor::JsonRequestTranslator;
using JsonRequestTranslatorPtr = std::unique_ptr<JsonRequestTranslator>;
using google::grpc::extractor::MessageStream;
using google::grpc::extractor::PathMatcherBuilder;
using google::grpc::extractor::PathMatcherUtility;
using google::grpc::extractor::RequestInfo;
using google::grpc::extractor::RequestMessageTranslator;
using RequestMessageTranslatorPtr = std::unique_ptr<RequestMessageTranslator>;
using google::grpc::extractor::ResponseToJsonTranslator;
using ResponseToJsonTranslatorPtr = std::unique_ptr<ResponseToJsonTranslator>;
using google::grpc::extractor::Extractor;
using ExtractorPtr = std::unique_ptr<Extractor>;
using google::grpc::extractor::ExtractorInputStream;
using ExtractorInputStreamPtr = std::unique_ptr<ExtractorInputStream>;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcFieldExtractor {

struct RcDetailsValues {
  // The gRPC json extractor filter failed to transcode when processing request headers.
  // This will generally be accompanied by details about the extractor failure.
  const std::string GrpcTranscodeFailedEarly = "early_grpc_json_transcode_failure";
  // The gRPC json extractor filter failed to transcode when processing the request body.
  // This will generally be accompanied by details about the extractor failure.
  const std::string GrpcTranscodeFailed = "grpc_json_transcode_failure";
};
using RcDetails = ConstSingleton<RcDetailsValues>;

namespace {

constexpr absl::string_view buffer_limits_runtime_feature =
    "envoy.reloadable_features.grpc_field_extractor_adhere_to_buffer_limits";

const Http::LowerCaseString& trailerHeader() {
  CONSTRUCT_ON_FIRST_USE(Http::LowerCaseString, "trailer");
}

// Extractor:
// https://github.com/grpc-ecosystem/grpc-httpjson-extractor/blob/master/src/include/grpc_extractor/extractor.h
// implementation based on JsonRequestTranslator & ResponseToJsonTranslator
class ExtractorImpl : public Extractor {
public:
  /**
   * Construct a extractor implementation
   * @param request_translator a JsonRequestTranslator that does the request translation
   * @param response_translator a ResponseToJsonTranslator that does the response translation
   */
  ExtractorImpl(RequestMessageTranslatorPtr request_translator,
                 JsonRequestTranslatorPtr json_request_translator,
                 ResponseToJsonTranslatorPtr response_translator)
      : request_translator_(std::move(request_translator)),
        json_request_translator_(std::move(json_request_translator)),
        request_message_stream_(request_translator_ ? *request_translator_
                                                    : json_request_translator_->Output()),
        response_translator_(std::move(response_translator)),
        request_stream_(request_message_stream_.CreateInputStream()),
        response_stream_(response_translator_->CreateInputStream()) {}

  // Extractor
  ::google::grpc::extractor::ExtractorInputStream* RequestOutput() override {
    return request_stream_.get();
  }
  ProtobufUtil::Status RequestStatus() override { return request_message_stream_.Status(); }

  ZeroCopyInputStream* ResponseOutput() override { return response_stream_.get(); }
  ProtobufUtil::Status ResponseStatus() override { return response_translator_->Status(); }

private:
  RequestMessageTranslatorPtr request_translator_;
  JsonRequestTranslatorPtr json_request_translator_;
  MessageStream& request_message_stream_;
  ResponseToJsonTranslatorPtr response_translator_;
  ExtractorInputStreamPtr request_stream_;
  ExtractorInputStreamPtr response_stream_;
};

} // namespace

ExtractorConfig::ExtractorConfig(
    const envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor&
        proto_config,
    Api::Api& api) {
  disabled_ = proto_config.field_extractions().empty();
  if (disabled_) {
    return;
  }

  FileDescriptorSet descriptor_set;
  switch (proto_config.descriptor_set_case()) {
  case envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor::
      DescriptorSetCase::kProtoDescriptor:
    if (!descriptor_set.ParseFromString(
            api.fileSystem().fileReadToEnd(proto_config.proto_descriptor()))) {
      throw EnvoyException("extractor_filter: Unable to parse proto descriptor");
    }
    break;
  case envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor::
      DescriptorSetCase::kProtoDescriptorBin:
    if (!descriptor_set.ParseFromString(proto_config.proto_descriptor_bin())) {
      throw EnvoyException("extractor_filter: Unable to parse proto descriptor");
    }
    break;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
  for (const auto& file : descriptor_set.file()) {
    addFileDescriptor(file);
  }


  for (const auto& extraction : proto_config.field_extractions()) {
    auto service = descriptor_pool_.FindServiceByName(extraction.service());
    if (service == nullptr) {
      throw EnvoyException(absl::StrCat(
        "extractor_filter: Could not find '", service_name, "' in the proto descriptor"));
    }
    for (const std::string& method_name : extraction.methods()) {
      auto method = service->FindMethodByName(method_name);
      if (method == nullptr) {
        throw EnvoyException(absl::StrCat(
          "extractor_filter: Could not find '", method_name,
          "' in the proto descriptor for service '", service->full_name(), "'"));
      }
      const std::string method_path = absl::StrCat("/", service->full_name(), "/", method->name());
      auto field_path = resolveFieldPath();  // TODO!!!
      method_extractions_[method_path].field_extractions.emplace_back({
        extraction.extraction_name(), std::move(field_path);
      });
    }
  }
}

void ExtractorConfig::addFileDescriptor(const Protobuf::FileDescriptorProto& file) {
  if (descriptor_pool_.BuildFile(file) == nullptr) {
    throw EnvoyException("extractor_filter: Unable to build proto descriptor pool");
  }
}

// TODO!
Status ExtractorConfig::resolveField(const Protobuf::Descriptor* descriptor,
                                          const std::string& field_path_str,
                                          std::vector<const ProtobufWkt::Field*>* field_path,
                                          bool* is_http_body) {
  const ProtobufWkt::Type* message_type =
      type_helper_->Info()->GetTypeByTypeUrl(Grpc::Common::typeUrl(descriptor->full_name()));
  if (message_type == nullptr) {
    return ProtobufUtil::Status(StatusCode::kNotFound,
                                "Could not resolve type: " + descriptor->full_name());
  }

  Status status = type_helper_->ResolveFieldPath(
      *message_type, field_path_str == "*" ? "" : field_path_str, field_path);
  if (!status.ok()) {
    return status;
  }

  if (field_path->empty()) {
    *is_http_body = descriptor->full_name() == google::api::HttpBody::descriptor()->full_name();
  } else {
    const ProtobufWkt::Type* body_type =
        type_helper_->Info()->GetTypeByTypeUrl(field_path->back()->type_url());
    *is_http_body = body_type != nullptr &&
                    body_type->name() == google::api::HttpBody::descriptor()->full_name();
  }
  return Status();
}

ProtobufUtil::Status ExtractorConfig::createExtractor(
    const Http::RequestHeaderMap& headers, ZeroCopyInputStream& request_input,
    google::grpc::extractor::ExtractorInputStream& response_input,
    std::unique_ptr<Extractor>& extractor, MethodInfoSharedPtr& method_info) const {

  ASSERT(!disabled_);
  std::string path(headers.getPathValue());
  const size_t pos = path.find('?');
  if (pos != std::string::npos) {
    path = path.substr(0, pos);
  }

  struct RequestInfo request_info;
  std::vector<VariableBinding> variable_bindings;
  method_info =
      path_matcher_->Lookup(method, path, args, &variable_bindings, &request_info.body_field_path);
  if (!method_info) {
    return ProtobufUtil::Status(StatusCode::kNotFound,
                                "Could not resolve " + path + " to a method.");
  }

  auto status = methodToRequestInfo(method_info, &request_info);
  if (!status.ok()) {
    return status;
  }

  for (const auto& binding : variable_bindings) {
    google::grpc::extractor::RequestWeaver::BindingInfo resolved_binding;
    status = type_helper_->ResolveFieldPath(*request_info.message_type, binding.field_path,
                                            &resolved_binding.field_path);
    if (!status.ok()) {
      if (ignore_unknown_query_parameters_) {
        continue;
      }
      return status;
    }

    // HttpBody fields should be passed as-is and not be parsed as JSON.
    const bool is_http_body = method_info->request_type_is_http_body_;
    const bool is_inside_http_body =
        is_http_body && absl::c_equal(absl::MakeSpan(resolved_binding.field_path)
                                          .subspan(0, method_info->request_body_field_path.size()),
                                      method_info->request_body_field_path);
    if (!is_inside_http_body) {
      resolved_binding.value = binding.value;
      request_info.variable_bindings.emplace_back(std::move(resolved_binding));
    }
  }

  RequestMessageTranslatorPtr request_translator;
  JsonRequestTranslatorPtr json_request_translator;
  if (method_info->request_type_is_http_body_) {
    request_translator = std::make_unique<RequestMessageTranslator>(*type_helper_->Resolver(),
                                                                    false, std::move(request_info));
    request_translator->Input().StartObject("")->EndObject();
  } else {
    json_request_translator = std::make_unique<JsonRequestTranslator>(
        type_helper_->Resolver(), &request_input, std::move(request_info),
        method_info->descriptor_->client_streaming(), true);
  }

  const auto response_type_url =
      Grpc::Common::typeUrl(method_info->descriptor_->output_type()->full_name());
  ResponseToJsonTranslatorPtr response_translator{new ResponseToJsonTranslator(
      type_helper_->Resolver(), response_type_url, method_info->descriptor_->server_streaming(),
      &response_input, print_options_)};

  extractor = std::make_unique<ExtractorImpl>(std::move(request_translator),
                                                std::move(json_request_translator),
                                                std::move(response_translator));
  return ProtobufUtil::Status();
}

ProtobufUtil::Status
ExtractorConfig::methodToRequestInfo(const MethodInfoSharedPtr& method_info,
                                          google::grpc::extractor::RequestInfo* info) const {
  const std::string& request_type_full_name = method_info->descriptor_->input_type()->full_name();
  auto request_type_url = Grpc::Common::typeUrl(request_type_full_name);
  info->message_type = type_helper_->Info()->GetTypeByTypeUrl(request_type_url);
  if (info->message_type == nullptr) {
    ENVOY_LOG(debug, "Cannot resolve input-type: {}", request_type_full_name);
    return ProtobufUtil::Status(StatusCode::kNotFound,
                                "Could not resolve type: " + request_type_full_name);
  }

  return ProtobufUtil::Status();
}

ExtractorFilter::ExtractorFilter(ExtractorConfig& config) : config_(config) {}

void ExtractorFilter::initPerRouteConfig() {
  const auto* route_local = Http::Utility::resolveMostSpecificPerFilterConfig<ExtractorConfig>(
      "envoy.filters.http.grpc_field_extractor", decoder_callbacks_->route());

  per_route_config_ = route_local ? route_local : &config_;
}

Http::FilterHeadersStatus ExtractorFilter::decodeHeaders(Http::RequestHeaderMap& headers,
                                                              bool end_stream) {
  initPerRouteConfig();
  if (per_route_config_->disabled()) {
    return Http::FilterHeadersStatus::Continue;
  }

  if (!Grpc::Common::isGrpcRequestHeaders(headers)) {
    ENVOY_LOG(debug, "Request headers do not look like gRPC. Request is passed through "
                     "without extractor.");
    return Http::FilterHeadersStatus::Continue;
  }

  const auto status =
      per_route_config_->maybeCreateExtractor(headers, request_in_, response_in_, extractor_, method_);
  if (!status.ok()) {
    ENVOY_LOG(debug, "Failed to create field extractor: {}", status.message());
    return Http::FilterHeadersStatus::Continue;
  }

  if (method_->request_type_is_http_body_) {
    if (headers.ContentType() != nullptr) {
      absl::string_view content_type = headers.getContentTypeValue();
      content_type_.assign(content_type.begin(), content_type.end());
    }

    bool done = !readToBuffer(*extractor_->RequestOutput(), initial_request_data_);
    if (!done) {
      ENVOY_LOG(
          debug,
          "Transcoding of query arguments of HttpBody request is not done (unexpected state)");
      error_ = true;
      decoder_callbacks_->sendLocalReply(
          Http::Code::BadRequest, "Bad request", nullptr, absl::nullopt,
          absl::StrCat(RcDetails::get().GrpcTranscodeFailedEarly, "{BAD_REQUEST}"));
      return Http::FilterHeadersStatus::StopIteration;
    }
    if (checkIfExtractorFailed(RcDetails::get().GrpcTranscodeFailed)) {
      return Http::FilterHeadersStatus::StopIteration;
    }
  }

  headers.removeContentLength();
  headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Grpc);
  headers.setEnvoyOriginalPath(headers.getPathValue());
  headers.addReferenceKey(Http::Headers::get().EnvoyOriginalMethod, headers.getMethodValue());
  headers.setPath("/" + method_->descriptor_->service()->full_name() + "/" +
                  method_->descriptor_->name());
  headers.setReferenceMethod(Http::Headers::get().MethodValues.Post);
  headers.setReferenceTE(Http::Headers::get().TEValues.Trailers);

  if (!per_route_config_->matchIncomingRequestInfo()) {
    decoder_callbacks_->clearRouteCache();
  }

  if (end_stream && method_->request_type_is_http_body_) {
    maybeSendHttpBodyRequestMessage();
  } else if (end_stream) {
    request_in_.finish();

    if (checkIfExtractorFailed(RcDetails::get().GrpcTranscodeFailedEarly)) {
      return Http::FilterHeadersStatus::StopIteration;
    }

    Buffer::OwnedImpl data;
    readToBuffer(*extractor_->RequestOutput(), data);

    if (data.length() > 0) {
      decoder_callbacks_->addDecodedData(data, true);
    }
  }
  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus ExtractorFilter::decodeData(Buffer::Instance& data, bool end_stream) {
  ASSERT(!error_);

  if (!extractor_) {
    return Http::FilterDataStatus::Continue;
  }

  if (method_->request_type_is_http_body_) {
    request_data_.move(data);
    if (decoderBufferLimitReached(request_data_.length())) {
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }

    // TODO(euroelessar): Upper bound message size for streaming case.
    if (end_stream || method_->descriptor_->client_streaming()) {
      maybeSendHttpBodyRequestMessage();
    } else {
      // TODO(euroelessar): Avoid buffering if content length is already known.
      return Http::FilterDataStatus::StopIterationAndBuffer;
    }
  } else {
    request_in_.move(data);
    if (decoderBufferLimitReached(request_in_.bytesStored())) {
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }

    if (end_stream) {
      request_in_.finish();
    }

    readToBuffer(*extractor_->RequestOutput(), data);
  }

  if (checkIfExtractorFailed(RcDetails::get().GrpcTranscodeFailed)) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }
  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus ExtractorFilter::decodeTrailers(Http::RequestTrailerMap&) {
  ASSERT(!error_);

  if (!extractor_) {
    return Http::FilterTrailersStatus::Continue;
  }

  if (method_->request_type_is_http_body_) {
    maybeSendHttpBodyRequestMessage();
  } else {
    request_in_.finish();

    Buffer::OwnedImpl data;
    readToBuffer(*extractor_->RequestOutput(), data);

    if (data.length()) {
      decoder_callbacks_->addDecodedData(data, true);
    }
  }
  return Http::FilterTrailersStatus::Continue;
}

void ExtractorFilter::setDecoderFilterCallbacks(
    Http::StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

Http::FilterHeadersStatus ExtractorFilter::encodeHeaders(Http::ResponseHeaderMap& headers,
                                                              bool end_stream) {
  if (!Grpc::Common::isGrpcResponseHeaders(headers, end_stream)) {
    error_ = true;
  }

  if (error_ || !extractor_) {
    return Http::FilterHeadersStatus::Continue;
  }

  response_headers_ = &headers;

  if (end_stream) {
    if (method_->descriptor_->server_streaming()) {
      // When there is no body in a streaming response, a empty JSON array is
      // returned by default. Set the content type correctly.
      headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Json);
    }

    // In gRPC wire protocol, headers frame with end_stream is a trailers-only response.
    // The return value from encodeTrailers is ignored since it is always continue.
    doTrailers(headers);

    return Http::FilterHeadersStatus::Continue;
  }

  headers.setReferenceContentType(Http::Headers::get().ContentTypeValues.Json);

  // In case of HttpBody in response - content type is unknown at this moment.
  // So "Continue" only for regular streaming use case and StopIteration for
  // all other cases (non streaming, streaming + httpBody)
  if (method_->descriptor_->server_streaming() && !method_->response_type_is_http_body_) {
    return Http::FilterHeadersStatus::Continue;
  }
  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus ExtractorFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  if (error_ || !extractor_) {
    return Http::FilterDataStatus::Continue;
  }

  has_body_ = true;

  if (method_->response_type_is_http_body_) {
    bool frame_processed = buildResponseFromHttpBodyOutput(*response_headers_, data);
    if (!method_->descriptor_->server_streaming()) {
      return Http::FilterDataStatus::StopIterationAndBuffer;
    }
    if (!http_body_response_headers_set_ && !frame_processed) {
      return Http::FilterDataStatus::StopIterationAndBuffer;
    }
    return Http::FilterDataStatus::Continue;
  }

  response_in_.move(data);
  if (encoderBufferLimitReached(response_in_.bytesStored())) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  if (end_stream) {
    response_in_.finish();
  }

  readToBuffer(*extractor_->ResponseOutput(), data);

  if (!method_->descriptor_->server_streaming() && !end_stream) {
    // Buffer until the response is complete.
    return Http::FilterDataStatus::StopIterationAndBuffer;
  }
  // TODO(lizan): Check ResponseStatus

  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus
ExtractorFilter::encodeTrailers(Http::ResponseTrailerMap& trailers) {
  doTrailers(trailers);

  return Http::FilterTrailersStatus::Continue;
}

void ExtractorFilter::doTrailers(Http::ResponseHeaderOrTrailerMap& headers_or_trailers) {
  if (error_ || !extractor_ || !per_route_config_ || per_route_config_->disabled()) {
    return;
  }

  response_in_.finish();

  const absl::optional<Grpc::Status::GrpcStatus> grpc_status =
      Grpc::Common::getGrpcStatus(headers_or_trailers, true);
  if (grpc_status && maybeConvertGrpcStatus(*grpc_status, headers_or_trailers)) {
    return;
  }

  if (!method_->response_type_is_http_body_) {
    Buffer::OwnedImpl data;
    readToBuffer(*extractor_->ResponseOutput(), data);
    if (data.length()) {
      encoder_callbacks_->addEncodedData(data, true);
    }
  }

  // If there was no previous headers frame, this |trailers| map is our |response_headers_|,
  // so there is no need to copy headers from one to the other.
  const bool is_trailers_only_response = response_headers_ == &headers_or_trailers;
  const bool is_server_streaming = method_->descriptor_->server_streaming();

  if (is_server_streaming && !is_trailers_only_response) {
    // Continue if headers were sent already.
    return;
  }

  if (!grpc_status || grpc_status.value() == Grpc::Status::WellKnownGrpcStatus::InvalidCode) {
    response_headers_->setStatus(enumToInt(Http::Code::ServiceUnavailable));
  } else {
    response_headers_->setStatus(Grpc::Utility::grpcToHttpStatus(grpc_status.value()));
    if (!is_trailers_only_response) {
      response_headers_->setGrpcStatus(grpc_status.value());
    }
  }

  if (!is_trailers_only_response) {
    // Copy the grpc-message header if it exists.
    const Http::HeaderEntry* grpc_message_header = headers_or_trailers.GrpcMessage();
    if (grpc_message_header) {
      response_headers_->setGrpcMessage(grpc_message_header->value().getStringView());
    }
  }

  // remove Trailer headers if the client connection was http/1
  if (encoder_callbacks_->streamInfo().protocol() < Http::Protocol::Http2) {
    response_headers_->remove(trailerHeader());
  }

  if (!method_->descriptor_->server_streaming()) {
    // Set content-length for non-streaming responses.
    response_headers_->setContentLength(
        encoder_callbacks_->encodingBuffer() ? encoder_callbacks_->encodingBuffer()->length() : 0);
  }
}

void ExtractorFilter::setEncoderFilterCallbacks(
    Http::StreamEncoderFilterCallbacks& callbacks) {
  encoder_callbacks_ = &callbacks;
}

bool ExtractorFilter::checkIfExtractorFailed(const std::string& details) {
  const auto& request_status = extractor_->RequestStatus();
  if (!request_status.ok()) {
    ENVOY_LOG(debug, "Transcoding request error {}", request_status.ToString());
    error_ = true;
    decoder_callbacks_->sendLocalReply(
        Http::Code::BadRequest,
        absl::string_view(request_status.message().data(), request_status.message().size()),
        nullptr, absl::nullopt,
        absl::StrCat(details, "{", MessageUtil::codeEnumToString(request_status.code()), "}"));

    return true;
  }
  return false;
}

bool ExtractorFilter::readToBuffer(Protobuf::io::ZeroCopyInputStream& stream,
                                        Buffer::Instance& data) {
  const void* out;
  int size;
  while (stream.Next(&out, &size)) {
    data.add(out, size);

    if (size == 0) {
      return true;
    }
  }
  return false;
}

void ExtractorFilter::maybeSendHttpBodyRequestMessage() {
  if (first_request_sent_ && request_data_.length() == 0) {
    return;
  }

  Buffer::OwnedImpl message_payload;
  message_payload.move(initial_request_data_);
  HttpBodyUtils::appendHttpBodyEnvelope(message_payload, method_->request_body_field_path,
                                        std::move(content_type_), request_data_.length());
  content_type_.clear();
  message_payload.move(request_data_);

  Envoy::Grpc::Encoder().prependFrameHeader(Envoy::Grpc::GRPC_FH_DEFAULT, message_payload);

  decoder_callbacks_->addDecodedData(message_payload, true);

  first_request_sent_ = true;
}

bool ExtractorFilter::buildResponseFromHttpBodyOutput(
    Http::ResponseHeaderMap& response_headers, Buffer::Instance& data) {
  std::vector<Grpc::Frame> frames;
  decoder_.decode(data, frames);
  if (frames.empty()) {
    return false;
  }

  google::api::HttpBody http_body;
  for (auto& frame : frames) {
    if (frame.length_ > 0) {
      http_body.Clear();
      Buffer::ZeroCopyInputStreamImpl stream(std::move(frame.data_));
      if (!HttpBodyUtils::parseMessageByFieldPath(&stream, method_->response_body_field_path,
                                                  &http_body)) {
        // TODO(euroelessar): Return error to client.
        encoder_callbacks_->resetStream();
        return true;
      }
      const auto& body = http_body.data();

      data.add(body);

      if (!method_->descriptor_->server_streaming()) {
        // Non streaming case: single message with content type / length
        response_headers.setContentType(http_body.content_type());
        response_headers.setContentLength(body.size());
        return true;
      } else if (!http_body_response_headers_set_) {
        // Streaming case: set content type only once from first HttpBody message
        response_headers.setContentType(http_body.content_type());
        http_body_response_headers_set_ = true;
      }
    }
  }

  return true;
}

bool ExtractorFilter::maybeConvertGrpcStatus(Grpc::Status::GrpcStatus grpc_status,
                                                  Http::ResponseHeaderOrTrailerMap& trailers) {
  ASSERT(per_route_config_ && !per_route_config_->disabled());
  if (!per_route_config_->convertGrpcStatus()) {
    return false;
  }

  // Send a serialized status only if there was no body.
  if (has_body_) {
    return false;
  }

  if (grpc_status == Grpc::Status::WellKnownGrpcStatus::Ok ||
      grpc_status == Grpc::Status::WellKnownGrpcStatus::InvalidCode) {
    return false;
  }

  // TODO(mattklein123): The dynamic cast here is needed because ResponseHeaderOrTrailerMap is not
  // a header map. This can likely be cleaned up.
  auto status_details =
      Grpc::Common::getGrpcStatusDetailsBin(dynamic_cast<Http::HeaderMap&>(trailers));
  if (!status_details) {
    // If no rpc.Status object was sent in the grpc-status-details-bin header,
    // construct it from the grpc-status and grpc-message headers.
    status_details.emplace();
    status_details->set_code(grpc_status);

    auto grpc_message_header = trailers.GrpcMessage();
    if (grpc_message_header) {
      auto message = grpc_message_header->value().getStringView();
      auto decoded_message = Http::Utility::PercentEncoding::decode(message);
      status_details->set_message(decoded_message.data(), decoded_message.size());
    }
  }

  std::string json_status;
  auto translate_status =
      per_route_config_->translateProtoMessageToJson(*status_details, &json_status);
  if (!translate_status.ok()) {
    ENVOY_LOG(debug, "Transcoding status error {}", translate_status.ToString());
    return false;
  }

  response_headers_->setStatus(Grpc::Utility::grpcToHttpStatus(grpc_status));

  bool is_trailers_only_response = response_headers_ == &trailers;
  if (is_trailers_only_response) {
    // Drop the gRPC status headers, we already have them in the JSON body.
    response_headers_->removeGrpcStatus();
    response_headers_->removeGrpcMessage();
    response_headers_->remove(Http::Headers::get().GrpcStatusDetailsBin);
  }

  // remove Trailer headers if the client connection was http/1
  if (encoder_callbacks_->streamInfo().protocol() < Http::Protocol::Http2) {
    response_headers_->remove(trailerHeader());
  }

  response_headers_->setReferenceContentType(Http::Headers::get().ContentTypeValues.Json);

  response_headers_->setContentLength(json_status.length());

  Buffer::OwnedImpl status_data(json_status);
  encoder_callbacks_->addEncodedData(status_data, false);
  return true;
}

bool ExtractorFilter::decoderBufferLimitReached(uint64_t buffer_length) {
  if (!Runtime::runtimeFeatureEnabled(buffer_limits_runtime_feature)) {
    return false;
  }

  if (buffer_length > decoder_callbacks_->decoderBufferLimit()) {
    ENVOY_LOG(debug,
              "Request rejected because the extractor's internal buffer size exceeds the "
              "configured limit: {} > {}",
              buffer_length, decoder_callbacks_->decoderBufferLimit());
    error_ = true;
    decoder_callbacks_->sendLocalReply(
        Http::Code::PayloadTooLarge,
        "Request rejected because the extractor's internal buffer size exceeds the configured "
        "limit.",
        nullptr, absl::nullopt,
        absl::StrCat(RcDetails::get().GrpcTranscodeFailed, "{request_buffer_size_limit_reached}"));
    return true;
  }
  return false;
}

bool ExtractorFilter::encoderBufferLimitReached(uint64_t buffer_length) {
  if (!Runtime::runtimeFeatureEnabled(buffer_limits_runtime_feature)) {
    return false;
  }

  if (buffer_length > encoder_callbacks_->encoderBufferLimit()) {
    ENVOY_LOG(debug,
              "Response not transcoded because the extractor's internal buffer size exceeds the "
              "configured limit: {} > {}",
              buffer_length, encoder_callbacks_->encoderBufferLimit());
    error_ = true;
    encoder_callbacks_->sendLocalReply(
        Http::Code::InternalServerError,
        "Response not transcoded because the extractor's internal buffer size exceeds the "
        "configured limit.",
        nullptr, absl::nullopt,
        absl::StrCat(RcDetails::get().GrpcTranscodeFailed, "{response_buffer_size_limit_reached}"));
    return true;
  }
  return false;
}

} // namespace GrpcFieldExtractor
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
