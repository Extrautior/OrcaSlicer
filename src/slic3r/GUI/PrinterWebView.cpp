#include "PrinterWebView.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/uri.h>
#include <wx/utils.h>
#include <wx/webview.h>

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

namespace Slic3r {
namespace GUI {

namespace {

using json = nlohmann::json;

struct CrealityDownloadItem
{
    std::string filename;
    std::string url;
};

std::string json_string_value(const json& obj, const char* key, const std::string& fallback = {})
{
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : fallback;
}

std::string creality_model_name(const std::string& model)
{
    static const std::map<std::string, std::string> names = {
        {"F008", "K2 Plus"},
        {"F012", "K2 Pro"},
        {"F018", "Creality Hi"},
        {"F021", "K2"},
    };
    auto it = names.find(model);
    return it != names.end() ? it->second : model;
}

std::string host_address_from_config(const DynamicPrintConfig& cfg)
{
    std::string host = cfg.opt_string("print_host");
    if (host.empty())
        host = cfg.opt_string("print_host_webui");

    std::string address = host;
    size_t scheme = address.find("://");
    if (scheme != std::string::npos)
        address = address.substr(scheme + 3);
    size_t path = address.find('/');
    if (path != std::string::npos)
        address = address.substr(0, path);
    return address.empty() ? host : address;
}

std::string make_info_url(const std::string& address)
{
    if (boost::algorithm::starts_with(address, "http://") ||
        boost::algorithm::starts_with(address, "https://"))
        return address + (boost::algorithm::ends_with(address, "/") ? "info" : "/info");
    return "http://" + address + "/info";
}

wxString file_url_for_path(const boost::filesystem::path& path)
{
    wxString url = wxString::FromUTF8(path.string());
    url.Replace("\\", "/");
    url.Replace("#", "%23");
    url = wxURI(url).BuildURI();
    return "file://" + url;
}

std::string creality_download_default_dir()
{
    std::string configured = wxGetApp().app_config ? wxGetApp().app_config->get("download_path") : "";
    if (!configured.empty() && boost::filesystem::exists(configured))
        return configured;
    return wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Downloads).ToUTF8().data();
}

std::string creality_safe_export_filename(std::string filename)
{
    if (filename.empty())
        filename = "creality-export";

    for (char& c : filename) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || static_cast<unsigned char>(c) < 32)
            c = '_';
    }

    while (!filename.empty() && (filename.back() == '.' || filename.back() == ' '))
        filename.pop_back();
    return filename.empty() ? "creality-export" : filename;
}

std::string creality_filename_from_url(const std::string& url)
{
    std::string path = url;
    size_t query = path.find_first_of("?#");
    if (query != std::string::npos)
        path = path.substr(0, query);
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
        path = path.substr(slash + 1);
    return creality_safe_export_filename(Http::url_decode(path));
}

std::vector<CrealityDownloadItem> creality_download_items_from_message(const json& message)
{
    std::vector<CrealityDownloadItem> items;
    auto it = message.find("urls");
    if (it == message.end())
        return items;

    if (it->is_string()) {
        std::string url = it->get<std::string>();
        items.push_back({creality_filename_from_url(url), url});
    } else if (it->is_object()) {
        for (const auto& item : it->items()) {
            if (item.value().is_string()) {
                std::string url = item.value().get<std::string>();
                std::string filename = creality_safe_export_filename(Http::url_decode(item.key()));
                if (filename.empty())
                    filename = creality_filename_from_url(url);
                items.push_back({filename, url});
            }
        }
    } else if (it->is_array()) {
        for (const json& value : *it) {
            if (value.is_string()) {
                std::string url = value.get<std::string>();
                items.push_back({creality_filename_from_url(url), url});
            }
        }
    }

    return items;
}

