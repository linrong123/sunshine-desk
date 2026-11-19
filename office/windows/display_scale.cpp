/**
 * @file office/windows/display_scale.cpp
 * @brief Targeted Windows system DPI scaling via private DisplayConfig packets.
 * @details Packet layout and standard scale table are based on imniko/SetDPI (Unlicense).
 * See office/licenses/SetDPI-Unlicense.txt and office/DISPLAY_SCALE.md for compatibility limits.
 */
#ifdef _WIN32

  #include "office/display_scale.h"

  #include "src/config.h"
  #include "src/display_device.h"

  #include <algorithm>
  #include <array>
  #include <map>
  #include <shellscalingapi.h>
  #include <sstream>
  #include <tuple>
  #include <windows.h>

namespace office {
  namespace {
    /** @brief Private DisplayConfig GET packet; relative indices are not public Windows API. */
    struct dpi_get_packet {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header {};  ///< Type -3, source adapter, and source ID.
      std::int32_t minimum {};  ///< Number of steps below the recommended scale to 100 percent.
      std::int32_t current {};  ///< Current scale relative to the recommended scale.
      std::int32_t maximum {};  ///< Maximum scale relative to the recommended scale.
    };

    /** @brief Private DisplayConfig SET packet; never used for an unverified target. */
    struct dpi_set_packet {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header {};  ///< Type -4, source adapter, and source ID.
      std::int32_t relative {};  ///< Requested standard scale relative to the recommendation.
    };

    static_assert(sizeof(dpi_get_packet) == 32, "Windows private DPI GET packet layout changed");
    static_assert(sizeof(dpi_set_packet) == 24, "Windows private DPI SET packet layout changed");

    /** @brief Selected active source and monitor, retained only inside a single operation. */
    struct selected_source {
      LUID adapter {};  ///< DisplayConfig source adapter identity.
      UINT32 id {};  ///< Source ID within the adapter.
      std::wstring name;  ///< GDI source name used to find its HMONITOR.
      std::string identity;  ///< Private configured-source and complete active-topology fingerprint.
    };

    /** @brief Monitor lookup context for EnumDisplayMonitors. */
    struct monitor_lookup {
      std::wstring name;  ///< Expected GDI source name.
      HMONITOR monitor {};  ///< Matching monitor handle, if unique.
      unsigned matches {};  ///< Number of matching handles; ambiguous matches are refused.
    };

