//
// Created by saeju on 26. 4. 28..
//

#ifndef EDGEON_CLIENT_PROTOCOL_H
#define EDGEON_CLIENT_PROTOCOL_H
#include <string>

enum class StreamType {
    RTSP,
    ONVIF
};

struct camera_info {
    int                 id; // DB id
    int                 camera_id; // 사용자 조회 id
    std::string         name; // 이름
    StreamType          stream_type;
    std::string         url; // 스트림 주소
    bool                enabled;
};

#endif //EDGEON_CLIENT_PROTOCOL_H
