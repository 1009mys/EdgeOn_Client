#include "DBManager.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace {

camera_info parseCameraRow(const QSqlQuery& query)
{
    camera_info camera{};
    camera.id = query.value(0).toInt();
    camera.camera_id = query.value(1).toInt();
    camera.name = query.value(2).toString().toStdString();
    camera.stream_type = static_cast<StreamType>(query.value(3).toInt());
    camera.url = query.value(4).toString().toStdString();
    camera.enabled = query.value(5).toInt() != 0;
    camera.record_enabled = query.value(6).toInt() != 0;
    return camera;
}

}

int DBManager::getNextCameraId()
{
    return next_cameras_id;
}

void DBManager::setNextCameraId(int id)
{
    next_cameras_id = id;
}

std::expected<void, QString> DBManager::createCamerasTable()
{
    QSqlQuery query(db);

    const QString createCameraTable = R"(
        CREATE TABLE IF NOT EXISTS cameras (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            camera_id   INTEGER NOT NULL UNIQUE,
            name        TEXT    NOT NULL,
            stream_type INTEGER NOT NULL DEFAULT 0,
            url         TEXT    NOT NULL,
            enabled     INTEGER NOT NULL DEFAULT 1,
            record_enabled INTEGER NOT NULL DEFAULT 0
        )
    )";

    if (!query.exec(createCameraTable)) {
        return std::unexpected(query.lastError().text());
    }

    // 신규 DB는 camera_id를 1부터 사용
    setNextCameraId(1);
    return {};
}

std::expected<void, QString> DBManager::validateCamerasTable()
{
    QSqlQuery query(db);

    if (!query.exec("SELECT name FROM sqlite_master WHERE type='table' AND name='cameras'")) {
        return std::unexpected(query.lastError().text());
    }
    if (!query.next()) {
        return std::unexpected(QString("cameras 테이블이 존재하지 않습니다."));
    }

    const QStringList requiredColumns = {"id", "camera_id", "name", "stream_type", "url", "enabled", "record_enabled"};

    if (!query.exec("PRAGMA table_info(cameras)")) {
        return std::unexpected(query.lastError().text());
    }

    QStringList existingColumns;
    while (query.next()) {
        existingColumns << query.value(1).toString();
    }

    for (const QString& col : requiredColumns) {
        if (!existingColumns.contains(col)) {
            return std::unexpected(QString("cameras 테이블에 필수 컬럼 '%1' 이(가) 없습니다.").arg(col));
        }
    }

    // 기존 DB의 다음 camera_id를 계산
    if (!query.exec("SELECT COALESCE(MAX(camera_id), 0) + 1 FROM cameras")) {
        return std::unexpected(query.lastError().text());
    }
    if (query.next()) {
        setNextCameraId(query.value(0).toInt());
    } else {
        setNextCameraId(1);
    }

    return {};
}

