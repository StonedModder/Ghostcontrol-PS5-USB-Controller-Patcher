/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "task_capture.hpp"
#include "app_log.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace ghostcontrol {
namespace {
void be32(std::vector<unsigned char> &out,std::uint32_t n) {
    for(int shift=24;shift>=0;shift-=8) out.push_back(static_cast<unsigned char>(n>>shift));
}
std::uint32_t crc(const unsigned char *p,std::size_t n) {
    std::uint32_t c=0xffffffffu;
    for(std::size_t i=0;i<n;++i) {
        c^=p[i];for(int b=0;b<8;++b)c=(c>>1)^(0xedb88320u & (0u-(c&1u)));
    }
    return c^0xffffffffu;
}
void chunk(std::vector<unsigned char> &png,const char *name,const std::vector<unsigned char> &data) {
    be32(png,static_cast<std::uint32_t>(data.size()));const auto start=png.size();
    png.insert(png.end(),name,name+4);png.insert(png.end(),data.begin(),data.end());
    be32(png,crc(png.data()+start,data.size()+4));
}
bool save_png(const char *path,const std::vector<unsigned char> &rgba,int width,int height) {
    std::vector<unsigned char> raw;
    for(int y=height-1;y>=0;--y) {
        raw.push_back(0);
        auto start=rgba.begin()+static_cast<std::ptrdiff_t>(y)*width*4;
        raw.insert(raw.end(),start,start+width*4);
    }
    std::vector<unsigned char> compressed{0x78,0x01};
    for(std::size_t i=0;i<raw.size();) {
        const auto count=static_cast<std::uint16_t>(std::min<std::size_t>(65535,raw.size()-i));
        const auto inverted=static_cast<std::uint16_t>(~count);
        compressed.push_back(i+count==raw.size()?1:0);
        compressed.push_back(static_cast<unsigned char>(count));compressed.push_back(static_cast<unsigned char>(count>>8));
        compressed.push_back(static_cast<unsigned char>(inverted));compressed.push_back(static_cast<unsigned char>(inverted>>8));
        compressed.insert(compressed.end(),raw.begin()+static_cast<std::ptrdiff_t>(i),raw.begin()+static_cast<std::ptrdiff_t>(i+count));i+=count;
    }
    std::uint32_t a=1,b=0;
    for(auto value:raw){a=(a+value)%65521;b=(b+a)%65521;}be32(compressed,(b<<16)|a);
    std::vector<unsigned char> png{137,80,78,71,13,10,26,10},header;
    be32(header,static_cast<std::uint32_t>(width));be32(header,static_cast<std::uint32_t>(height));
    header.insert(header.end(),{8,6,0,0,0});chunk(png,"IHDR",header);chunk(png,"IDAT",compressed);chunk(png,"IEND",{});
    auto *f=std::fopen(path,"wb");if(!f)return false;
    fchmod(fileno(f),0666);
    const bool written=std::fwrite(png.data(),1,png.size(),f)==png.size();
    const bool closed=std::fclose(f)==0;return written&&closed;
}
} // namespace
void capture_task_frame(hui::gfx::Renderer &renderer) {
    static unsigned calls=0;
    if(++calls%4 || unlink("/app0/devloop-shot")!=0)return;
    constexpr int width=960,height=540;
    hui::gfx::Canvas canvas;
    if(!canvas.create(width,height,1)){app_log("task capture: framebuffer unavailable");return;}
    renderer.present(canvas.framebuffer(),width,height);
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width)*height*4);
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    const auto error=glGetError();
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    if(error){app_log("task capture: GL error %u",error);return;}
    mkdir("/app0/screenshots",0777);chmod("/app0/screenshots",0777);
    const bool saved=save_png("/app0/screenshots/workspace.png",rgba,width,height);
    app_log("task capture: own frame saved=%d",saved?1:0);
}
} // namespace ghostcontrol
