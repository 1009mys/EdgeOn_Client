#include "DBManager.h"

#include <QSqlError>
#include <QSqlQuery>
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