std::expected<void, QString> DBManager::createDetectionTables()
{
    QSqlQuery query(db);

    const QString createFramesTable = R"(
        CREATE TABLE IF NOT EXISTS detection_frames (
            camera_id                   INTEGER NOT NULL,
            frame_utc_ms                INTEGER NOT NULL,
            frame_seq                   INTEGER NOT NULL,
            capture_utc_ms              INTEGER NOT NULL,
            stored_utc_ms               INTEGER NOT NULL,
            frame_width                 INTEGER NOT NULL,
            frame_height                INTEGER NOT NULL,
            stream_url                  TEXT    NOT NULL,
            model_name                  TEXT    NOT NULL,
            model_provider              TEXT    NOT NULL,
            model_input_width           INTEGER NOT NULL,
            model_input_height          INTEGER NOT NULL,
            confidence_threshold        REAL    NOT NULL,
            iou_threshold               REAL    NOT NULL,
            inference_ms                REAL    NOT NULL,
            detection_count             INTEGER NOT NULL,
            record_requested            INTEGER NOT NULL DEFAULT 0,
            analysis_enabled            INTEGER NOT NULL DEFAULT 0,
            record_segment_start_utc_ms INTEGER NOT NULL DEFAULT 0,
            record_segment_end_utc_ms   INTEGER NOT NULL DEFAULT 0,
            record_segment_file_path    TEXT    NOT NULL DEFAULT '',
            PRIMARY KEY (camera_id, frame_utc_ms, frame_seq)
        ) WITHOUT ROWID
    )";

    const QString createDetectionsTable = R"(
        CREATE TABLE IF NOT EXISTS detections (
            camera_id                   INTEGER NOT NULL,
            frame_utc_ms                INTEGER NOT NULL,
            frame_seq                   INTEGER NOT NULL,
            det_index                   INTEGER NOT NULL,
            stored_utc_ms               INTEGER NOT NULL,
            capture_utc_ms              INTEGER NOT NULL,
            frame_width                 INTEGER NOT NULL,
            frame_height                INTEGER NOT NULL,
            stream_url                  TEXT    NOT NULL,
            model_name                  TEXT    NOT NULL,
            model_provider              TEXT    NOT NULL,
            model_input_width           INTEGER NOT NULL,
            model_input_height          INTEGER NOT NULL,
            confidence_threshold        REAL    NOT NULL,
            iou_threshold               REAL    NOT NULL,
            inference_ms                REAL    NOT NULL,
            class_id                    INTEGER NOT NULL,
            score                       REAL    NOT NULL,
            box_x                       REAL    NOT NULL,
            box_y                       REAL    NOT NULL,
            box_width                   REAL    NOT NULL,
            box_height                  REAL    NOT NULL,
            record_segment_start_utc_ms INTEGER NOT NULL DEFAULT 0,
            record_segment_end_utc_ms   INTEGER NOT NULL DEFAULT 0,
            record_segment_file_path    TEXT    NOT NULL DEFAULT '',
            PRIMARY KEY (camera_id, frame_utc_ms, frame_seq, det_index),
            FOREIGN KEY (camera_id, frame_utc_ms, frame_seq)
                REFERENCES detection_frames (camera_id, frame_utc_ms, frame_seq)
                ON DELETE CASCADE
        ) WITHOUT ROWID
    )";

    const QString createView = R"(
        CREATE VIEW IF NOT EXISTS v_detection_results AS
        SELECT
            f.camera_id,
            f.frame_utc_ms,
            f.frame_seq,
            f.capture_utc_ms,
            f.stored_utc_ms,
            f.frame_width,
            f.frame_height,
            f.stream_url,
            f.model_name,
            f.model_provider,
            f.model_input_width,
            f.model_input_height,
            f.confidence_threshold,
            f.iou_threshold,
            f.inference_ms,
            f.detection_count,
            f.record_requested,
            f.analysis_enabled,
            f.record_segment_start_utc_ms,
            f.record_segment_end_utc_ms,
            f.record_segment_file_path,
            d.det_index,
            d.class_id,
            d.score,
            d.box_x,
            d.box_y,
            d.box_width,
            d.box_height
        FROM detection_frames AS f
        LEFT JOIN detections AS d
          ON d.camera_id = f.camera_id
         AND d.frame_utc_ms = f.frame_utc_ms
         AND d.frame_seq = f.frame_seq
    )";

    if (!query.exec(createFramesTable)) {
        return std::unexpected(query.lastError().text());
    }
    if (!query.exec(createDetectionsTable)) {
        return std::unexpected(query.lastError().text());
    }
    if (!query.exec(createView)) {
        return std::unexpected(query.lastError().text());
    }

    return {};
}

