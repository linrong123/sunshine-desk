/**
 * @file office/nvhttp_routes.h
 * @brief Office extensions mounted on the paired-client GameStream HTTPS server.
 *
 * `src/nvhttp.cpp` calls `add_serverinfo_capabilities()` from `serverinfo` and
 * `register_https_routes()` once after the upstream HTTPS routes. New Office HTTPS
 * endpoints are added to the route table in `office/nvhttp_routes.cpp`; they are never
 * registered on the plain-HTTP server, so every request has passed client-certificate
 * verification first.
 */
#pragma once

// standard includes
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "src/nvhttp.h"

namespace nvhttp {
  /**
   * @brief Client accepted by the most recent successful TLS client-certificate verification.
   *
   * Defined in `src/nvhttp.cpp`. Like the upstream launch-session identity, it reflects the latest
   * handshake, so two clients completing handshakes at the same moment can race.
   * @return Certificate PEM and paired device name; empty if no client was verified yet.
   */
  std::pair<std::string, std::string> office_verified_client();
}  // namespace nvhttp

namespace office {
  using https_server_t = SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>;  ///< GameStream HTTPS server type that verifies paired client certificates.
  using https_response_t = std::shared_ptr<https_server_t::Response>;  ///< GameStream HTTPS response handle.
  using https_request_t = std::shared_ptr<https_server_t::Request>;  ///< GameStream HTTPS request handle.

  /**
   * @brief Advertise Office capabilities in the GameStream `serverinfo` response.
   * @param tree Response tree that already contains the upstream `root` fields.
   */
  void add_serverinfo_capabilities(boost::property_tree::ptree &tree);

  /**
   * @brief Register every Office endpoint on the GameStream HTTPS server.
   * @param server HTTPS server whose TLS verification admits only enabled paired clients.
   */
  void register_https_routes(https_server_t &server);

  /**
   * @brief Set global streamed cursor visibility.
   * @param response GameStream XML response.
   * @param request Request containing exactly one `visible=0` or `visible=1` parameter.
   */
  void cursor(https_response_t response, https_request_t request);

  /**
   * @brief Read or set real capture-display scaling.
   * @param response GameStream XML response carrying actual values after verified readback.
   * @param request Parameterless GET, or POST with `scale` and the previously queried `displayToken`.
   */
  void display_scale(https_response_t response, https_request_t request);

  /**
   * @brief Start, query, or cancel the calling client's connection request.
   * @param response GameStream XML response with `state` and `timeoutSeconds`.
   * @param request GET with exactly one `action=start`, `action=status`, or `action=cancel` parameter.
   */
  void connect_request_handler(https_response_t response, https_request_t request);

  /**
   * @brief Gate `/launch` and `/resume` on host approval for the calling client.
   * @param tree Response tree; receives the 403 result when the stream is rejected.
   * @param result_field Result element set to 0 on rejection, such as `root.gamesession`.
   * @param request Verified GameStream HTTPS request.
   * @return `true` if the request was rejected and the handler must return.
   */
  bool reject_unapproved_stream(boost::property_tree::ptree &tree, std::string_view result_field, const https_request_t &request);

  /**
   * @brief Record an explicit quit (`/cancel`) by the calling client; ends its reconnect grace.
   */
  void quit_current_client();
}  // namespace office

#ifdef SUNSHINE_TESTS
namespace nvhttp::test_support {
  /**
   * @brief Create the production HTTPS server class with paired-client TLS verification enabled.
   *
   * Defined in the test-only section of `src/nvhttp.cpp` because the server class is private to that file.
   * Verification uses the same `verify_client_certificate()` and enabled-client checks as
   * `nvhttp::start()`; failed verification returns the GameStream 401 XML response.
   * The server listens on 127.0.0.1 with an ephemeral port and has no routes registered.
   *
   * @param certificate_file Path to the server certificate PEM file.
   * @param private_key_file Path to the matching private key PEM file.
   * @return HTTPS server ready for route registration.
   */
  std::unique_ptr<SimpleWeb::ServerBase<SunshineHTTPS>> make_verified_https_server(
    const std::string &certificate_file,
    const std::string &private_key_file
  );

  /**
   * @brief Run the production `/launch` handler with local audio disabled.
   * @param response GameStream XML response.
   * @param request Verified GameStream HTTPS request.
   */
  void launch_for_test(
    std::shared_ptr<SimpleWeb::ServerBase<SunshineHTTPS>::Response> response,
    std::shared_ptr<SimpleWeb::ServerBase<SunshineHTTPS>::Request> request
  );
}  // namespace nvhttp::test_support
#endif
