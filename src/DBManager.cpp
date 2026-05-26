//
// Created by saeju on 26. 4. 28..
//

#include "DBManager.h"

#include <QDebug>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>

DBManager& DBManager::instance(const QString& path)
{
    static DBManager inst(path);
    return inst;
}

DBManager::DBManager(const QString& path)
{
    db_path = path;
}

void DBManager::initialize()
{
    const bool fileExisted = QFile::exists(db_path);

    // DB 커넥션 등록 (아직 open하지 않음)
    db = QSqlDatabase::addDatabase("QSQLITE", "edgeon_connection");
    db.setDatabaseName(db_path);

    if (!db.open()) {
        qCritical() << "DB 파일을 열 수 없습니다:" << db.lastError().text();
        return;
    }

    if (fileExisted) {
        // 파일이 이미 있는 경우 → 내부 스키마 검증
        if (const auto result = validateDB(); !result) {
            qCritical() << "DB 검증 실패:" << result.error();
            return;
        }
        qDebug() << "기존 DB 검증 성공!";
    } else {
        // 새 파일 → 테이블 생성
        if (const auto result = createTables(); !result) {
            qCritical() << "테이블 생성 실패:" << result.error();
            return;
        }
        qDebug() << "DB 초기화(신규) 성공!";
    }
}


std::expected<void, QString> DBManager::createTables()
{
    if (const auto result = createCamerasTable(); !result) {
        return std::unexpected(result.error());
    }

    return {};
}

std::expected<void, QString> DBManager::validateDB()
{
    if (const auto result = validateCamerasTable(); !result) {
        return std::unexpected(result.error());
    }

    return {};
}