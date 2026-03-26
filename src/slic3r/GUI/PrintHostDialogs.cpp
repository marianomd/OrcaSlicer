#include "PrintHostDialogs.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>

#include <wx/frame.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/choice.h>
#include <wx/wrapsizer.h>
#include <wx/wupdlock.h>
#include <wx/debug.h>
#include <wx/msgdlg.h>

#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/algorithm/string.hpp>
#include <nlohmann/json.hpp>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "libslic3r/AppConfig.hpp"
#include "NotificationManager.hpp"
#include "ExtraRenderers.hpp"
#include "format.hpp"

namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace Slic3r {
namespace GUI {

static const char *CONFIG_KEY_PATH  = "printhost_path";
static const char *CONFIG_KEY_GROUP = "printhost_group";
static const char* CONFIG_KEY_STORAGE = "printhost_storage";

PrintHostSendDialog::PrintHostSendDialog(const fs::path &path, PrintHostPostUploadActions post_actions, const wxArrayString &groups, const wxArrayString& storage_paths, const wxArrayString& storage_names, bool switch_to_device_tab)
    : MsgDialog(static_cast<wxWindow*>(wxGetApp().mainframe), _L("Send G-code to printer host"), _L("Upload to Printer Host with the following filename:"), 0) // Set style = 0 to avoid default creation of the "OK" button. 
                                                                                                                                                               // All buttons will be added later in this constructor 
    , txt_filename(new wxTextCtrl(this, wxID_ANY))
    , combo_groups(!groups.IsEmpty() ? new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, groups, wxCB_READONLY) : nullptr)
    , combo_storage(storage_names.GetCount() > 1 ? new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, storage_names, wxCB_READONLY) : nullptr)
    , post_upload_action(PrintHostPostUploadAction::None)
    , m_paths(storage_paths)
    , m_switch_to_device_tab(switch_to_device_tab)
    , m_path(path)
    , m_post_actions(post_actions)
    , m_storage_names(storage_names)
{
#ifdef __APPLE__
    txt_filename->OSXDisableAllSmartSubstitutions();
#endif
}
void PrintHostSendDialog::init()
{
    const auto& path = m_path;
    const auto& storage_paths = m_paths;
    const auto& post_actions = m_post_actions;
    const auto& storage_names = m_storage_names;

    const AppConfig* app_config = wxGetApp().app_config;

    auto *label_dir_hint = new wxStaticText(this, wxID_ANY, _L("Use forward slashes ( / ) as a directory separator if needed."));
    label_dir_hint->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());

    content_sizer->Add(txt_filename, 0, wxEXPAND);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);
    
    if (combo_groups != nullptr) {
        // Repetier specific: Show a selection of file groups.
        auto *label_group = new wxStaticText(this, wxID_ANY, _L("Group"));
        content_sizer->Add(label_group);
        content_sizer->Add(combo_groups, 0, wxBOTTOM, 2*VERT_SPACING);        
        wxString recent_group = from_u8(app_config->get("recent", CONFIG_KEY_GROUP));
        if (! recent_group.empty())
            combo_groups->SetValue(recent_group);
    }

    if (combo_storage != nullptr) {
        // PrusaLink specific: User needs to choose a storage
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ":");
        content_sizer->Add(label_group);
        content_sizer->Add(combo_storage, 0, wxBOTTOM, 2 * VERT_SPACING);
        combo_storage->SetValue(storage_names.front());
        wxString recent_storage = from_u8(app_config->get("recent", CONFIG_KEY_STORAGE));
        if (!recent_storage.empty())
            combo_storage->SetValue(recent_storage); 
    } else if (storage_names.GetCount() == 1){
        // PrusaLink specific: Show which storage has been detected.
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ": " + storage_names.front());
        content_sizer->Add(label_group);
        m_preselected_storage = storage_paths.front();
    }


    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/') {
        recent_path += '/';
    }
    const auto recent_path_len = recent_path.Length();
    recent_path += path.filename().wstring();
    wxString stem(path.stem().wstring());
    const auto stem_len = stem.Length();

    txt_filename->SetValue(recent_path);

    auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto checkbox       = new ::CheckBox(this, wxID_APPLY);
    checkbox->SetValue(m_switch_to_device_tab);
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        m_switch_to_device_tab = e.IsChecked();
        e.Skip();
    });
    checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

    auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Switch to Device tab after upload."), wxDefaultPosition, wxDefaultSize, 0);
    checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
    checkbox_text->SetFont(::Label::Body_13);
    checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
    content_sizer->Add(checkbox_sizer);
    content_sizer->AddSpacer(VERT_SPACING);

    if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
        m_valid_suffix = recent_path.substr(extension_start);
    // .gcode suffix control
    auto validate_path = [this](const wxString &path) -> bool {
        if (! path.Lower().EndsWith(m_valid_suffix.Lower())) {
            MessageDialog msg_wingow(this, wxString::Format(_L("Upload filename doesn't end with \"%s\". Do you wish to continue?"), m_valid_suffix), wxString(SLIC3R_APP_NAME), wxYES | wxNO);
            if (msg_wingow.ShowModal() == wxID_NO)
                return false;
        }
        return true;
    };

    auto* btn_ok = add_button(wxID_OK, true, _L("Upload"));
    btn_ok->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
        if (validate_path(txt_filename->GetValue())) {
            post_upload_action = PrintHostPostUploadAction::None;
            EndDialog(wxID_OK);
        }
    });
    txt_filename->SetFocus();
    
    // if (post_actions.has(PrintHostPostUploadAction::QueuePrint)) {
    //     auto* btn_print = add_button(wxID_ADD, false, _L("Upload to Queue"));
    //     btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
    //         if (validate_path(txt_filename->GetValue())) {
    //             post_upload_action = PrintHostPostUploadAction::QueuePrint;
    //             EndDialog(wxID_OK);
    //         }
    //         });
    // }

    if (post_actions.has(PrintHostPostUploadAction::StartPrint)) {
        auto* btn_print = add_button(wxID_YES, false, _L("Upload and Print"));
        btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
            if (validate_path(txt_filename->GetValue())) {
                post_upload_action = PrintHostPostUploadAction::StartPrint;
                EndDialog(wxID_OK);
            }
        });
    }

    // if (post_actions.has(PrintHostPostUploadAction::StartSimulation)) {
    //     // Using wxID_MORE as a button identifier to be different from the other buttons, wxID_MORE has no other meaning here.
    //     auto* btn_simulate = add_button(wxID_MORE, false, _L("Upload and Simulate"));
    //     btn_simulate->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
    //         if (validate_path(txt_filename->GetValue())) {
    //             post_upload_action = PrintHostPostUploadAction::StartSimulation;
    //             EndDialog(wxID_OK);
    //         }        
    //     });
    // }

    add_button(wxID_CANCEL,false, L("Cancel"));
    finalize();

