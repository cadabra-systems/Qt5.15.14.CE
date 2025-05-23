/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtSql module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qsql_mysql_p.h"

#include <qcoreapplication.h>
#include <qvariant.h>
#include <qdatetime.h>
#include <qsqlerror.h>
#include <qsqlfield.h>
#include <qsqlindex.h>
#include <qsqlquery.h>
#include <qsqlrecord.h>
#include <qstringlist.h>
#if QT_CONFIG(textcodec)
#include <qtextcodec.h>
#endif
#include <qvector.h>
#include <qfile.h>
#include <qdebug.h>
#include <QtSql/private/qsqldriver_p.h>
#include <QtSql/private/qsqlresult_p.h>

#ifdef Q_OS_WIN32
// comment the next line out if you want to use MySQL/embedded on Win32 systems.
// note that it will crash if you don't statically link to the mysql/e library!
# define Q_NO_MYSQL_EMBEDDED
#endif

Q_DECLARE_METATYPE(MYSQL_RES*)
Q_DECLARE_METATYPE(MYSQL*)
Q_DECLARE_METATYPE(MYSQL_STMT*)

// MySQL above version 8 removed my_bool typedef while MariaDB kept it,
// by redefining it we can regain source compatibility.
using my_bool = decltype(mysql_stmt_bind_result(nullptr, nullptr));

QT_BEGIN_NAMESPACE

class QMYSQLDriverPrivate : public QSqlDriverPrivate
{
    Q_DECLARE_PUBLIC(QMYSQLDriver)

public:
    QMYSQLDriverPrivate() : QSqlDriverPrivate(QSqlDriver::MySqlServer)
#if QT_CONFIG(textcodec)
        , tc(QTextCodec::codecForLocale())
#endif
    {}
    MYSQL *mysql = nullptr;
    QTextCodec *tc = nullptr;
    bool preparedQuerysEnabled = false;
};

static inline QString toUnicode(QTextCodec *tc, const char *str)
{
#if !QT_CONFIG(textcodec)
    Q_UNUSED(tc);
    return QString::fromLatin1(str);
#else
    return tc->toUnicode(str);
#endif
}

static inline QString toUnicode(QTextCodec *tc, const char *str, int length)
{
#if !QT_CONFIG(textcodec)
    Q_UNUSED(tc);
    return QString::fromLatin1(str, length);
#else
    return tc->toUnicode(str, length);
#endif
}

static inline QByteArray fromUnicode(QTextCodec *tc, const QString &str)
{
#if !QT_CONFIG(textcodec)
    Q_UNUSED(tc);
    return str.toLatin1();
#else
    return tc->fromUnicode(str);
#endif
}

static inline QVariant qDateFromString(const QString &val)
{
#if !QT_CONFIG(datestring)
    Q_UNUSED(val);
    return QVariant(val);
#else
    if (val.isEmpty())
        return QVariant(QDate());
    return QVariant(QDate::fromString(val, Qt::ISODate));
#endif
}

static inline QVariant qTimeFromString(const QString &val)
{
#if !QT_CONFIG(datestring)
    Q_UNUSED(val);
    return QVariant(val);
#else
    if (val.isEmpty())
        return QVariant(QTime());
    return QVariant(QTime::fromString(val, Qt::ISODate));
#endif
}

static inline QVariant qDateTimeFromString(QString &val)
{
#if !QT_CONFIG(datestring)
    Q_UNUSED(val);
    return QVariant(val);
#else
    if (val.isEmpty())
        return QVariant(QDateTime());
    if (val.length() == 14)
        // TIMESTAMPS have the format yyyyMMddhhmmss
        val.insert(4, QLatin1Char('-')).insert(7, QLatin1Char('-')).insert(10,
                    QLatin1Char('T')).insert(13, QLatin1Char(':')).insert(16, QLatin1Char(':'));
    return QVariant(QDateTime::fromString(val, Qt::ISODate));
#endif
}

// check if this client and server version of MySQL/MariaDB support prepared statements
static inline bool checkPreparedQueries(MYSQL *mysql)
{
    std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> stmt(mysql_stmt_init(mysql), &mysql_stmt_close);
    if (!stmt)
        return false;

    static const char dummyQuery[] = "SELECT ? + ?";
    if (mysql_stmt_prepare(stmt.get(), dummyQuery, sizeof(dummyQuery) - 1))
        return false;

    return mysql_stmt_param_count(stmt.get()) == 2;
}

class QMYSQLResultPrivate;

class QMYSQLResult : public QSqlResult
{
    Q_DECLARE_PRIVATE(QMYSQLResult)
    friend class QMYSQLDriver;

public:
    explicit QMYSQLResult(const QMYSQLDriver *db);
    ~QMYSQLResult();

    QVariant handle() const override;
protected:
    void cleanup();
    bool fetch(int i) override;
    bool fetchNext() override;
    bool fetchLast() override;
    bool fetchFirst() override;
    QVariant data(int field) override;
    bool isNull(int field) override;
    bool reset (const QString& query) override;
    int size() override;
    int numRowsAffected() override;
    QVariant lastInsertId() const override;
    QSqlRecord record() const override;
    void virtual_hook(int id, void *data) override;
    bool nextResult() override;
    void detachFromResultSet() override;

    bool prepare(const QString &stmt) override;
    bool exec() override;
};

class QMYSQLResultPrivate: public QSqlResultPrivate
{
    Q_DECLARE_PUBLIC(QMYSQLResult)

public:
    Q_DECLARE_SQLDRIVER_PRIVATE(QMYSQLDriver)

    using QSqlResultPrivate::QSqlResultPrivate;

    bool bindInValues();
    void bindBlobs();

    MYSQL_RES *result = nullptr;
    MYSQL_ROW row;

    struct QMyField
    {
        char *outField = nullptr;
        const MYSQL_FIELD *myField = nullptr;
        QMetaType::Type type = QMetaType::UnknownType;
        my_bool nullIndicator = false;
        ulong bufLength = 0ul;
    };

    QVector<QMyField> fields;

    MYSQL_STMT *stmt = nullptr;
    MYSQL_RES *meta = nullptr;

    MYSQL_BIND *inBinds = nullptr;
    MYSQL_BIND *outBinds = nullptr;

    int rowsAffected = 0;
    bool hasBlobs = false;
    bool preparedQuery = false;
};

