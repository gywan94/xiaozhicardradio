#pragma once
#include "hls_playlist.h"
#include <cassert>
inline void RadioSelfTest() {
    using namespace radio;
    const std::string base="https://example.test/radio/live/list.m3u8?old=1";
    assert(Resolve(base,"../audio/1.aac")=="https://example.test/radio/audio/1.aac");
    assert(Resolve(base,"/stream/1.ts")=="https://example.test/stream/1.ts");
    assert(Resolve(base,"//cdn.test/a.ts")=="https://cdn.test/a.ts");
    assert(Resolve(base,"?new=2")=="https://example.test/radio/live/list.m3u8?new=2");
    std::vector<Segment> entries;
    assert(Parse(base,"#EXTM3U\n#EXTINF:5,\na.ts\nb.ts\n",entries));
    assert(entries.size()==2 && entries[0].sequence==0 && !entries[0].sequenced);
    assert(Parse(base,"#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:10\na.ts\n#EXT-X-DISCONTINUITY\nb.ts\n",entries));
    assert(entries[0].sequence==10 && !entries[0].discontinuity && entries[1].sequence==11 && entries[1].discontinuity);
    assert(!Parse(base,"#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=key\na.ts\n",entries));
    assert(!Parse(base,"#EXTM3U\n#EXT-X-BYTERANGE:10@0\na.ts\n",entries));
    assert(!Parse(base,"#EXTM3U\n#EXT-X-MAP:URI=init.mp4\na.m4s\n",entries));
}
