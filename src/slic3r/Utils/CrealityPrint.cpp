#include "CrealityPrint.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <sstream>
#include <exception>
#include <vector>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/nowide/convert.hpp>

#include <curl/curl.h>
#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/format.hpp"
#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Bonjour.hpp"
#include "slic3r/GUI/BonjourDialog.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

#include <fstream>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
using std::to_string;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

CrealityPrint::CrealityPrint(DynamicPrintConfig* config) : 
    m_host(config->opt_string("print_host")), 
    m_web_ui(config->opt_string("print_host_webui")),
    m_cafile(config->opt_string("printhost_cafile")),
    m_port(config->opt_string("printhost_port")),
    m_apikey(config->opt_string("printhost_apikey")),
    m_ssl_revoke_best_effort(config->opt_bool("printhost_ssl_ignore_revoke"))
{}

const char* CrealityPrint::get_name() const { return "Creality Print"; }

std::string CrealityPrint::get_host() const {
    return m_host;
}
void  CrealityPrint::set_auth(Http& http) const
{
    http.header("Authorization", "Bearer " + m_apikey);
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
}

wxString CrealityPrint::get_test_ok_msg() const { return _(L("Connected to CrealityPrint successfully!")); }

wxString CrealityPrint::get_test_failed_msg(wxString& msg) const
{
    return GUI::format_wxstr("%s: %s", _L("Could not connect to CrealityPrint"), msg.Truncate(256));
}

