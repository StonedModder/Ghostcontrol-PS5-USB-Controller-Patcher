/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ground-up task workspace painter. No generic row, card or tab renderer.
 */
#include "toolbox_ui.hpp"
#include "app_log.hpp"
#include "ui/glyphs.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace ghostcontrol {
namespace {
using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Align;
using hui::gfx::Rect;
using hui::ui::Fonts;
/* Ink ramp for the workbench, with the teal reserved for the live value and
 * the selection. A hairline is the primary text at 8%. */
const auto ink = Color::rgb(0xeef0f5);
const auto muted = Color::rgb(0x7a8296);
const auto blue = Color::rgb(0x5fc8b8);
const auto line = Color::rgb(0xeef0f5, 0.08f);
const auto base = Color::rgb(0x0b0d14);
const auto green = Color::rgb(0x4cc38a);
const auto amber = Color::rgb(0xd8a657);

/* A quiet status is still the page's figure, so it reads in primary ink; only
 * a real condition takes a semantic colour. Muted is for meta, not for the
 * number the page is about. */
Color tone_color(TaskTone tone) {
    return tone == TaskTone::good ? green : tone == TaskTone::warning ? amber :
           tone == TaskTone::busy ? blue : ink;
}
// UTF-8-safe wrap, shared by failure reasons, prompts, notes and reports.
std::vector<std::string> wrap(const hui::ui::FontRef &font, std::string_view text,
                              float size, float width) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '\n') { lines.push_back(current); current.clear(); ++pos; continue; }
        std::size_t end = pos + 1;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
        const std::string next(text.substr(pos, end - pos));
        if (!current.empty() && font.measure(current + next, size) > width) {
            const auto space = current.find_last_of(' ');
            if (space != std::string::npos && space > current.size() / 3) {
                lines.push_back(current.substr(0, space)); current.erase(0, space + 1);
            } else { lines.push_back(current); current.clear(); }
        }
        if (!current.empty() || next != " ") current += next;
        pos = end;
    }
    if (!current.empty() || lines.empty()) lines.push_back(current);
    return lines;
}
std::string fit(const hui::ui::FontRef &font, std::string_view text, float size, float width) {
    std::string s(text);
    if(font.measure(s,size)<=width) return s;
    while(!s.empty() && font.measure(s+"...",size)>width) {
        auto end=s.size()-1;
        while(end>0 && (static_cast<unsigned char>(s[end])&0xc0)==0x80) --end;
        s.resize(end);
    }
    return s+"...";
}
float paragraph(DrawList &list, const hui::ui::FontRef &font, std::string_view text,
                float x, float y, float size, float width, Color color,
                std::size_t maximum = 5) {
    const auto lines = wrap(font, text, size, width);
    // Each block has a finite budget. Signal continuation, never paint hidden
    // lines and mistake scissoring for a working layout.
    for (std::size_t i = 0; i < lines.size() && i < maximum; ++i) {
        const auto shown = i+1==maximum && lines.size()>maximum
            ? fit(font,lines[i]+"...",size,width) : lines[i];
        hui::ui::text(list, font, shown, x, y, size, color);
        y += size * 1.35f;
    }
    return y;
}
void eyebrow(DrawList &list, const Fonts &fonts, std::string_view s, float x, float y) {
    hui::ui::text(list, fonts.semibold, fit(fonts.semibold,s,18,1824-x), x, y, 18, blue);
}

