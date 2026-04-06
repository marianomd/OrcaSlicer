#include "Flashforge.hpp"
#include <algorithm>
#include <ctime>
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include <set>
#include <map>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>

#include <wx/frame.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include <boost/beast/core/detail/base64.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "Http.hpp"
#include "TCPConsole.hpp"
#include "SerialMessage.hpp"
#include "SerialMessageType.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;
using json = nlohmann::json;

namespace Slic3r {

namespace {

constexpr unsigned short FLASHFORGE_DISCOVERY_PORT        = 48899;
constexpr unsigned short FLASHFORGE_DISCOVERY_LISTEN_PORT = 18007;

const std::array<unsigned char, 20> FLASHFORGE_DISCOVERY_MESSAGE = {
    0x77, 0x77, 0x77, 0x2e, 0x75, 0x73, 0x72, 0x22,
    0x65, 0x36, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

std::string trim_null_terminated_ascii(const char* data, size_t len)
{
    std::string out(data, data + len);
    const auto  pos = out.find('\0');
    if (pos != std::string::npos)
        out.resize(pos);
    boost::trim(out);
    return out;
}

bool parse_discovery_response(const std::vector<unsigned char>& response, const std::string& ip_address, FlashforgeDiscoveredPrinter& printer)
{
    if (response.size() < 0xC4)
        return false;

    printer.name          = trim_null_terminated_ascii(reinterpret_cast<const char*>(response.data()), 32);
    printer.serial_number = trim_null_terminated_ascii(reinterpret_cast<const char*>(response.data() + 0x92), 32);
    printer.ip_address    = ip_address;
    return !(printer.name.empty() && printer.serial_number.empty());
}

std::vector<std::string> get_discovery_broadcast_addresses()
{
    std::set<std::string> addresses = {"255.255.255.255", "192.168.0.255", "192.168.1.255"};

    try {
        boost::asio::io_context          io_context;
        boost::asio::ip::tcp::resolver   resolver(io_context);
        boost::system::error_code        ec;
        const auto                       host_name = boost::asio::ip::host_name(ec);
        if (!ec) {
            const auto results = resolver.resolve(boost::asio::ip::tcp::v4(), host_name, "", ec);
            if (!ec) {
                for (const auto& entry : results) {
                    const auto addr = entry.endpoint().address();
                    if (!addr.is_v4())
                        continue;

                    const auto bytes = addr.to_v4().to_bytes();
                    if (bytes[0] == 127)
                        continue;

                    addresses.insert((boost::format("%1%.%2%.%3%.255") % static_cast<int>(bytes[0]) % static_cast<int>(bytes[1]) % static_cast<int>(bytes[2])).str());
                }
            }
        }
    } catch (...) {
    }

    return {addresses.begin(), addresses.end()};
}

std::string safe_config_string(DynamicPrintConfig* config, const char* key)
{
    if (config == nullptr)
        return {};

    if (const auto* opt = config->option<ConfigOptionString>(key); opt != nullptr)
        return opt->value;

    return {};
}

std::string mask_secret(const std::string& value, size_t prefix = 2, size_t suffix = 2)
{
    if (value.empty())
        return value;

    if (value.size() <= prefix + suffix)
        return std::string(value.size(), '*');

    return value.substr(0, prefix) + std::string(value.size() - prefix - suffix, '*') + value.substr(value.size() - suffix);
}

std::string trim_for_log(const std::string& value, size_t limit = 1600)
{
    if (value.size() <= limit)
        return value;
    return value.substr(0, limit) + "...<truncated>";
}

std::string sanitize_json_for_log(const std::string& body)
{
    const auto parsed = json::parse(body, nullptr, false, true);
    if (parsed.is_discarded() || !parsed.is_object())
        return trim_for_log(body);

    json sanitized = parsed;
    for (const char* key : {"checkCode", "serialNumber"}) {
        if (sanitized.contains(key) && sanitized[key].is_string()) {
            const std::string raw = sanitized[key].get<std::string>();
            sanitized[key] = (std::string(key) == "checkCode") ? mask_secret(raw) : trim_for_log(raw, 16);
        }
    }

    return trim_for_log(sanitized.dump());
}

std::string json_field_to_log_string(const json& object, const char* key)
{
    if (!object.is_object() || !object.contains(key))
        return "<missing>";
    const json& value = object[key];
    if (value.is_null())
        return "null";
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer() || value.is_number_unsigned())
        return std::to_string(value.get<long long>());
    if (value.is_number_float())
        return std::to_string(value.get<double>());
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_array())
        return (boost::format("array(%1%)") % value.size()).str();
    if (value.is_object())
        return "object";
    return "<unknown>";
}

bool try_parse_json_int(const json& value, int& out)
{
    try {
        if (value.is_number_integer() || value.is_number_unsigned()) {
            out = value.get<int>();
            return true;
        }

        if (value.is_boolean()) {
            out = value.get<bool>() ? 1 : 0;
            return true;
        }

        if (value.is_string()) {
            std::string text = value.get<std::string>();
            boost::trim(text);
            if (text.empty())
                return false;

            size_t pos = 0;
            const long parsed = std::stol(text, &pos, 10);
            if (pos == text.size()) {
                out = static_cast<int>(parsed);
                return true;
            }
        }
    } catch (...) {
    }

    return false;
}

bool validate_local_api_response(const std::string& response_body, wxString& error_msg)
{
    const auto parsed = json::parse(response_body, nullptr, false, true);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error_msg = _(L("Flashforge returned an invalid JSON response."));
        return false;
    }