bool CrealityPrint::test(wxString& msg) const
{ 
    bool res = true;
    const char* name = get_name();
    auto url = make_url("info");

    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get version at: %2%") % name % url;
    // Here we do not have to add custom "Host" header - the url contains host filled by user and libCurl will set the header by itself.
    auto http = Http::get(std::move(url));
    set_auth(http);
    http.timeout_max(5)
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting version: %2%, HTTP %3%, body: `%4%`") % name % error % status %
                                            body;
            res = false;
            msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got version: %2%") % name % body;
            try {
                auto info = json::parse(body);
                if (info.contains("model")) {
                    m_model = info["model"].get<std::string>();
                    BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Detected model: %2%") % name % m_model;
                }
            } catch (const json::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Failed to parse /info response: %2%") % name % e.what();
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();

    return res;
}

PrintHostPostUploadActions CrealityPrint::get_post_upload_actions() const {
    return PrintHostPostUploadAction::StartPrint; 
}

bool CrealityPrint::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{   
    const char* name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    wxString test_msg;
    if (!test(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    bool res = true;
    auto url = make_url("upload/" + safe_filename(upload_filename.string()));

    auto  http = Http::post(url); // std::move(url));
    set_auth(http);
    if (!supports_multi_color_print())
        http.form_add("path", upload_parent_path.string());
    http.form_add_file("file", upload_data.source_path.string(), upload_filename.string())

        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
                wxString errormsg;
                if (!start_print(errormsg, safe_filename(upload_filename.string()), upload_data.extended_info)) {
                    error_fn(std::move(errormsg));
                    res = false;
                }
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file to %2%: %3%, HTTP %4%, body: `%5%`") % name % url % error %
                                            status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                // Upload was canceled
                BOOST_LOG_TRIVIAL(info) << name << ": Upload canceled";
                res = false;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
#endif
        .perform_sync();
    return res;
}

static std::string creality_host_from_url(const std::string& url, std::string* port = nullptr);

std::string CrealityPrint::make_url(const std::string &path) const
{
    std::string port;
    std::string host = creality_host_from_url(m_host, &port);
    const bool  https = boost::algorithm::starts_with(m_host, "https://");

    // Users often enter the Fluidd/Moonraker web UI URL so Orca's Device tab opens.
    // Creality's upload and /info API still live on the base printer HTTP host.
    std::string base = (https ? "https://" : "http://") + (host.empty() ? m_host : host);
    if (!port.empty() && port != "4408" && port != "7125" &&
        !(https && port == "443") && !(!https && port == "80"))
        base += ":" + port;

    if (!base.empty() && base.back() == '/')
        return (boost::format("%1%%2%") % base % path).str();

    return (boost::format("%1%/%2%") % base % path).str();
}

std::string CrealityPrint::safe_filename(const std::string &filename) const
{
    std::string safe_filename = filename;
    std::replace(safe_filename.begin(), safe_filename.end(), ' ', '_');

    return safe_filename;
}

static bool parse_int(const std::string& value, int& out)
{
    try {
        size_t pos = 0;
        int parsed = std::stoi(value, &pos);
        if (pos != value.size())
            return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

static int get_extended_info_int(const std::map<std::string, std::string>& info, const std::string& key, int fallback = 0)
{
    auto it = info.find(key);
    if (it == info.end())
        return fallback;

    int parsed = fallback;
    return parse_int(it->second, parsed) ? parsed : fallback;
}

static std::string creality_host_from_url(const std::string& url, std::string* port)
{
    std::string host = url;
    const size_t scheme_pos = host.find("://");
    if (scheme_pos != std::string::npos)
        host = host.substr(scheme_pos + 3);

    const size_t path_pos = host.find('/');
    if (path_pos != std::string::npos)
        host = host.substr(0, path_pos);

    const size_t at_pos = host.rfind('@');
    if (at_pos != std::string::npos)
        host = host.substr(at_pos + 1);

    if (!host.empty() && host.front() == '[') {
        const size_t end = host.find(']');
        if (end != std::string::npos) {
            if (port && end + 2 < host.size() && host[end + 1] == ':')
                *port = host.substr(end + 2);
            return host.substr(1, end - 1);
        }
    }

    const size_t colon_pos = host.rfind(':');
    if (colon_pos != std::string::npos && host.find(':') == colon_pos) {
        if (port)
            *port = host.substr(colon_pos + 1);
        host = host.substr(0, colon_pos);
    } else if (port) {
        port->clear();
    }

    return host;
}

static int json_int_value(const json& obj, const char* key, int fallback = 0)
{
    auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number_integer())
        return it->get<int>();
    if (it->is_string()) {
        int parsed = fallback;
        return parse_int(it->get<std::string>(), parsed) ? parsed : fallback;
    }
    return fallback;
}

static std::string json_string_value(const json& obj, const char* key, const std::string& fallback = {})
{
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_string())
        return fallback;
    return it->get<std::string>();
}

static double json_double_value(const json& obj, const char* key, double fallback = 0.0)
{
    auto it = obj.find(key);
    if (it == obj.end())
        return fallback;
    if (it->is_number())
        return it->get<double>();
    if (it->is_string()) {
        try {
            size_t pos = 0;
            double parsed = std::stod(it->get<std::string>(), &pos);
            return pos == it->get<std::string>().size() ? parsed : fallback;
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

static std::string normalize_creality_cfs_color(std::string color)
{
    if (color.size() == 8 && color[0] == '#')
        color = "#" + color.substr(2);
    return color.empty() ? "#FFFFFF" : color;
}

static std::string creality_material_color(std::string color)
{
    if (color.empty())
        return "";
    if (color[0] != '#')
        color = "#" + color;
    if (color.size() == 8)
        return color;
    if (color.size() == 7)
        return "#0" + color.substr(1);
    return "#0FFFFFF";
}

static void ws_connect(net::io_context& ioc, websocket::stream<beast::tcp_stream>& ws,
                       const std::string& host_url, const std::string& port)
{
    std::string host = creality_host_from_url(host_url);

    tcp::resolver resolver{ioc};
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
    auto const results = resolver.resolve(host, port);
    beast::get_lowest_layer(ws).connect(results);
    host += ':' + std::to_string(beast::get_lowest_layer(ws).socket().remote_endpoint().port());

    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(http::field::user_agent,
                std::string(BOOST_BEAST_VERSION_STRING) + " websocket-client-coro");
        }));
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
    ws.handshake(host, "/");

#ifdef _WIN32
    DWORD recv_timeout = 3000;
#else
    struct timeval recv_timeout = {3, 0};
#endif
    setsockopt(beast::get_lowest_layer(ws).socket().native_handle(),
               SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recv_timeout), sizeof(recv_timeout));
}

static void ws_write_json(websocket::stream<beast::tcp_stream>& ws, const json& cmd)
{
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
    ws.write(net::buffer(cmd.dump()));
}

static bool ws_is_timeout(beast::error_code ec)
{
    return ec == net::error::would_block
        || ec == net::error::timed_out
        || ec == beast::error::timeout;
}

static std::string ws_send_and_read(websocket::stream<beast::tcp_stream>& ws, const json& cmd, const std::string& expected_key, int max_reads = 20)
{
    ws_write_json(ws, cmd);

    for (int i = 0; i < max_reads; i++) {
        beast::flat_buffer buf;
        beast::error_code ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
        ws.read(buf, ec);
        if (ws_is_timeout(ec))
            break;
        if (ec)
            throw beast::system_error{ec};
        std::string msg = beast::buffers_to_string(buf.data());
        if (msg.find(expected_key) != std::string::npos)
            return msg;
    }
    BOOST_LOG_TRIVIAL(warning) << "CrealityPrint: No '" << expected_key << "' response after " << max_reads << " messages";
    return {};
}

void CrealityPrint::query_model() const
{
    if (!m_model.empty())
        return;

    wxString msg;
    test(msg);
}

bool CrealityPrint::supports_multi_color_print() const
{
    query_model();
    // Creality printers verified to expose the CFS control path on websocket port 9999.
    return m_model == "F008"    // K2 Plus
        || m_model == "F012"    // K2 Pro
        || m_model == "F021"    // K2
        || m_model == "F018";   // Hi
}

std::string CrealityPrint::model_name() const
{
    static const std::map<std::string, std::string> names = {
        {"F008", "K2 Plus"},
        {"F012", "K2 Pro"},
        {"F021", "K2"},
        {"F018", "Hi"},
    };
    query_model();
    if (m_model.empty())
        return "unreachable";
    auto it = names.find(m_model);
    return it != names.end() ? it->second : "unknown (" + m_model + ")";
}

std::string CrealityPrint::query_boxes_info() const
{
    try {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        json boxs_query = {{"method", "get"}, {"params", {{"boxsInfo", 1}}}};
        std::string result = ws_send_and_read(ws, boxs_query, "boxsInfo");
        ws.close(websocket::close_code::normal);
        return result;
    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to query boxsInfo: " << e.what();
        return {};
    }
}

std::vector<CrealityPrint::CfsSlotInfo> CrealityPrint::query_cfs_slots(bool include_external_spool) const
{
    std::vector<CfsSlotInfo> slots;

    if (!supports_multi_color_print())
        return slots;

    const std::string boxes_json = query_boxes_info();
    if (boxes_json.empty())
        return slots;

    try {
        auto resp = json::parse(boxes_json);
        if (!resp.contains("boxsInfo") || !resp["boxsInfo"].contains("materialBoxs") ||
            !resp["boxsInfo"]["materialBoxs"].is_array())
            return slots;

        for (const auto& box : resp["boxsInfo"]["materialBoxs"]) {
            if (!box.is_object())
                continue;

            int box_id = json_int_value(box, "id", -1);
            int box_type = json_int_value(box, "type", 0);
            int box_humidity = json_int_value(box, "humidity", -1);
            int box_temp = json_int_value(box, "temp", 0);
            bool is_ext_box = box_type == 1 || box_id == 0;

            if (box_type == 0 && json_int_value(box, "state", 0) != 1)
                continue;
            if (is_ext_box && !include_external_spool)
                continue;

            if (!box.contains("materials") || !box["materials"].is_array() || box["materials"].empty()) {
                if (is_ext_box) {
                    slots.push_back({
                        "Ext",
                        "External spool",
                        "#FFFFFF",
                        "External spool",
                        "",
                        "",
                        0,
                        box_type,
                        0,
                        0,
                        0,
                        0.0,
                        box_humidity,
                        box_temp,
                        true,
                        false
                    });
                }
                continue;
            }

            for (const auto& mat : box["materials"]) {
                if (!mat.is_object())
                    continue;

                int slot_id = json_int_value(mat, "id", -1);
                if (slot_id < 0)
                    continue;

                std::string material_type = json_string_value(mat, "type");
                const bool has_material_type = !material_type.empty();
                if (!is_ext_box && (json_int_value(mat, "state", 0) != 1 || material_type.empty()))
                    continue;
                if (is_ext_box && material_type.empty())
                    material_type = "External spool";

                int mapped_box_id = is_ext_box ? 0 : box_id;
                slots.push_back({
                    is_ext_box ? "Ext" : "T" + std::to_string(mapped_box_id) + std::string(1, 'A' + slot_id),
                    material_type,
                    normalize_creality_cfs_color(json_string_value(mat, "color", "#FFFFFF")),
                    json_string_value(mat, "name"),
                    json_string_value(mat, "vendor"),
                    json_string_value(mat, "rfid"),
                    mapped_box_id,
                    box_type,
                    slot_id,
                    json_int_value(mat, "minTemp", 0),
                    json_int_value(mat, "maxTemp", 0),
                    json_double_value(mat, "pressure", 0.0),
                    box_humidity,
                    box_temp,
                    is_ext_box,
                    has_material_type
                });
            }
        }
    } catch (const json::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to parse boxsInfo slots: " << e.what();
    }

    return slots;
}

bool CrealityPrint::set_cfs_slot_filament(wxString& msg, const CfsSlotInfo& slot, const std::string& type,
                                          const std::string& color, const std::string& name,
                                          const std::string& vendor, int min_temp, int max_temp,
                                          double pressure) const
{
    if (!supports_multi_color_print()) {
        msg = _L("This Creality printer does not expose CFS material editing.");
        return false;
    }

    if (type.empty()) {
        msg = _L("Select a filament type before saving the CFS slot.");
        return false;
    }

    try {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        json material = {
            {"boxId", slot.box_id},
            {"id", slot.material_id},
            {"rfid", slot.rfid},
            {"type", type},
            {"vendor", vendor},
            {"name", name.empty() ? type : name},
            {"color", creality_material_color(color)},
            {"minTemp", min_temp + 0.00000001},
            {"maxTemp", max_temp + 0.00000001},
            {"pressure", pressure}
        };
        if (slot.box_type == 2)
            material["boxType"] = slot.box_type;

        json cmd = {{"method", "set"}, {"params", {{"modifyMaterial", material}}}};
        ws_write_json(ws, cmd);

        beast::flat_buffer buffer;
        beast::error_code ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(2));
        ws.read(buffer, ec);
        if (ec && !ws_is_timeout(ec))
            throw beast::system_error{ec};

        ws.close(websocket::close_code::normal);
        return true;
    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to set CFS material: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

bool CrealityPrint::reset_cfs_slot_filament(wxString& msg, const CfsSlotInfo& slot) const
{
    if (!supports_multi_color_print()) {
        msg = _L("This Creality printer does not expose CFS material editing.");
        return false;
    }

    try {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        json material = {
            {"boxId", slot.box_id},
            {"id", slot.material_id},
            {"rfid", ""},
            {"type", ""},
            {"vendor", ""},
            {"name", ""},
            {"color", ""},
            {"minTemp", 0},
            {"maxTemp", 0},
            {"pressure", 0}
        };
        if (slot.box_type == 2)
            material["boxType"] = slot.box_type;

        json cmd = {{"method", "set"}, {"params", {{"modifyMaterial", material}}}};
        ws_write_json(ws, cmd);

        beast::flat_buffer buffer;
        beast::error_code ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(2));
        ws.read(buffer, ec);
        if (ec && !ws_is_timeout(ec))
            throw beast::system_error{ec};

        ws.close(websocket::close_code::normal);
        return true;
    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to clear CFS material: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

bool CrealityPrint::feed_or_retract_cfs_slot(wxString& msg, const CfsSlotInfo& slot, bool feed) const
{
    if (!supports_multi_color_print()) {
        msg = _L("This Creality printer does not expose CFS feed/retract controls.");
        return false;
    }

    try {
        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        json cmd = {
            {"method", "set"},
            {"params", {
                {"feedInOrOut", {
                    {"boxId", slot.box_id},
                    {"materialId", slot.material_id},
                    {"isFeed", feed ? 1 : 0}
                }}
            }}
        };
        ws_write_json(ws, cmd);

        beast::flat_buffer buffer;
        beast::error_code ec;
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(2));
        ws.read(buffer, ec);
        if (ec && !ws_is_timeout(ec))
            throw beast::system_error{ec};

        ws.close(websocket::close_code::normal);
        return true;
    } catch (std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Failed to feed/retract CFS material: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

bool CrealityPrint::start_print(wxString &msg, const std::string &filename, const std::map<std::string, std::string>& extended_info) const
{
    try {
        const std::string gcode_path = "/mnt/UDISK/printer_data/gcodes/" + filename;

        net::io_context ioc;
        websocket::stream<beast::tcp_stream> ws{ioc};
        ws_connect(ioc, ws, m_host, "9999");

        if (supports_multi_color_print()) {
            // Build colorMatch list from the mapping provided by the dialog
            bool use_spool_holder = false;
            json color_list = json::array();
            for (int i = 0; ; i++) {
                auto it = extended_info.find("colorMatch_" + std::to_string(i));
                if (it == extended_info.end())
                    break;
                // Value format: "toolId\ttype\tcolor\tboxId\tmaterialId"
                auto val = it->second;
                std::vector<std::string> parts;
                std::istringstream iss(val);
                std::string part;
                while (std::getline(iss, part, '\t'))
                    parts.push_back(part);
                if (parts.size() >= 5) {
                    int box_id = 0;
                    int material_id = 0;
                    if (!parse_int(parts[3], box_id) || !parse_int(parts[4], material_id)) {
                        BOOST_LOG_TRIVIAL(warning) << "CrealityPrint: Ignoring invalid CFS mapping entry: " << val;
                        continue;
                    }
                    if (box_id == 0)
                        use_spool_holder = true;
                    color_list.push_back({
                        {"id", parts[0]},
                        {"type", parts[1]},
                        {"color", parts[2]},
                        {"boxId", box_id},
                        {"materialId", material_id}
                    });
                }
            }

            if (color_list.empty()) {
                int gcode_filament_count = 0;
                if (GUI::wxGetApp().preset_bundle) {
                    auto full_config = GUI::wxGetApp().preset_bundle->full_config();
                    if (auto* filament_colors = full_config.option<ConfigOptionStrings>("filament_colour"))
                        gcode_filament_count = static_cast<int>(filament_colors->values.size());
                }

                const bool allow_ext_spool = gcode_filament_count <= 1;
                auto slots = query_cfs_slots(allow_ext_spool);
                for (const auto& slot : slots) {
                    if (slot.external || slot.type.empty())
                        continue;
                    color_list.push_back({
                        {"id", "T1" + std::string(1, static_cast<char>('A' + static_cast<int>(color_list.size())))},
                        {"type", slot.type},
                        {"color", slot.color},
                        {"boxId", slot.box_id},
                        {"materialId", slot.material_id}
                    });
                    if (gcode_filament_count > 0 && color_list.size() >= static_cast<size_t>(gcode_filament_count))
                        break;
                }

                if (color_list.empty() && allow_ext_spool) {
                    for (const auto& slot : slots) {
                        if (!slot.external)
                            continue;
                        use_spool_holder = true;
                        color_list.push_back({
                            {"id", "T1A"},
                            {"type", slot.type},
                            {"color", slot.color},
                            {"boxId", slot.box_id},
                            {"materialId", slot.material_id}
                        });
                        break;
                    }
                }

                if (color_list.empty()) {
                    msg = _L("Creality CFS filament mapping is missing. Use Upload only, or reopen the dialog after the printer/CFS is reachable.");
                    return false;
                }
            }

            if (use_spool_holder && color_list.size() > 1) {
                msg = _L("External spool printing cannot be mixed with CFS or multiple mapped filaments.");
                return false;
            }

            int enable_self_test = get_extended_info_int(extended_info, "enableSelfTest", 0);

            if (use_spool_holder) {
                json cmd = {
                    {"method", "set"},
                    {"params", {
                        {"opGcodeFile", "printprt:" + gcode_path},
                        {"enableSelfTest", enable_self_test}
                    }}
                };
                ws_write_json(ws, cmd);
            } else {
                json color_match = {
                    {"method", "set"},
                    {"params", {
                        {"colorMatch", {
                            {"path", gcode_path},
                            {"list", color_list}
                        }}
                    }}
                };
                ws_write_json(ws, color_match);

                json multi_color_print = {
                    {"method", "set"},
                    {"params", {
                        {"multiColorPrint", {
                            {"gcode", gcode_path},
                            {"enableSelfTest", enable_self_test}
                        }}
                    }}
                };
                ws_write_json(ws, multi_color_print);
            }
        } else {
            json cmd = {
                {"method", "set"},
                {"params", {
                    {"opGcodeFile", "printprt:/usr/data/printer_data/gcodes/" + filename}
                }}
            };
            ws_write_json(ws, cmd);

            beast::flat_buffer buffer;
            beast::error_code ec;
            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(3));
            ws.read(buffer, ec);
            if (ec && !ws_is_timeout(ec))
                throw beast::system_error{ec};
        }

        ws.close(websocket::close_code::normal);
        return true;
    } catch(std::exception const& e) {
        BOOST_LOG_TRIVIAL(error) << "CrealityPrint: Error starting print: " << e.what();
        msg = wxString::FromUTF8(e.what());
        return false;
    }
}

}