void facts(DrawList &list, const Fonts &fonts, const std::vector<TaskFact> &values,
           float x, float y, float width, std::size_t limit = 3) {
    const std::size_t n=std::min(values.size(),limit);
    if(!n) return;
    const float stride=std::min(130.0f,(815.0f-y)/static_cast<float>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const float top=y+static_cast<float>(i)*stride;
        hui::ui::text(list,fonts.semibold,fit(fonts.semibold,values[i].label,18,width),x,top,18,blue);
        const std::size_t rows=stride>=105 ? 2 : 1;
        paragraph(list,fonts.regular,values[i].value,x,top+35,27,width,ink,rows);
    }
}
void item_list(DrawList &list, const Fonts &fonts, const std::vector<TaskItem> &items,
               float x, float y, float width, std::size_t limit = 4) {
    const std::size_t n=std::min(items.size(),limit);
    if(!n) return;
    const float stride=std::min(150.0f,(805.0f-y)/static_cast<float>(n));
    for (std::size_t i=0;i<n;++i) {
        const float top=y+static_cast<float>(i)*stride;
        list.line(x,top-25,x+width,top-25,1,line);
        paragraph(list,fonts.semibold,items[i].title,x,top+12,27,width,ink,1);
        if (!items[i].detail.empty())
            paragraph(list,fonts.regular,items[i].detail,x,top+50,24,width,muted,stride>=120?2:1);
    }
    if (items.size() > limit)
        hui::ui::text(list,fonts.regular,"More in Tools / Details",x,814,20,muted);
}
void action_button(DrawList &list, const Fonts &fonts, const TaskButton &button,
                   float x, float y, float width, bool selected, bool primary) {
    // Buttons contain only a verb. No status, note, reading, dot, or badge.
    list.bordered_rect({x,y,width,64},6,
        selected ? blue : Color::rgb(0x0c1925), selected ? 2.0f : 1.0f,
        selected ? Color::rgb(0xa9ddff) : line);
    const auto labels=wrap(fonts.semibold,button.label,25,width-32);
    const float first=labels.size()>1 ? y+25 : y+41;
    for(std::size_t i=0;i<std::min<std::size_t>(2,labels.size());++i)
        hui::ui::text(list,fonts.semibold,i==1 && labels.size()>2 ? fit(fonts.semibold,labels[i]+"...",25,width-32) : labels[i],
            x+width/2,first+static_cast<float>(i)*29,25,
            selected ? base : (primary ? ink : muted),Align::center);
    if (selected) list.line(x+18,y+74,x+width-18,y+74,3,blue);
}
/* One support code: the plate to scan, its name, and the whole payload
 * underneath so it can still be typed when a camera will not cooperate. The
 * plate stays white on purpose -- a scanner needs the quiet zone and the
 * contrast -- and the code is never tinted to the theme. A code that did not
 * load says so and still prints its string. */
void qr_card(DrawList &list, const Fonts &fonts, const QrCode &code, const Rect &card,
             std::string_view title, std::string_view kind, std::string_view payload) {
    list.bordered_rect(card,14,Color::rgb(0x11141c,0.92f),1.0f,line);
    hui::ui::text(list,fonts.semibold,title,card.x+32,card.y+44,20,blue);
    const Rect plate{card.x+(card.w-300)*0.5f,card.y+60,300,300};
    if(code.present()) {
        list.rounded_rect(plate,10,Color::rgb(0xffffff));
        list.image(code.texture,plate.inset(8),hui::gfx::kFullUv,Color::rgb(0xffffff));
    } else {
        list.bordered_rect(plate,10,Color::rgb(0x0c1925),1.0f,line);
        paragraph(list,fonts.regular,std::string(title)+" code unavailable in this build.",
                  card.x+32,card.y+196,24,card.w-64,amber,2);
    }
    hui::ui::text(list,fonts.regular,kind,card.x+32,card.y+392,17,muted);
    paragraph(list,fonts.mono,payload,card.x+32,card.y+420,18,card.w-64,ink,2);
}
} // namespace


void ToolboxUi::rebuild_task(const ServiceSnapshot::View &view) {
    std::size_t previous = row_index_; // catalog identity remembered per workspace
    if (button_focus_ < task_.buttons.size()) previous = task_.buttons[button_focus_].action;
    const auto current = fresh_view(view);
    task_ = build_task_page(section_index_, section(), current,
        [this,&current](const Row &r){ return row_reading(r,current); });
    if (task_.scene == TaskScene::audio && !now_playing_.empty())
        task_.facts.insert(task_.facts.begin(), {"Now playing", now_playing_});
    if (section_index_ == 5) {
        const auto &p = app_preference_view_;
        task_.facts.push_back({"Start with this app",p.pending ? "Saving..." :
            !p.loaded || !p.authoritative_known ? "Not set" : p.authoritative ? "On" : "Off"});
        if (lifecycle_view_.active > lifecycle_view_.completed) {
            task_.tone = TaskTone::busy;
            task_.detail = lifecycle_outcome_text(lifecycle_view_);
        }
    }
    const auto found = std::find_if(task_.buttons.begin(),task_.buttons.end(),
        [previous](const auto &b){return b.action == previous;});
    button_focus_ = found == task_.buttons.end() ? 0 : static_cast<std::size_t>(found-task_.buttons.begin());
    if (tool_index_ >= task_.more.size()) tool_index_ = 0;
}