    int  result_code = 0;
    bool has_code    = false;

    if (parsed.contains("code"))
        has_code = try_parse_json_int(parsed["code"], result_code);
    if (!has_code && parsed.contains("err"))
        has_code = try_parse_json_int(parsed["err"], result_code);

    if (has_code && result_code != 0) {
        std::string message;
        if (parsed.contains("message") && parsed["message"].is_string())
            message = parsed["message"].get<std::string>();
        else if (parsed.contains("msg") && parsed["msg"].is_string())
            message = parsed["msg"].get<std::string>();

        if (message.empty())
            message = "Request failed";

        error_msg = GUI::from_u8((boost::format("Flashforge local API error %1%: %2%") % result_code % message).str());
        return false;
    }

    return true;
}

} // namespace

Flashforge::Flashforge(DynamicPrintConfig* config)
    : m_host()
    , m_serial_number()
    , m_check_code()
    , m_supports_material_station(false)
    , m_console_port("8899")
    , m_gcFlavor(gcfMarlinLegacy)
    , m_bufferSize(4096) // 4K buffer size
{
    m_host          = safe_config_string(config, "print_host");
    m_serial_number = safe_config_string(config, "flashforge_serial_number");
    m_check_code    = safe_config_string(config, "printhost_apikey");
    const auto printer_model = safe_config_string(config, "printer_model");
    m_supports_material_station = boost::icontains(printer_model, "AD5X");

    if (config != nullptr) {
        if (const auto* gcode_flavor = config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor"); gcode_flavor != nullptr)
        m_gcFlavor = gcode_flavor->value;
    }
}

const char* Flashforge::get_name() const { return "Flashforge"; }

bool Flashforge::discover_printers(std::vector<FlashforgeDiscoveredPrinter>& printers, wxString& msg, int timeout_ms, int idle_timeout_ms, int max_retries)
{
    printers.clear();

    try {
        const auto broadcast_addresses = get_discovery_broadcast_addresses();
        std::map<std::string, FlashforgeDiscoveredPrinter> by_ip;

        for (int attempt = 0; attempt < std::max(1, max_retries); ++attempt) {
            boost::asio::io_context      io_context;
            boost::asio::ip::udp::socket socket(io_context);
            boost::system::error_code    ec;

            socket.open(boost::asio::ip::udp::v4(), ec);
            if (ec) {
                msg = wxString::FromUTF8(ec.message().c_str());
                return false;
            }

            socket.set_option(boost::asio::socket_base::broadcast(true), ec);
            if (ec) {
                msg = wxString::FromUTF8(ec.message().c_str());
                return false;
            }

            socket.set_option(boost::asio::socket_base::reuse_address(true), ec);
            if (ec) {
                msg = wxString::FromUTF8(ec.message().c_str());
                return false;
            }

            socket.bind({boost::asio::ip::udp::v4(), FLASHFORGE_DISCOVERY_LISTEN_PORT}, ec);
            if (ec) {
                msg = wxString::FromUTF8(ec.message().c_str());
                return false;
            }

            for (const auto& addr : broadcast_addresses) {
                socket.send_to(boost::asio::buffer(FLASHFORGE_DISCOVERY_MESSAGE),
                               {boost::asio::ip::make_address_v4(addr, ec), FLASHFORGE_DISCOVERY_PORT}, 0, ec);
                ec.clear();
            }

            socket.non_blocking(true, ec);
            if (ec) {
                msg = wxString::FromUTF8(ec.message().c_str());
                return false;
            }

            const auto start      = std::chrono::steady_clock::now();
            auto       last_reply = start;

            while (true) {
                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout_ms)
                    break;
                if (!by_ip.empty() && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reply).count() >= idle_timeout_ms)
                    break;

                std::vector<unsigned char> buffer(512);
                boost::asio::ip::udp::endpoint remote_endpoint;
                const auto received = socket.receive_from(boost::asio::buffer(buffer), remote_endpoint, 0, ec);
                if (!ec) {
                    buffer.resize(received);
                    FlashforgeDiscoveredPrinter printer;
                    if (parse_discovery_response(buffer, remote_endpoint.address().to_string(), printer)) {
                        by_ip[printer.ip_address] = std::move(printer);
                        last_reply               = std::chrono::steady_clock::now();
                    }
                } else if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
                    ec.clear();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                } else {
                    msg = wxString::FromUTF8(ec.message().c_str());
                    return false;
                }
            }

            if (!by_ip.empty())
                break;
        }

        for (auto& [_, printer] : by_ip)
            printers.emplace_back(std::move(printer));

        std::sort(printers.begin(), printers.end(), [](const FlashforgeDiscoveredPrinter& lhs, const FlashforgeDiscoveredPrinter& rhs) {
            if (lhs.name != rhs.name)
                return lhs.name < rhs.name;
            return lhs.ip_address < rhs.ip_address;
        });

        if (printers.empty()) {
            msg = _(L("No Flashforge printers were discovered on the local network."));
            return false;
        }

        return true;
    } catch (const std::exception& ex) {
        msg = wxString::FromUTF8(ex.what());
        return false;
    }
}

