/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "overlay.hpp"
#include "service_snapshot.hpp"
#include "json_reply.hpp"
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <string_view>
#include <utility>
#include <vector>

namespace ghostcontrol {
/* UI-owned form controller. Header-only so every existing app/host source-list
 * consumes the same implementation without a second, easily omitted link list.
 * No filesystem/socket calls: only worker receipts authorize a single-key write.
 * Physical capture remains the separate restricted binder, unchanged. */
class ProfileEditor {
    enum class Stage { closed, fields, operation, value, text, writing, reading };
    struct Field { std::string key, value, fallback; bool has_default = true; };
    Stage stage_ = Stage::closed;
    ServiceSnapshot *service_ = nullptr;
    std::string id_, message_, wanted_, edited_key_;
    std::vector<Field> fields_;
    std::size_t selected_ = 0;
    unsigned long long source_ = 0, pending_ = 0;
    bool accepted_ = false;
    static constexpr const char *buttons_[] = {"SHARE","L3","R3","OPTIONS","UP","RIGHT","DOWN","LEFT","L2","R2","L1","R1","TRIANGLE","CIRCLE","CROSS","SQUARE","PS","TOUCHPAD"};
    static bool complete(const ServiceSnapshot::Fetch &f) {
        return f.reached && f.valid && f.generation &&
            f.state == ServiceSnapshot::ResultState::acknowledged;
    }
    bool fresh() const {
        if (!service_ || !source_) return false;
        const auto own = service_->fetch_request(source_);
        const auto latest = service_->last_reply("PROFILE_GET");
        return complete(own) && complete(latest) && own.generation == latest.generation &&
            service_->last_reply("STATUS").valid &&
            json_raw_value(json_raw_value(service_->last_reply("STATUS").body, "capabilities"), "profiles") == "true";
    }
    /* The service stores a mapping sum in its own button order, so a layer
     * authored as R1+SHARE is handed back as SHARE+R1. Compare a mapping as the
     * set of terms it names, never as the exact text that was typed, or a save
     * that really happened would be reported as a mismatch and invite a retry. */
    static std::vector<std::string> mapping_terms(const std::string &value) {
        std::vector<std::string> terms;
        std::size_t pos=0;
        while(pos<=value.size()) {
            const auto end=std::min(value.find('+',pos),value.size());
            terms.push_back(value.substr(pos,end-pos));
            if(end==value.size()) break;
            pos=end+1;
        }
        std::sort(terms.begin(),terms.end());
        return terms;
    }
    static bool equivalent(const std::string &key, const std::string &actual, const std::string &wanted) {
        if(actual==wanted) return true;
        if(key.starts_with("map.")) return mapping_terms(actual)==mapping_terms(wanted);
        if(key!="match.vid" && key!="match.pid") return false;
        if(actual=="any" || wanted=="any") return false;
        char *end=nullptr; errno=0; const auto a=std::strtoul(actual.c_str(),&end,0);
        if(errno || end==actual.c_str() || *end) return false;
        errno=0; const auto b=std::strtoul(wanted.c_str(),&end,0);
        return !errno && end!=wanted.c_str() && !*end && a==b;
    }
    static bool decode(std::string_view body, const std::string &id, std::map<std::string,std::string> &values) {
        JsonReply reply(body);
        if (!reply.success() || reply.string_field("encoding") != "hex" || reply.string_field("id") != id) return false;
        const std::string hex = reply.string_field("data");
        if (hex.empty() || hex.size() % 2 || hex.size() > 65536) return false;
        std::string ini;
        auto nibble = [](char c) { if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return -1; };
        for (std::size_t i=0;i<hex.size();i+=2) {
            int a=nibble(hex[i]), b=nibble(hex[i+1]); if(a<0||b<0) return false;
            char c=static_cast<char>(a*16+b); if(c==0 || c=='\r') return false; ini+=c;
        }
        std::size_t pos=0;
        while(pos<ini.size()) {
            const auto end=ini.find('\n',pos), eq=ini.find('=',pos);
            const auto stop=end==std::string::npos?ini.size():end;
            if(eq<stop) {
                std::string value=ini.substr(eq+1,stop-eq-1);
                // gc_profile_serialize quotes name/protocol; gc_ini strips
                // only the paired outer quotes, not JSON-style escapes.
                if(value.size()>=2 && ((value.front()=='"' && value.back()=='"') ||
                    (value.front()=='\'' && value.back()=='\''))) value=value.substr(1,value.size()-2);
                if(!values.emplace(ini.substr(pos,eq-pos),std::move(value)).second) return false;
            }
            pos=stop+1;
        }
        return values.count("id") && values.at("id")==id && values.count("name") && values.count("enabled");
    }
    bool load(std::string_view body) {
        std::map<std::string,std::string> v;
        if(!decode(body,id_,v)) return false;
        fields_.clear();
        auto add=[&](std::string key,std::string fallback) {
            auto it=v.find(key);
            fields_.push_back({key,it==v.end()?fallback:it->second,fallback,true});
        };
        add("name","Default identity"); add("enabled","true");
        add("match.vid","any"); add("match.pid","any"); add("match.protocol","any");
        for (const char *side : {"left","right"}) {
            add(std::string(side)+".deadzone","0.000");
            add(std::string(side)+".sensitivity","1.000");
            add(std::string(side)+".trigger_threshold","0");
            add(std::string(side)+".invert_x","false"); add(std::string(side)+".invert_y","false");
        }
        for(const char *button:buttons_) add(std::string("map.")+button,button);
        for(const char *extra:{"INPUT15","INPUT16","INPUT17","INPUT18"}) add(std::string("map.")+extra,"NONE");
        for(const char *button:buttons_) {
            std::string key=std::string("hid.button.")+button;
            if(v.count(key) && !v.at(key).empty()) fields_.push_back({key,v.at(key),{},false});
        }
        for(const char *axis:{"LX","LY","RX","RY","L2","R2"})
            for(const char *prefix:{"axis.","hid.axis."}) {
                std::string key=std::string(prefix)+axis;
                if(v.count(key) && !v.at(key).empty()) fields_.push_back({key,v.at(key),{},false});
            }
        selected_=std::min(selected_,fields_.size()-1);
        return true;
    }
    // Display names only. Field keys and picker values remain the service values.
    static std::string value_label(const std::string &value) {
        static const std::map<std::string,std::string> labels={
            {"true","On"},{"false","Off"},{"any","Any"},{"NONE","None"},
            {"nintendo","Nintendo"},{"xbox_gip","Xbox (GIP)"},{"xusb","Xbox (XInput)"},
            {"ds3","DualShock 3"},{"ds4","DualShock 4"},{"ds4_wireless","DualShock 4 (wireless)"},
            {"logitech","Logitech"},{"hid","Generic HID"},
            {"SHARE","Share"},{"OPTIONS","Options"},{"UP","Up"},{"RIGHT","Right"},
            {"DOWN","Down"},{"LEFT","Left"},{"TRIANGLE","Triangle"},{"CIRCLE","Circle"},
            {"CROSS","Cross"},{"SQUARE","Square"},{"TOUCHPAD","Touchpad"},
            {"INPUT15","Extra input 15"},{"INPUT16","Extra input 16"},
            {"INPUT17","Extra input 17"},{"INPUT18","Extra input 18"}};
        const auto it=labels.find(value); return it==labels.end()?value:it->second;
    }
    static std::string display_value(const std::string &key, const std::string &value) {
        // Names and learned expressions are user text, not enum labels.
        return key=="enabled" || key.find(".invert_")!=std::string::npos ||
            key=="match.protocol" || key.starts_with("map.") ? value_label(value) : value;
    }
    static std::string field_label(const std::string &key) {
        static const std::map<std::string,std::string> labels={
            {"name","Profile name"},{"enabled","Profile enabled"},
            {"match.vid","USB vendor ID"},{"match.pid","USB product ID"},{"match.protocol","Controller protocol"},
            {"left.deadzone","Left stick deadzone"},{"right.deadzone","Right stick deadzone"},
            {"left.sensitivity","Left stick sensitivity"},{"right.sensitivity","Right stick sensitivity"},
            {"left.trigger_threshold","Left trigger threshold"},{"right.trigger_threshold","Right trigger threshold"},
            {"left.invert_x","Invert left stick horizontal"},{"left.invert_y","Invert left stick vertical"},
            {"right.invert_x","Invert right stick horizontal"},{"right.invert_y","Invert right stick vertical"}};
        const auto it=labels.find(key); if(it!=labels.end()) return it->second;
        if(key.starts_with("map.")) return value_label(key.substr(4))+" output";
        if(key.starts_with("hid.button.")) return "Learned "+value_label(key.substr(11))+" input";
        if(key.starts_with("hid.axis.")) return "Learned axis "+key.substr(9);
        if(key.starts_with("axis.")) return "Axis "+key.substr(5)+" input";
        return key;
    }
    /* A mapping value may be a sum of buttons, which the service reads as
     * "these together". That is how a held modifier (a layer) is expressed:
     * map.SQUARE=L1+CROSS means Square only registers while L1 is held. The
     * picker offers one of these per modifier so a layer can be authored
     * without typing an expression. */
    struct ModifierChord { const char *label; std::string value; };
    static std::vector<ModifierChord> modifier_chords(std::string_view source) {
        static const char *const modifiers[]={"L1","L2","R1","R2","L3","R3"};
        std::vector<ModifierChord> out;
        const std::string target(source);
        /* The four learnable inputs cannot be named in a sum: the service
         * refuses them as a mapping target, so nothing is offered for them. */
        if (target.starts_with("INPUT")) return out;
        for(const char *modifier:modifiers) {
            if(target==modifier) continue;
            out.push_back({modifier,std::string(modifier)+"+"+target});
        }
        return out;
    }
    /* Every choice the value picker offers for one map.* field, in list order:
     * the saved value first, then NONE, then each plain output, then one held
     * modifier per modifier so a layer can be authored without typing a sum.
     * Built here, not inside show_value, so a host test can prove the layer
     * values are really offered rather than merely computable. */
    static std::vector<std::pair<std::string,std::string>> mapping_options(const std::string &key,const std::string &saved) {
        std::vector<std::pair<std::string,std::string>> out;
        out.push_back({"Keep saved: "+display_value(key,saved),saved});
        out.push_back({display_value(key,"NONE"),"NONE"});
        for(const char *button:buttons_) out.push_back({display_value(key,button),button});
        for(const auto &chord:modifier_chords(key.substr(4))) {
            if(chord.value==saved) continue;
            out.push_back({std::string("Hold ")+chord.label+": "+display_value(key,chord.value),chord.value});
        }
        return out;
    }
    OverlayRequest picker(std::string label) const {
        OverlayRequest r; r.kind=OverlayKind::picker; r.title="Profile: "+id_; r.label=std::move(label); return r;
    }
    void show_fields(OverlayHost &host,hui::ui::Feedback &feedback) {
        auto r=picker("Choose a setting. Changes are saved to this profile.");
        for(const auto &f:fields_) { r.items.push_back(field_label(f.key)+": "+display_value(f.key,f.value)); r.item_values.push_back(f.key); }
        r.index=static_cast<int>(selected_); stage_=Stage::fields; host.open(r,feedback);
    }
    void show_operation(OverlayHost &host,hui::ui::Feedback &feedback) {
        const auto &f=fields_[selected_]; auto r=picker(field_label(f.key)+": "+display_value(f.key,f.value));
        r.items={"Keep saved value","Change value"}; r.item_values={"keep","edit"};
        if(f.has_default) { r.items.push_back("Reset this setting: "+display_value(f.key,f.fallback)); r.item_values.push_back("default"); }
        else { r.label+=" (learned input; reset unavailable)"; }
        stage_=Stage::operation; host.open(r,feedback);
    }
    void show_value(OverlayHost &host,hui::ui::Feedback &feedback) {
        const auto &f=fields_[selected_]; auto r=picker(field_label(f.key));
        /* A mapping can be narrowed to a held modifier, which the service reads
         * as a layer: the output only fires while that button is down. Say so
         * once in the prompt instead of leaving "L1+CROSS" unexplained. */
        if(f.key.starts_with("map.")) r.label+=". Hold a modifier to make this a layer.";
        if(f.key.starts_with("map.")) {
            for(const auto &choice:mapping_options(f.key,f.value)) {
                r.items.push_back(choice.first); r.item_values.push_back(choice.second);
            }
        } else {
            r.items={"Keep saved: "+display_value(f.key,f.value)}; r.item_values={f.value};
            auto add=[&](std::string value) { r.items.push_back(display_value(f.key,value)); r.item_values.push_back(value); };
            if(f.key=="enabled" || f.key.find(".invert_")!=std::string::npos) { add("true"); add("false"); }
            else if(f.key=="match.protocol") for(const char *p:{"any","nintendo","xbox_gip","xusb","ds3","ds4","logitech","hid","ds4_wireless"}) add(p);
            else if(f.key.ends_with(".deadzone") || f.key.ends_with(".sensitivity") || f.key.ends_with(".trigger_threshold")) {
                const bool dead=f.key.ends_with(".deadzone"), trigger=f.key.ends_with(".trigger_threshold");
                for(int i=dead||trigger?0:1;i<=(dead?95:trigger?16:30);++i) {
                    char value[32]; if(trigger) std::snprintf(value,sizeof(value),"%d",i==16?255:i*16);
                    else std::snprintf(value,sizeof(value),"%.3f",dead?i/100.0:i/10.0);
                    add(value);
                }
            } else {
                r.kind=OverlayKind::text; r.text=f.value; r.max_length=f.key=="name"?95:512;
                if(f.key=="match.vid" || f.key=="match.pid") { r.max_length=16; r.label=field_label(f.key)+" (any or numeric ID, e.g. 0x054c)"; }
                stage_=Stage::text; host.open(r,feedback); return;
            }
        }
        stage_=Stage::value; host.open(r,feedback);
    }
    void submit(std::string value,OverlayHost &host,hui::ui::Feedback &feedback) {
        const auto &f=fields_[selected_];
        if(!fresh()) { message_="Profile changed or unavailable. Nothing sent. Reopen the editor."; stage_=Stage::closed; host.close(feedback); return; }
        if(value==f.value) { message_="Kept saved value. Nothing changed."; show_fields(host,feedback); return; }
        if(value.empty() || value.size()>512 || std::any_of(value.begin(),value.end(),[](unsigned char c){return c<32 || c==127;})) {
            message_="Enter a value within the length limit, without control characters. Nothing sent."; show_operation(host,feedback); return;
        }
        edited_key_=f.key; wanted_=std::move(value);
        const std::string arg=id_+"|"+edited_key_+"|"+wanted_;
        pending_=service_->queue_fetch_request("PROFILE_EDIT",arg.c_str());
        if(!pending_) { message_="Couldn't send the change. Nothing sent."; show_operation(host,feedback); return; }
        stage_=Stage::writing; host.close(feedback); message_="Saving...";
    }
public:
    /* The exact chord values the picker offers for one mapped source. Exposed
     * so a host test can prove every value it offers is one the service's own
     * profile parser accepts, without driving the whole editor. */
    static std::vector<ModifierChord> mapping_chords(std::string_view source) {
        return modifier_chords(source);
    }
    /* The exact label/value choices the value picker builds for one map.* field,
     * in order. Exposed so a host test can prove a held-modifier layer is really
     * listed for the player, not only computable. */
    static std::vector<std::pair<std::string,std::string>> mapping_choices(const std::string &key,const std::string &saved) {
        return mapping_options(key,saved);
    }
    bool active() const { return stage_!=Stage::closed; }
    std::string take_message() { std::string out; out.swap(message_); return out; }
    bool open(ServiceSnapshot *service,std::string id,unsigned long long source,OverlayHost &host,hui::ui::Feedback &feedback) {
        service_=service; id_=std::move(id); source_=source; selected_=0; pending_=0;
        if(!fresh() || !load(service_->fetch_request(source_).body)) { stage_=Stage::closed; message_="Couldn't load this profile. Nothing sent."; return false; }
        show_fields(host,feedback); return true;
    }
    void update(const hui::InputFrame &input,float dt,OverlayHost &host,hui::ui::Feedback &feedback) {
        if(!active()) return;
        if(stage_==Stage::writing || stage_==Stage::reading) {
            const auto f=service_->fetch_request(pending_);
            if(f.state==ServiceSnapshot::ResultState::queued) {
                if(input.is_pressed(hui::Action::back)) message_="Already sent. Can't cancel this save. Checking the result...";
                return;
            }
            if(stage_==Stage::writing) {
                accepted_=complete(f);
                if(!accepted_) message_=f.reached?"Service refused the change. Save not confirmed.":"Connection lost. Save not confirmed. Won't retry automatically.";
                pending_=service_->queue_fetch_request("PROFILE_GET",id_.c_str());
                if(!pending_) { stage_=Stage::closed; message_="Couldn't check the save. Check the profile before retrying."; return; }
                stage_=Stage::reading; return;
            }
            source_=pending_; pending_=0;
            std::map<std::string,std::string> values;
            const bool verified=complete(f) && fresh() && decode(f.body,id_,values) && values.count(edited_key_) && equivalent(edited_key_,values.at(edited_key_),wanted_);
            if(!complete(f) || !fresh() || !load(f.body)) { stage_=Stage::closed; message_="Couldn't check the result. The change may be saved. Won't undo or retry automatically."; return; }
            if(accepted_) message_=verified?"Saved. Controller effect not checked.":"Request accepted, but the saved value differs. Check the profile before retrying.";
            show_fields(host,feedback); return;
        }
        host.update(input,dt,feedback); OverlayResult result;
        if(!host.take_result(&result)) {
            if(!host.is_open()) {
                if(stage_==Stage::fields) { stage_=Stage::closed; message_="Profile editor closed. No further changes sent."; }
                else { message_="Cancelled. Nothing changed."; show_fields(host,feedback); }
            }
            return;
        }
        switch(stage_) {
        case Stage::fields:
            if(result.index<0 || static_cast<std::size_t>(result.index)>=fields_.size() || fields_[result.index].key!=result.item_value) { stage_=Stage::closed; return; }
            selected_=static_cast<std::size_t>(result.index); show_operation(host,feedback); break;
        case Stage::operation:
            if(result.item_value=="keep") { message_="Kept saved value. Nothing changed."; show_fields(host,feedback); }
            else if(result.item_value=="edit") show_value(host,feedback);
            else if(result.item_value=="default" && fields_[selected_].has_default) submit(fields_[selected_].fallback,host,feedback);
            break;
        case Stage::value:
            if(result.index==0) { message_="Kept saved value. Nothing changed."; show_fields(host,feedback); }
            else submit(result.item_value,host,feedback);
            break;
        case Stage::text: submit(result.text,host,feedback); break;
        default: break;
        }
    }
};
} // namespace ghostcontrol