void ToolboxUi::draw_workspace(DrawList &list, const Fonts &fonts,
                              const ServiceSnapshot::View &) const {
    const auto &p=task_;
    const auto color=tone_color(p.tone);
    /* An empty state is a state, not a hole: give the status block a surface so
     * the page never reads as a heading followed by dead space. */
    if (p.items.empty() && p.scene != TaskScene::setup) {
        /* The setup wizard paints its own prompt surface under a stepper it
         * owns, so it is not given a second card here. */
        const Rect card{80, 388, 1080, 272};
        list.bordered_rect(card, 14, Color::rgb(0x11141c, 0.92f), 1.0f,
                           Color::rgb(0xeef0f5, 0.08f));
    }
    eyebrow(list,fonts,p.eyebrow,96,220);
    paragraph(list,fonts.display,p.title,104,278,58,1680,ink,1);
    paragraph(list,fonts.regular,p.lead,96,328,26,1670,muted,2);
    // Blocks fit their assigned bands; do not hide broken layout with a scissor.
    switch(p.scene) {
    case TaskScene::controllers:
        paragraph(list,fonts.display,p.status,96,445,44,930,color,2);
        paragraph(list,fonts.regular,p.detail,96,550,27,860,muted,3);
        facts(list,fonts,p.facts,96,675,400,1);
        facts(list,fonts,p.facts.size()>1 ? std::vector<TaskFact>(p.facts.begin()+1,p.facts.end()) : std::vector<TaskFact>{},550,675,400,1);
        eyebrow(list,fonts,"Controller inventory",1120,408);
        item_list(list,fonts,p.items,1120,470,640,2);
        /* The community code lives in the corner every visit passes: one
         * glance, one scan, no trip to another page. It keeps the same ink as
         * the rest of the workbench and only the plate is white, because a
         * camera needs that contrast. */
        {
            const Rect card{1120,820,704,110};
            list.bordered_rect(card,12,Color::rgb(0x11141c,0.92f),1.0f,line);
            if(qr_discord_.present()) {
                list.rounded_rect({card.x+14,card.y+11,88,88},8,Color::rgb(0xffffff));
                list.image(qr_discord_.texture,{card.x+18,card.y+15,80,80},
                           hui::gfx::kFullUv,Color::rgb(0xffffff));
                hui::ui::text(list,fonts.semibold,"DISCORD",card.x+120,card.y+38,18,blue);
                hui::ui::text(list,fonts.semibold,"Scan to join",card.x+120,card.y+80,26,ink);
                /* The invite sits on the eyebrow's line, right aligned: the
                 * card reads in two rows, not four cramped ones. */
                hui::ui::text(list,fonts.regular,"discord.com/invite/kcgQPxhde",
                              card.x+card.w-24,card.y+38,14,muted,Align::right);
            } else {
                hui::ui::text(list,fonts.semibold,"DISCORD",card.x+24,card.y+38,18,blue);
                hui::ui::text(list,fonts.regular,"Community code unavailable in this build.",
                              card.x+24,card.y+80,22,amber);
            }
        }
        break;
    case TaskScene::donate: {
        /* Three ways to keep the work going, three codes. Every card carries
         * its whole string: a scan can fail, and a typed address cannot. */
        constexpr float kGap=24, kCardW=(1728.0f-2*24.0f)/3.0f;
        const Rect first{96,388,kCardW,452};
        qr_card(list,fonts,qr_discord_,first,"DISCORD","Invite",
                "https://discord.com/invite/kcgQPxhde");
        qr_card(list,fonts,qr_btc_,{first.x+kCardW+kGap,first.y,kCardW,first.h},"BITCOIN","Address",
                "bc1qa9zfgnccajsw8vg7k287qz5a7apf8pefj5jjx5");
        qr_card(list,fonts,qr_patreon_,{first.x+2*(kCardW+kGap),first.y,kCardW,first.h},
                "PATREON","Page","patreon.com/c/u4772250");
        break;
    }
    case TaskScene::setup: {
        /* The wizard's motion is read from the frame clocks the shell already
         * keeps: the section's entrance age, the decaying press age and the
         * free clock the overlay widgets read. None of it advances a step --
         * page.step is SETUP_STATUS -- and with motion switched off (the spirit
         * field is the app's one motion gate) or a clock that never advanced,
         * every factor settles so the rail is drawn complete but still. */
        const bool moves=ghostscape_.enabled() && clock_>0.0f;
        const float enter=moves?hui::tween::cubic_out(switch_age_/0.6f):1.0f;
        const float press=moves?std::exp(-press_age_*6.0f):0.0f;
        const float beat=moves?hui::ui::breathe(clock_,2.6f):0.0f;
        const Color accent=p.tone==TaskTone::quiet?blue:tone_color(p.tone);
        const Color quiet=Color::rgb(0xeef0f5,0.16f);
        const std::size_t steps=p.steps.size();
        const std::size_t at=steps?std::min(p.step,steps-1):0;
        const float left=108.0f,right=1748.0f,rail=404.0f;
        const float stride=steps>1?(right-left)/static_cast<float>(steps-1):0.0f;
        const float here=left+stride*static_cast<float>(at);
        /* The one number the page is about, on the header's own line. */
        if(steps) {
            const std::string caption="STEP "+std::to_string(at+1)+" OF "+std::to_string(steps)+
                "  ·  "+p.steps[at];
            hui::ui::text(list,fonts.mono,fit(fonts.mono,caption,17,1120),1824,224,17,accent,Align::right);
        }
        /* The rail is the wizard's real position: filled to the step the
         * service is on, ticked for the steps already behind it. */
        list.line(left,rail,right,rail,2,quiet);
        if(at) {
            list.line(left,rail,here,rail,3,accent.with_alpha(0.7f));
            /* The press that advanced the step sends a short glint past the
             * dot and it settles back as the age grows. */
            if(press>0.02f)
                list.line(here,rail,here+stride*0.03f*press,rail,3,accent.with_alpha(0.5f*press));
        }
        if(moves) {
            list.glow({here-46,rail-46,92,92},46,30,accent.with_alpha(0.16f*enter));
            /* While the service waits for a control, one bead runs from the
             * step it is on to the step it is going to. */
            if(p.tone==TaskTone::busy && at+1<steps) {
                const float phase=std::fmod(clock_,1.7f)/1.7f;
                list.circle(here+stride*hui::tween::smoothstep(phase),rail,4.5f,
                            accent.with_alpha(0.3f+0.5f*std::sin(phase*3.1416f)));
            }
        }
        for(std::size_t i=0;i<steps;++i) {
            const float x=left+stride*static_cast<float>(i);
            if(i<at) {
                /* A finished step keeps its tick: the wizard's own history. */
                list.circle(x,rail,9.0f+2.0f*press,blue.with_alpha(0.9f));
                list.line(x-4.0f,rail,x-1.1f,rail+3.2f,2.0f,base);
                list.line(x-1.1f,rail+3.2f,x+4.3f,rail-3.4f,2.0f,base);
            } else if(i==at) {
                /* The live step: a breathing ring in the page's tone, so
                 * listening, ready and blocked read differently. */
                list.ring(x,rail,15.0f+3.0f*beat+5.0f*press,2.0f,
                          accent.with_alpha(0.24f+0.24f*beat+0.4f*press));
                list.circle(x,rail,7.0f,accent);
            } else {
                list.ring(x,rail,7.0f,1.5f,quiet);
            }
            const auto label_color=i==at?ink:i<at?muted:Color::rgb(0x7a8296,0.8f);
            /* The rail now spans the gutter, so the last label is anchored to
             * it rather than run off the canvas from the last dot. */
            if(i+1==steps && steps>1)
                hui::ui::text(list,fonts.semibold,fit(fonts.semibold,p.steps[i],23,stride-16.0f),
                              1810.0f,440,23,label_color,Align::right);
            else
                hui::ui::text(list,fonts.semibold,fit(fonts.semibold,p.steps[i],23,stride-26.0f),
                              x-10.0f,440,23,label_color);
        }
        /* The prompt surface: an empty column is a state, not a hole. One
         * height for every state so a long explanation never runs past its
         * own card, buttons or not. */
        list.bordered_rect({80,470,1080,322},14,
                           Color::rgb(0x11141c,0.92f),1.0f,
                           p.tone==TaskTone::warning?amber.with_alpha(0.45f):line);
        if(!p.buttons.empty()) {
            /* The state word sits in the card's own top band, above the
             * prompt's ink, so it can never collide with the sentence it
             * qualifies. The dot breathes while the service is listening. */
            const char *chip=p.tone==TaskTone::warning?"NEEDS ATTENTION":
                             p.tone==TaskTone::busy?"LISTENING":
                             at>=3?"READY TO SAVE":at==0?"NOT STARTED":"ARMED";
            const float chip_w=fonts.mono.measure(chip,14);
            list.circle(1130.0f-chip_w-14.0f,492,3.0f,accent.with_alpha(0.55f+0.45f*beat));
            hui::ui::text(list,fonts.mono,chip,1130,498,14,accent,Align::right);
        }
        paragraph(list,fonts.display,p.status,96,548,46,1000,color,2);
        paragraph(list,fonts.regular,p.detail,96,670,28,1000,muted,3);
        /* What the draft holds, oldest first and newest lit: the same rows
         * Square opens in full, cut to what fits beside the prompt. */
        facts(list,fonts,p.facts,1190,505,600,2);
        if(!p.items.empty()) {
            const bool notice=p.items[0].title=="Mapping preview unavailable";
            const bool none=p.items[0].title=="No mappings to review yet";
            if(notice||none) {
                eyebrow(list,fonts,notice?"Draft preview":"Learned so far",1190,728);
                paragraph(list,fonts.semibold,notice?"Preview unavailable.":"No mappings yet.",
                          1190,766,24,600,ink,1);
                paragraph(list,fonts.regular,p.items[0].detail,1190,796,21,600,muted,2);
            } else {
                eyebrow(list,fonts,std::string(p.step>=3?"Draft mappings (":"Learned so far (")+
                        std::to_string(p.items.size())+")",1190,728);
                /* A row reads as the draft writes it -- source = target -- so
                 * the pair cannot be mistaken for two unrelated columns. */
                const std::size_t shown=std::min<std::size_t>(p.items.size(),3);
                for(std::size_t i=0;i<shown;++i) {
                    const float y=762.0f+30.0f*static_cast<float>(i);
                    const bool newest=i+1==p.items.size();
                    const std::string from=fit(fonts.semibold,p.items[i].title,23,300);
                    const float fx=1190.0f, fw=fonts.semibold.measure(from,23);
                    hui::ui::text(list,fonts.semibold,from,fx,y,23,ink);
                    hui::ui::text(list,fonts.regular,"=",fx+fw+10,y,23,muted);
                    hui::ui::text(list,fonts.regular,
                                  fit(fonts.regular,p.items[i].detail,23,1784.0f-(fx+fw+30.0f)),
                                  fx+fw+28,y,23,newest?accent:ink);
                    if(newest&&moves)
                        list.circle(1174,y-8,3.0f+2.0f*beat,accent.with_alpha(0.45f+0.4f*beat));
                }
            }
        }
        if(p.progress>=0) {
            list.line(96,792,1096,792,4,line);
            list.line(96,792,96+1000*std::clamp(p.progress,0.0f,1.0f),792,4,blue);
        }
        break;
    }
    case TaskScene::bluetooth:
        paragraph(list,fonts.display,p.status,96,450,46,990,color,2);
        paragraph(list,fonts.regular,p.detail,96,560,28,960,muted,4);
        for(std::size_t i=0;i<p.facts.size() && i<3;++i)
            facts(list,fonts,{p.facts[i]},96+static_cast<float>(i)*325,730,300,1);
        // Keep the lead and saved-device list unobstructed. The decorative
        // radar had no task meaning and its outer stroke crossed the lead;
        // moving it down would instead collide with the inventory below.
        item_list(list,fonts,p.items,1195,700,550,1);
        break;
    case TaskScene::profiles:
        item_list(list,fonts,p.items,96,420,820,4);
        paragraph(list,fonts.display,p.status,1060,442,42,730,color,2);
        paragraph(list,fonts.regular,p.detail,1060,550,28,720,muted,3);
        facts(list,fonts,p.facts,1060,710,710,1);
        break;
    case TaskScene::lights:
        paragraph(list,fonts.display,p.status,96,445,46,930,color,2);
        paragraph(list,fonts.regular,p.detail,96,550,27,900,muted,3);
        facts(list,fonts,p.facts,96,690,850,1);
        // Reserve this area for actual output results, never a controller render.
        for(std::size_t i=0;i<p.items.size() && i<2;++i) {
            const float y=420+static_cast<float>(i)*205;
            eyebrow(list,fonts,p.items[i].title,1140,y);
            const auto lines=wrap(fonts.regular,p.items[i].detail,26,650);
            float baseline=y+42;
            for(std::size_t j=0;j<lines.size() && j<4;++j) {
                hui::ui::text(list,fonts.regular,j==3 && lines.size()>4 ? "Square shows the full result." : lines[j],
                              1140,baseline,26,ink);
                baseline+=35;
            }
        }
        break;
    case TaskScene::service:
        list.line(1090,390,1090,790,1,line);
        paragraph(list,fonts.display,p.status,96,462,54,930,color,2);
        paragraph(list,fonts.regular,p.detail,96,590,28,920,muted,4);
        facts(list,fonts,p.facts,1170,415,600,3);
        break;
    case TaskScene::diagnostics: {
        eyebrow(list,fonts,"Live report",96,409);
        facts(list,fonts,p.facts,96,463,795,3);
        const auto rest=p.facts.size()>3 ? std::vector<TaskFact>(p.facts.begin()+3,p.facts.end()) : std::vector<TaskFact>{};
        facts(list,fonts,rest,1040,463,740,3);
        break;
    }
    case TaskScene::audio:
        // No audio controls on a build without a capture provider.
        for(int i=0;i<15;++i) {
            const float h=30+std::abs(7-i)*11.0f;
            list.line(1350+static_cast<float>(i)*20,540-h,1350+static_cast<float>(i)*20,540+h,5,line);
        }
        list.line(1305,670,1695,410,4,muted);
        paragraph(list,fonts.display,p.status,96,470,48,1060,color,2);
        paragraph(list,fonts.regular,p.detail,96,605,30,1040,muted,4);
        facts(list,fonts,p.facts,96,765,1030,1);
        break;
    case TaskScene::about:
        if(logo_texture_ && logo_h_>0) {
            const float height=330, width=height*static_cast<float>(logo_w_)/static_cast<float>(logo_h_);
            list.image(logo_texture_,{1410-width/2,410,width,height},hui::gfx::kFullUv,Color::rgb(0xffffff));
        }
        paragraph(list,fonts.display,p.status,96,465,48,1050,color,2);
        paragraph(list,fonts.regular,p.detail,96,590,30,1030,muted,4);
        if(p.facts.size()>2) facts(list,fonts,{p.facts[2]},96,765,1000,1);
        break;
    }
    const std::size_t n=std::min<std::size_t>(p.buttons.size(),4);
    float x=96;
    for(std::size_t i=0;i<n;++i) {
        const float width=std::min(400.0f,(1728.0f-24.0f*static_cast<float>(n-1))/static_cast<float>(n));
        action_button(list,fonts,p.buttons[i],x,850,width,i==button_focus_,i==0);x+=width+24;
    }
    if(p.buttons.empty() && p.scene != TaskScene::donate)
        hui::ui::text(list,fonts.regular,"Nothing to change on this page.",96,890,25,muted);
}