bool creality_download_url_to_file(const CrealityDownloadItem& item, const boost::filesystem::path& target, std::string& error)
{
    bool ok = false;
    std::string body;
    unsigned status = 0;

    Http::get(item.url)
        .timeout_connect(5)
        .timeout_max(300)
        .size_limit(1024ull * 1024ull * 1024ull)
        .on_complete([&](std::string response, unsigned http_status) {
            body = std::move(response);
            status = http_status;
            ok = http_status >= 200 && http_status < 300;
        })
        .on_error([&](std::string response, std::string err, unsigned http_status) {
            body = std::move(response);
            status = http_status;
            error = err.empty() ? (boost::format("HTTP %1%") % http_status).str() : err;
        })
#ifdef WIN32
        .ssl_revoke_best_effort(true)
#endif
        .perform_sync();

    if (!ok) {
        if (error.empty())
            error = (boost::format("HTTP %1%") % status).str();
        return false;
    }

    boost::filesystem::create_directories(target.parent_path());
    std::ofstream out(target.string(), std::ios::out | std::ios::binary);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!out.good()) {
        error = "Could not write exported file.";
        return false;
    }
    return true;
}

std::string sanitized_device_suffix(std::string value)
{
    boost::algorithm::replace_all(value, ":", "");
    boost::algorithm::replace_all(value, "-", "");
    boost::algorithm::replace_all(value, ".", "");
    if (value.size() > 4)
        return value.substr(value.size() - 4);
    return value;
}

} // anonymous namespace

PrinterWebView::PrinterWebView(wxWindow *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , m_creality_camera_keepalive_timer(this)
{
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    m_browser = WebView::CreateWebView(this, "");
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

#ifdef __linux__
    auto cookiesPath = boost::filesystem::path(data_dir() + "/cache/cookies.db");
    auto wv = static_cast<WebKitWebView*>(m_browser->GetNativeBackend());
    auto wv_ctx = webkit_web_view_get_context(wv);
    auto cookieManager = webkit_web_context_get_cookie_manager(wv_ctx);
    webkit_cookie_manager_set_persistent_storage(cookieManager, cookiesPath.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
#endif

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, &PrinterWebView::OnNavigating, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());
    Bind(wxEVT_TIMER, &PrinterWebView::OnCrealityCameraKeepAlive, this, m_creality_camera_keepalive_timer.GetId());

    SetSizer(topsizer);
    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    update_mode();
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);
}

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    m_creality_camera_keepalive_timer.Stop();
    SetEvtHandlerEnabled(false);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}

bool PrinterWebView::current_printer_is_creality_print() const
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return false;

    const DynamicPrintConfig& cfg = preset_bundle->printers.get_edited_preset().config;
    const auto* host_type_opt = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
    return host_type_opt && host_type_opt->value == htCrealityPrint &&
           cfg.has("print_host") && !cfg.opt_string("print_host").empty();
}

wxString PrinterWebView::creality_device_page_url() const
{
    boost::filesystem::path index = boost::filesystem::path(resources_dir()) / "web" / "deviceMgr" / "index.html";
    if (!boost::filesystem::exists(index))
        return {};

    std::string os = wxGetOsDescription().ToStdString();
    std::string query = "?version=" + wxGetApp().url_encode(SLIC3R_VERSION) +
                        "&port=0&os=" + wxGetApp().url_encode(os) +
                        "&customized=0";
    return file_url_for_path(index) + wxString::FromUTF8(query);
}

wxString PrinterWebView::resolve_url_for_current_printer(const wxString& requested_url)
{
    m_creality_device_page_active = current_printer_is_creality_print();
    m_creality_init_sent = false;

    if (!m_creality_device_page_active) {
        m_creality_camera_keepalive_timer.Stop();
        return requested_url;
    }

    wxString url = creality_device_page_url();
    if (url.empty()) {
        BOOST_LOG_TRIVIAL(error) << "Creality device page assets are missing; falling back to configured printer web UI.";
        m_creality_device_page_active = false;
        m_creality_camera_keepalive_timer.Stop();
        return requested_url;
    }

    return url;
}