bool Flashforge::test(wxString& msg) const
{
    if (!m_serial_number.empty() && !m_check_code.empty())
        return test_local_api(msg);

    BOOST_LOG_TRIVIAL(debug) << boost::format("[Flashforge Serial] testing connection");
    // Utils::TCPConsole console(m_host, m_console_port);
    Utils::TCPConsole client(m_host, m_console_port);
    client.enqueue_cmd(controlCommand);
    bool res = client.run_queue();
    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] testing connection failed");
    } else {
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] testing connection success");
    }
    return res;
}

wxString Flashforge::get_test_ok_msg() const
{
    if (!m_serial_number.empty() && !m_check_code.empty())
        return _(L("Connected to Flashforge local API successfully."));
    return _(L("Serial connection to Flashforge is working correctly."));
}

wxString Flashforge::get_test_failed_msg(wxString& msg) const
{
    const std::string prefix = (!m_serial_number.empty() && !m_check_code.empty()) ?
        _utf8(L("Could not connect to Flashforge local API")) :
        _utf8(L("Could not connect to Flashforge via serial"));
    return GUI::from_u8((boost::format("%s: %s") % prefix % std::string(msg.ToUTF8())).str());
}


bool Flashforge::connect(wxString& msg) const
{
    
    Utils::TCPConsole client(m_host, m_console_port);

    client.enqueue_cmd(controlCommand);
    client.enqueue_cmd(deviceInfoCommand);

    if (m_gcFlavor == gcfKlipper)
        client.enqueue_cmd(connectKlipperCommand);
    else {
        client.enqueue_cmd(connectLegacyCommand);

    }

    client.enqueue_cmd(statusCommand);
    

    bool res = client.run_queue();

    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Failed to initiate connection");
    } else
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Successfully initiated Connection");

    return res;
}

bool Flashforge::start_print(wxString& msg, const std::string& filename) const
{
    Utils::TCPConsole            client(m_host, m_console_port);
    Slic3r::Utils::SerialMessage startPrintCommand = {(boost::format("~M23 0:/user/%1%") % filename).str(), Slic3r::Utils::Command};
    client.enqueue_cmd(startPrintCommand);
    bool res = client.run_queue();

    if (!res) {
        msg = wxString::FromUTF8(client.error_message().c_str());
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Failed to start print %1%") % filename;
    } else
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Started print %1%") % filename;

    return res;
}