std::expected<void, QString> DBManager::validateDetectionTables()
{
    QSqlQuery query(db);

    if (!query.exec("SELECT name FROM sqlite_master WHERE type='table' AND name='detection_frames'")) {
        return std::unexpected(query.lastError().text());
    }
    if (!query.next()) {
        return std::unexpected(QString("detection_frames 테이블이 존재하지 않습니다."));
    }

    const QStringList frameColumns = {
        "camera_id", "frame_utc_ms", "frame_seq", "capture_utc_ms", "stored_utc_ms",
        "frame_width", "frame_height", "stream_url", "model_name", "model_provider",
        "model_input_width", "model_input_height", "confidence_threshold", "iou_threshold",
        "inference_ms", "detection_count", "record_requested", "analysis_enabled",
        "record_segment_start_utc_ms", "record_segment_end_utc_ms", "record_segment_file_path"
    };

    if (!query.exec("PRAGMA table_info(detection_frames)")) {
        return std::unexpected(query.lastError().text());
    }

    QStringList existingFrameColumns;
    while (query.next()) {
        existingFrameColumns << query.value(1).toString();
    }
    for (const QString& col : frameColumns) {
        if (!existingFrameColumns.contains(col)) {
            return std::unexpected(QString("detection_frames 테이블에 필수 컬럼 '%1' 이(가) 없습니다.").arg(col));
        }
    }

    if (!query.exec("SELECT name FROM sqlite_master WHERE type='table' AND name='detections'")) {
        return std::unexpected(query.lastError().text());
    }
    if (!query.next()) {
        return std::unexpected(QString("detections 테이블이 존재하지 않습니다."));
    }

    const QStringList detectionColumns = {
        "camera_id", "frame_utc_ms", "frame_seq", "det_index", "stored_utc_ms", "capture_utc_ms",
        "frame_width", "frame_height", "stream_url", "model_name", "model_provider",
        "model_input_width", "model_input_height", "confidence_threshold", "iou_threshold",
        "inference_ms", "class_id", "score", "box_x", "box_y", "box_width", "box_height",
        "record_segment_start_utc_ms", "record_segment_end_utc_ms", "record_segment_file_path"
    };

    if (!query.exec("PRAGMA table_info(detections)")) {
        return std::unexpected(query.lastError().text());
    }

    QStringList existingDetectionColumns;
    while (query.next()) {
        existingDetectionColumns << query.value(1).toString();
    }
    for (const QString& col : detectionColumns) {
        if (!existingDetectionColumns.contains(col)) {
            return std::unexpected(QString("detections 테이블에 필수 컬럼 '%1' 이(가) 없습니다.").arg(col));
        }
    }

    if (!query.exec("SELECT name FROM sqlite_master WHERE type='view' AND name='v_detection_results'")) {
        return std::unexpected(query.lastError().text());
    }
    if (!query.next()) {
        return std::unexpected(QString("v_detection_results 뷰가 존재하지 않습니다."));
    }

    return {};
}