    /**
     * @brief Match an HMONITOR to the selected DisplayConfig source name.
     * @param monitor Monitor supplied by Windows enumeration.
     * @param dc Unused monitor device context.
     * @param rectangle Unused monitor rectangle.
     * @param context Pointer to the lookup state.
     * @return TRUE to continue enumeration.
     */
    BOOL CALLBACK find_monitor(HMONITOR monitor, HDC dc, LPRECT rectangle, LPARAM context) {
      static_cast<void>(dc);
      static_cast<void>(rectangle);
      auto &lookup = *reinterpret_cast<monitor_lookup *>(context);
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO *>(&info)) && _wcsicmp(lookup.name.c_str(), info.szDevice) == 0) {
        lookup.monitor = monitor;
        ++lookup.matches;
      }
      return TRUE;
    }

    /**
     * @brief Describe Windows failures without leaking adapter or monitor identity.
     * @param operation Generic operation name.
     * @param status Windows result code.
     * @return Explicit non-success response, including console and driver limitations.
     */
    display_scale_result windows_failure(const char *operation, LONG status) {
      std::string reason;
      if (status == ERROR_ACCESS_DENIED) {
        reason = "No access to the Windows console desktop";
      } else if (status == ERROR_NOT_SUPPORTED) {
        reason = "A supported WDDM driver and DPI API are required";
      } else {
        reason = std::string(operation) + " failed (Windows error " + std::to_string(status) + ")";
      }
      return {503, std::move(reason), {}, {}};
    }

    /**
     * @brief Select only the configured capture source and fingerprint the active topology.
     * @param selected Output source; no display changes are performed.
     * @return Success or explicit ambiguity/API failure.
     */
    display_scale_result select_source(selected_source &selected) {
      std::vector<DISPLAYCONFIG_PATH_INFO> paths;
      std::vector<DISPLAYCONFIG_MODE_INFO> modes;
      LONG status = ERROR_INSUFFICIENT_BUFFER;
      for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 path_count {}, mode_count {};
        status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (status != ERROR_SUCCESS) {
          return windows_failure("Display enumeration", status);
        }
        if (path_count == 0 || path_count > 1024 || mode_count > 16384) {
          return {503, "No valid active display topology is available", {}, {}};
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (status == ERROR_SUCCESS) {
          paths.resize(path_count);
        }
      }
      if (status != ERROR_SUCCESS) {
        return windows_failure("Display enumeration", status);
      }

      const auto configured = config::video.output_name;
      const auto mapped = configured.empty() ? std::string {} : display_device::map_output_name(configured);
      if (!configured.empty() && mapped.empty()) {
        return {409, "The configured capture display cannot be mapped to an active source", {}, {}};
      }
      const std::wstring wanted(mapped.begin(), mapped.end());
      using source_key = std::tuple<LONG, DWORD, UINT32>;
      std::map<source_key, selected_source> sources;
      std::map<source_key, unsigned> target_counts;
      std::vector<std::string> topology;
      for (const auto &path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        status = DisplayConfigGetDeviceInfo(&source.header);
        if (status != ERROR_SUCCESS) {
          return windows_failure("Display source identification", status);
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target {};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        status = DisplayConfigGetDeviceInfo(&target.header);
        if (status != ERROR_SUCCESS) {
          return windows_failure("Display target identification", status);
        }
        if (target.monitorDevicePath[0] == L'\0') {
          return {503, "The display driver did not provide a stable target identity", {}, {}};
        }
        const source_key key {path.sourceInfo.adapterId.HighPart, path.sourceInfo.adapterId.LowPart, path.sourceInfo.id};
        sources[key] = {path.sourceInfo.adapterId, path.sourceInfo.id, source.viewGdiDeviceName, {}};
        ++target_counts[key];
        std::ostringstream fingerprint;
        fingerprint << path.sourceInfo.adapterId.HighPart << ':' << path.sourceInfo.adapterId.LowPart << ':' << path.sourceInfo.id << ':'
                    << path.targetInfo.adapterId.HighPart << ':' << path.targetInfo.adapterId.LowPart << ':' << path.targetInfo.id << ':';
        for (const auto character : target.monitorDevicePath) {
          if (character == L'\0') {
            break;
          }
          fingerprint << static_cast<unsigned>(character) << ',';
        }
        topology.push_back(fingerprint.str());
      }
      unsigned matches {};
      for (const auto &[key, source] : sources) {
        if ((configured.empty() && sources.size() == 1) || (!configured.empty() && _wcsicmp(wanted.c_str(), source.name.c_str()) == 0)) {
          if (target_counts[key] != 1) {
            return {409, "The capture source is cloned to multiple displays; independent scaling is unavailable", {}, {}};
          }
          selected = source;
          ++matches;
        }
      }
      if (matches != 1) {
        return {409, "The capture display does not identify exactly one active source", {}, {}};
      }
      std::sort(topology.begin(), topology.end());
      std::ostringstream identity;
      identity << configured.size() << ':' << configured << ':' << selected.adapter.HighPart << ':' << selected.adapter.LowPart << ':' << selected.id << ':';
      for (const auto &path : topology) {
        identity << path.size() << ':' << path;
      }
      selected.identity = identity.str();
      return {200, {}, {}, {}};
    }

    /**
     * @brief Cross-check private scale indices against effective system DPI, rejecting custom scales.
     * @param source Selected source whose actual monitor must be queried.
     * @param scale Scale decoded from the private DisplayConfig packet.
     * @return Success only when the actual horizontal and vertical DPI exactly match.
     */
    display_scale_result verify_effective_dpi(const selected_source &source, int scale) {
      const auto shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!shcore) {
        return {503, "Effective system DPI verification is unavailable", {}, {}};
      }
      const auto get_awareness = reinterpret_cast<HRESULT(WINAPI *)(HANDLE, PROCESS_DPI_AWARENESS *)>(GetProcAddress(shcore, "GetProcessDpiAwareness"));
      const auto get_dpi = reinterpret_cast<HRESULT(WINAPI *)(HMONITOR, MONITOR_DPI_TYPE, UINT *, UINT *)>(GetProcAddress(shcore, "GetDpiForMonitor"));
      PROCESS_DPI_AWARENESS awareness {};
      monitor_lookup lookup {source.name, nullptr, 0};
      UINT x {}, y {};
      const bool verified = get_awareness && get_dpi && SUCCEEDED(get_awareness(nullptr, &awareness)) && awareness == PROCESS_PER_MONITOR_DPI_AWARE &&
                            EnumDisplayMonitors(nullptr, nullptr, find_monitor, reinterpret_cast<LPARAM>(&lookup)) && lookup.matches == 1 &&
                            SUCCEEDED(get_dpi(lookup.monitor, MDT_EFFECTIVE_DPI, &x, &y)) && x == static_cast<UINT>(scale * 96 / 100) && y == x;
      FreeLibrary(shcore);
      if (!verified) {
        return {503, "Effective DPI does not confirm a standard scale; custom scaling or unavailable per-monitor DPI awareness is unsupported", {}, {}};
      }
      return {200, {}, {}, {}};
    }

    /**
     * @brief Read and validate both private relative indices and effective system DPI.
     * @param source Selected capture source with a private topology identity.
     * @return Snapshot carrying only real supported system scales.
     */
    display_scale_result read_source(const selected_source &source) {
      dpi_get_packet packet;
      packet.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3);
      packet.header.size = sizeof(packet);
      packet.header.adapterId = source.adapter;
      packet.header.id = source.id;
      const auto status = DisplayConfigGetDeviceInfo(&packet.header);
      if (status != ERROR_SUCCESS) {
        return windows_failure("Display scaling query", status);
      }
      const auto values = decode_display_scale(packet.minimum, packet.current, packet.maximum);
      if (!values) {
        return {503, "Windows returned custom or unknown display scaling indices", {}, {}};
      }
      auto verified = verify_effective_dpi(source, values->current);
      if (verified.status != 200) {
        return verified;
      }
      return {200, {}, display_scale_snapshot {source.identity, *values}, {}};
    }

    /** @brief Windows-only implementation; no constructor or destructor changes display state. */
    class windows_display_scale_backend final: public display_scale_backend {
    public:
      /** @copydoc display_scale_backend::read */
      display_scale_result read() override {
        selected_source selected;
        auto result = select_source(selected);
        return result.status == 200 ? read_source(selected) : result;
      }

      /** @copydoc display_scale_backend::write */
      display_scale_result write(const display_scale_snapshot &expected, int scale) override {
        selected_source selected;
        auto result = select_source(selected);
        if (result.status != 200) {
          return result;
        }
        if (selected.identity != expected.identity) {
          return {409, "The capture display topology changed before applying scaling", {}, {}};
        }
        auto fresh = read_source(selected);
        if (fresh.status != 200) {
          return fresh;
        }
        const auto &values = fresh.snapshot->values;
        const auto requested = std::find(values.supported.begin(), values.supported.end(), scale);
        const auto recommended = std::find(values.supported.begin(), values.supported.end(), values.recommended);
        if (requested == values.supported.end() || recommended == values.supported.end()) {
          return {422, "The requested scale is no longer supported", {}, {}};
        }
        // Recheck topology after the DPI cross-check, immediately before the OS write.
        selected_source last;
        result = select_source(last);
        if (result.status != 200) {
          return result;
        }
        if (last.identity != expected.identity) {
          return {409, "The capture display topology changed before applying scaling", {}, {}};
        }
        dpi_set_packet packet;
        packet.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4);
        packet.header.size = sizeof(packet);
        packet.header.adapterId = selected.adapter;
        packet.header.id = selected.id;
        packet.relative = static_cast<std::int32_t>(requested - recommended);
        const auto status = DisplayConfigSetDeviceInfo(&packet.header);
        return status == ERROR_SUCCESS ? display_scale_result {200, {}, {}, {}} : windows_failure("Display scaling write", status);
      }
    };
  }  // namespace

  std::unique_ptr<display_scale_backend> make_windows_display_scale_backend() {
    return std::make_unique<windows_display_scale_backend>();
  }
}  // namespace office

#endif