bool Flashforge::upload(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn, InfoFn info_fn) const
{
    if (!m_serial_number.empty() && !m_check_code.empty())
        return upload_local_api(std::move(upload_data), std::move(progress_fn), std::move(error_fn));

    bool res = true;
    wxString errormsg;

    Utils::TCPConsole client(m_host, m_console_port);

    try {

        res = connect(errormsg);

        std::ifstream newfile;
        newfile.open(upload_data.source_path.c_str(), std::ios::binary); // open a file to perform read operation using file object
        std::string gcodeFile;
        if (newfile.is_open()) {                                         // checking whether the file is open
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Reading file...");
            newfile.seekg(0, std::ios::end);
            std::ifstream::pos_type pos = newfile.tellg();

            std::vector<char> result(pos);

            newfile.seekg(0, std::ios::beg);
            newfile.read(&result[0], pos);

            gcodeFile = std::string(result.begin(), result.end()); // TODO: Find more efficient way of breaking ifstream

            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Reading file...done size is %1%") % gcodeFile.size();

            newfile.close(); // close the file object.
        }
        Slic3r::Utils::SerialMessage fileuploadCommand =
            {(boost::format("~M28 %1% 0:/user/%2%") % gcodeFile.size() % upload_data.upload_path.generic_string()).str(),
             Slic3r::Utils::Command};
        client.enqueue_cmd(fileuploadCommand);

        //client.set_tcp_queue_delay(std::chrono::nanoseconds(10000));

        for (int bytePos = 0; bytePos < gcodeFile.size(); bytePos += m_bufferSize) { // TODO: Find more efficient way of breaking ifstream

            int bytePosEnd  = (gcodeFile.size() - bytePos > m_bufferSize - 1) ? m_bufferSize : gcodeFile.size();
            Slic3r::Utils::SerialMessage dataCommand = {gcodeFile.substr(bytePos, bytePosEnd), Slic3r::Utils::Data}; // Break into smaller byte chunks

            client.enqueue_cmd(dataCommand);

        }

        res = client.run_queue();

        if (res)
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Sent %1% ") % gcodeFile.size();


        if (!res) {
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] error %1%") % client.error_message().c_str();
            errormsg = wxString::FromUTF8(client.error_message().c_str());
            error_fn(std::move(errormsg));
        } else {

            client.set_tcp_queue_delay(std::chrono::milliseconds(3000));

            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] Sending file save command ");
            
            client.enqueue_cmd(saveFileCommand);

            res = client.run_queue();

            if (upload_data.post_action == PrintHostPostUploadAction::StartPrint)
                res = start_print(errormsg, upload_data.upload_path.string());
        }

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge Serial] error %1%") % e.what();
        errormsg = wxString::FromUTF8(e.what());
        error_fn(std::move(errormsg));
    }

    return res;
}

bool Flashforge::test_local_api(wxString& msg) const
{
    std::string body;
    return request_local_api_json("detail", json{{"serialNumber", m_serial_number}, {"checkCode", m_check_code}}.dump(), body, msg);
}