std::expected<void, QString> DBManager::saveDetectionResults(const detection_frame_info& frameInfo,
                                                             const std::vector<detection_result>& detections)
{
    if (frameInfo.camera_id <= 0) {
        return std::unexpected(QString("camera_id는 1 이상이어야 합니다."));
    }
    if (frameInfo.frame_utc_ms <= 0 || frameInfo.frame_seq < 0) {
        return std::unexpected(QString("frame_utc_ms/frame_seq가 올바르지 않습니다."));
    }

    const qint64 storedUtcMs = QDateTime::currentMSecsSinceEpoch();

    if (!db.transaction()) {
        return std::unexpected(db.lastError().text());
    }

    auto rollbackOnError = [this]() {
        if (db.isOpen()) {
            db.rollback();
        }
    };

    QSqlQuery frameQuery(db);
    if (!frameQuery.prepare(R"(
        INSERT OR REPLACE INTO detection_frames (
            camera_id, frame_utc_ms, frame_seq, capture_utc_ms, stored_utc_ms,
            frame_width, frame_height, stream_url, model_name, model_provider,
            model_input_width, model_input_height, confidence_threshold, iou_threshold,
            inference_ms, detection_count, record_requested, analysis_enabled,
            record_segment_start_utc_ms, record_segment_end_utc_ms, record_segment_file_path
        ) VALUES (
            :camera_id, :frame_utc_ms, :frame_seq, :capture_utc_ms, :stored_utc_ms,
            :frame_width, :frame_height, :stream_url, :model_name, :model_provider,
            :model_input_width, :model_input_height, :confidence_threshold, :iou_threshold,
            :inference_ms, :detection_count, :record_requested, :analysis_enabled,
            :record_segment_start_utc_ms, :record_segment_end_utc_ms, :record_segment_file_path
        )
    )")) {
        rollbackOnError();
        return std::unexpected(frameQuery.lastError().text());
    }

    frameQuery.bindValue(":camera_id", frameInfo.camera_id);
    frameQuery.bindValue(":frame_utc_ms", frameInfo.frame_utc_ms);
    frameQuery.bindValue(":frame_seq", static_cast<qint64>(frameInfo.frame_seq));
    frameQuery.bindValue(":capture_utc_ms", frameInfo.capture_utc_ms > 0 ? frameInfo.capture_utc_ms : frameInfo.frame_utc_ms);
    frameQuery.bindValue(":stored_utc_ms", storedUtcMs);
    frameQuery.bindValue(":frame_width", frameInfo.frame_width);
    frameQuery.bindValue(":frame_height", frameInfo.frame_height);
    frameQuery.bindValue(":stream_url", frameInfo.stream_url);
    frameQuery.bindValue(":model_name", frameInfo.model_name);
    frameQuery.bindValue(":model_provider", frameInfo.model_provider);
    frameQuery.bindValue(":model_input_width", frameInfo.model_input_width);
    frameQuery.bindValue(":model_input_height", frameInfo.model_input_height);
    frameQuery.bindValue(":confidence_threshold", frameInfo.confidence_threshold);
    frameQuery.bindValue(":iou_threshold", frameInfo.iou_threshold);
    frameQuery.bindValue(":inference_ms", frameInfo.inference_ms);
    frameQuery.bindValue(":detection_count", frameInfo.detection_count);
    frameQuery.bindValue(":record_requested", frameInfo.record_requested ? 1 : 0);
    frameQuery.bindValue(":analysis_enabled", frameInfo.analysis_enabled ? 1 : 0);
    frameQuery.bindValue(":record_segment_start_utc_ms", frameInfo.record_segment_start_utc_ms);
    frameQuery.bindValue(":record_segment_end_utc_ms", frameInfo.record_segment_end_utc_ms);
    frameQuery.bindValue(":record_segment_file_path", frameInfo.record_segment_file_path);

    if (!frameQuery.exec()) {
        rollbackOnError();
        return std::unexpected(frameQuery.lastError().text());
    }

    if (!detections.empty()) {
        QSqlQuery detectionQuery(db);
        if (!detectionQuery.prepare(R"(
            INSERT OR REPLACE INTO detections (
                camera_id, frame_utc_ms, frame_seq, det_index, stored_utc_ms, capture_utc_ms,
                frame_width, frame_height, stream_url, model_name, model_provider,
                model_input_width, model_input_height, confidence_threshold, iou_threshold,
                inference_ms, class_id, score, box_x, box_y, box_width, box_height,
                record_segment_start_utc_ms, record_segment_end_utc_ms, record_segment_file_path
            ) VALUES (
                :camera_id, :frame_utc_ms, :frame_seq, :det_index, :stored_utc_ms, :capture_utc_ms,
                :frame_width, :frame_height, :stream_url, :model_name, :model_provider,
                :model_input_width, :model_input_height, :confidence_threshold, :iou_threshold,
                :inference_ms, :class_id, :score, :box_x, :box_y, :box_width, :box_height,
                :record_segment_start_utc_ms, :record_segment_end_utc_ms, :record_segment_file_path
            )
        )")) {
            rollbackOnError();
            return std::unexpected(detectionQuery.lastError().text());
        }

        for (const auto& det : detections) {
            detectionQuery.bindValue(":camera_id", det.camera_id > 0 ? det.camera_id : frameInfo.camera_id);
            detectionQuery.bindValue(":frame_utc_ms", det.frame_utc_ms > 0 ? det.frame_utc_ms : frameInfo.frame_utc_ms);
            detectionQuery.bindValue(":frame_seq", static_cast<qint64>(det.frame_seq >= 0 ? det.frame_seq : frameInfo.frame_seq));
            detectionQuery.bindValue(":det_index", det.det_index);
            detectionQuery.bindValue(":stored_utc_ms", det.stored_utc_ms > 0 ? det.stored_utc_ms : storedUtcMs);
            detectionQuery.bindValue(":capture_utc_ms", det.capture_utc_ms > 0 ? det.capture_utc_ms : frameInfo.capture_utc_ms);
            detectionQuery.bindValue(":frame_width", det.frame_width > 0 ? det.frame_width : frameInfo.frame_width);
            detectionQuery.bindValue(":frame_height", det.frame_height > 0 ? det.frame_height : frameInfo.frame_height);
            detectionQuery.bindValue(":stream_url", det.stream_url.isEmpty() ? frameInfo.stream_url : det.stream_url);
            detectionQuery.bindValue(":model_name", det.model_name.isEmpty() ? frameInfo.model_name : det.model_name);
            detectionQuery.bindValue(":model_provider", det.model_provider.isEmpty() ? frameInfo.model_provider : det.model_provider);
            detectionQuery.bindValue(":model_input_width", det.model_input_width > 0 ? det.model_input_width : frameInfo.model_input_width);
            detectionQuery.bindValue(":model_input_height", det.model_input_height > 0 ? det.model_input_height : frameInfo.model_input_height);
            detectionQuery.bindValue(":confidence_threshold", det.confidence_threshold > 0.0f ? det.confidence_threshold : frameInfo.confidence_threshold);
            detectionQuery.bindValue(":iou_threshold", det.iou_threshold > 0.0f ? det.iou_threshold : frameInfo.iou_threshold);
            detectionQuery.bindValue(":inference_ms", det.inference_ms > 0.0 ? det.inference_ms : frameInfo.inference_ms);
            detectionQuery.bindValue(":class_id", det.class_id);
            detectionQuery.bindValue(":score", det.score);
            detectionQuery.bindValue(":box_x", det.box_x);
            detectionQuery.bindValue(":box_y", det.box_y);
            detectionQuery.bindValue(":box_width", det.box_width);
            detectionQuery.bindValue(":box_height", det.box_height);
            detectionQuery.bindValue(":record_segment_start_utc_ms",
                                     det.record_segment_start_utc_ms > 0 ? det.record_segment_start_utc_ms
                                                                        : frameInfo.record_segment_start_utc_ms);
            detectionQuery.bindValue(":record_segment_end_utc_ms",
                                     det.record_segment_end_utc_ms > 0 ? det.record_segment_end_utc_ms
                                                                       : frameInfo.record_segment_end_utc_ms);
            detectionQuery.bindValue(":record_segment_file_path",
                                     det.record_segment_file_path.isEmpty() ? frameInfo.record_segment_file_path
                                                                            : det.record_segment_file_path);

            if (!detectionQuery.exec()) {
                rollbackOnError();
                return std::unexpected(detectionQuery.lastError().text());
            }
        }
    }

    if (!db.commit()) {
        rollbackOnError();
        return std::unexpected(db.lastError().text());
    }

    return {};
}

