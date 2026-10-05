#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sqlite3.h>

int compareStrings(void*, int leftSize, const void* left, int rightSize, const void* right) {
    const int result = std::memcmp(left, right, std::min(leftSize, rightSize));
    return result != 0 ? result : (leftSize > rightSize) - (leftSize < rightSize);
}

void nativeValue(sqlite3_context* context, int, sqlite3_value**) {
    sqlite3_result_int(context, 42);
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
        database.setDatabaseName(QStringLiteral(":memory:"));
        if (!database.open()) {
            std::cerr << database.lastError().text().toStdString() << '\n';
            return 1;
        }
        QVariant handleValue = database.driver()->handle();
        if (!handleValue.isValid() || std::strcmp(handleValue.typeName(), "sqlite3*") != 0) {
            return 2;
        }
        auto* handle = *static_cast<sqlite3**>(handleValue.data());
        if (!handle || sqlite3_create_collation(handle,
                               "mixxx_sdk_probe",
                               SQLITE_UTF8,
                               nullptr,
                               compareStrings) != SQLITE_OK ||
                sqlite3_create_function(handle,
                        "native_sqlite_value",
                        0,
                        SQLITE_UTF8,
                        nullptr,
                        nativeValue,
                        nullptr,
                        nullptr) != SQLITE_OK) {
            return 3;
        }
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("SELECT native_sqlite_value()")) ||
                !query.next() || query.value(0).toInt() != 42) {
            return 4;
        }
        if (!query.exec(QStringLiteral(
                    "SELECT value FROM (SELECT 'b' AS value UNION ALL SELECT 'a') "
                    "ORDER BY value COLLATE mixxx_sdk_probe")) ||
                !query.next() || query.value(0).toString() != QStringLiteral("a") ||
                !query.next() || query.value(0).toString() != QStringLiteral("b")) {
            return 5;
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(QSqlDatabase::defaultConnection);
    std::cout << "QSQLITE driver handles support the system SQLite API\n";
}