bool Flashforge::fetch_material_slots(std::vector<FlashforgeMaterialSlot>& slots, bool* supports_material_station, wxString& msg) const
{
    slots.clear();

    if (m_serial_number.empty() || m_check_code.empty()) {
        msg = _(L("Flashforge local API requires both serial number and access code."));
        return false;
    }

    std::string body;
    if (!request_local_api_json("detail", json{{"serialNumber", m_serial_number}, {"checkCode", m_check_code}}.dump(), body, msg))
        return false;

    BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge DIAG] /detail response body: %1%") % trim_for_log(body);

    const auto parsed = json::parse(body, nullptr, false, true);
    if (parsed.is_discarded()) {
        msg = _(L("Flashforge returned an invalid JSON response."));
        return false;
    }

    const auto& detail = parsed.contains("detail") ? parsed["detail"] : parsed;
    const auto& station = detail.contains("matlStationInfo") ? detail["matlStationInfo"] :
                          detail.contains("MatlStationInfo") ? detail["MatlStationInfo"] : json();
    const auto& slot_infos = station.contains("slotInfos") ? station["slotInfos"] :
                             station.contains("SlotInfos") ? station["SlotInfos"] : json::array();

    bool reports_material_station = false;

    int has_material_station_flag = 0;
    if (detail.contains("hasMatlStation") && try_parse_json_int(detail["hasMatlStation"], has_material_station_flag))
        reports_material_station = has_material_station_flag != 0;
    else if (detail.contains("HasMatlStation") && try_parse_json_int(detail["HasMatlStation"], has_material_station_flag))
        reports_material_station = has_material_station_flag != 0;

    int slot_count = 0;
    if (station.contains("slotCnt") && try_parse_json_int(station["slotCnt"], slot_count))
        reports_material_station = reports_material_station || slot_count > 0;
    else if (station.contains("SlotCnt") && try_parse_json_int(station["SlotCnt"], slot_count))
        reports_material_station = reports_material_station || slot_count > 0;

    if (slot_infos.is_array() && !slot_infos.empty())
        reports_material_station = true;

    if (supports_material_station != nullptr)
        *supports_material_station = reports_material_station || m_supports_material_station;

    BOOST_LOG_TRIVIAL(info)
        << boost::format("[Flashforge DIAG] /detail model=%1% hasMatlStation=%2% slotCnt=%3% slotInfos=%4% supportsMaterialStation=%5% (fallbackFromModel=%6%)")
               % json_field_to_log_string(detail, "name")
               % (detail.contains("hasMatlStation") ? json_field_to_log_string(detail, "hasMatlStation") :
                  detail.contains("HasMatlStation") ? json_field_to_log_string(detail, "HasMatlStation") : "<missing>")
               % (station.contains("slotCnt") ? json_field_to_log_string(station, "slotCnt") :
                  station.contains("SlotCnt") ? json_field_to_log_string(station, "SlotCnt") : "<missing>")
               % (slot_infos.is_array() ? std::to_string(slot_infos.size()) : std::string("<not-array>"))
               % ((supports_material_station != nullptr && *supports_material_station) ? "true" : "false")
               % (m_supports_material_station ? "true" : "false");

    for (const auto& slot : slot_infos) {
        FlashforgeMaterialSlot info;
        info.slot_id        = slot.value("slotId", static_cast<int>(slots.size()) + 1);
        info.has_filament   = slot.value("hasFilament", false);
        info.material_name  = slot.value("materialName", std::string());
        info.material_color = slot.value("materialColor", std::string());
        slots.emplace_back(std::move(info));

        const auto& parsed_slot = slots.back();
        BOOST_LOG_TRIVIAL(info)
            << boost::format("[Flashforge DIAG] /detail slot slotId=%1% hasFilament=%2% materialName='%3%' materialColor='%4%'")
                   % parsed_slot.slot_id
                   % (parsed_slot.has_filament ? "true" : "false")
                   % parsed_slot.material_name
                   % parsed_slot.material_color;
    }

    return true;
}