std::expected<void, QString> DBManager::purgeDetectionResultsBefore(qint64 cutoffUtcMs)
{
    if (cutoffUtcMs <= 0) {
        return {};
    }

    if (!db.transaction()) {
        return std::unexpected(db.lastError().text());
    }

    QSqlQuery query(db);
    if (!query.prepare("DELETE FROM detection_frames WHERE frame_utc_ms < :cutoff")) {
        db.rollback();
        return std::unexpected(query.lastError().text());
    }

    query.bindValue(":cutoff", cutoffUtcMs);
    if (!query.exec()) {
        db.rollback();
        return std::unexpected(query.lastError().text());
    }

    if (!db.commit()) {
        db.rollback();
        return std::unexpected(db.lastError().text());
    }

    return {};
}

std::expected<void, QString> DBManager::addCamera(const camera_info& camera)
{
    QSqlQuery query(db);
    if (!query.prepare(R"(
        INSERT INTO cameras (camera_id, name, stream_type, url, enabled, record_enabled)
        VALUES (:camera_id, :name, :stream_type, :url, :enabled, :record_enabled)
    )")) {
        return std::unexpected(query.lastError().text());
    }

    int cameraId = camera.camera_id;
    if (cameraId <= 0) {
        cameraId = getNextCameraId();
    }

    query.bindValue(":camera_id", cameraId);
    query.bindValue(":name", QString::fromStdString(camera.name));
    query.bindValue(":stream_type", static_cast<int>(camera.stream_type));
    query.bindValue(":url", QString::fromStdString(camera.url));
    query.bindValue(":enabled", camera.enabled ? 1 : 0);
    query.bindValue(":record_enabled", camera.record_enabled ? 1 : 0);

    if (!query.exec()) {
        return std::unexpected(query.lastError().text());
    }

    if (cameraId >= getNextCameraId()) {
        setNextCameraId(cameraId + 1);
    }

    return {};
}