#ifdef __linux__
    // On Linux with GTK2 when text control lose the focus then selection (colored background) disappears but text color stay white
    // and as a result the text is invisible with light mode
    // see https://github.com/prusa3d/PrusaSlicer/issues/4532
    // Workaround: Unselect text selection explicitly on kill focus
    txt_filename->Bind(wxEVT_KILL_FOCUS, [this](wxEvent& e) {
        e.Skip();
        txt_filename->SetInsertionPoint(txt_filename->GetLastPosition());
    }, txt_filename->GetId());
#endif /* __linux__ */

    Bind(wxEVT_SHOW, [=](const wxShowEvent &) {
        // Another similar case where the function only works with EVT_SHOW + CallAfter,
        // this time on Mac.
        CallAfter([=]() {
            txt_filename->SetInsertionPoint(0);
            txt_filename->SetSelection(recent_path_len, recent_path_len + stem_len);
        });
    });
}

fs::path PrintHostSendDialog::filename() const
{
    return into_path(txt_filename->GetValue());
}

PrintHostPostUploadAction PrintHostSendDialog::post_action() const
{
    return post_upload_action;
}

std::string PrintHostSendDialog::group() const
{
     if (combo_groups == nullptr) {
         return "";
     } else {
         wxString group = combo_groups->GetValue();
         return into_u8(group);
    }
}

std::string PrintHostSendDialog::storage() const
{
    if (!combo_storage)
        return GUI::format("%1%", m_preselected_storage);
    if (combo_storage->GetSelection() < 0 || combo_storage->GetSelection() >= int(m_paths.size()))
        return {};
    return into_u8(m_paths[combo_storage->GetSelection()]);
}

void PrintHostSendDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {
        // Persist path and print settings
        wxString path = txt_filename->GetValue();
        int last_slash = path.Find('/', true);
		if (last_slash == wxNOT_FOUND)
			path.clear();
		else
            path = path.SubString(0, last_slash);
                
		AppConfig *app_config = wxGetApp().app_config;
		app_config->set("recent", CONFIG_KEY_PATH, into_u8(path));

        if (combo_groups != nullptr) {
            wxString group = combo_groups->GetValue();
            app_config->set("recent", CONFIG_KEY_GROUP, into_u8(group));
        }
        if (combo_storage != nullptr) {
            wxString storage = combo_storage->GetValue();
            app_config->set("recent", CONFIG_KEY_STORAGE, into_u8(storage));
        }
    }

    MsgDialog::EndModal(ret);
}

FlashforgePrintHostSendDialog::FlashforgePrintHostSendDialog(const fs::path&             path,
                                                             PrintHostPostUploadActions  post_actions,
                                                             const wxArrayString&        groups,
                                                             const wxArrayString&        storage_paths,
                                                             const wxArrayString&        storage_names,
                                                             bool                        switch_to_device_tab,
                                                             const Slic3r::Flashforge*   host,
                                                             const std::vector<FilamentInfo>& project_filaments)
    : PrintHostSendDialog(path, post_actions, groups, storage_paths, storage_names, switch_to_device_tab)
    , m_host(host)
    , m_project_filaments(project_filaments)
{}

void FlashforgePrintHostSendDialog::init()
{
    const AppConfig* app_config = wxGetApp().app_config;
    const auto&      path       = m_path;

    std::string leveling = app_config->get("recent", CONFIG_KEY_LEVELING);
    if (!leveling.empty())
        m_leveling_before_print = leveling == "1";

    std::string timelapse = app_config->get("recent", CONFIG_KEY_TIMELAPSE);
    if (!timelapse.empty())
        m_time_lapse_video = timelapse == "1";

    std::string use_ifs = app_config->get("recent", CONFIG_KEY_IFS);
    if (!use_ifs.empty())
        m_use_material_station = use_ifs == "1";
    else
        m_use_material_station = m_project_filaments.size() > 1;

    this->SetMinSize(wxSize(560, 420));

    auto* label_dir_hint = new wxStaticText(this, wxID_ANY, _L("Use forward slashes ( / ) as a directory separator if needed."));
    label_dir_hint->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());
    content_sizer->Add(txt_filename, 0, wxEXPAND);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);

    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/')
        recent_path += '/';
    const auto recent_path_len = recent_path.Length();
    recent_path += path.filename().wstring();
    wxString stem(path.stem().wstring());
    const auto stem_len = stem.Length();
    txt_filename->SetValue(recent_path);

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this, wxID_APPLY);
        checkbox->SetValue(m_switch_to_device_tab);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_switch_to_device_tab = e.IsChecked();
            e.Skip();
        });
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Switch to Device tab after upload."));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        content_sizer->Add(checkbox_sizer);
        content_sizer->AddSpacer(VERT_SPACING);
    }

    m_flashforge_options_sizer = new wxBoxSizer(wxVERTICAL);

    auto add_option_checkbox = [this](wxBoxSizer* parent, const wxString& label, bool value, std::function<void(bool)> setter, ::CheckBox** out = nullptr) {
        auto row      = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox = new ::CheckBox(this);
        checkbox->SetValue(value);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [setter](wxCommandEvent& e) { setter(e.IsChecked()); });
        row->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto text = new wxStaticText(this, wxID_ANY, label);
        text->SetFont(::Label::Body_13);
        text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        row->Add(text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        parent->Add(row);
        parent->AddSpacer(FromDIP(6));

        if (out != nullptr)
            *out = checkbox;
    };

    add_option_checkbox(m_flashforge_options_sizer, _L("Leveling before print"), m_leveling_before_print,
                        [this](bool checked) { m_leveling_before_print = checked; }, &m_checkbox_leveling);
    add_option_checkbox(m_flashforge_options_sizer, _L("Time-lapse"), m_time_lapse_video,
                        [this](bool checked) { m_time_lapse_video = checked; }, &m_checkbox_timelapse);
    add_option_checkbox(m_flashforge_options_sizer, _L("Enable IFS"), m_use_material_station,
                        [this](bool checked) {
                            m_use_material_station = checked;
                            if (m_mapping_section_sizer != nullptr)
                                m_mapping_section_sizer->ShowItems(checked);
                            Layout();
                            Fit();
                        }, &m_checkbox_ifs);

    m_status_text = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status_text->SetFont(::Label::Body_12);
    m_flashforge_options_sizer->Add(m_status_text, 0, wxTOP | wxBOTTOM, FromDIP(4));

    m_mapping_section_sizer = new wxBoxSizer(wxVERTICAL);
    m_mapping_wrap_sizer    = new wxWrapSizer(wxHORIZONTAL, wxWRAPSIZER_DEFAULT_FLAGS);
    m_mapping_section_sizer->Add(m_mapping_wrap_sizer, 0, wxEXPAND | wxTOP, FromDIP(10));
    m_flashforge_options_sizer->Add(m_mapping_section_sizer, 0, wxEXPAND);

    content_sizer->Add(m_flashforge_options_sizer, 0, wxEXPAND);

    load_slots();
    rebuild_mapping_rows();

    if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
        m_valid_suffix = recent_path.substr(extension_start);

    auto validate_path = [this](const wxString& filename) -> bool {
        if (!filename.Lower().EndsWith(m_valid_suffix.Lower())) {
            MessageDialog msg_wingow(this, wxString::Format(_L("Upload filename doesn't end with \"%s\". Do you wish to continue?"), m_valid_suffix),
                                     wxString(SLIC3R_APP_NAME), wxYES | wxNO);
            if (msg_wingow.ShowModal() == wxID_NO)
                return false;
        }
        return validate_before_close();
    };

    auto* btn_ok = add_button(wxID_OK, true, _L("Upload"));
    btn_ok->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
        if (validate_path(txt_filename->GetValue())) {
            post_upload_action = PrintHostPostUploadAction::None;
            EndDialog(wxID_OK);
        }
    });

    if (m_post_actions.has(PrintHostPostUploadAction::StartPrint)) {
        auto* btn_print = add_button(wxID_YES, false, _L("Upload and Print"));
        btn_print->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
            if (validate_path(txt_filename->GetValue())) {
                post_upload_action = PrintHostPostUploadAction::StartPrint;
                EndDialog(wxID_OK);
            }
        });
    }

    add_button(wxID_CANCEL, false, _L("Cancel"));
    finalize();

    Bind(wxEVT_SHOW, [=](const wxShowEvent&) {
        CallAfter([=]() {
            txt_filename->SetInsertionPoint(0);
            txt_filename->SetSelection(recent_path_len, recent_path_len + stem_len);
        });
    });
}

void FlashforgePrintHostSendDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {
        AppConfig* app_config = wxGetApp().app_config;
        app_config->set("recent", CONFIG_KEY_LEVELING, m_leveling_before_print ? "1" : "0");
        app_config->set("recent", CONFIG_KEY_TIMELAPSE, m_time_lapse_video ? "1" : "0");
        app_config->set("recent", CONFIG_KEY_IFS, m_use_material_station ? "1" : "0");
    }

    PrintHostSendDialog::EndModal(ret);
}

std::map<std::string, std::string> FlashforgePrintHostSendDialog::extendedInfo() const
{
    json mappings = json::array();
    int  mapped_count = 0;

    if (m_use_material_station) {
        for (const auto& row : m_mapping_rows) {
            if (row.choice == nullptr || row.tool_id < 0)
                continue;

            const std::string slot_id_text = slot_choice_value_to_id(row.choice->GetStringSelection());
            if (slot_id_text.empty())
                continue;

            const int slot_id = std::stoi(slot_id_text);
            const auto filament_it = std::find_if(m_project_filaments.begin(), m_project_filaments.end(), [&](const FilamentInfo& item) { return item.id == row.tool_id; });
            const auto slot_it     = std::find_if(m_slots.begin(), m_slots.end(), [&](const FlashforgeMaterialSlot& slot) { return slot.slot_id == slot_id; });
            if (filament_it == m_project_filaments.end() || slot_it == m_slots.end())
                continue;

            mappings.push_back({
                {"toolId", filament_it->id},
                {"slotId", slot_it->slot_id},
                {"materialName", slot_it->material_name},
                {"toolMaterialColor", filament_it->color},
                {"slotMaterialColor", slot_it->material_color}
            });
            ++mapped_count;
        }
    }

    return {
        {"levelingBeforePrint", m_leveling_before_print ? "1" : "0"},
        {"timeLapseVideo", m_time_lapse_video ? "1" : "0"},
        {"useMatlStation", m_use_material_station ? "1" : "0"},
        {"gcodeToolCnt", std::to_string(mapped_count)},
        {"materialMappings", mappings.dump()}
    };
}

void FlashforgePrintHostSendDialog::load_slots()
{
    m_slots.clear();

    if (m_host == nullptr) {
        m_status_text->SetLabel(_L("Flashforge host is not available."));
        return;
    }

    wxString msg;
    if (!m_host->fetch_material_slots(m_slots, msg)) {
        m_status_text->SetLabel(msg.empty() ? _L("Unable to read IFS slots from printer.") : msg);
        return;
    }

    m_status_text->SetLabel(wxString::Format(_L("Detected %d IFS slots on printer."), static_cast<int>(m_slots.size())));
}

void FlashforgePrintHostSendDialog::rebuild_mapping_rows()
{
    if (m_mapping_wrap_sizer == nullptr)
        return;

    m_mapping_wrap_sizer->Clear(true);
    m_mapping_rows.clear();

    if (m_project_filaments.empty()) {
        m_mapping_wrap_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Slice the plate first to get project material information.")), 0, wxALL, FromDIP(2));
        return;
    }

    for (const auto& filament : m_project_filaments) {
        auto* column = new wxBoxSizer(wxVERTICAL);
        auto* card   = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(180), FromDIP(72)));
        card->SetMinSize(wxSize(FromDIP(180), FromDIP(72)));
        card->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        auto* top_row    = new wxBoxSizer(wxHORIZONTAL);

        auto* color_panel = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18)));
        color_panel->SetMinSize(wxSize(FromDIP(18), FromDIP(18)));
        color_panel->SetMaxSize(wxSize(FromDIP(18), FromDIP(18)));
        color_panel->SetBackgroundColour(to_wx_colour(filament.color));
        top_row->Add(color_panel, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));

        auto* info_sizer = new wxBoxSizer(wxVERTICAL);
        auto* type_label = new wxStaticText(card, wxID_ANY, from_u8(filament.get_display_filament_type()));
        type_label->SetFont(::Label::Body_13);
        type_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        info_sizer->Add(type_label, 0, wxBOTTOM, FromDIP(2));

        auto* mapped_label = new wxStaticText(card, wxID_ANY, _L("Unassigned"));
        mapped_label->SetFont(::Label::Body_12);
        mapped_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        info_sizer->Add(mapped_label, 0);

        top_row->Add(info_sizer, 1, wxALIGN_CENTER_VERTICAL);
        card_sizer->Add(top_row, 1, wxEXPAND | wxALL, FromDIP(10));
        card->SetSizer(card_sizer);
        column->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(6));

        auto* choice = new wxChoice(this, wxID_ANY);
        choice->Append(_L("Unassigned"));
        for (const auto& slot : m_slots) {
            if (!slot.has_filament)
                continue;

            if (normalize_material(slot.material_name) != normalize_material(filament.type))
                continue;

            const wxString item = wxString::Format("IFS %d - %s", slot.slot_id, from_u8(slot.material_name));
            choice->Append(item);
        }
        choice->SetMinSize(wxSize(FromDIP(180), -1));
        if (choice->GetCount() > 0)
            choice->SetSelection(0);

        choice->Bind(wxEVT_CHOICE, [this, choice](wxCommandEvent&) { ensure_unique_slot_selection(choice); });
        column->Add(choice, 0, wxEXPAND);

        m_mapping_wrap_sizer->Add(column, 0, wxRIGHT | wxBOTTOM, FromDIP(10));
        m_mapping_rows.push_back({filament.id, card, mapped_label, choice});
    }

    auto_assign_mappings();
    if (!m_use_material_station) {
        m_mapping_section_sizer->ShowItems(false);
    }
}