void ToolboxUi::draw_directory(DrawList &list,const Fonts &fonts) const {
    eyebrow(list,fonts,"Workspaces",96,220);
    hui::ui::text(list,fonts.display,"What do you want to do?",104,278,58,ink);
    static const char *descriptions[]={"See your controllers and saved device rules.",
        "Learn a controller, one button at a time.","Connect a wireless controller.",
        "Create and edit saved button layouts.","Adjust the lightbar and test vibration.",
        "Start Ghosttooth API and manage controller input.","Inspect device details and service logs.",
        "Check this build's audio support.","Support the project and what your donations fund.",
        "Credits, licenses and build information."};
    const char *description = "";
    if(menu_index_ < sizeof(descriptions)/sizeof(descriptions[0]))
        description = descriptions[menu_index_];
    /* Fit every workspace above the footer: a tenth entry overran it. */
    const float step=sections_.size()>1
        ? std::min(61.0f,(930.0f-370.0f)/static_cast<float>(sections_.size()-1)) : 61.0f;
    for(std::size_t i=0;i<sections_.size();++i) {
        const float y=370+static_cast<float>(i)*step;
        if(i==menu_index_) list.line(96,y-24,96,y+9,4,blue);
        hui::ui::text(list,fonts.mono,(i<9?"0":"")+std::to_string(i+1),125,y,21,muted);
        hui::ui::text(list,i==menu_index_?fonts.semibold:fonts.regular,sections_[i].name,
                      190,y,32,i==menu_index_?ink:muted);
    }
    paragraph(list,fonts.display,sections_[menu_index_].name,985,449,54,800,ink,2);
    paragraph(list,fonts.regular,description,985,570,32,790,muted,4);
    hui::ui::text(list,fonts.semibold,"Press Cross to go there",985,765,26,blue);
}