std::expected<void, QString> DBManager::updateCameraByCameraId(const camera_info& camera)
{
    if (camera.camera_id <= 0) {
        return std::unexpected(QString("camera_id는 1 이상이어야 합니다."));
    }

    QSqlQuery query(db);
    if (!query.prepare(R"(
        UPDATE cameras
        SET name = :name,
            stream_type = :stream_type,
            url = :url,
            enabled = :enabled,
            record_enabled = :record_enabled
        WHERE camera_id = :camera_id
    )")) {
        return std::unexpected(query.lastError().text());
    }

    query.bindValue(":name", QString::fromStdString(camera.name));
    query.bindValue(":stream_type", static_cast<int>(camera.stream_type));
    query.bindValue(":url", QString::fromStdString(camera.url));
    query.bindValue(":enabled", camera.enabled ? 1 : 0);
    query.bindValue(":record_enabled", camera.record_enabled ? 1 : 0);
    query.bindValue(":camera_id", camera.camera_id);

    if (!query.exec()) {
        return std::unexpected(query.lastError().text());
    }

    if (query.numRowsAffected() == 0) {
        return std::unexpected(QString("수정할 camera_id=%1 데이터가 없습니다.").arg(camera.camera_id));
    }

    return {};
}

std::expected<camera_info, QString> DBManager::getCameraByCameraId(int cameraId)
{
    if (cameraId <= 0) {
        return std::unexpected(QString("camera_id는 1 이상이어야 합니다."));
    }

    QSqlQuery query(db);
    if (!query.prepare(R"(
        SELECT id, camera_id, name, stream_type, url, enabled, record_enabled
        FROM cameras
        WHERE camera_id = :camera_id
    )")) {
        return std::unexpected(query.lastError().text());
    }

    query.bindValue(":camera_id", cameraId);
    if (!query.exec()) {
        return std::unexpected(query.lastError().text());
    }

    if (!query.next()) {
        return std::unexpected(QString("camera_id=%1 데이터가 없습니다.").arg(cameraId));
    }

    return parseCameraRow(query);
}

std::expected<std::vector<camera_info>, QString> DBManager::listCameras()
{
    QSqlQuery query(db);
    if (!query.prepare(R"(
        SELECT id, camera_id, name, stream_type, url, enabled, record_enabled
        FROM cameras
        ORDER BY id ASC
    )")) {
        return std::unexpected(query.lastError().text());
    }

    if (!query.exec()) {
        return std::unexpected(query.lastError().text());
    }

    std::vector<camera_info> cameras;
    while (query.next()) {
        cameras.emplace_back(parseCameraRow(query));
    }

    return cameras;
}

std::expected<void, QString> DBManager::deleteCameraByCameraId(int cameraId)
{
    if (cameraId <= 0) {
        return std::unexpected(QString("camera_id는 1 이상이어야 합니다."));
    }

    QSqlQuery query(db);
    if (!query.prepare("DELETE FROM cameras WHERE camera_id = :camera_id")) {
        return std::unexpected(query.lastError().text());
    }

    query.bindValue(":camera_id", cameraId);
    if (!query.exec()) {
        return std::unexpected(query.lastError().text());
    }

    if (query.numRowsAffected() == 0) {
        return std::unexpected(QString("삭제할 camera_id=%1 데이터가 없습니다.").arg(cameraId));
    }

    return {};
}
