// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <libadwaitamm.h>
#include <memory>
#include "config.hpp"

namespace ds::frontend {

class MainWindow;

class Application : public Adw::Application {
protected:
    Application();
public:
    static Glib::RefPtr<Application> create();

protected:
    void on_activate() override;
    void on_startup() override;
    void on_shutdown() override;
    void on_open(const type_vec_files& files, const Glib::ustring& hint) override;

private:
    Config                       m_config;
    std::unique_ptr<MainWindow>  m_window;
};

} // namespace ds::frontend