#if QT_CONFIG(textcodec)
static QTextCodec* codec(MYSQL* mysql)
{
    QTextCodec* heuristicCodec = QTextCodec::codecForName(mysql_character_set_name(mysql));
    if (heuristicCodec)
        return heuristicCodec;
    return QTextCodec::codecForLocale();
}
#endif // textcodec

static QSqlError qMakeError(const QString& err, QSqlError::ErrorType type,
                            const QMYSQLDriverPrivate* p)
{
    const char *cerr = p->mysql ? mysql_error(p->mysql) : 0;
    return QSqlError(QLatin1String("QMYSQL: ") + err,
                     p->tc ? toUnicode(p->tc, cerr) : QString::fromLatin1(cerr),
                     type, QString::number(mysql_errno(p->mysql)));
}


static QMetaType::Type qDecodeMYSQLType(int mysqltype, uint flags)
{
    QMetaType::Type type;
    switch (mysqltype) {
    case FIELD_TYPE_TINY :
        type = (flags & UNSIGNED_FLAG) ? QMetaType::UChar : QMetaType::Char;
        break;
    case FIELD_TYPE_SHORT :
        type = (flags & UNSIGNED_FLAG) ? QMetaType::UShort : QMetaType::Short;
        break;
    case FIELD_TYPE_LONG :
    case FIELD_TYPE_INT24 :
        type = (flags & UNSIGNED_FLAG) ? QMetaType::UInt : QMetaType::Int;
        break;
    case FIELD_TYPE_YEAR :
        type = QMetaType::Int;
        break;
    case FIELD_TYPE_LONGLONG :
        type = (flags & UNSIGNED_FLAG) ? QMetaType::ULongLong : QMetaType::LongLong;
        break;
    case FIELD_TYPE_FLOAT :
    case FIELD_TYPE_DOUBLE :
    case FIELD_TYPE_DECIMAL :
#if defined(FIELD_TYPE_NEWDECIMAL)
    case FIELD_TYPE_NEWDECIMAL:
#endif
        type = QMetaType::Double;
        break;
    case FIELD_TYPE_DATE :
        type = QMetaType::QDate;
        break;
    case FIELD_TYPE_TIME :
        // A time field can be within the range '-838:59:59' to '838:59:59' so
        // use QString instead of QTime since QTime is limited to 24 hour clock
        type = QMetaType::QString;
        break;
    case FIELD_TYPE_DATETIME :
    case FIELD_TYPE_TIMESTAMP :
        type = QMetaType::QDateTime;
        break;
    case FIELD_TYPE_STRING :
    case FIELD_TYPE_VAR_STRING :
    case FIELD_TYPE_BLOB :
    case FIELD_TYPE_TINY_BLOB :
    case FIELD_TYPE_MEDIUM_BLOB :
    case FIELD_TYPE_LONG_BLOB :
    case FIELD_TYPE_GEOMETRY :
    case MYSQL_TYPE_JSON :
        type = (flags & BINARY_FLAG) ? QMetaType::QByteArray : QMetaType::QString;
        break;
    default:
    case FIELD_TYPE_ENUM :
    case FIELD_TYPE_SET :
        type = QMetaType::QString;
        break;
    }
    return type;
}

static QSqlField qToField(MYSQL_FIELD *field, QTextCodec *tc)
{
    QSqlField f(toUnicode(tc, field->name),
                QVariant::Type(qDecodeMYSQLType(int(field->type), field->flags)),
                toUnicode(tc, field->table));
    f.setRequired(IS_NOT_NULL(field->flags));
    f.setLength(field->length);
    f.setPrecision(field->decimals);
    f.setSqlType(field->type);
    f.setAutoValue(field->flags & AUTO_INCREMENT_FLAG);
    return f;
}

static QSqlError qMakeStmtError(const QString& err, QSqlError::ErrorType type,
                            MYSQL_STMT* stmt)
{
    const char *cerr = mysql_stmt_error(stmt);
    return QSqlError(QLatin1String("QMYSQL3: ") + err,
                     QString::fromLatin1(cerr),
                     type, QString::number(mysql_stmt_errno(stmt)));
}

static bool qIsBlob(int t)
{
    return t == MYSQL_TYPE_TINY_BLOB
           || t == MYSQL_TYPE_BLOB
           || t == MYSQL_TYPE_MEDIUM_BLOB
           || t == MYSQL_TYPE_LONG_BLOB
           || t == MYSQL_TYPE_JSON;
}

static bool qIsInteger(int t)
{
    return t == QMetaType::Char || t == QMetaType::UChar
        || t == QMetaType::Short || t == QMetaType::UShort
        || t == QMetaType::Int || t == QMetaType::UInt
        || t == QMetaType::LongLong || t == QMetaType::ULongLong;
}

void QMYSQLResultPrivate::bindBlobs()
{
    MYSQL_BIND *bind;
    for (int i = 0; i < fields.count(); ++i) {
        const MYSQL_FIELD *fieldInfo = fields.at(i).myField;
        if (qIsBlob(inBinds[i].buffer_type) && meta && fieldInfo) {
            bind = &inBinds[i];
            bind->buffer_length = fieldInfo->max_length;
            delete[] static_cast<char*>(bind->buffer);
            bind->buffer = new char[fieldInfo->max_length];
            fields[i].outField = static_cast<char*>(bind->buffer);
        }
    }
}

bool QMYSQLResultPrivate::bindInValues()
{
    int i = 0;

    if (!meta)
        meta = mysql_stmt_result_metadata(stmt);
    if (!meta)
        return false;

    fields.resize(mysql_num_fields(meta));

    inBinds = new MYSQL_BIND[fields.size()];
    memset(inBinds, 0, fields.size() * sizeof(MYSQL_BIND));

    const MYSQL_FIELD *fieldInfo;

    while((fieldInfo = mysql_fetch_field(meta))) {
        MYSQL_BIND *bind = &inBinds[i];

        QMyField &f = fields[i];
        f.myField = fieldInfo;
        bind->buffer_length = f.bufLength = fieldInfo->length + 1;
        bind->buffer_type = fieldInfo->type;
        f.type = qDecodeMYSQLType(fieldInfo->type, fieldInfo->flags);
        if (qIsBlob(fieldInfo->type)) {
            // the size of a blob-field is available as soon as we call
            // mysql_stmt_store_result()
            // after mysql_stmt_exec() in QMYSQLResult::exec()
            bind->buffer_length = f.bufLength = 0;
            hasBlobs = true;
        } else if (qIsInteger(f.type)) {
            bind->buffer_length = f.bufLength = 8;
        } else {
            bind->buffer_type = MYSQL_TYPE_STRING;
        }

        bind->is_null = &f.nullIndicator;
        bind->length = &f.bufLength;
        bind->is_unsigned = fieldInfo->flags & UNSIGNED_FLAG ? 1 : 0;

        char *field = new char[bind->buffer_length + 1]{};
        bind->buffer = f.outField = field;

        ++i;
    }
    return true;
}