void ToolboxUi::draw_tools(DrawList &list,const Fonts &fonts,
                          const ServiceSnapshot::View &view) const {
    eyebrow(list,fonts,std::string(section().name)+" / tools",96,220);
    hui::ui::text(list,fonts.display,"More controls",104,278,58,ink);
    const auto &buttons=task_.more;
    if(buttons.empty()) {
        paragraph(list,fonts.regular,"This page has no extra controls.",96,440,32,1550,muted);return;
    }
    const std::size_t start=tool_index_>5?tool_index_-5:0;
    for(std::size_t i=start;i<buttons.size() && i<start+8;++i) {
        const float y=380+static_cast<float>(i-start)*64;
        if(i==tool_index_) list.line(96,y-27,96,y+8,4,blue);
        paragraph(list,i==tool_index_?fonts.semibold:fonts.regular,buttons[i].label,
                  124,y,29,740,i==tool_index_?ink:muted,1);
    }
    const auto &r=section().rows[buttons[tool_index_].action];
    paragraph(list,fonts.display,buttons[tool_index_].label,990,420,40,820,ink,2);
    paragraph(list,fonts.regular,r.note?r.note:"",990,550,28,800,muted,4);
    // A setting's reading lives here, apart from the selectable command.
    if(r.live!=LiveSource::none || r.app_launch_opt_in || r.starts_companion || r.stops_companion || r.restarts_companion) {
        eyebrow(list,fonts,"Current value",990,713);
        paragraph(list,fonts.regular,row_reading(r,view),990,755,30,790,green,2);
    }
    hui::ui::text(list,fonts.mono,std::to_string(tool_index_+1)+" / "+std::to_string(buttons.size()),
                  96,920,23,muted);
}