void FlashforgePrintHostSendDialog::auto_assign_mappings()
{
    std::set<std::string> used_slots;
    for (size_t idx = 0; idx < m_project_filaments.size() && idx < m_mapping_rows.size(); ++idx) {
        auto&       filament = m_project_filaments[idx];
        auto*       choice   = m_mapping_rows[idx].choice;
        if (choice == nullptr)
            continue;

        int fallback_selection = wxNOT_FOUND;
        for (unsigned int i = 1; i < choice->GetCount(); ++i) {
            const auto slot_id = slot_choice_value_to_id(choice->GetString(i));
            if (slot_id.empty() || used_slots.count(slot_id) > 0)
                continue;

            const auto slot_it = std::find_if(m_slots.begin(), m_slots.end(), [&](const FlashforgeMaterialSlot& slot) { return std::to_string(slot.slot_id) == slot_id; });
            if (slot_it == m_slots.end())
                continue;

            if (fallback_selection == wxNOT_FOUND)
                fallback_selection = static_cast<int>(i);

            if (!filament.color.empty() && !slot_it->material_color.empty() &&
                boost::iequals(filament.color, slot_it->material_color)) {
                choice->SetSelection(i);
                used_slots.insert(slot_id);
                break;
            }
        }

        if (choice->GetSelection() <= 0 && fallback_selection != wxNOT_FOUND) {
            choice->SetSelection(fallback_selection);
            used_slots.insert(slot_choice_value_to_id(choice->GetStringSelection()));
        }

        refresh_mapping_card(m_mapping_rows[idx]);
    }
}

void FlashforgePrintHostSendDialog::ensure_unique_slot_selection(wxChoice* changed_choice)
{
    const auto selected_slot = slot_choice_value_to_id(changed_choice->GetStringSelection());
    if (selected_slot.empty())
        return;

    for (const auto& row : m_mapping_rows) {
        if (row.choice == nullptr || row.choice == changed_choice)
            continue;

        if (slot_choice_value_to_id(row.choice->GetStringSelection()) == selected_slot)
            row.choice->SetSelection(0);
    }

    for (auto& row : m_mapping_rows)
        refresh_mapping_card(row);
}

void FlashforgePrintHostSendDialog::refresh_mapping_card(MappingRow& row)
{
    if (row.card == nullptr || row.mapped_label == nullptr || row.choice == nullptr)
        return;

    const std::string slot_id_text = slot_choice_value_to_id(row.choice->GetStringSelection());
    if (slot_id_text.empty()) {
        row.mapped_label->SetLabel(_L("Unassigned"));
        row.mapped_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        row.card->Refresh();
        return;
    }

    const auto slot_it = std::find_if(m_slots.begin(), m_slots.end(), [&](const FlashforgeMaterialSlot& slot) { return std::to_string(slot.slot_id) == slot_id_text; });
    if (slot_it == m_slots.end()) {
        row.mapped_label->SetLabel(_L("Unassigned"));
        row.mapped_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        row.card->Refresh();
        return;
    }

    row.mapped_label->SetLabel(wxString::Format("IFS %d - %s", slot_it->slot_id, from_u8(slot_it->material_name)));
    row.mapped_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#009688")));
    row.card->Refresh();
}

bool FlashforgePrintHostSendDialog::validate_before_close()
{
    if (!m_use_material_station && m_project_filaments.size() > 1) {
        show_error(this, _L("This plate uses multiple materials. Enable IFS and assign each tool to a printer slot."));
        return false;
    }

    if (!m_use_material_station)
        return true;

    for (const auto& row : m_mapping_rows) {
        if (row.choice != nullptr && row.choice->GetSelection() <= 0) {
            show_error(this, _L("Each project material must be assigned to an IFS slot before printing."));
            return false;
        }
    }

    return true;
}

std::string FlashforgePrintHostSendDialog::slot_choice_value_to_id(const wxString& value) const
{
    if (value.empty() || value == _L("Unassigned"))
        return {};

    const wxString prefix = "IFS ";
    if (!value.StartsWith(prefix))
        return {};

    wxString rest = value.Mid(prefix.Length());
    wxString id;
    for (wxUniChar c : rest) {
        if (!wxIsdigit(c))
            break;
        id.Append(c);
    }
    return into_u8(id);
}

std::string FlashforgePrintHostSendDialog::normalize_material(const std::string& material) const
{
    std::string normalized = boost::to_upper_copy(material);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](unsigned char ch) { return !std::isalnum(ch); }), normalized.end());
    return normalized;
}

wxColour FlashforgePrintHostSendDialog::to_wx_colour(const std::string& color) const
{
    wxColour wx_color(from_u8(color));
    if (!wx_color.IsOk())
        return wxColour("#999999");
    return wx_color;
}

wxDEFINE_EVENT(EVT_PRINTHOST_PROGRESS, PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_ERROR,    PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_CANCEL,   PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_INFO,  PrintHostQueueDialog::Event);

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id)
    : wxEvent(winid, eventType)
    , job_id(job_id)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, int progress)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , progress(progress)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, wxString error)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , status(std::move(error))
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, wxString tag, wxString status)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , tag(std::move(tag))
    , status(std::move(status))
{}