bool Flashforge::upload_local_api(PrintHostUpload upload_data, ProgressFn progress_fn, ErrorFn error_fn) const
{
    bool        res            = true;
    std::string material_map_b64;
    std::string material_map_json = "[]";
    auto        leveling_before_print = upload_data.extended_info["levelingBeforePrint"] == "1";
    auto        time_lapse_video      = upload_data.extended_info["timeLapseVideo"] == "1";
    auto        use_material_station  = upload_data.extended_info["useMatlStation"] == "1";

    if (auto it = upload_data.extended_info.find("materialMappings"); it != upload_data.extended_info.end())
        material_map_json = it->second;

    material_map_b64.resize(boost::beast::detail::base64::encoded_size(material_map_json.size()));
    material_map_b64.resize(boost::beast::detail::base64::encode(material_map_b64.data(), material_map_json.data(), material_map_json.size()));

    auto        url      = make_http_url("uploadGcode");
    auto        filename = upload_data.upload_path.filename().string();
    std::string file_size;
    try {
        file_size = std::to_string(fs::file_size(upload_data.source_path));
    } catch (...) {
        file_size = "0";
    }

    auto http = Http::post(url);
    BOOST_LOG_TRIVIAL(info)
        << boost::format("[Flashforge DIAG] uploadGcode request serial=%1% checkCode=%2% file=%3% fileSize=%4% printNow=%5% leveling=%6% useMatlStation=%7% gcodeToolCnt=%8% materialMappingsBytes=%9%")
               % trim_for_log(m_serial_number, 16)
               % mask_secret(m_check_code)
               % filename
               % file_size
               % (upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
               % (leveling_before_print ? "true" : "false")
               % (use_material_station ? "true" : "false")
               % upload_data.extended_info["gcodeToolCnt"]
               % material_map_json.size();
    http.header("serialNumber", m_serial_number)
        .header("checkCode", m_check_code)
        .header("fileSize", file_size)
        .header("printNow", upload_data.post_action == PrintHostPostUploadAction::StartPrint ? "true" : "false")
        .header("levelingBeforePrint", leveling_before_print ? "true" : "false")
        .header("flowCalibration", "false")
        .header("firstLayerInspection", "false")
        .header("timeLapseVideo", time_lapse_video ? "true" : "false")
        .header("useMatlStation", use_material_station ? "true" : "false")
        .header("gcodeToolCnt", upload_data.extended_info["gcodeToolCnt"])
        .header("materialMappings", material_map_b64)
        .form_add_file("gcodeFile", upload_data.source_path.string(), filename)
        .on_complete([&](std::string body, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge DIAG] uploadGcode response: HTTP %1% body: %2%") % status % trim_for_log(body);
            wxString msg;
            if (!validate_local_api_response(body, msg)) {
                BOOST_LOG_TRIVIAL(error) << boost::format("[Flashforge HTTP] upload rejected by printer: HTTP %1% body: `%2%`") % status % body;
                error_fn(msg);
                res = false;
            } else {
                BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge HTTP] upload complete: HTTP %1% body: %2%") % status % body;
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("[Flashforge HTTP] upload failed: %1%, HTTP %2%, body: `%3%`") % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool& cancel) {
            progress_fn(std::move(progress), cancel);
            if (cancel)
                res = false;
        })
        .perform_sync();

    return res;
}

bool Flashforge::request_local_api_json(const std::string& path, const std::string& body, std::string& response_body, wxString& error_msg) const
{
    bool ok = true;
    BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge DIAG] POST /%1% request body: %2%") % path % sanitize_json_for_log(body);
    auto http = Http::post(make_http_url(path));
    http.header("Content-Type", "application/json")
        .set_post_body(body)
        .on_complete([&](std::string body_text, unsigned) {
            response_body = std::move(body_text);
            BOOST_LOG_TRIVIAL(info) << boost::format("[Flashforge DIAG] POST /%1% response body: %2%") % path % trim_for_log(response_body);
            if (!validate_local_api_response(response_body, error_msg))
                ok = false;
        })
        .on_error([&](std::string body_text, std::string error, unsigned status) {
            response_body = std::move(body_text);
            error_msg     = format_error(response_body, error, status);
            BOOST_LOG_TRIVIAL(error)
                << boost::format("[Flashforge DIAG] POST /%1% failed: error=%2% HTTP=%3% body=%4%")
                       % path
                       % error
                       % status
                       % trim_for_log(response_body);
            ok            = false;
        })
        .perform_sync();
    return ok;
}

std::string Flashforge::make_http_url(const std::string& path) const
{
    return (boost::format("http://%1%:8898/%2%") % extract_host_name() % path).str();
}

std::string Flashforge::extract_host_name() const
{
    std::string host = m_host;
    if (host.find("://") == std::string::npos) {
        const auto slash_pos = host.find('/');
        if (slash_pos != std::string::npos)
            host = host.substr(0, slash_pos);
        return host;
    }

    std::string out = host;
    CURLU*       hurl = curl_url();
    if (!hurl)
        return host;

    const auto rc = curl_url_set(hurl, CURLUPART_URL, host.c_str(), 0);
    if (rc == CURLUE_OK) {
        char* raw_host = nullptr;
        if (curl_url_get(hurl, CURLUPART_HOST, &raw_host, 0) == CURLUE_OK && raw_host != nullptr) {
            out = raw_host;
            curl_free(raw_host);
        }
    }

    curl_url_cleanup(hurl);
    return out;
}

int Flashforge::get_err_code_from_body(const std::string& body) const
{
    pt::ptree          root;
    std::istringstream iss(body); // wrap returned json to istringstream
    pt::read_json(iss, root);

    return root.get<int>("err", 0);
}

} // namespace Slic3r