void ToolboxUi::draw_reader(DrawList &list,const Fonts &fonts) const {
    if(focus_!=Focus::reader) return;
    /* A sheet over the field rather than a second page: the page body is not
     * drawn at all in this focus, so the spirits stay faintly behind the card
     * and the sheet never stacks two layers of text. */
    eyebrow(list,fonts,std::string(section().name)+" / report",96,220);
    paragraph(list,fonts.display,reader_title_.empty()?row().label:reader_title_,96,278,46,1700,ink,1);
    // Scroll physical wrapped lines, not logical records: even a single very
    // long diagnostic can be read to its last character.
    reader_visual_lines_.clear();
    for(const auto &s:reader_lines_) {
        const auto lines=wrap(fonts.mono,s,22,960);
        reader_visual_lines_.insert(reader_visual_lines_.end(),lines.begin(),lines.end());
    }
    const std::size_t first=reader_scroll_;
    const std::size_t last=std::min(reader_visual_lines_.size(),first+15);
    const std::size_t shown=last>first?last-first:0;
    /* The card hugs the text it holds; a short report gets a short card. */
    const float body_h=std::max(120.0f,std::min(548.0f,40.0f+static_cast<float>(shown)*30.0f));
    list.bordered_rect({96,318,1024,body_h},14,Color::rgb(0x11141c,0.92f),1.0f,line);
    float y=352;
    for(std::size_t i=first;i<last;++i) {
        hui::ui::text(list,fonts.mono,reader_visual_lines_[i],128,y,22,ink);
        y+=30;
    }
    const std::string position=reader_visual_lines_.empty()?"0 / 0":
        std::to_string(std::min(reader_scroll_+1,reader_visual_lines_.size()))+" / "+std::to_string(reader_visual_lines_.size());
    hui::ui::text(list,fonts.regular,reader_fetch_pending_?"Refreshing...":position,96,318+body_h+38,20,muted);
}