wxEvent *PrintHostQueueDialog::Event::Clone() const
{
    return new Event(*this);
}

PrintHostQueueDialog::PrintHostQueueDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Print host upload queue"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , on_progress_evt(this, EVT_PRINTHOST_PROGRESS, &PrintHostQueueDialog::on_progress, this)
    , on_error_evt(this, EVT_PRINTHOST_ERROR, &PrintHostQueueDialog::on_error, this)
    , on_cancel_evt(this, EVT_PRINTHOST_CANCEL, &PrintHostQueueDialog::on_cancel, this)
    , on_info_evt(this, EVT_PRINTHOST_INFO, &PrintHostQueueDialog::on_info, this)
{
    const auto em = GetTextExtent("m").x;

    auto *topsizer = new wxBoxSizer(wxVERTICAL);

    std::vector<int> widths;
    widths.reserve(7);
    if (!load_user_data(UDT_COLS, widths)) {
        widths.clear();
        for (size_t i = 0; i < 7; i++)
            widths.push_back(-1);
    }

    job_list = new wxDataViewListCtrl(this, wxID_ANY);

    // MSW DarkMode: workaround for the selected item in the list
    auto append_text_column = [this](const wxString& label, int width, wxAlignment align = wxALIGN_LEFT,
                                     int flags = wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE) {
#ifdef _WIN32
            job_list->AppendColumn(new wxDataViewColumn(label, new TextRenderer(), job_list->GetColumnCount(), width, align, flags));
#else
            job_list->AppendTextColumn(label, wxDATAVIEW_CELL_INERT, width, align, flags);
#endif
    };

    // Note: Keep these in sync with Column
    append_text_column(_L("ID"), widths[0]);
    job_list->AppendProgressColumn(_L("Progress"),      wxDATAVIEW_CELL_INERT, widths[1], wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    append_text_column(_L("Status"),widths[2]);
    append_text_column(_L("Host"),  widths[3]);
    append_text_column(_CTX(L_CONTEXT("Size", "OfFile"), "OfFile"), widths[4]);
    append_text_column(_L("Filename"),      widths[5]);
    append_text_column(_L("Message"), widths[6]);
    //append_text_column(_L("Error Message"), -1, wxALIGN_CENTER, wxDATAVIEW_COL_HIDDEN);
 
    auto *btnsizer = new wxBoxSizer(wxHORIZONTAL);
    btn_cancel = new wxButton(this, wxID_DELETE, _L("Cancel selected"));
    btn_cancel->Disable();
    btn_error = new wxButton(this, wxID_ANY, _L("Show error message"));
    btn_error->Disable();
    // Note: The label needs to be present, otherwise we get accelerator bugs on Mac
    auto *btn_close = new wxButton(this, wxID_CANCEL, _L("Close"));
    btnsizer->Add(btn_cancel, 0, wxRIGHT, SPACING);
    btnsizer->Add(btn_error, 0);
    btnsizer->AddStretchSpacer();
    btnsizer->Add(btn_close);

    topsizer->Add(job_list, 1, wxEXPAND | wxBOTTOM, SPACING);
    topsizer->Add(btnsizer, 0, wxEXPAND);
    SetSizer(topsizer);

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(job_list);

    std::vector<int> size;
    SetSize(load_user_data(UDT_SIZE, size) ? wxSize(size[0] * em, size[1] * em) : wxSize(HEIGHT * em, WIDTH * em));

    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        OnSize(evt); 
        save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
     });
    
    std::vector<int> pos;
    if (load_user_data(UDT_POSITION, pos))
        SetPosition(wxPoint(pos[0], pos[1]));

    Bind(wxEVT_MOVE, [this](wxMoveEvent& evt) {
        save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
    });

    job_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) { on_list_select(); });

    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }

        const JobState state = get_state(selected);
        if (state < ST_ERROR) {
            GUI::wxGetApp().printhost_job_queue().cancel(selected);
        }
    });

    btn_error->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }
        GUI::show_error(nullptr, job_list->GetTextValue(selected, COL_ERRORMSG));
    });
}

void PrintHostQueueDialog::append_job(const PrintHostJob &job)
{
    wxCHECK_RET(!job.empty(), "PrintHostQueueDialog: Attempt to append an empty job");

    wxVector<wxVariant> fields;
    fields.push_back(wxVariant(wxString::Format("%d", job_list->GetItemCount() + 1)));
    fields.push_back(wxVariant(0));
    fields.push_back(wxVariant(_L("Queued")));
    fields.push_back(wxVariant(job.printhost->get_host()));
    boost::system::error_code ec;
    boost::uintmax_t size_i = boost::filesystem::file_size(job.upload_data.source_path, ec);
    std::stringstream stream;
    if (ec) {
        stream << "unknown";
        size_i = 0;
        BOOST_LOG_TRIVIAL(error) << ec.message();
    } else 
        stream << std::fixed << std::setprecision(2) << ((float)size_i / 1024 / 1024) << "MB";
    fields.push_back(wxVariant(stream.str()));
    fields.push_back(wxVariant(from_path(job.upload_data.upload_path)));
    fields.push_back(wxVariant(""));
    job_list->AppendItem(fields, static_cast<wxUIntPtr>(ST_NEW));
    // Both strings are UTF-8 encoded.
    upload_names.emplace_back(job.printhost->get_host(), job.upload_data.upload_path.string());

    wxGetApp().notification_manager()->push_upload_job_notification(job_list->GetItemCount(), (float)size_i / 1024 / 1024, job.upload_data.upload_path.string(), job.printhost->get_host());
}

void PrintHostQueueDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    const int& em = em_unit();

    msw_buttons_rescale(this, em, { wxID_DELETE, wxID_CANCEL, btn_error->GetId() });

    SetMinSize(wxSize(HEIGHT * em, WIDTH * em));

    Fit();
    Refresh();

    save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
}

void PrintHostQueueDialog::on_sys_color_changed()
{
#ifdef _WIN32
    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(job_list);
#endif
}

PrintHostQueueDialog::JobState PrintHostQueueDialog::get_state(int idx)
{
    wxCHECK_MSG(idx >= 0 && idx < job_list->GetItemCount(), ST_ERROR, "Out of bounds access to job list");
    return static_cast<JobState>(job_list->GetItemData(job_list->RowToItem(idx)));
}