QMYSQLResult::QMYSQLResult(const QMYSQLDriver* db)
    : QSqlResult(*new QMYSQLResultPrivate(this, db))
{
}

QMYSQLResult::~QMYSQLResult()
{
    cleanup();
}

QVariant QMYSQLResult::handle() const
{
    Q_D(const QMYSQLResult);
    if(d->preparedQuery)
        return d->meta ? QVariant::fromValue(d->meta) : QVariant::fromValue(d->stmt);
    else
        return QVariant::fromValue(d->result);
}

void QMYSQLResult::cleanup()
{
    Q_D(QMYSQLResult);
    if (d->result)
        mysql_free_result(d->result);

// must iterate trough leftover result sets from multi-selects or stored procedures
// if this isn't done subsequent queries will fail with "Commands out of sync"
    while (driver() && d->drv_d_func()->mysql && mysql_next_result(d->drv_d_func()->mysql) == 0) {
        MYSQL_RES *res = mysql_store_result(d->drv_d_func()->mysql);
        if (res)
            mysql_free_result(res);
    }

    if (d->stmt) {
        if (mysql_stmt_close(d->stmt))
            qWarning("QMYSQLResult::cleanup: unable to free statement handle");
        d->stmt = 0;
    }

    if (d->meta) {
        mysql_free_result(d->meta);
        d->meta = 0;
    }

    int i;
    for (i = 0; i < d->fields.count(); ++i)
        delete[] d->fields[i].outField;

    if (d->outBinds) {
        delete[] d->outBinds;
        d->outBinds = 0;
    }

    if (d->inBinds) {
        delete[] d->inBinds;
        d->inBinds = 0;
    }

    d->hasBlobs = false;
    d->fields.clear();
    d->result = NULL;
    d->row = NULL;
    setAt(-1);
    setActive(false);
}

bool QMYSQLResult::fetch(int i)
{
    Q_D(QMYSQLResult);
    if (!driver())
        return false;
    if (isForwardOnly()) { // fake a forward seek
        if (at() < i) {
            int x = i - at();
            while (--x && fetchNext()) {};
            return fetchNext();
        } else {
            return false;
        }
    }
    if (at() == i)
        return true;
    if (d->preparedQuery) {
        mysql_stmt_data_seek(d->stmt, i);

        int nRC = mysql_stmt_fetch(d->stmt);
        if (nRC) {
#ifdef MYSQL_DATA_TRUNCATED
            if (nRC == 1 || nRC == MYSQL_DATA_TRUNCATED)
#else
            if (nRC == 1)
#endif
                setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                         "Unable to fetch data"), QSqlError::StatementError, d->stmt));
            return false;
        }
    } else {
        mysql_data_seek(d->result, i);
        d->row = mysql_fetch_row(d->result);
        if (!d->row)
            return false;
    }

    setAt(i);
    return true;
}

bool QMYSQLResult::fetchNext()
{
    Q_D(QMYSQLResult);
    if (!driver())
        return false;
    if (d->preparedQuery) {
        int nRC = mysql_stmt_fetch(d->stmt);
        if (nRC) {
#ifdef MYSQL_DATA_TRUNCATED
            if (nRC == 1 || nRC == MYSQL_DATA_TRUNCATED)
#else
            if (nRC == 1)
#endif // MYSQL_DATA_TRUNCATED
                setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                                    "Unable to fetch data"), QSqlError::StatementError, d->stmt));
            return false;
        }
    } else {
        d->row = mysql_fetch_row(d->result);
        if (!d->row)
            return false;
    }
    setAt(at() + 1);
    return true;
}

bool QMYSQLResult::fetchLast()
{
    Q_D(QMYSQLResult);
    if (!driver())
        return false;
    if (isForwardOnly()) { // fake this since MySQL can't seek on forward only queries
        bool success = fetchNext(); // did we move at all?
        while (fetchNext()) {};
        return success;
    }

    my_ulonglong numRows;
    if (d->preparedQuery) {
        numRows = mysql_stmt_num_rows(d->stmt);
    } else {
        numRows = mysql_num_rows(d->result);
    }
    if (at() == int(numRows))
        return true;
    if (!numRows)
        return false;
    return fetch(numRows - 1);
}

bool QMYSQLResult::fetchFirst()
{
    if (at() == 0)
        return true;

    if (isForwardOnly())
        return (at() == QSql::BeforeFirstRow) ? fetchNext() : false;
    return fetch(0);
}

QVariant QMYSQLResult::data(int field)
{
    Q_D(QMYSQLResult);
    if (!isSelect() || field >= d->fields.count()) {
        qWarning("QMYSQLResult::data: column %d out of range", field);
        return QVariant();
    }

    if (!driver())
        return QVariant();

    int fieldLength = 0;
    const QMYSQLResultPrivate::QMyField &f = d->fields.at(field);
    QString val;
    if (d->preparedQuery) {
        if (f.nullIndicator)
            return QVariant(QVariant::Type(f.type));

        if (qIsInteger(f.type)) {
            QVariant variant(f.type, f.outField);
            // we never want to return char variants here, see QTBUG-53397
            if (static_cast<int>(f.type) == QMetaType::UChar)
                return variant.toUInt();
            else if (static_cast<int>(f.type) == QMetaType::Char)
                return variant.toInt();
            return variant;
        }

        if (f.type != QMetaType::QByteArray)
            val = toUnicode(d->drv_d_func()->tc, f.outField, f.bufLength);
    } else {
        if (d->row[field] == NULL) {
            // NULL value
            return QVariant(QVariant::Type(f.type));
        }

        fieldLength = mysql_fetch_lengths(d->result)[field];

        if (f.type != QMetaType::QByteArray)
            val = toUnicode(d->drv_d_func()->tc, d->row[field], fieldLength);
    }

    switch (static_cast<int>(f.type)) {
    case QMetaType::LongLong:
        return QVariant(val.toLongLong());
    case QMetaType::ULongLong:
        return QVariant(val.toULongLong());
    case QMetaType::Char:
    case QMetaType::Short:
    case QMetaType::Int:
        return QVariant(val.toInt());
    case QMetaType::UChar:
    case QMetaType::UShort:
    case QMetaType::UInt:
        return QVariant(val.toUInt());
    case QMetaType::Double: {
        QVariant v;
        bool ok=false;
        double dbl = val.toDouble(&ok);
        switch(numericalPrecisionPolicy()) {
            case QSql::LowPrecisionInt32:
                v=QVariant(dbl).toInt();
                break;
            case QSql::LowPrecisionInt64:
                v = QVariant(dbl).toLongLong();
                break;
            case QSql::LowPrecisionDouble:
                v = QVariant(dbl);
                break;
            case QSql::HighPrecision:
            default:
                v = val;
                ok = true;
                break;
        }
        if(ok)
            return v;
        return QVariant();
    }
    case QMetaType::QDate:
        return qDateFromString(val);
    case QMetaType::QTime:
        return qTimeFromString(val);
    case QMetaType::QDateTime:
        return qDateTimeFromString(val);
    case QMetaType::QByteArray: {

        QByteArray ba;
        if (d->preparedQuery) {
            ba = QByteArray(f.outField, f.bufLength);
        } else {
            ba = QByteArray(d->row[field], fieldLength);
        }
        return QVariant(ba);
    }
    case QMetaType::QString:
    default:
        return QVariant(val);
    }
    Q_UNREACHABLE();
}

