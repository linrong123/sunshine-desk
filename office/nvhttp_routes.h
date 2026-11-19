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

// lib includes
#include <boost/property_tree/ptree.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "src/nvhttp.h"

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
}  // namespace nvhttp::test_support
#endif
