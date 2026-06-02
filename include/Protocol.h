//
// Created by saeju on 26. 4. 28..
//

#ifndef EDGEON_CLIENT_PROTOCOL_H
#define EDGEON_CLIENT_PROTOCOL_H
#include <QString>

#include <string>
#include <vector>

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
    bool                record_enabled;
};

struct detection_frame_info {
    int     camera_id{0};
    qint64  frame_utc_ms{0};
    qint64  frame_seq{0};
    qint64  capture_utc_ms{0};
    int     frame_width{0};
    int     frame_height{0};
    QString stream_url;
    QString model_name;
    QString model_provider;
    int     model_input_width{0};
    int     model_input_height{0};
    float   confidence_threshold{0.0f};
    float   iou_threshold{0.0f};
    double  inference_ms{0.0};
    int     detection_count{0};
    bool    record_requested{false};
    bool    analysis_enabled{false};
    qint64  record_segment_start_utc_ms{0};
    qint64  record_segment_end_utc_ms{0};
    QString record_segment_file_path;
};

struct detection_result {
    int     camera_id{0};
    qint64  frame_utc_ms{0};
    qint64  frame_seq{0};
    int     det_index{0};
    qint64  stored_utc_ms{0};
    qint64  capture_utc_ms{0};
    int     frame_width{0};
    int     frame_height{0};
    QString stream_url;
    QString model_name;
    QString model_provider;
    int     model_input_width{0};
    int     model_input_height{0};
    float   confidence_threshold{0.0f};
    float   iou_threshold{0.0f};
    double  inference_ms{0.0};
    int     class_id{-1};
    float   score{0.0f};
    float   box_x{0.0f};
    float   box_y{0.0f};
    float   box_width{0.0f};
    float   box_height{0.0f};
    qint64  record_segment_start_utc_ms{0};
    qint64  record_segment_end_utc_ms{0};
    QString record_segment_file_path;
};

#endif //EDGEON_CLIENT_PROTOCOL_H
