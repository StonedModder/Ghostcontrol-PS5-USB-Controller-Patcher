/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "screens.hpp"
#include "service_snapshot.hpp"
namespace ghostcontrol {
/* Frame-side state: mailbox only. Completion belongs to exactly request_. */
class AudioScreen {
    unsigned long long request_ = 0;
    std::string feedback_ = "No Audio action requested.";
public:
    bool press(const Row &, ServiceSnapshot &);
    void update(ServiceSnapshot &);
    void cancel(ServiceSnapshot &);
    bool pending() const { return request_ != 0; }
    unsigned long long request_id() const { return request_; }
    const std::string &feedback() const { return feedback_; }
};
}