bool QMYSQLResult::isNull(int field)
{
   Q_D(const QMYSQLResult);
   if (field < 0 || field >= d->fields.count())
       return true;
   if (d->preparedQuery)
       return d->fields.at(field).nullIndicator;
   else
       return d->row[field] == NULL;
}

bool QMYSQLResult::reset (const QString& query)
{
    Q_D(QMYSQLResult);
    if (!driver() || !driver()->isOpen() || driver()->isOpenError())
        return false;

    d->preparedQuery = false;

    cleanup();

    const QByteArray encQuery(fromUnicode(d->drv_d_func()->tc, query));
    if (mysql_real_query(d->drv_d_func()->mysql, encQuery.data(), encQuery.length())) {
        setLastError(qMakeError(QCoreApplication::translate("QMYSQLResult", "Unable to execute query"),
                     QSqlError::StatementError, d->drv_d_func()));
        return false;
    }
    d->result = mysql_store_result(d->drv_d_func()->mysql);
    if (!d->result && mysql_field_count(d->drv_d_func()->mysql) > 0) {
        setLastError(qMakeError(QCoreApplication::translate("QMYSQLResult", "Unable to store result"),
                    QSqlError::StatementError, d->drv_d_func()));
        return false;
    }
    int numFields = mysql_field_count(d->drv_d_func()->mysql);
    setSelect(numFields != 0);
    d->fields.resize(numFields);
    d->rowsAffected = mysql_affected_rows(d->drv_d_func()->mysql);

    if (isSelect()) {
        for(int i = 0; i < numFields; i++) {
            MYSQL_FIELD* field = mysql_fetch_field_direct(d->result, i);
            d->fields[i].type = qDecodeMYSQLType(field->type, field->flags);
        }
        setAt(QSql::BeforeFirstRow);
    }
    setActive(true);
    return isActive();
}

int QMYSQLResult::size()
{
    Q_D(const QMYSQLResult);
    if (driver() && isSelect())
        if (d->preparedQuery)
            return mysql_stmt_num_rows(d->stmt);
        else
            return int(mysql_num_rows(d->result));
    else
        return -1;
}

int QMYSQLResult::numRowsAffected()
{
    Q_D(const QMYSQLResult);
    return d->rowsAffected;
}

void QMYSQLResult::detachFromResultSet()
{
    Q_D(QMYSQLResult);

    if (d->preparedQuery) {
        mysql_stmt_free_result(d->stmt);
    }
}

QVariant QMYSQLResult::lastInsertId() const
{
    Q_D(const QMYSQLResult);
    if (!isActive() || !driver())
        return QVariant();

    if (d->preparedQuery) {
        quint64 id = mysql_stmt_insert_id(d->stmt);
        if (id)
            return QVariant(id);
    } else {
        quint64 id = mysql_insert_id(d->drv_d_func()->mysql);
        if (id)
            return QVariant(id);
    }
    return QVariant();
}

QSqlRecord QMYSQLResult::record() const
{
    Q_D(const QMYSQLResult);
    QSqlRecord info;
    MYSQL_RES *res;
    if (!isActive() || !isSelect() || !driver())
        return info;

    res = d->preparedQuery ? d->meta : d->result;

    if (!mysql_errno(d->drv_d_func()->mysql)) {
        mysql_field_seek(res, 0);
        MYSQL_FIELD* field = mysql_fetch_field(res);
        while(field) {
            info.append(qToField(field, d->drv_d_func()->tc));
            field = mysql_fetch_field(res);
        }
    }
    mysql_field_seek(res, 0);
    return info;
}

bool QMYSQLResult::nextResult()
{
    Q_D(QMYSQLResult);
    if (!driver())
        return false;

    setAt(-1);
    setActive(false);

    if (d->result && isSelect())
        mysql_free_result(d->result);
    d->result = 0;
    setSelect(false);

    for (int i = 0; i < d->fields.count(); ++i)
        delete[] d->fields[i].outField;
    d->fields.clear();

    int status = mysql_next_result(d->drv_d_func()->mysql);
    if (status > 0) {
        setLastError(qMakeError(QCoreApplication::translate("QMYSQLResult", "Unable to execute next query"),
                     QSqlError::StatementError, d->drv_d_func()));
        return false;
    } else if (status == -1) {
        return false;   // No more result sets
    }

    d->result = mysql_store_result(d->drv_d_func()->mysql);
    int numFields = mysql_field_count(d->drv_d_func()->mysql);
    if (!d->result && numFields > 0) {
        setLastError(qMakeError(QCoreApplication::translate("QMYSQLResult", "Unable to store next result"),
                     QSqlError::StatementError, d->drv_d_func()));
        return false;
    }

    setSelect(numFields > 0);
    d->fields.resize(numFields);
    d->rowsAffected = mysql_affected_rows(d->drv_d_func()->mysql);

    if (isSelect()) {
        for (int i = 0; i < numFields; i++) {
            MYSQL_FIELD* field = mysql_fetch_field_direct(d->result, i);
            d->fields[i].type = qDecodeMYSQLType(field->type, field->flags);
        }
    }

    setActive(true);
    return true;
}

