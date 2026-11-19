/**
 * @file office/nvhttp_routes.cpp
 * @brief Office extensions mounted on the paired-client GameStream HTTPS server.
 */
// standard includes
#include <array>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>

// lib includes
#include <boost/property_tree/xml_parser.hpp>

// local includes
#include "office/display_scale.h"
#include "office/nvhttp_routes.h"
#include "office/policy.h"
#include "src/globals.h"

namespace office {
  namespace {
    namespace pt = boost::property_tree;

    /**
     * @brief One Office endpoint on the GameStream HTTPS server.
     */
    struct https_route_t {
      std::string_view pattern;  ///< Anchored Simple-Web-Server path regular expression.
      std::string_view method;  ///< HTTP method accepted by the handler.
      void (*handler)(https_response_t, https_request_t);  ///< Request handler.
    };

    /// Every Office HTTPS endpoint; add new paired-client extensions here.
    constexpr std::array https_routes {
      https_route_t {"^/office/cursor$", "GET", cursor},
      https_route_t {"^/office/display-scale$", "GET", display_scale},
      https_route_t {"^/office/display-scale$", "POST", display_scale},
    };

    /**
     * @brief Serialize a GameStream XML response and close the connection afterwards.
     * @param response Response to write.
     * @param tree XML tree with a `root` element.
     */
    void write_xml(const https_response_t &response, const pt::ptree &tree) {
      std::ostringstream data;
      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    }
  }  // namespace

  void add_serverinfo_capabilities(boost::property_tree::ptree &tree) {
    tree.put("root.OfficeCursorSet", 1);
#ifdef _WIN32
    tree.put("root.OfficeDisplayScale", 1);
#endif
  }

  void register_https_routes(https_server_t &server) {
    for (const auto &route : https_routes) {
      server.resource[std::string {route.pattern}][std::string {route.method}] = route.handler;
    }
  }

  void cursor(https_response_t response, https_request_t request) {
    const auto visible = cursor_visibility(request->parse_query_string());
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", visible.has_value() ? 200 : 400);
    if (visible.has_value()) {
      display_cursor.store(*visible);
      tree.put("root.cursorVisible", *visible ? 1 : 0);
    } else {
      tree.put("root.<xmlattr>.status_message", "Expected exactly one visible=0 or visible=1 parameter");
    }
    write_xml(response, tree);
  }

  void display_scale(https_response_t response, https_request_t request) {
    const auto args = request->parse_query_string();
    display_scale_result result;
    try {
      if (!request->content.string().empty()) {
        result = {400, "Display scaling requests require an empty body", {}, {}};
      } else {
        result = request_display_scale(request->method == "POST", {args.begin(), args.end()});
      }
    } catch (const std::exception &) {
      result = {503, "System display scaling is temporarily unavailable", {}, {}};
    }
    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", result.status);
    if (result.status == 200 && result.snapshot) {
      const auto &values = result.snapshot->values;
      tree.put("root.scale", values.current);
      tree.put("root.recommended", values.recommended);
      std::string supported;
      for (const auto scale : values.supported) {
        if (!supported.empty()) {
          supported += ',';
        }
        supported += std::to_string(scale);
      }
      tree.put("root.supportedScales", supported);
      tree.put("root.displayToken", result.token);
    }
    if (!result.message.empty()) {
      tree.put("root.message", result.message);
    }
    write_xml(response, tree);
  }
}  // namespace office
