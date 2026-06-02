//
// Created by saeju on 26. 4. 28..
//

#include "DBManager.h"

#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>

namespace {

void configureSqliteConnection(QSqlDatabase& db)
{
    if (!db.isOpen()) {
        return;
    }

    QSqlQuery query(db);
    query.exec("PRAGMA foreign_keys = ON");
    query.exec("PRAGMA journal_mode = WAL");
    query.exec("PRAGMA synchronous = NORMAL");
    query.exec("PRAGMA temp_store = MEMORY");
    query.exec("PRAGMA cache_size = -20000");
}

} // namespace

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

    configureSqliteConnection(db);

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

    if (const auto purgeResult = purgeExpiredDetectionResults(); !purgeResult) {
        qWarning() << "감지 결과 보존 정리 실패:" << purgeResult.error();
    }
}


std::expected<void, QString> DBManager::createTables()
{
    if (const auto result = createCamerasTable(); !result) {
        return std::unexpected(result.error());
    }

    if (const auto result = createDetectionTables(); !result) {
        return std::unexpected(result.error());
    }

    return {};
}

std::expected<void, QString> DBManager::validateDB()
{
    if (const auto result = validateCamerasTable(); !result) {
        return std::unexpected(result.error());
    }

    if (const auto result = validateDetectionTables(); !result) {
        return std::unexpected(result.error());
    }

    return {};
}

std::expected<void, QString> DBManager::purgeExpiredDetectionResults()
{
    QSettings settings;
    const int retentionDays = settings.value("detection/retentionDays", 30).toInt();
    if (retentionDays <= 0) {
        return {};
    }

    const qint64 cutoffUtcMs = QDateTime::currentMSecsSinceEpoch()
        - static_cast<qint64>(retentionDays) * 24LL * 60LL * 60LL * 1000LL;
    return purgeDetectionResultsBefore(cutoffUtcMs);
}

