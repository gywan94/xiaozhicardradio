#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <cstdlib>
namespace radio {
struct Segment { uint64_t sequence; std::string url; bool discontinuity=false; bool sequenced=true; };
inline std::string Resolve(const std::string& base,const std::string& uri) {
    if(uri.rfind("http://",0)==0||uri.rfind("https://",0)==0)return uri;
    auto scheme=base.find("://");if(scheme==std::string::npos)return {};
    if(uri.rfind("//",0)==0)return base.substr(0,scheme+1)+uri;
    auto host_end=base.find_first_of("/?#",scheme+3);
    auto origin=host_end==std::string::npos?base:base.substr(0,host_end);
    auto clean=base.substr(0,base.find_first_of("?#"));
    if(!uri.empty()&&uri[0]=='?')return clean+uri;
    std::string path;
    if(!uri.empty()&&uri[0]=='/')path=uri;
    else {
        auto oldpath=clean.substr(origin.size());
        auto slash=oldpath.rfind('/');
        path=(slash==std::string::npos?"/":oldpath.substr(0,slash+1))+uri;
    }
    auto suffix_pos=path.find_first_of("?#");
    auto suffix=suffix_pos==std::string::npos?"":path.substr(suffix_pos);
    auto raw=path.substr(0,suffix_pos);std::istringstream parts(raw);std::string part;
    std::vector<std::string> normalized;
    while(std::getline(parts,part,'/')) {
        if(part.empty()||part==".")continue;
        if(part==".."){if(!normalized.empty())normalized.pop_back();}
        else normalized.push_back(part);
    }
    std::string result=origin;
    for(const auto& item:normalized)result+="/"+item;
    if(normalized.empty()||(!raw.empty()&&raw.back()=='/'))result+="/";
    return result+suffix;
}
inline bool Parse(const std::string& base,const std::string& text,std::vector<Segment>& out) {
    out.clear();if(text.rfind("#EXTM3U",0)!=0)return false;
    std::istringstream lines(text);std::string line;uint64_t sequence=0;bool have_sequence=false,discontinuity=false;
    while(std::getline(lines,line)) {
        while(!line.empty()&&(line.back()=='\r'||line.back()==' '||line.back()=='\t'))line.pop_back();
        if(line=="#EXT-X-DISCONTINUITY")discontinuity=true;
        if(line.rfind("#EXT-X-MAP:",0)==0||line.rfind("#EXT-X-BYTERANGE:",0)==0)return false;
        if(line.rfind("#EXT-X-STREAM-INF",0)==0)return false;
        if(line.rfind("#EXT-X-KEY:",0)==0&&line.find("METHOD=NONE")==std::string::npos)return false;
        if(line.rfind("#EXT-X-MEDIA-SEQUENCE:",0)==0) {
            char* end=nullptr;sequence=strtoull(line.c_str()+22,&end,10);
            if(end==line.c_str()+22||*end)return false;
            have_sequence=true;
        }
        if(!line.empty()&&line[0]!='#') {
            if(out.size()>=64)return false;
            auto url=Resolve(base,line);if(url.empty())return false;out.push_back({sequence++,url,discontinuity,have_sequence});discontinuity=false;
        }
    }
    return !out.empty();
}
}