void ToolboxUi::draw_toast(DrawList &list,const Fonts &fonts) const {
    if(overlay_.is_open() || toast_text_.empty() || toast_age_>=7) return;
    /* A neutral strip with one semantic marker, not a coloured band: the bar
     * reports state, and the state's own colour belongs in the dot and the
     * leading rule only. */
    list.rounded_rect({0,935,1920,65},0,Color::rgb(0x11141c,0.96f));
    list.line(0,935,1920,935,1,line);
    const bool bad = toast_text_.find("not responding") != std::string::npos ||
                     toast_text_.find("offline") != std::string::npos;
    list.circle(112,967,5,bad?amber:blue);
    list.line(96,949,96,986,2,bad?amber:blue);
    paragraph(list,fonts.regular,toast_text_,132,961,23,1660,ink,2);
}

void ToolboxUi::draw_hints(DrawList &list,const Fonts &fonts) const {
    const auto style=hui::ui::GlyphStyle::dark();
    if(overlay_.is_open()) return; // The editor owns its complete footer.
    std::array<hui::ui::Hint,5> hints{};int n=0;
    hints[n++]={hui::ui::Button::dpad,focus_==Focus::reader?"Scroll":"Move"};
    if(focus_==Focus::reader) {
        if(reader_refreshable()) hints[n++]={hui::ui::Button::cross,"Refresh"};
        hints[n++]={hui::ui::Button::circle,"Back"};
    } else if(page_menu_) {
        hints[n++]={hui::ui::Button::cross,"Go to workspace"};
        hints[n++]={hui::ui::Button::circle,"Back"};
    } else {
        if(tools_open_ || !task_.buttons.empty()) hints[n++]={hui::ui::Button::cross,"Select"};
        hints[n++]={hui::ui::Button::circle,tools_open_?"Back":"Workspaces"};
        /* Square opens the page's own report. On the funding page that report
         * is the three full strings, which is the option a person needs when a
         * code will not scan; the verb stays the shared one so the footer row
         * keeps meaning the same thing on every workspace. */
        if(!tools_open_) hints[n++]={hui::ui::Button::square,task_.scene==TaskScene::setup?"Review mappings":"Details"};
        hints[n++]={hui::ui::Button::options,"Tools"};
    }
    hui::ui::draw_hints(list,fonts,style,hints.data(),n,1824,true);
}