void QMYSQLResult::virtual_hook(int id, void *data)
{
    QSqlResult::virtual_hook(id, data);
}

static MYSQL_TIME *toMySqlDate(QDate date, QTime time, int type)
{
    Q_ASSERT(type == QMetaType::QTime || type == QMetaType::QDate
             || type == QMetaType::QDateTime);

    MYSQL_TIME *myTime = new MYSQL_TIME;
    memset(myTime, 0, sizeof(MYSQL_TIME));

    if (type == QMetaType::QTime || type == QMetaType::QDateTime) {
        myTime->hour = time.hour();
        myTime->minute = time.minute();
        myTime->second = time.second();
        myTime->second_part = time.msec() * 1000;
    }
    if (type == QMetaType::QDate || type == QMetaType::QDateTime) {
        myTime->year = date.year();
        myTime->month = date.month();
        myTime->day = date.day();
    }

    return myTime;
}

bool QMYSQLResult::prepare(const QString& query)
{
    Q_D(QMYSQLResult);
    if (!driver())
        return false;

    cleanup();
    if (!d->drv_d_func()->preparedQuerysEnabled)
        return QSqlResult::prepare(query);

    int r;

    if (query.isEmpty())
        return false;

    if (!d->stmt)
        d->stmt = mysql_stmt_init(d->drv_d_func()->mysql);
    if (!d->stmt) {
        setLastError(qMakeError(QCoreApplication::translate("QMYSQLResult", "Unable to prepare statement"),
                     QSqlError::StatementError, d->drv_d_func()));
        return false;
    }

    const QByteArray encQuery(fromUnicode(d->drv_d_func()->tc, query));
    r = mysql_stmt_prepare(d->stmt, encQuery.constData(), encQuery.length());
    if (r != 0) {
        setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                     "Unable to prepare statement"), QSqlError::StatementError, d->stmt));
        cleanup();
        return false;
    }

    const auto paramCount = mysql_stmt_param_count(d->stmt);
    if (paramCount > 0) // allocate memory for outvalues
        d->outBinds = new MYSQL_BIND[paramCount]();

    setSelect(d->bindInValues());
    d->preparedQuery = true;
    return true;
}

bool QMYSQLResult::exec()
{
    Q_D(QMYSQLResult);
    if (!driver())
        return false;
    if (!d->preparedQuery)
        return QSqlResult::exec();
    if (!d->stmt)
        return false;

    int r = 0;
    MYSQL_BIND* currBind;
    QVector<MYSQL_TIME *> timeVector;
    QVector<QByteArray> stringVector;
    QVector<my_bool> nullVector;

    const QVector<QVariant> values = boundValues();

    r = mysql_stmt_reset(d->stmt);
    if (r != 0) {
        setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                     "Unable to reset statement"), QSqlError::StatementError, d->stmt));
        return false;
    }

    if (mysql_stmt_param_count(d->stmt) > 0 &&
        mysql_stmt_param_count(d->stmt) == (uint)values.count()) {

        nullVector.resize(values.count());
        for (int i = 0; i < values.count(); ++i) {
            const QVariant &val = boundValues().at(i);
            void *data = const_cast<void *>(val.constData());

            currBind = &d->outBinds[i];

            nullVector[i] = static_cast<my_bool>(val.isNull());
            currBind->is_null = &nullVector[i];
            currBind->length = 0;
            currBind->is_unsigned = 0;

            switch (val.userType()) {
                case QMetaType::QByteArray:
                    currBind->buffer_type = MYSQL_TYPE_BLOB;
                    currBind->buffer = const_cast<char *>(val.toByteArray().constData());
                    currBind->buffer_length = val.toByteArray().size();
                    break;

                case QMetaType::QTime:
                case QMetaType::QDate:
                case QMetaType::QDateTime: {
                    MYSQL_TIME *myTime = toMySqlDate(val.toDate(), val.toTime(), val.userType());
                    timeVector.append(myTime);

                    currBind->buffer = myTime;
                    switch (val.userType()) {
                    case QMetaType::QTime:
                        currBind->buffer_type = MYSQL_TYPE_TIME;
                        myTime->time_type = MYSQL_TIMESTAMP_TIME;
                        break;
                    case QMetaType::QDate:
                        currBind->buffer_type = MYSQL_TYPE_DATE;
                        myTime->time_type = MYSQL_TIMESTAMP_DATE;
                        break;
                    case QMetaType::QDateTime:
                        currBind->buffer_type = MYSQL_TYPE_DATETIME;
                        myTime->time_type = MYSQL_TIMESTAMP_DATETIME;
                        break;
                    default:
                        break;
                    }
                    currBind->buffer_length = sizeof(MYSQL_TIME);
                    currBind->length = 0;
                    break; }
                case QMetaType::UInt:
                case QMetaType::Int:
                    currBind->buffer_type = MYSQL_TYPE_LONG;
                    currBind->buffer = data;
                    currBind->buffer_length = sizeof(int);
                    currBind->is_unsigned = (val.userType() != QMetaType::Int);
                break;
                case QMetaType::Bool:
                    currBind->buffer_type = MYSQL_TYPE_TINY;
                    currBind->buffer = data;
                    currBind->buffer_length = sizeof(bool);
                    currBind->is_unsigned = false;
                    break;
                case QMetaType::Double:
                    currBind->buffer_type = MYSQL_TYPE_DOUBLE;
                    currBind->buffer = data;
                    currBind->buffer_length = sizeof(double);
                    break;
                case QMetaType::LongLong:
                case QMetaType::ULongLong:
                    currBind->buffer_type = MYSQL_TYPE_LONGLONG;
                    currBind->buffer = data;
                    currBind->buffer_length = sizeof(qint64);
                    currBind->is_unsigned = (val.userType() == QMetaType::ULongLong);
                    break;
                case QMetaType::QString:
                default: {
                    QByteArray ba = fromUnicode(d->drv_d_func()->tc, val.toString());
                    stringVector.append(ba);
                    currBind->buffer_type = MYSQL_TYPE_STRING;
                    currBind->buffer = const_cast<char *>(ba.constData());
                    currBind->buffer_length = ba.length();
                    break; }
            }
        }

        r = mysql_stmt_bind_param(d->stmt, d->outBinds);
        if (r != 0) {
            setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                         "Unable to bind value"), QSqlError::StatementError, d->stmt));
            qDeleteAll(timeVector);
            return false;
        }
    }
    r = mysql_stmt_execute(d->stmt);

    qDeleteAll(timeVector);

    if (r != 0) {
        setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                     "Unable to execute statement"), QSqlError::StatementError, d->stmt));
        return false;
    }
    //if there is meta-data there is also data
    setSelect(d->meta);

    d->rowsAffected = mysql_stmt_affected_rows(d->stmt);

    if (isSelect()) {
        my_bool update_max_length = true;

        r = mysql_stmt_bind_result(d->stmt, d->inBinds);
        if (r != 0) {
            setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                         "Unable to bind outvalues"), QSqlError::StatementError, d->stmt));
            return false;
        }
        if (d->hasBlobs)
            mysql_stmt_attr_set(d->stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &update_max_length);

        r = mysql_stmt_store_result(d->stmt);
        if (r != 0) {
            setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                         "Unable to store statement results"), QSqlError::StatementError, d->stmt));
            return false;
        }

        if (d->hasBlobs) {
            // mysql_stmt_store_result() with STMT_ATTR_UPDATE_MAX_LENGTH set to true crashes
            // when called without a preceding call to mysql_stmt_bind_result()
            // in versions < 4.1.8
            d->bindBlobs();
            r = mysql_stmt_bind_result(d->stmt, d->inBinds);
            if (r != 0) {
                setLastError(qMakeStmtError(QCoreApplication::translate("QMYSQLResult",
                             "Unable to bind outvalues"), QSqlError::StatementError, d->stmt));
                return false;
            }
        }
        setAt(QSql::BeforeFirstRow);
    }
    setActive(true);
    return true;
}

