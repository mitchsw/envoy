#pragma once

#include "envoy/extensions/filters/http/grpc_field_extractor/v3/config.pb.h"
#include "envoy/extensions/filters/http/grpc_field_extractor/v3/config.pb.validate.h"

#include "source/extensions/filters/http/common/factory_base.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcFieldExtractor {

/**
 * Config registration for the gRPC field extractor filter. @see NamedHttpFilterConfigFactory.
 */
class GrpcFieldExtractorFilterConfig
    : public Common::FactoryBase<
          envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor> {
public:
  GrpcFieldExtractorFilterConfig() : FactoryBase("envoy.filters.http.grpc_field_extractor") {}

private:
  Http::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor&
          proto_config,
      const std::string& stats_prefix, Server::Configuration::FactoryContext& context) override;

  Router::RouteSpecificFilterConfigConstSharedPtr createRouteSpecificFilterConfigTyped(
      const envoy::extensions::filters::http::grpc_field_extractor::v3::GrpcFieldExtractor&,
      Server::Configuration::ServerFactoryContext& context,
      ProtobufMessage::ValidationVisitor& validator) override;
};

} // namespace GrpcFieldExtractor
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
