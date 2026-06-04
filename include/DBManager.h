//
// Created by saeju on 26. 4. 28..
//

#ifndef EDGEON_CLIENT_DBMANAGER_H
#define EDGEON_CLIENT_DBMANAGER_H

#include <QSqlDatabase>

#include <expected>
#include <vector>

#include "Protocol.h"

class DBManager {
public:
    // 싱글톤 인스턴스 접근자
    static DBManager& instance(const QString& path = {});

    // 복사/이동 금지 (싱글톤)
    DBManager(const DBManager&)            = delete;
    DBManager& operator=(const DBManager&) = delete;

    void initialize();

    std::expected<void, QString> saveDetectionResults(const detection_frame_info& frameInfo,
                                                      const std::vector<detection_result>& detections);
    std::expected<void, QString> purgeDetectionResultsBefore(qint64 cutoffUtcMs);
    std::expected<std::vector<detection_frame_group>, QString> listDetectionFramesByRange(int cameraId,
                                                                                           qint64 startUtcMs,
                                                                                           qint64 endUtcMs);


private:
    explicit DBManager(const QString& path);

    QString db_path;
    QSqlDatabase db; // SQLite

    // 필요한 테이블을 생성 (없으면 CREATE TABLE IF NOT EXISTS)
    std::expected<void, QString> createTables();
    // DB 파일이 이미 존재할 때 내부 스키마/테이블을 검증
    std::expected<void, QString> validateDB();
    std::expected<void, QString> createDetectionTables();
    std::expected<void, QString> validateDetectionTables();
    std::expected<void, QString> purgeExpiredDetectionResults();


// ========================================================================================================
// cameras 테이블 관련 / DBManager_cameras.cpp에 정의
// ========================================================================================================
public:
    // cameras CRUD (SQL injection 방지: 내부에서 prepare/bind 사용)
    std::expected<void, QString> addCamera(const camera_info& camera);
    std::expected<void, QString> updateCameraByCameraId(const camera_info& camera);
    std::expected<camera_info, QString> getCameraByCameraId(int cameraId);
    std::expected<std::vector<camera_info>, QString> listCameras();
    std::expected<void, QString> deleteCameraByCameraId(int cameraId);
private:
    int next_cameras_id = 0;
    int getNextCameraId();
    void setNextCameraId(int id);
    std::expected<void, QString> createCamerasTable();
    std::expected<void, QString> validateCamerasTable();
};



#endif //EDGEON_CLIENT_DBMANAGER_H
