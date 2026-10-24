// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <gtkmm.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../vmu.hpp"

namespace ds::frontend {

// The VMU manager: DuckStation's memory card editor, for Dreamcast memory
// units. Two cards side by side. Saves can be copied or moved between them
// (buttons or drag and drop), deleted, protected, exported as DCI or VMS +
// VMI and imported from DCI, VMS/VMI or whole card images (raw or Nexus
// DCM), replacing a save of the same name if asked. Cards can be opened
// from anywhere, created, formatted and saved under another name or format.
// Changes are written only when Save is pressed, and the engine's own cards
// never while a game is running (the engine holds them).
class VmuManager : public Gtk::Window {
public:
    // `vmu_dirs`: where the engine's VMU images live. `game_running`: asked
    // before writing one of them. `play`: starts the mini-game on a card.
    VmuManager(std::vector<std::filesystem::path> vmu_dirs, std::function<bool()> game_running,
               std::function<void(const std::filesystem::path& card)> play = {});
    ~VmuManager() override;

    // For scripted test runs (DS_TEST_SCRIPT): act on the left (0) or right
    // (1) card as the buttons would.
    void test_import(int side, const std::vector<std::filesystem::path>& paths);
    void test_select_all(int side);
    void test_transfer(int side, bool move);
    void test_save(int side);

private:
    // One row: the file and its icon, animated by the window's timer.
    struct Row {
        VmuImage::File             file;
        Gtk::Picture*              icon = nullptr;
        std::vector<Glib::RefPtr<Gdk::Texture>> frames;
    };
    struct Pane {
        Gtk::Box        box{Gtk::Orientation::VERTICAL, 8};
        Gtk::DropDown   chooser;
        Gtk::ListBox    list;
        Gtk::Label      status;
        Gtk::Button     save{"Save"};
        Gtk::Button     play;
        std::vector<std::filesystem::path> paths;
        std::filesystem::path path;
        VmuImage        image;
        bool            loaded   = false;
        bool            modified = false;
        std::vector<Row> rows;
        Pane*           other = nullptr;
    };

    void build_pane(Pane& pane, const char* copy_icon, const char* move_icon);
    Gtk::Widget& build_details();
    void scan_cards();
    void fill_chooser(Pane& pane);
    void add_card_path(const std::filesystem::path& path);
    void load_card(Pane& pane, std::size_t index);
    void open_card(Pane& pane, const std::filesystem::path& path);
    void refresh(Pane& pane);
    void show_details(const Pane& pane);
    std::vector<const VmuImage::File*> selected(const Pane& pane) const;
    bool can_write(const Pane& pane);
    bool engine_card(const std::filesystem::path& path) const;
    void notify(const std::string& text);
    void mark_modified(Pane& pane);
    bool tick();

    void transfer(Pane& from, Pane& to, bool move);
    void delete_selected(Pane& pane);
    void toggle_protect(Pane& pane);
    void export_selected(Pane& pane, bool vms);
    void import_dialog(Pane& pane);
    void import_paths(Pane& pane, const std::vector<std::filesystem::path>& paths);
    // `done` receives the names that landed on the card.
    void add_incoming(Pane& pane, std::vector<VmuImage::Incoming> files, std::string notes,
                      std::function<void(const std::vector<std::string>&)> done = {});
    void new_card(Pane& pane);
    void open_dialog(Pane& pane);
    void save_card(Pane& pane);
    void save_as(Pane& pane);
    void format_card(Pane& pane);
    void confirm_discard(Pane& pane, std::function<void()> then);

    std::vector<std::filesystem::path> m_dirs;
    std::function<bool()>              m_game_running;
    std::function<void(const std::filesystem::path&)> m_play;
    std::vector<std::filesystem::path> m_cards;
    Gtk::Box   m_root{Gtk::Orientation::VERTICAL, 10};
    Gtk::Label m_message;
    Pane       m_left, m_right;
    bool       m_loading = false;
    Pane*      m_drag_from = nullptr;   // card a save is being dragged from

    // Details of the last selected file.
    Gtk::Box     m_details{Gtk::Orientation::HORIZONTAL, 14};
    Gtk::Picture m_detail_icon;
    Gtk::Picture m_detail_eyecatch;
    Gtk::Label   m_detail_text;
    std::vector<Glib::RefPtr<Gdk::Texture>> m_detail_frames;
    int          m_detail_speed = 0;

    unsigned          m_ticks = 0;
    sigc::connection  m_timer;
};

} // namespace ds::frontend
