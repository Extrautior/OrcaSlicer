#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_

#include <nlohmann/json.hpp>
#include <boost/optional.hpp>
#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/webview.h>

namespace Slic3r {
namespace GUI {

class PrinterWebView : public wxPanel {
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnNavigating(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void OnCrealityCameraKeepAlive(wxTimerEvent& evt);
    void resume_creality_page();
    void reload();
    void update_mode();

    bool Show(bool show = true) override;

private:
    void SendAPIKey();
    bool current_printer_is_creality_print() const;
    wxString creality_device_page_url() const;
    wxString resolve_url_for_current_printer(const wxString& requested_url);
    nlohmann::json query_creality_info(const std::string& address) const;
    nlohmann::json build_creality_device_data() const;
    nlohmann::json load_creality_machine_list() const;
    nlohmann::json current_creality_device_ref() const;
    void send_creality_command(const std::string& command, const nlohmann::json& data);
    void send_creality_capabilities();
    void send_creality_initial_state();
    void inject_creality_logo_light_control();
    boost::optional<bool> query_creality_logo_light_state() const;
    bool set_creality_logo_light(bool on) const;
    void handle_creality_script_message(const nlohmann::json& message);

    wxWebView* m_browser { nullptr };
    long m_zoomFactor { 100 };
    wxString m_apikey;
    bool m_apikey_sent { false };
    wxString m_url_deferred;
    bool m_creality_device_page_active { false };
    bool m_creality_init_sent { false };
    wxTimer m_creality_camera_keepalive_timer;
};

} // GUI
} // Slic3r

#endif /* slic3r_PrinterWebView_hpp_ */