void PrintHostQueueDialog::set_state(int idx, JobState state)
{
    wxCHECK_RET(idx >= 0 && idx < job_list->GetItemCount(), "Out of bounds access to job list");
    job_list->SetItemData(job_list->RowToItem(idx), static_cast<wxUIntPtr>(state));

    switch (state) {
        case ST_NEW:        job_list->SetValue(_L("Queued"), idx, COL_STATUS); break;
        case ST_PROGRESS:   job_list->SetValue(_L("Uploading"), idx, COL_STATUS); break;
        case ST_ERROR:      job_list->SetValue(_L("Error"), idx, COL_STATUS); break;
        case ST_CANCELLING: job_list->SetValue(_L("Canceling"), idx, COL_STATUS); break;
        case ST_CANCELLED:  job_list->SetValue(_L("Canceled"), idx, COL_STATUS); break;
        case ST_COMPLETED:  job_list->SetValue(_L("Completed"), idx, COL_STATUS); break;
    }
    // This might be ambigous call, but user data needs to be saved time to time
    save_user_data(UDT_SIZE | UDT_POSITION | UDT_COLS);
}

void PrintHostQueueDialog::on_list_select()
{
    int selected = job_list->GetSelectedRow();
    if (selected != wxNOT_FOUND) {
        const JobState state = get_state(selected);
        btn_cancel->Enable(state < ST_ERROR);
        btn_error->Enable(state == ST_ERROR);
        Layout();
    } else {
        btn_cancel->Disable();
    }
}

void PrintHostQueueDialog::on_progress(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    if (evt.progress < 100) {
        set_state(evt.job_id, ST_PROGRESS);
        job_list->SetValue(wxVariant(evt.progress), evt.job_id, COL_PROGRESS);
    } else {
        set_state(evt.job_id, ST_COMPLETED);
        job_list->SetValue(wxVariant(100), evt.job_id, COL_PROGRESS);
    }

    on_list_select();

    if (evt.progress > 0)
    {
        wxVariant nm, hst;
        job_list->GetValue(nm, evt.job_id, COL_FILENAME);
        job_list->GetValue(hst, evt.job_id, COL_HOST);
        wxGetApp().notification_manager()->set_upload_job_notification_percentage(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()), evt.progress / 100.f);
    }
}

void PrintHostQueueDialog::on_error(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_ERROR);

    auto errormsg = format_wxstr("%1%\n%2%", _L("Error uploading to print host") + ":", evt.status);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);
    job_list->SetValue(wxVariant(errormsg), evt.job_id, COL_ERRORMSG);    // Stashes the error message into a hidden column for later

    on_list_select();

    GUI::show_error(nullptr, errormsg);

    wxVariant nm, hst;
    job_list->GetValue(nm, evt.job_id, COL_FILENAME);
    job_list->GetValue(hst, evt.job_id, COL_HOST);
    wxGetApp().notification_manager()->upload_job_notification_show_error(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()));
}

void PrintHostQueueDialog::on_cancel(Event &evt)
{
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_CANCELLED);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);

    on_list_select();

    wxVariant nm, hst;
    job_list->GetValue(nm, evt.job_id, COL_FILENAME);
    job_list->GetValue(hst, evt.job_id, COL_HOST);
    wxGetApp().notification_manager()->upload_job_notification_show_canceled(evt.job_id + 1, into_u8(nm.GetString()), into_u8(hst.GetString()));
}

void PrintHostQueueDialog::on_info(Event& evt)
{
    /*
    wxCHECK_RET(evt.job_id < (size_t)job_list->GetItemCount(), "Out of bounds access to job list");
    
    if (evt.tag == L"resolve") {
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_HOST);
        wxGetApp().notification_manager()->set_upload_job_notification_host(evt.job_id + 1, into_u8(evt.status));
    } else if (evt.tag == L"complete") {
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_ERRORMSG);
        wxGetApp().notification_manager()->set_upload_job_notification_completed(evt.job_id + 1);
        wxGetApp().notification_manager()->set_upload_job_notification_status(evt.job_id + 1, into_u8(evt.status));
    } else if(evt.tag == L"complete_with_warning"){
        wxVariant hst(evt.status);
        job_list->SetValue(hst, evt.job_id, COL_ERRORMSG);
        wxGetApp().notification_manager()->set_upload_job_notification_completed_with_warning(evt.job_id + 1);
        wxGetApp().notification_manager()->set_upload_job_notification_status(evt.job_id + 1, into_u8(evt.status));
    } else if (evt.tag == L"set_complete_off") {
        wxGetApp().notification_manager()->set_upload_job_notification_comp_on_100(evt.job_id + 1, false);
    }
    */
}

void PrintHostQueueDialog::get_active_jobs(std::vector<std::pair<std::string, std::string>>& ret)
{
    int ic = job_list->GetItemCount();
    for (int i = 0; i < ic; i++)
    {
        auto item = job_list->RowToItem(i);
        auto data = job_list->GetItemData(item);
        JobState st = static_cast<JobState>(data);
        if(st == JobState::ST_NEW || st == JobState::ST_PROGRESS)
            ret.emplace_back(upload_names[i]);       
    }
}
void PrintHostQueueDialog::save_user_data(int udt)
{
    const auto em = GetTextExtent("m").x;
    auto *app_config = wxGetApp().app_config;
    if (udt & UserDataType::UDT_SIZE) {
        
        app_config->set("print_host_queue_dialog_height", std::to_string(this->GetSize().x / em));
        app_config->set("print_host_queue_dialog_width", std::to_string(this->GetSize().y / em));
    }
    if (udt & UserDataType::UDT_POSITION)
    {
        app_config->set("print_host_queue_dialog_x", std::to_string(this->GetPosition().x));
        app_config->set("print_host_queue_dialog_y", std::to_string(this->GetPosition().y));
    }
    if (udt & UserDataType::UDT_COLS)
    {
        for (size_t i = 0; i < job_list->GetColumnCount() - 1; i++)
        {
            app_config->set("print_host_queue_dialog_column_" + std::to_string(i), std::to_string(job_list->GetColumn(i)->GetWidth()));
        }
    }    
}
bool PrintHostQueueDialog::load_user_data(int udt, std::vector<int>& vector)
{
    auto* app_config = wxGetApp().app_config;
    auto hasget = [app_config](const std::string& name, std::vector<int>& vector)->bool {
        if (app_config->has(name)) {
            std::string val = app_config->get(name);
            if (!val.empty() || val[0]!='\0') {
                vector.push_back(std::stoi(val));
                return true;
            }
        }
        return false;
    };
    if (udt & UserDataType::UDT_SIZE) {
        if (!hasget("print_host_queue_dialog_height",vector))
            return false;
        if (!hasget("print_host_queue_dialog_width", vector))
            return false;
    }
    if (udt & UserDataType::UDT_POSITION)
    {
        if (!hasget("print_host_queue_dialog_x", vector))
            return false;
        if (!hasget("print_host_queue_dialog_y", vector))
            return false;
    }
    if (udt & UserDataType::UDT_COLS)
    {
        for (size_t i = 0; i < 7; i++)
        {
            if (!hasget("print_host_queue_dialog_column_" + std::to_string(i), vector))
                return false;
        }
    }
    return true;
}