json PrinterWebView::query_creality_info(const std::string& address) const
{
    json info = json::object();
    if (address.empty())
        return info;

    std::string url = make_info_url(address);
    Http::get(url)
        .timeout_max(1)
        .on_complete([&](std::string body, unsigned) {
            try {
                info = json::parse(body);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "Creality device /info parse failed: " << e.what();
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(warning) << "Creality device /info failed: HTTP " << status << " " << error << " " << body;
        })
#ifdef WIN32
        .ssl_revoke_best_effort(true)
#endif
        .perform_sync();
    return info;
}

json PrinterWebView::build_creality_device_data() const
{
    json root = json::object();
    json groups = json::array();
    json group = json::object();
    json list = json::array();

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return root;

    const auto& preset = preset_bundle->printers.get_edited_preset();
    const DynamicPrintConfig& cfg = preset.config;
    std::string address = host_address_from_config(cfg);
    json info = query_creality_info(address);

    std::string model = json_string_value(info, "model", "F018");
    std::string mac = json_string_value(info, "mac");
    if (mac.empty())
        mac = "ORCA_" + sanitized_device_suffix(address);

    std::string display_name = creality_model_name(model);
    std::string suffix = sanitized_device_suffix(mac);
    if (!suffix.empty())
        display_name += "-" + suffix;
    if (display_name.empty())
        display_name = preset.name;

    json device;
    device["address"] = address;
    device["mac"] = mac;
    device["model"] = model;
    device["name"] = display_name;
    device["type"] = 3;
    device["connectType"] = 3;
    device["oldPrinter"] = false;
    device["moonrakerPort"] = 0;
    device["fluiddPort"] = 0;
    device["mainsailPort"] = 0;
    device["deviceType"] = 0;
    device["online"] = true;
    device["video"] = true;
    device["webrtcSupport"] = 1;
    device["machinePlatformMotionEnable"] = 1;
    device["machine_LED_light_exist"] = 1;
    device["auxiliary_fan"] = 0;
    device["support_air_filtration"] = 0;
    device["machine_ptc_exist"] = 0;
    device["materialDetector1"] = 0;
    device["data"] = json::object();

    list.push_back(device);
    group["group"] = "Current Printer";
    group["list"] = list;
    groups.push_back(group);

    root["groups"] = groups;
    root["current_device"] = {{"mac", mac}};
    root["mergeState"] = false;
    return root;
}

json PrinterWebView::load_creality_machine_list() const
{
    boost::filesystem::path file = boost::filesystem::path(resources_dir()) / "profiles" / "Creality" / "machineList.json";
    if (boost::filesystem::exists(file)) {
        try {
            std::ifstream in(file.string(), std::ios::in | std::ios::binary);
            json parsed = json::parse(in);
            if (parsed.contains("printerList"))
                return parsed["printerList"];
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "Failed to load Creality machineList.json: " << e.what();
        }
    }

    return json::array({
        {
            {"engineVersion", "3.0.0"},
            {"printerIntName", "F018"},
            {"nozzleDiameter", json::array({"0.4"})},
            {"name", "Creality Hi"},
            {"type", 1},
            {"xSize", 260},
            {"ySize", 260},
            {"zSize", 300},
            {"seriesId", 77814183},
            {"supMaterialType", json::array({"PLA", "ABS", "TPU", "PETG", "PLA-CF"})}
        }
    });
}

json PrinterWebView::current_creality_device_ref() const
{
    json data = build_creality_device_data();
    if (data.contains("groups") && !data["groups"].empty() &&
        data["groups"][0].contains("list") && !data["groups"][0]["list"].empty())
        return data["groups"][0]["list"][0];
    return json::object();
}

void PrinterWebView::send_creality_command(const std::string& command, const json& data)
{
    if (!m_browser || !m_creality_device_page_active)
        return;

    json command_json;
    command_json["command"] = command;
    command_json["data"] = data;

    std::string encoded = wxGetApp().url_encode(command_json.dump(-1, ' ', true, json::error_handler_t::replace));
    WebView::RunScript(m_browser, wxString::Format("window.handleStudioCmd('%s');", wxString::FromUTF8(encoded)));
}

void PrinterWebView::send_creality_capabilities()
{
    if (!m_browser)
        return;

    json device = current_creality_device_ref();
    std::string address = json_string_value(device, "address");
    if (address.empty())
        return;

    json response;
    response["address"] = address;
    response["direction"] = 1;
    response["machine_LED_light_exist"] = 1;
    response["auxiliary_fan"] = 0;
    response["support_air_filtration"] = 0;
    response["machine_ptc_exist"] = 0;
    send_creality_command("req_device_move_direction", response);
}
void PrinterWebView::send_creality_initial_state()
{
    if (m_creality_init_sent)
        return;

    m_creality_init_sent = true;
    json device_data = build_creality_device_data();
    send_creality_command("init_device", device_data);
    send_creality_capabilities();

    auto send_delayed_creality_command = [this](const std::string& command, const json& data, int delay_ms) {
        if (!m_browser || !m_creality_device_page_active)
            return;
        json command_json;
        command_json["command"] = command;
        command_json["data"] = data;
        std::string encoded = wxGetApp().url_encode(command_json.dump(-1, ' ', true, json::error_handler_t::replace));
        WebView::RunScript(m_browser, wxString::Format(
            "setTimeout(function(){ if (window.handleStudioCmd) window.handleStudioCmd('%s'); }, %d);",
            wxString::FromUTF8(encoded), delay_ms));
    };

    auto clear_delayed_loading_overlay = [this](int delay_ms) {
        if (!m_browser || !m_creality_device_page_active)
            return;

        WebView::RunScript(m_browser, wxString::Format(R"JS(
            setTimeout(function() {
                try {
                    document.querySelectorAll('.el-loading-mask').forEach(function(el) {
                        var text = (el.textContent || '').trim();
                        if (!text || text.indexOf('Loading') >= 0)
                            el.remove();
                    });
                    if (document.body)
                        document.body.style.overflow = '';
                } catch (e) {}
            }, %d);
        )JS", delay_ms));
    };

    if (device_data.contains("groups") && !device_data["groups"].empty() &&
        device_data["groups"][0].contains("list") && !device_data["groups"][0]["list"].empty()) {
        const json& device = device_data["groups"][0]["list"][0];
        std::string mac = json_string_value(device, "mac");
        std::string address = json_string_value(device, "address");
        std::string name = json_string_value(device, "name");
        send_creality_command("set_current_device", json{{"device_id", mac}});
        send_creality_command("forward_device_detail", json{{"ip", address}, {"name", name}});
        send_delayed_creality_command("init_device", device_data, 350);
        send_delayed_creality_command("set_current_device", json{{"device_id", mac}}, 550);
        send_delayed_creality_command("forward_device_detail", json{{"ip", address}, {"name", name}}, 700);
        send_delayed_creality_command("init_device", device_data, 1100);
        send_delayed_creality_command("set_current_device", json{{"device_id", mac}}, 1300);
        send_delayed_creality_command("forward_device_detail", json{{"ip", address}, {"name", name}}, 1500);
        clear_delayed_loading_overlay(2200);
    }
    if (m_browser) {
        m_browser->CallAfter([this]() {
            if (m_creality_device_page_active) {
                send_creality_capabilities();
                json device = current_creality_device_ref();
                send_creality_command("set_current_device", json{{"device_id", json_string_value(device, "mac")}});
                send_creality_command("forward_device_detail", json{{"ip", json_string_value(device, "address")}, {"name", json_string_value(device, "name")}});
            }
        });
    }
    send_creality_command("get_lang", json(wxGetApp().app_config->get("language")));
    send_creality_command("is_dark_theme", json(wxGetApp().dark_mode()));

    json user;
    user["bLogin"] = 0;
    user["token"] = "";
    user["userId"] = "";
    user["region"] = wxGetApp().app_config->get("region");
    user["pid"] = "";
    send_creality_command("get_user", user);
}

void PrinterWebView::handle_creality_script_message(const json& message)
{
    std::string command = json_string_value(message, "command");
    if (command.empty())
        return;

    if (command == "init_device") {
        m_creality_init_sent = false;
        send_creality_initial_state();
    } else if (command == "request_all_device" || command == "get_devices") {
        json data;
        data["data"]["printerList"] = json::array();
        data["data"]["currentActivePrinterMac"] = "";
        json device_data = build_creality_device_data();
        if (device_data.contains("groups")) {
            data["data"]["printerList"] = device_data["groups"];
            data["data"]["currentActivePrinterMac"] = device_data["current_device"]["mac"];
        }
        send_creality_command(command, data);
    } else if (command == "get_lang") {
        send_creality_command("get_lang", json(wxGetApp().app_config->get("language")));
    } else if (command == "is_dark_theme") {
        send_creality_command("is_dark_theme", json(wxGetApp().dark_mode()));
    } else if (command == "get_user") {
        json user;
        user["bLogin"] = 0;
        user["token"] = "";
        user["userId"] = "";
        user["region"] = wxGetApp().app_config->get("region");
        user["pid"] = "";
        send_creality_command("get_user", user);
    } else if (command == "get_machine_list") {
        send_creality_command("get_machine_list", load_creality_machine_list());
    } else if (command == "get_device_merge_state") {
        send_creality_command("get_device_merge_state", json(false));
    } else if (command == "get_current_device") {
        json device = current_creality_device_ref();
        send_creality_command("get_current_device", json{{"mac", json_string_value(device, "mac")}});
    } else if (command == "req_device_move_direction") {
        json device = current_creality_device_ref();
        json response;
        response["address"] = message.value("address", json_string_value(device, "address"));
        response["direction"] = 1;
        response["machine_LED_light_exist"] = 1;
        response["auxiliary_fan"] = 0;
        response["support_air_filtration"] = 0;
        response["machine_ptc_exist"] = 0;
        send_creality_command("req_device_move_direction", response);
    } else if (command == "get_webrtc_local_param") {
        json response;
        // The Creality device page already sends a base64 encoded WebRTC offer.
        // Echo it back unchanged so the page's own local WebRTC handler can POST
        // the exact payload to /call/webrtc_local, matching Creality Print.
        response["sdp"] = message.value("sdp", "");
        response["url"] = message.value("url", "");
        if (message.contains("token"))
            response["token"] = message["token"];
        if (message.contains("videoEncryption"))
            response["videoEncryption"] = message["videoEncryption"];
        response["status"] = 0;
        send_creality_command("get_webrtc_local_param", response);
    } else if (command == "orca_camera_watchdog_reload") {
        BOOST_LOG_TRIVIAL(info) << "Creality camera watchdog requested device page reload";
        m_creality_init_sent = false;
        if (m_browser)
            m_browser->Reload();
    } else if (command == "get_region") {
        send_creality_command("get_region", json(wxGetApp().app_config->get("region")));
    } else if (command == "common_openurl" && message.contains("url") && message["url"].is_string()) {
        wxLaunchDefaultBrowser(wxString::FromUTF8(message["url"].get<std::string>()));
    } else if (command == "down_files") {
        std::vector<CrealityDownloadItem> items = creality_download_items_from_message(message);
        if (items.empty()) {
            send_creality_command("update_download_progress", json(0));
            return;
        }

        std::vector<std::pair<CrealityDownloadItem, boost::filesystem::path>> downloads;
        std::string default_dir = creality_download_default_dir();
        if (items.size() == 1) {
            wxFileDialog dialog(
                this,
                _L("Export file"),
                wxString::FromUTF8(default_dir),
                wxString::FromUTF8(items.front().filename),
                _L("All files (*.*)|*.*"),
                wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (dialog.ShowModal() != wxID_OK) {
                send_creality_command("update_download_progress", json(0));
                return;
            }
            downloads.push_back({items.front(), boost::filesystem::path(dialog.GetPath().ToStdWstring())});
        } else {
            wxDirDialog dialog(
                this,
                _L("Choose export folder"),
                wxString::FromUTF8(default_dir),
                wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
            if (dialog.ShowModal() != wxID_OK) {
                send_creality_command("update_download_progress", json(0));
                return;
            }
            boost::filesystem::path folder(dialog.GetPath().ToStdWstring());
            for (const CrealityDownloadItem& item : items)
                downloads.push_back({item, folder / item.filename});
        }

        send_creality_command("update_download_progress", json(1));
        std::thread([this, downloads = std::move(downloads)]() {
            std::string last_error;
            bool all_ok = true;
            for (size_t i = 0; i < downloads.size(); ++i) {
                std::string error;
                bool ok = creality_download_url_to_file(downloads[i].first, downloads[i].second, error);
                if (!ok) {
                    all_ok = false;
                    last_error = downloads[i].first.filename + ": " + error;
                    BOOST_LOG_TRIVIAL(error) << "Creality export failed: " << last_error;
                    break;
                }
                int progress = static_cast<int>(((i + 1) * 100) / downloads.size());
                if (m_browser) {
                    m_browser->CallAfter([this, progress]() {
                        send_creality_command("update_download_progress", json(progress));
                    });
                }
            }

            if (m_browser) {
                m_browser->CallAfter([this, all_ok, last_error]() {
                    send_creality_command("update_download_progress", json(all_ok ? 100 : 0));
                    if (!all_ok)
                        wxMessageBox(wxString::FromUTF8(last_error), _L("Export failed"), wxOK | wxICON_ERROR, this);
                });
            }
        }).detach();
    } else if (command == "update_devices" || command == "set_current_device" ||
               command == "add_device" || command == "update_device" ||
               command == "viewDetialEvent" || command == "sync_mappinp_cfs_filament") {
        BOOST_LOG_TRIVIAL(trace) << "Creality device page command handled locally: " << command;
    } else {
        BOOST_LOG_TRIVIAL(trace) << "Creality device page command not implemented in Orca bridge: " << command;
    }
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt)
{
    try {
        std::string input = evt.GetString().ToUTF8().data();
        if (input.empty())
            return;

        json message;
        try {
            message = json::parse(input);
        } catch (...) {
            message = json::parse(wxGetApp().url_decode(input));
        }

        // Export/open-url commands may arrive from a still-visible Creality page
        // after Orca has refreshed printer state and cleared the active flag.
        // Keep those bridge commands alive so Local Files export never becomes a no-op.
        std::string command = json_string_value(message, "command");
        if (!m_creality_device_page_active &&
            command != "down_files" &&
            command != "common_openurl" &&
            command != "orca_camera_watchdog_reload")
            return;

        handle_creality_script_message(message);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality device page script message failed: " << e.what();
    }
}

void PrinterWebView::OnNavigating(wxWebViewEvent& evt)
{
    wxString url = evt.GetURL();
    const wxString download_scheme = "orca-creality-download:";
    if (!url.StartsWith(download_scheme))
        return;

    evt.Veto();
    try {
        wxString encoded = url.Mid(download_scheme.Length());
        std::string payload = wxGetApp().url_decode(encoded.ToUTF8().data());
        json message = json::parse(payload);
        handle_creality_script_message(message);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Creality device page download navigation failed: " << e.what();
    }
}

void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    if (m_browser == nullptr)
        return;

    wxString final_url = resolve_url_for_current_printer(url);
    m_apikey = m_creality_device_page_active ? wxString() : apikey;
    m_apikey_sent = false;
    if (m_creality_device_page_active) {
        m_creality_init_sent = false;
        m_browser->RemoveAllUserScripts();
        m_browser->AddUserScript(R"JS(
            (function() {
                function patchWxBridge() {
                    if (!window.wx || !window.wx.postMessage || window.wx.__orcaJsonBridge)
                        return;
                    const originalPostMessage = window.wx.postMessage.bind(window.wx);
                    window.wx.postMessage = function(message) {
                        if (typeof message !== "string") {
                            try { message = JSON.stringify(message); }
                            catch (e) { message = String(message); }
                        }
                        return originalPostMessage(message);
                    };
                    window.wx.__orcaJsonBridge = true;
                }
                patchWxBridge();
                document.addEventListener("DOMContentLoaded", patchWxBridge);
                setTimeout(patchWxBridge, 100);
                setTimeout(patchWxBridge, 1000);
            })();
        )JS");
    }

    if (this->IsShown()) {
        m_url_deferred.clear();
        m_browser->LoadURL(final_url);
    } else {
        m_url_deferred = final_url;
    }
    if (!m_creality_device_page_active)
        m_creality_camera_keepalive_timer.Stop();
    UpdateState();
}

bool PrinterWebView::Show(bool show)
{
    if (show && !m_url_deferred.empty()) {
        m_browser->LoadURL(m_url_deferred);
        m_url_deferred.clear();
    }
    if (!show)
        m_creality_camera_keepalive_timer.Stop();
    else if (m_creality_device_page_active && !m_creality_camera_keepalive_timer.IsRunning())
        m_creality_camera_keepalive_timer.Start(60000);
    return wxPanel::Show(show);
}

void PrinterWebView::reload()
{
    m_creality_init_sent = false;
    m_browser->Reload();
}

void PrinterWebView::resume_creality_page()
{
    if (!m_creality_device_page_active)
        return;

    send_creality_initial_state();
    if (!m_creality_camera_keepalive_timer.IsRunning())
        m_creality_camera_keepalive_timer.Start(60000);
}

void PrinterWebView::update_mode()
{
    m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
}

void PrinterWebView::UpdateState()
{
}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    m_creality_camera_keepalive_timer.Stop();
    this->Hide();
}

void PrinterWebView::OnCrealityCameraKeepAlive(wxTimerEvent& evt)
{
    if (!m_browser || !m_creality_device_page_active || !IsShownOnScreen())
        return;

    send_creality_capabilities();

    WebView::RunScript(m_browser, R"JS(
        (function() {
            const now = Date.now();
            const state = window.__orcaCrealityCameraWatch || (window.__orcaCrealityCameraWatch = {
                lastChange: now,
                signature: "",
                reloads: 0,
                lastHardReload: 0
            });

            const media = Array.from(document.querySelectorAll("video,img,iframe,canvas"));
            const signature = media.map((el) => {
                const tag = el.tagName;
                if (tag === "VIDEO")
                    return ["video", el.currentTime || 0, el.videoWidth || 0, el.videoHeight || 0, el.readyState || 0, el.paused ? 1 : 0].join(":");
                if (tag === "IMG" || tag === "IFRAME")
                    return [tag.toLowerCase(), el.src || ""].join(":");
                return ["canvas", el.width || 0, el.height || 0].join(":");
            }).join("|");

            const loadingVisible = Array.from(document.querySelectorAll(".el-loading-mask,.spinner-wrapper")).some((el) => {
                const rect = el.getBoundingClientRect();
                const style = window.getComputedStyle(el);
                return rect.width > 20 && rect.height > 20 && style.display !== "none" && style.visibility !== "hidden" && style.opacity !== "0";
            });

            if (signature && signature !== state.signature) {
                state.signature = signature;
                state.lastChange = now;
                state.reloads = 0;
                if (!loadingVisible) {
                    document.querySelectorAll(".el-loading-mask").forEach((el) => {
                        const text = (el.textContent || "").trim();
                        if (!text || /Loading|Reconnecting/i.test(text))
                            el.remove();
                    });
                }
                return;
            }

            if (loadingVisible && now - state.lastChange > 25000 && now - state.lastHardReload > 60000) {
                state.lastHardReload = now;
                try {
                    window.wx && window.wx.postMessage && window.wx.postMessage(JSON.stringify({command:"orca_camera_watchdog_reload"}));
                } catch (e) {
                    window.location.reload();
                }
                return;
            }

            if (now - state.lastChange < 60000)
                return;

            for (const video of document.querySelectorAll("video")) {
                try {
                    if (video.paused || video.readyState < 2)
                        video.play().catch(() => {});
                    else {
                        video.pause();
                        video.load();
                        video.play().catch(() => {});
                    }
                } catch (e) {}
            }

            for (const el of document.querySelectorAll("img,iframe")) {
                const src = el.src || "";
                if (/camera|webrtc|video|stream|snapshot/i.test(src)) {
                    const sep = src.indexOf("?") >= 0 ? "&" : "?";
                    el.src = src.replace(/([?&])_orca_camera_keepalive=\d+/, "$1_orca_camera_keepalive=" + now);
                    if (el.src === src)
                        el.src = src + sep + "_orca_camera_keepalive=" + now;
                }
            }

            state.lastChange = now;
            state.reloads += 1;
            if (state.reloads >= 2 && now - state.lastHardReload > 60000) {
                state.lastHardReload = now;
                try {
                    window.wx && window.wx.postMessage && window.wx.postMessage(JSON.stringify({command:"orca_camera_watchdog_reload"}));
                } catch (e) {
                    window.location.reload();
                }
            }
        })();
    )JS");
}

void PrinterWebView::SendAPIKey()
{
    if (m_creality_device_page_active)
        return;
    if (m_apikey_sent || m_apikey.IsEmpty())
        return;

    m_apikey_sent = true;
    wxString script = wxString::Format(R"(
    if (window.fetch) {
        const originalFetch = window.fetch;
        window.fetch = function(input, init = {}) {
            init.headers = init.headers || {};
            init.headers['X-API-Key'] = '%s';
            return originalFetch(input, init);
        };
    }
)",
                                       m_apikey);
    m_browser->RemoveAllUserScripts();
    m_browser->AddUserScript(script);
    m_browser->Reload();
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
    case wxWEBVIEW_NAV_ERR_CONNECTION: e = "wxWEBVIEW_NAV_ERR_CONNECTION"; break;
    case wxWEBVIEW_NAV_ERR_CERTIFICATE: e = "wxWEBVIEW_NAV_ERR_CERTIFICATE"; break;
    case wxWEBVIEW_NAV_ERR_AUTH: e = "wxWEBVIEW_NAV_ERR_AUTH"; break;
    case wxWEBVIEW_NAV_ERR_SECURITY: e = "wxWEBVIEW_NAV_ERR_SECURITY"; break;
    case wxWEBVIEW_NAV_ERR_NOT_FOUND: e = "wxWEBVIEW_NAV_ERR_NOT_FOUND"; break;
    case wxWEBVIEW_NAV_ERR_REQUEST: e = "wxWEBVIEW_NAV_ERR_REQUEST"; break;
    case wxWEBVIEW_NAV_ERR_USER_CANCELLED: e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED"; break;
    case wxWEBVIEW_NAV_ERR_OTHER: e = "wxWEBVIEW_NAV_ERR_OTHER"; break;
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": error loading page %1% %2% %3% %4%") %
        evt.GetURL() % evt.GetTarget() % e % evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;

    if (m_creality_device_page_active) {
        send_creality_initial_state();
        if (!m_creality_camera_keepalive_timer.IsRunning())
            m_creality_camera_keepalive_timer.Start(60000);
        return;
    }

    SendAPIKey();
}

} // namespace GUI
} // namespace Slic3r
