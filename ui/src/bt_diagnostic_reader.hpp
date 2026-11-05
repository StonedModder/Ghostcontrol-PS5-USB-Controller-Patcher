/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "json_reply.hpp"
#include <algorithm>
#include <vector>
namespace ghostcontrol {
// Only the allowlisted diagnostic subtree is projected. Never print the reply,
// pairing inventory, unknown strings or refusal payloads into this reader.
inline std::vector<std::string> bt_diagnostic_lines(std::string_view body) {
 if(body.size()>32768) return {"Bluetooth diagnostics malformed or unavailable."};
 JsonReply root(body); if(!root.success())return {"Bluetooth diagnostics malformed or unavailable."};
 auto raw=root.raw("diagnostics");
 if(raw=="null")return {"Bluetooth diagnostics unavailable: no engine snapshot."};
 JsonReply d(raw);if(!d.valid)return {"Bluetooth diagnostics unavailable or malformed."};
 JsonReply r(d.raw("radio")),e(r.raw("endpoints")),c(r.raw("counters")),f(d.raw("failures"));
 std::vector<std::string> lines={"Measured Bluetooth diagnostics (redacted request snapshot)",
 d.raw("radio")=="null"?"Radio never opened":d.raw("radioOpen")=="false"?"Radio closed; retained session is stale":d.raw("radioOpen")=="true"?"Radio open":"Radio state unavailable"};
 auto number=[](std::string_view v){return !v.empty()&&v.size()<=20&&std::all_of(v.begin(),v.end(),[](char ch){return ch>='0'&&ch<='9';});};
 auto add=[&](const JsonReply &j,const char*key,bool age=false){auto v=j.raw(key);std::string value=number(v)?std::string(v):age&&v=="null"?"never":v.empty()?"unavailable":"unknown";lines.emplace_back(std::string(key)+": "+value+(age&&number(v)?" ms":""));};
 auto boolean=[&](const JsonReply&j,const char*key){auto v=j.raw(key);lines.emplace_back(std::string(key)+": "+(v=="true"?"true":v=="false"?"false":v.empty()?"unavailable":"unknown"));};
 add(d,"sampleAgeMs",true); auto age=d.raw("sampleAgeMs");if(number(age)&&(age.size()>4||(age.size()==4&&age>"3000")))lines.emplace_back("Engine sample stale (over 3000 ms)");
 boolean(d,"radioOpen");boolean(r,"current");
 auto hex=[&](const JsonReply&j,const char*key,bool usb){auto v=j.string_field(key);bool ok=usb?v.size()==9&&v[4]==':':v.size()==4&&v.substr(0,2)=="0x";if(ok)for(size_t i=usb?0:2;i<v.size();i++)if(!(usb&&i==4)&&!std::isxdigit(static_cast<unsigned char>(v[i])))ok=false;lines.emplace_back(std::string(key)+": "+(ok?v:j.raw(key).empty()||j.raw(key)=="null"?"unavailable":"unknown"));};
 hex(r,"usb",true);add(r,"interface");hex(e,"events",false);hex(e,"aclIn",false);hex(e,"aclOut",false);boolean(e,"fromDescriptor");add(r,"hciFunctions");add(r,"aclMtu");add(r,"aclBuffers");boolean(r,"le");
 for(const char*key:{"reads","readsEmpty","events","acl","eventQueueDrops","aclQueueDrops","partialEventDrops","aclStale","aclMalformed","txNoCredit","txFailed","probes","resetAttempts"})add(c,key);
 add(r,"lastRxAgeMs",true);for(const char*key:{"openAttempts","openFailures","transportLosses"})add(f,key);
 auto failure=f.string_field("last");bool safe=false;for(auto word:{"controller did not answer the host setup","Bluetooth USB transport did not open","Bluetooth transport stopped responding"})if(failure==word)safe=true;
 // Fixed producer failure families only; arbitrary service text is not redacted.
 lines.emplace_back(std::string("last failure: ")+(f.raw("last")=="null"?"none":safe?failure:f.raw("last").empty()?"unavailable":"reported (text redacted)"));add(f,"lastAgeMs",true);
 return lines;
}
}