ElegooPrintHostSendDialog::ElegooPrintHostSendDialog(const fs::path&            path,
                                                     PrintHostPostUploadActions post_actions,
                                                     const wxArrayString&       groups,
                                                     const wxArrayString&       storage_paths,
                                                     const wxArrayString&       storage_names,
                                                     bool                       switch_to_device_tab)
    : PrintHostSendDialog(path, post_actions, groups, storage_paths, storage_names, switch_to_device_tab)
    , m_timeLapse(0)
    , m_heatedBedLeveling(0)
    , m_BedType(BedType::btPTE)
{}

void ElegooPrintHostSendDialog::init() {

    auto preset_bundle = wxGetApp().preset_bundle;
    auto model_id = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);

    if (!boost::starts_with(model_id, "Elegoo-C")) {
        PrintHostSendDialog::init();
        return;
    }
           
    const auto& path = m_path;
    const auto& storage_paths = m_paths;
    const auto& post_actions  = m_post_actions;
    const auto& storage_names = m_storage_names;

    this->SetMinSize(wxSize(500, 300));
    const AppConfig* app_config = wxGetApp().app_config;

    std::string uploadAndPrint = app_config->get("recent", CONFIG_KEY_UPLOADANDPRINT);
    if (!uploadAndPrint.empty())
        post_upload_action = static_cast<PrintHostPostUploadAction>(std::stoi(uploadAndPrint));

    std::string timeLapse = app_config->get("recent", CONFIG_KEY_TIMELAPSE);
    if (!timeLapse.empty())
        m_timeLapse = std::stoi(timeLapse);
    std::string heatedBedLeveling = app_config->get("recent", CONFIG_KEY_HEATEDBEDLEVELING);
    if (!heatedBedLeveling.empty())
        m_heatedBedLeveling = std::stoi(heatedBedLeveling);
    std::string bedType = app_config->get("recent", CONFIG_KEY_BEDTYPE);
    if (!bedType.empty())
        m_BedType = static_cast<BedType>(std::stoi(bedType));

    auto* label_dir_hint = new wxStaticText(this, wxID_ANY, _L("Use forward slashes ( / ) as a directory separator if needed."));
    label_dir_hint->Wrap(CONTENT_WIDTH * wxGetApp().em_unit());

    wxSizerFlags flags = wxSizerFlags().Border(wxRIGHT, 16).Expand();

    content_sizer->Add(txt_filename, flags);
    content_sizer->AddSpacer(4);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);

    if (combo_groups != nullptr) {
        // Repetier specific: Show a selection of file groups.
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Group"));
        content_sizer->Add(label_group);
        content_sizer->Add(combo_groups, 0, wxBOTTOM, 2 * VERT_SPACING);
        wxString recent_group = from_u8(app_config->get("recent", CONFIG_KEY_GROUP));
        if (!recent_group.empty())
            combo_groups->SetValue(recent_group);
    }

    if (combo_storage != nullptr) {
        // PrusaLink specific: User needs to choose a storage
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ":");
        content_sizer->Add(label_group);
        content_sizer->Add(combo_storage, 0, wxBOTTOM, 2 * VERT_SPACING);
        combo_storage->SetValue(storage_names.front());
        wxString recent_storage = from_u8(app_config->get("recent", CONFIG_KEY_STORAGE));
        if (!recent_storage.empty())
            combo_storage->SetValue(recent_storage);
    } else if (storage_names.GetCount() == 1) {
        // PrusaLink specific: Show which storage has been detected.
        auto* label_group = new wxStaticText(this, wxID_ANY, _L("Upload to storage") + ": " + storage_names.front());
        content_sizer->Add(label_group);
        m_preselected_storage = m_paths.front();
    }

    wxString recent_path = from_u8(app_config->get("recent", CONFIG_KEY_PATH));
    if (recent_path.Length() > 0 && recent_path[recent_path.Length() - 1] != '/') {
        recent_path += '/';
    }
    const auto recent_path_len = recent_path.Length();
    recent_path += path.filename().wstring();
    wxString   stem(path.stem().wstring());
    const auto stem_len = stem.Length();

    txt_filename->SetValue(recent_path);

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this, wxID_APPLY);
        checkbox->SetValue(m_switch_to_device_tab);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_switch_to_device_tab = e.IsChecked();
            e.Skip();
        });
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Switch to Device tab after upload."), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        content_sizer->Add(checkbox_sizer);
        content_sizer->AddSpacer(VERT_SPACING);
    }
    warning_text = new wxStaticText(this, wxID_ANY,
                                    _L("The selected bed type does not match the file. Please confirm before starting the print."),
                                    wxDefaultPosition, wxDefaultSize, 0);
    uploadandprint_sizer = new wxBoxSizer(wxVERTICAL);
    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this);
        checkbox->SetValue(post_upload_action == PrintHostPostUploadAction::StartPrint);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            if (e.IsChecked()) {
                post_upload_action = PrintHostPostUploadAction::StartPrint;
            } else {
                post_upload_action = PrintHostPostUploadAction::None;
            }
            refresh();
            e.Skip();
        });
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Upload and Print"), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        content_sizer->Add(checkbox_sizer);
        content_sizer->AddSpacer(VERT_SPACING);
    }

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this);
        checkbox->SetValue(m_timeLapse == 1);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_timeLapse = e.IsChecked() ? 1 : 0;
            e.Skip();
        });
        checkbox_sizer->AddSpacer(16);
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Time-lapse"), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        uploadandprint_sizer->Add(checkbox_sizer);
        uploadandprint_sizer->AddSpacer(VERT_SPACING);
    }

    {
        auto checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto checkbox       = new ::CheckBox(this);
        checkbox->SetValue(m_heatedBedLeveling == 1);
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
            m_heatedBedLeveling = e.IsChecked() ? 1 : 0;
            e.Skip();
        });
        checkbox_sizer->AddSpacer(16);
        checkbox_sizer->Add(checkbox, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

        auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Heated Bed Leveling"), wxDefaultPosition, wxDefaultSize, 0);
        checkbox_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
        checkbox_text->SetFont(::Label::Body_13);
        checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
        uploadandprint_sizer->Add(checkbox_sizer);
        uploadandprint_sizer->AddSpacer(VERT_SPACING);
    }

    {
        auto radioBoxA = new ::RadioBox(this);
        auto radioBoxB = new ::RadioBox(this);
        if (m_BedType == BedType::btPC)
            radioBoxB->SetValue(true);
        else
            radioBoxA->SetValue(true);

        radioBoxA->Bind(wxEVT_LEFT_DOWN, [this, radioBoxA, radioBoxB](wxMouseEvent& e) {
            radioBoxA->SetValue(true);
            radioBoxB->SetValue(false);
            m_BedType = BedType::btPTE;
            refresh();
        });
        radioBoxB->Bind(wxEVT_LEFT_DOWN, [this, radioBoxA, radioBoxB](wxMouseEvent& e) {
            radioBoxA->SetValue(false);
            radioBoxB->SetValue(true);
            m_BedType = BedType::btPC;
            refresh();
        });

        {
            auto radio_sizer = new wxBoxSizer(wxHORIZONTAL);
            radio_sizer->AddSpacer(16);
            radio_sizer->Add(radioBoxA, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

            auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Textured Build Plate (Side A)"), wxDefaultPosition, wxDefaultSize, 0);
            radio_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
            checkbox_text->SetFont(::Label::Body_13);
            checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
            uploadandprint_sizer->Add(radio_sizer);
            uploadandprint_sizer->AddSpacer(VERT_SPACING);
        }
        {
            auto radio_sizer = new wxBoxSizer(wxHORIZONTAL);
            radio_sizer->AddSpacer(16);
            radio_sizer->Add(radioBoxB, 0, wxALL | wxALIGN_CENTER, FromDIP(2));

            auto checkbox_text = new wxStaticText(this, wxID_ANY, _L("Smooth Build Plate (Side B)"), wxDefaultPosition, wxDefaultSize, 0);
            radio_sizer->Add(checkbox_text, 0, wxALL | wxALIGN_CENTER, FromDIP(2));
            checkbox_text->SetFont(::Label::Body_13);
            checkbox_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));
            uploadandprint_sizer->Add(radio_sizer);
            uploadandprint_sizer->AddSpacer(VERT_SPACING);
        }
    }
    {
        auto h_sizer = new wxBoxSizer(wxHORIZONTAL);
        warning_text->SetFont(::Label::Body_13);
        warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF1001")));
        // wrapping the text
        warning_text->Wrap(350);
        h_sizer->AddSpacer(16);
        h_sizer->Add(warning_text);

        uploadandprint_sizer->Add(h_sizer);
        uploadandprint_sizer->AddSpacer(VERT_SPACING);
    }

    content_sizer->Add(uploadandprint_sizer);
    uploadandprint_sizer->Show(post_upload_action == PrintHostPostUploadAction::StartPrint);
    warning_text->Show(post_upload_action == PrintHostPostUploadAction::StartPrint && appBedType() != m_BedType);

    uploadandprint_sizer->Layout();

    if (size_t extension_start = recent_path.find_last_of('.'); extension_start != std::string::npos)
        m_valid_suffix = recent_path.substr(extension_start);
    // .gcode suffix control
    auto validate_path = [this](const wxString& path) -> bool {
        if (!path.Lower().EndsWith(m_valid_suffix.Lower())) {
            MessageDialog msg_wingow(this,
                                     wxString::Format(_L("Upload filename doesn't end with \"%s\". Do you wish to continue?"),
                                                      m_valid_suffix),
                                     wxString(SLIC3R_APP_NAME), wxYES | wxNO);
            if (msg_wingow.ShowModal() == wxID_NO)
                return false;
        }
        return true;
    };

    auto* btn_ok = add_button(wxID_OK, true, _L("Upload"));
    btn_ok->Bind(wxEVT_BUTTON, [this, validate_path](wxCommandEvent&) {
        if (validate_path(txt_filename->GetValue())) {
            // post_upload_action = PrintHostPostUploadAction::None;
            EndDialog(wxID_OK);
        }
    });
    txt_filename->SetFocus();

    add_button(wxID_CANCEL, false, _L("Cancel"));
    finalize();