/////////////////////////////////////////////////////////

static int qMySqlConnectionCount = 0;
static bool qMySqlInitHandledByUser = false;

static void qLibraryInit()
{
#ifndef Q_NO_MYSQL_EMBEDDED
    if (qMySqlInitHandledByUser || qMySqlConnectionCount > 1)
        return;

# if MYSQL_VERSION_ID >= 50003
    if (mysql_library_init(0, 0, 0)) {
# else
    if (mysql_server_init(0, 0, 0)) {
# endif
        qWarning("QMYSQLDriver::qServerInit: unable to start server.");
    }
#endif // Q_NO_MYSQL_EMBEDDED

#if defined(MARIADB_BASE_VERSION) || defined(MARIADB_VERSION_ID)
    qAddPostRoutine([]() { mysql_server_end(); });
#endif
}

static void qLibraryEnd()
{
#if !defined(MARIADB_BASE_VERSION) && !defined(MARIADB_VERSION_ID)
# if !defined(Q_NO_MYSQL_EMBEDDED)
#  if MYSQL_VERSION_ID >= 50003
    mysql_library_end();
#  else
    mysql_server_end();
#  endif
# endif
#endif
}

QMYSQLDriver::QMYSQLDriver(QObject * parent)
    : QSqlDriver(*new QMYSQLDriverPrivate, parent)
{
    init();
    qLibraryInit();
}

/*!
    Create a driver instance with the open connection handle, \a con.
    The instance's parent (owner) is \a parent.
*/

QMYSQLDriver::QMYSQLDriver(MYSQL * con, QObject * parent)
    : QSqlDriver(*new QMYSQLDriverPrivate, parent)
{
    Q_D(QMYSQLDriver);
    init();
    if (con) {
        d->mysql = (MYSQL *) con;
#if QT_CONFIG(textcodec)
        d->tc = codec(con);
#endif
        setOpen(true);
        setOpenError(false);
        if (qMySqlConnectionCount == 1)
            qMySqlInitHandledByUser = true;
    } else {
        qLibraryInit();
    }
}

void QMYSQLDriver::init()
{
    Q_D(QMYSQLDriver);
    d->mysql = 0;
    qMySqlConnectionCount++;
}

QMYSQLDriver::~QMYSQLDriver()
{
    qMySqlConnectionCount--;
    if (qMySqlConnectionCount == 0 && !qMySqlInitHandledByUser)
        qLibraryEnd();
}

bool QMYSQLDriver::hasFeature(DriverFeature f) const
{
    Q_D(const QMYSQLDriver);
    switch (f) {
    case Transactions:
// CLIENT_TRANSACTION should be defined in all recent mysql client libs > 3.23.34
#ifdef CLIENT_TRANSACTIONS
        if (d->mysql) {
            if ((d->mysql->server_capabilities & CLIENT_TRANSACTIONS) == CLIENT_TRANSACTIONS)
                return true;
        }
#endif
        return false;
    case NamedPlaceholders:
    case BatchOperations:
    case SimpleLocking:
    case EventNotifications:
    case FinishQuery:
    case CancelQuery:
        return false;
    case QuerySize:
    case BLOB:
    case LastInsertId:
    case Unicode:
    case LowPrecisionNumbers:
        return true;
    case PreparedQueries:
    case PositionalPlaceholders:
        return d->preparedQuerysEnabled;
    case MultipleResultSets:
        return true;
    }
    return false;
}

static void setOptionFlag(uint &optionFlags, const QString &opt)
{
    if (opt == QLatin1String("CLIENT_COMPRESS"))
        optionFlags |= CLIENT_COMPRESS;
    else if (opt == QLatin1String("CLIENT_FOUND_ROWS"))
        optionFlags |= CLIENT_FOUND_ROWS;
    else if (opt == QLatin1String("CLIENT_IGNORE_SPACE"))
        optionFlags |= CLIENT_IGNORE_SPACE;
    else if (opt == QLatin1String("CLIENT_INTERACTIVE"))
        optionFlags |= CLIENT_INTERACTIVE;
    else if (opt == QLatin1String("CLIENT_NO_SCHEMA"))
        optionFlags |= CLIENT_NO_SCHEMA;
    else if (opt == QLatin1String("CLIENT_ODBC"))
        optionFlags |= CLIENT_ODBC;
    else if (opt == QLatin1String("CLIENT_SSL"))
        qWarning("QMYSQLDriver: SSL_KEY, SSL_CERT and SSL_CA should be used instead of CLIENT_SSL.");
    else
        qWarning("QMYSQLDriver::open: Unknown connect option '%s'", opt.toLocal8Bit().constData());
}

bool QMYSQLDriver::open(const QString& db,
                         const QString& user,
                         const QString& password,
                         const QString& host,
                         int port,
                         const QString& connOpts)
{
    Q_D(QMYSQLDriver);
    if (isOpen())
        close();

    /* This is a hack to get MySQL's stored procedure support working.
       Since a stored procedure _may_ return multiple result sets,
       we have to enable CLIEN_MULTI_STATEMENTS here, otherwise _any_
       stored procedure call will fail.
    */
    unsigned int optionFlags = CLIENT_MULTI_STATEMENTS;
    const QStringList opts(connOpts.split(QLatin1Char(';'), Qt::SkipEmptyParts));
    QString unixSocket;
    QString sslCert;
    QString sslCA;
    QString sslKey;
    QString sslCAPath;
    QString sslCipher;
    my_bool reconnect=false;
    uint connectTimeout = 0;
    uint readTimeout = 0;
    uint writeTimeout = 0;

    // extract the real options from the string
    for (int i = 0; i < opts.count(); ++i) {
        QString tmp(opts.at(i).simplified());
        int idx;
        if ((idx = tmp.indexOf(QLatin1Char('='))) != -1) {
            QString val = tmp.mid(idx + 1).simplified();
            QString opt = tmp.left(idx).simplified();
            if (opt == QLatin1String("UNIX_SOCKET"))
                unixSocket = val;
            else if (opt == QLatin1String("MYSQL_OPT_RECONNECT")) {
                if (val == QLatin1String("TRUE") || val == QLatin1String("1") || val.isEmpty())
                    reconnect = true;
            } else if (opt == QLatin1String("MYSQL_OPT_CONNECT_TIMEOUT"))
                connectTimeout = val.toInt();
            else if (opt == QLatin1String("MYSQL_OPT_READ_TIMEOUT"))
                readTimeout = val.toInt();
            else if (opt == QLatin1String("MYSQL_OPT_WRITE_TIMEOUT"))
                writeTimeout = val.toInt();
            else if (opt == QLatin1String("SSL_KEY"))
                sslKey = val;
            else if (opt == QLatin1String("SSL_CERT"))
                sslCert = val;
            else if (opt == QLatin1String("SSL_CA"))
                sslCA = val;
            else if (opt == QLatin1String("SSL_CAPATH"))
                sslCAPath = val;
            else if (opt == QLatin1String("SSL_CIPHER"))
                sslCipher = val;
            else if (val == QLatin1String("TRUE") || val == QLatin1String("1"))
                setOptionFlag(optionFlags, tmp.left(idx).simplified());
            else
                qWarning("QMYSQLDriver::open: Illegal connect option value '%s'",
                         tmp.toLocal8Bit().constData());
        } else {
            setOptionFlag(optionFlags, tmp);
        }
    }

    if (!(d->mysql = mysql_init((MYSQL*) 0))) {
        setLastError(qMakeError(tr("Unable to allocate a MYSQL object"),
                     QSqlError::ConnectionError, d));
        setOpenError(true);
        return false;
    }

    if (!sslKey.isNull() || !sslCert.isNull() || !sslCA.isNull() ||
        !sslCAPath.isNull() || !sslCipher.isNull()) {
       mysql_ssl_set(d->mysql,
                        sslKey.isNull() ? static_cast<const char *>(0)
                                        : QFile::encodeName(sslKey).constData(),
                        sslCert.isNull() ? static_cast<const char *>(0)
                                         : QFile::encodeName(sslCert).constData(),
                        sslCA.isNull() ? static_cast<const char *>(0)
                                       : QFile::encodeName(sslCA).constData(),
                        sslCAPath.isNull() ? static_cast<const char *>(0)
                                           : QFile::encodeName(sslCAPath).constData(),
                        sslCipher.isNull() ? static_cast<const char *>(0)
                                           : sslCipher.toLocal8Bit().constData());
    }

#if MYSQL_VERSION_ID >= 50100
    if (connectTimeout != 0)
        mysql_options(d->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
    if (readTimeout != 0)
        mysql_options(d->mysql, MYSQL_OPT_READ_TIMEOUT, &readTimeout);
    if (writeTimeout != 0)
        mysql_options(d->mysql, MYSQL_OPT_WRITE_TIMEOUT, &writeTimeout);
#endif
    MYSQL *mysql = mysql_real_connect(d->mysql,
                                      host.isNull() ? static_cast<const char *>(0)
                                                    : host.toLocal8Bit().constData(),
                                      user.isNull() ? static_cast<const char *>(0)
                                                    : user.toLocal8Bit().constData(),
                                      password.isNull() ? static_cast<const char *>(0)
                                                        : password.toLocal8Bit().constData(),
                                      db.isNull() ? static_cast<const char *>(0)
                                                  : db.toLocal8Bit().constData(),
                                      (port > -1) ? port : 0,
                                      unixSocket.isNull() ? static_cast<const char *>(0)
                                                          : unixSocket.toLocal8Bit().constData(),
                                      optionFlags);

    if (mysql == d->mysql) {
        if (!db.isEmpty() && mysql_select_db(d->mysql, db.toLocal8Bit().constData())) {
            setLastError(qMakeError(tr("Unable to open database '%1'").arg(db), QSqlError::ConnectionError, d));
            mysql_close(d->mysql);
            setOpenError(true);
            return false;
        }
#if MYSQL_VERSION_ID >= 50100
        if (reconnect)
            mysql_options(d->mysql, MYSQL_OPT_RECONNECT, &reconnect);
#endif
    } else {
        setLastError(qMakeError(tr("Unable to connect"),
                     QSqlError::ConnectionError, d));
        mysql_close(d->mysql);
        d->mysql = NULL;
        setOpenError(true);
        return false;
    }

#if MYSQL_VERSION_ID >= 50007
    if (mysql_get_client_version() >= 50503 && mysql_get_server_version(d->mysql) >= 50503) {
        // force the communication to be utf8mb4 (only utf8mb4 supports 4-byte characters)
        mysql_set_character_set(d->mysql, "utf8mb4");
#if QT_CONFIG(textcodec)
        d->tc = QTextCodec::codecForName("UTF-8");
#endif
    } else
    {
        // force the communication to be utf8
        mysql_set_character_set(d->mysql, "utf8");
#if QT_CONFIG(textcodec)
        d->tc = codec(d->mysql);
#endif
    }
#endif  // MYSQL_VERSION_ID >= 50007

    d->preparedQuerysEnabled = checkPreparedQueries(d->mysql);

#if QT_CONFIG(thread)
    mysql_thread_init();
#endif

    setOpen(true);
    setOpenError(false);
    return true;
}

void QMYSQLDriver::close()
{
    Q_D(QMYSQLDriver);
    if (isOpen()) {
#if QT_CONFIG(thread)
        mysql_thread_end();
#endif
        mysql_close(d->mysql);
        d->mysql = NULL;
        setOpen(false);
        setOpenError(false);
    }
}

QSqlResult *QMYSQLDriver::createResult() const
{
    return new QMYSQLResult(this);
}

QStringList QMYSQLDriver::tables(QSql::TableType type) const
{
    Q_D(const QMYSQLDriver);
    QStringList tl;
    QSqlQuery q(createResult());
    if (type & QSql::Tables) {
        QString sql = QLatin1String("select table_name from information_schema.tables where table_schema = '") + QLatin1String(d->mysql->db) + QLatin1String("' and table_type = 'BASE TABLE'");
        q.exec(sql);

        while (q.next())
            tl.append(q.value(0).toString());
    }
    if (type & QSql::Views) {
        QString sql = QLatin1String("select table_name from information_schema.tables where table_schema = '") + QLatin1String(d->mysql->db) + QLatin1String("' and table_type = 'VIEW'");
        q.exec(sql);

        while (q.next())
            tl.append(q.value(0).toString());
    }
    return tl;
}

QSqlIndex QMYSQLDriver::primaryIndex(const QString& tablename) const
{
    QSqlIndex idx;
    if (!isOpen())
        return idx;

    QSqlQuery i(createResult());
    QString stmt(QLatin1String("show index from %1;"));
    QSqlRecord fil = record(tablename);
    i.exec(stmt.arg(escapeIdentifier(tablename, QSqlDriver::TableName)));
    while (i.isActive() && i.next()) {
        if (i.value(2).toString() == QLatin1String("PRIMARY")) {
            idx.append(fil.field(i.value(4).toString()));
            idx.setCursorName(i.value(0).toString());
            idx.setName(i.value(2).toString());
        }
    }

    return idx;
}

QSqlRecord QMYSQLDriver::record(const QString& tablename) const
{
    Q_D(const QMYSQLDriver);
    QString table=tablename;
    if(isIdentifierEscaped(table, QSqlDriver::TableName))
        table = stripDelimiters(table, QSqlDriver::TableName);

    QSqlRecord info;
    if (!isOpen())
        return info;
    MYSQL_RES* r = mysql_list_fields(d->mysql, table.toLocal8Bit().constData(), 0);
    if (!r) {
        return info;
    }
    MYSQL_FIELD* field;

    while ((field = mysql_fetch_field(r)))
        info.append(qToField(field, d->tc));
    mysql_free_result(r);
    return info;
}

QVariant QMYSQLDriver::handle() const
{
    Q_D(const QMYSQLDriver);
    return QVariant::fromValue(d->mysql);
}

bool QMYSQLDriver::beginTransaction()
{
    Q_D(QMYSQLDriver);
#ifndef CLIENT_TRANSACTIONS
    return false;
#endif
    if (!isOpen()) {
        qWarning("QMYSQLDriver::beginTransaction: Database not open");
        return false;
    }
    if (mysql_query(d->mysql, "BEGIN WORK")) {
        setLastError(qMakeError(tr("Unable to begin transaction"),
                                QSqlError::StatementError, d));
        return false;
    }
    return true;
}

bool QMYSQLDriver::commitTransaction()
{
    Q_D(QMYSQLDriver);
#ifndef CLIENT_TRANSACTIONS
    return false;
#endif
    if (!isOpen()) {
        qWarning("QMYSQLDriver::commitTransaction: Database not open");
        return false;
    }
    if (mysql_query(d->mysql, "COMMIT")) {
        setLastError(qMakeError(tr("Unable to commit transaction"),
                                QSqlError::StatementError, d));
        return false;
    }
    return true;
}

bool QMYSQLDriver::rollbackTransaction()
{
    Q_D(QMYSQLDriver);
#ifndef CLIENT_TRANSACTIONS
    return false;
#endif
    if (!isOpen()) {
        qWarning("QMYSQLDriver::rollbackTransaction: Database not open");
        return false;
    }
    if (mysql_query(d->mysql, "ROLLBACK")) {
        setLastError(qMakeError(tr("Unable to rollback transaction"),
                                QSqlError::StatementError, d));
        return false;
    }
    return true;
}

QString QMYSQLDriver::formatValue(const QSqlField &field, bool trimStrings) const
{
    Q_D(const QMYSQLDriver);
    QString r;
    if (field.isNull()) {
        r = QStringLiteral("NULL");
    } else {
        switch (+field.type()) {
        case QMetaType::Double:
            r = QString::number(field.value().toDouble(), 'g', field.precision());
            break;
        case QMetaType::QString:
            // Escape '\' characters
            r = QSqlDriver::formatValue(field, trimStrings);
            r.replace(QLatin1String("\\"), QLatin1String("\\\\"));
            break;
        case QMetaType::QByteArray:
            if (isOpen()) {
                const QByteArray ba = field.value().toByteArray();
                // buffer has to be at least length*2+1 bytes
                char* buffer = new char[ba.size() * 2 + 1];
                int escapedSize = int(mysql_real_escape_string(d->mysql, buffer,
                                      ba.data(), ba.size()));
                r.reserve(escapedSize + 3);
                r.append(QLatin1Char('\'')).append(toUnicode(d->tc, buffer)).append(QLatin1Char('\''));
                delete[] buffer;
                break;
            } else {
                qWarning("QMYSQLDriver::formatValue: Database not open");
            }
            // fall through
        default:
            r = QSqlDriver::formatValue(field, trimStrings);
        }
    }
    return r;
}

QString QMYSQLDriver::escapeIdentifier(const QString &identifier, IdentifierType) const
{
    QString res = identifier;
    if(!identifier.isEmpty() && !identifier.startsWith(QLatin1Char('`')) && !identifier.endsWith(QLatin1Char('`')) ) {
        res.prepend(QLatin1Char('`')).append(QLatin1Char('`'));
        res.replace(QLatin1Char('.'), QLatin1String("`.`"));
    }
    return res;
}

bool QMYSQLDriver::isIdentifierEscaped(const QString &identifier, IdentifierType type) const
{
    Q_UNUSED(type);
    return identifier.size() > 2
        && identifier.startsWith(QLatin1Char('`')) //left delimited
        && identifier.endsWith(QLatin1Char('`')); //right delimited
}

QT_END_NAMESPACE

#include "moc_qsql_mysql_p.cpp"