void ToolboxUi::draw(DrawList &list,const Fonts &fonts,const ServiceSnapshot::View &view) const {
    list.rounded_rect({0,0,1920,1080},0,base);
    /* Spirits first: everything after this point is a surface that should hide
     * them, which is what gives the workspace its depth. */
    ghostscape_.draw(list, (overlay_.is_open() || tools_open_ || page_menu_ || focus_==Focus::reader || beta_dialog_.is_open()) ? 0.35f : 1.0f);
    /* One tone step, inside the same neutral, to separate the readout column
     * from the work area. No hue gradient. */
    list.gradient_rect_h({910,0,1010,935},0,Color::rgb(0x11141c,0.0f),Color::rgb(0x11141c,0.55f));
    // Every content mode reserves y < 190 for the same persistent chrome.
    // Paint it last too: opaque readers/editors cannot erase the brand.
    if(page_menu_) draw_directory(list,fonts);
    else if(tools_open_) draw_tools(list,fonts,view);
    else if(focus_!=Focus::reader) draw_workspace(list,fonts,view);
    draw_reader(list,fonts);
    if(!overlay_.is_open() && (picker_row_ || setup_picker_row_ || chain_.active || setup_save_request_)) {
        list.rounded_rect({0,190,1920,810},0,base);
        eyebrow(list,fonts,section().name,96,220);
        hui::ui::text(list,fonts.display,setup_save_request_?"Saving mappings...":"Loading your choices...",96,360,54,ink);
        paragraph(list,fonts.regular,setup_save_request_?"Wait for the save result before continuing.":"Circle cancels this selection.",96,430,30,1500,muted);
    }
    draw_overlay(list,fonts);
    draw_toast(list,fonts);
    draw_hints(list,fonts);
    list.rounded_rect({0,0,1920,190},0,base);
    const float logo_h=58;
    const float logo_w=logo_h_>0 ? logo_h*static_cast<float>(logo_w_)/static_cast<float>(logo_h_) : 0;
    if(logo_texture_ && logo_h_>0)
        list.image(logo_texture_,{96,54,logo_w,logo_h},hui::gfx::kFullUv,Color::rgb(0xffffff));
    const float name_x=96+std::max(58.0f,logo_w)+24;
    hui::ui::text(list,fonts.semibold,"Ghostcontrol Toolbox",name_x,97,28,ink);
    const bool up=view.link==ServiceSnapshot::Link::up;
    const std::string status=up?"Ghosttooth API connected":view.link==ServiceSnapshot::Link::down?"Ghosttooth API offline":"Checking Ghosttooth API";
    const float status_x=1824-fonts.regular.measure(status,23);
    list.circle(status_x-22,89,5,up?green:amber);
    hui::ui::text(list,fonts.regular,status,status_x,97,23,muted);
    float nav_size=23,total=0;
    for(const auto &s:sections_) total+=fonts.semibold.measure(s.name,nav_size);
    /* A tenth workspace made the row wider than the header: shrink the label
     * size until every page name still fits between the gutters. */
    while(nav_size>14.0f && total+8.0f*(static_cast<float>(sections_.size())-1.0f)>1728.0f) {
        nav_size-=1.0f; total=0;
        for(const auto &s:sections_) total+=fonts.semibold.measure(s.name,nav_size);
    }
    const float gap=std::max(8.0f,(1728-total)/static_cast<float>(sections_.size()-1));
    float x=96;
    for(std::size_t i=0;i<sections_.size();++i) {
        const float w=fonts.semibold.measure(sections_[i].name,nav_size);
        hui::ui::text(list,fonts.semibold,sections_[i].name,x,143,nav_size,i==section_index_?ink:muted);
        if(i==section_index_) list.line(x,151,x+w,151,3,blue);
        x+=w+gap;
    }
    hui::ui::text(list,fonts.mono,std::to_string(section_index_+1)+" / "+std::to_string(sections_.size()),1824,178,19,blue,Align::right);
    /* Beta disclaimer last: above header, nav and every overlay. */
    if (beta_dialog_.visible()) {
        hui::ui::Canvas canvas{list, fonts, 0, clock_};
        beta_dialog_.draw(canvas);
    }
}
} // namespace ghostcontrol