#ifdef __linux__
    // On Linux with GTK2 when text control lose the focus then selection (colored background) disappears but text color stay white
    // and as a result the text is invisible with light mode
    // see https://github.com/prusa3d/PrusaSlicer/issues/4532
    // Workaround: Unselect text selection explicitly on kill focus
    txt_filename->Bind(
        wxEVT_KILL_FOCUS,
        [this](wxEvent& e) {
            e.Skip();
            txt_filename->SetInsertionPoint(txt_filename->GetLastPosition());
        },
        txt_filename->GetId());
#endif /* __linux__ */

    Bind(wxEVT_SHOW, [=](const wxShowEvent&) {
        // Another similar case where the function only works with EVT_SHOW + CallAfter,
        // this time on Mac.
        CallAfter([=]() {
            txt_filename->SetInsertionPoint(0);
            txt_filename->SetSelection(recent_path_len, recent_path_len + stem_len);
        });
    });
}

void ElegooPrintHostSendDialog::EndModal(int ret)
{
    if (ret == wxID_OK) {

        AppConfig* app_config = wxGetApp().app_config;
        app_config->set("recent", CONFIG_KEY_UPLOADANDPRINT, std::to_string(static_cast<int>(post_upload_action)));
        app_config->set("recent", CONFIG_KEY_TIMELAPSE, std::to_string(m_timeLapse));
        app_config->set("recent", CONFIG_KEY_HEATEDBEDLEVELING, std::to_string(m_heatedBedLeveling));
        app_config->set("recent", CONFIG_KEY_BEDTYPE, std::to_string(static_cast<int>(m_BedType)));
    }

    PrintHostSendDialog::EndModal(ret);
}

BedType ElegooPrintHostSendDialog::appBedType() const
{
    std::string str_bed_type = wxGetApp().app_config->get("curr_bed_type");
    int bed_type_value = atoi(str_bed_type.c_str());
    return static_cast<BedType>(bed_type_value);
}

void ElegooPrintHostSendDialog::refresh()
{
    if (uploadandprint_sizer) {
        if (post_upload_action == PrintHostPostUploadAction::StartPrint) {
            uploadandprint_sizer->Show(true);
        } else {
            uploadandprint_sizer->Show(false);
        }
    }
    if (warning_text) {
        warning_text->Show(post_upload_action == PrintHostPostUploadAction::StartPrint && appBedType() != m_BedType);
    }
    this->Layout();
    this->Fit();
}

}}
