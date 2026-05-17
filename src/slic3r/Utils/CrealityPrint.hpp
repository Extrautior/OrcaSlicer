#ifndef slic3r_CrealityPrint_hpp_
#define slic3r_CrealityPrint_hpp_

#include <map>
#include <string>
#include <vector>
#include <wx/string.h>
#include <boost/optional.hpp>
#include <boost/asio/ip/address.hpp>

#include "PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;
class CrealityPrint : public PrintHost
{
public:
    CrealityPrint(DynamicPrintConfig* config);
    ~CrealityPrint() override = default;

    const char* get_name() const override;
    virtual bool can_test() const { return true; };
    std::string  get_host() const override;
    bool has_auto_discovery() const override { return true; }

    wxString                           get_test_ok_msg() const override;
    wxString                           get_test_failed_msg(wxString& msg) const override;
    virtual bool                       test(wxString& curl_msg) const override;
    PrintHostPostUploadActions         get_post_upload_actions() const;
    bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool supports_multi_color_print() const;

    struct CfsSlotInfo {
        std::string tool_id;      // e.g. "T1A" or "Ext"
        std::string type;         // e.g. "PLA"
        std::string color;        // normalized "#RRGGBB"
        std::string name;
        std::string vendor;
        std::string rfid;
        int         box_id = 0;
        int         box_type = 0;
        int         material_id = 0;
        int         min_temp = 0;
        int         max_temp = 0;
        double      pressure = 0.0;
        int         humidity = -1;
        int         box_temp = 0;
        bool        external = false;
        bool        loaded = true;
    };

    std::string query_boxes_info() const;
    std::vector<CfsSlotInfo> query_cfs_slots(bool include_external_spool) const;
    bool set_cfs_slot_filament(wxString& msg, const CfsSlotInfo& slot, const std::string& type,
                               const std::string& color, const std::string& name,
                               const std::string& vendor, int min_temp, int max_temp,
                               double pressure) const;
    bool reset_cfs_slot_filament(wxString& msg, const CfsSlotInfo& slot) const;
    bool feed_or_retract_cfs_slot(wxString& msg, const CfsSlotInfo& slot, bool feed) const;
    std::string model_name() const;

protected:
    virtual void set_auth(Http& http) const;
private:
    std::string m_host;
    std::string m_port;
    std::string m_apikey;
    std::string m_cafile;
    std::string m_web_ui;
    bool        m_ssl_revoke_best_effort;
    mutable std::string m_model;

    std::string make_url(const std::string& path) const;
    bool start_print(wxString& msg, const std::string& filename, const std::map<std::string, std::string>& extended_info) const;
    std::string safe_filename(const std::string& filename) const;
    void query_model() const;
};
} // namespace Slic3r

#endif
