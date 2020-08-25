#if !defined(ARCADIA_BUILD)
#    include "config_core.h"
#endif

#if USE_MYSQL

#include <Databases/MySQL/MaterializeMySQLSyncThread.h>

#    include <cstdlib>
#    include <random>
#    include <Columns/ColumnTuple.h>
#    include <DataStreams/CountingBlockOutputStream.h>
#    include <DataStreams/OneBlockInputStream.h>
#    include <DataStreams/copyData.h>
#    include <Databases/MySQL/DatabaseMaterializeMySQL.h>
#    include <Databases/MySQL/MaterializeMetadata.h>
#    include <Formats/MySQLBlockInputStream.h>
#    include <IO/ReadBufferFromString.h>
#    include <Interpreters/Context.h>
#    include <Interpreters/executeQuery.h>
#    include <Storages/StorageMergeTree.h>
#    include <Common/quoteString.h>
#    include <Common/setThreadName.h>
#    include <common/sleep.h>
#    include <ext/bit_cast.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int SYNTAX_ERROR;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int ILLEGAL_MYSQL_VARIABLE;
}

static constexpr auto MYSQL_BACKGROUND_THREAD_NAME = "MySQLDBSync";

static Context createQueryContext(const Context & global_context)
{
    Settings new_query_settings = global_context.getSettings();
    new_query_settings.insert_allow_materialized_columns = true;

    Context query_context(global_context);
    query_context.setSettings(new_query_settings);
    CurrentThread::QueryScope query_scope(query_context);

    query_context.getClientInfo().query_kind = ClientInfo::QueryKind::SECONDARY_QUERY;
    query_context.setCurrentQueryId(""); // generate random query_id
    return query_context;
}

static BlockIO tryToExecuteQuery(const String & query_to_execute, Context & query_context, const String & database, const String & comment)
{
    try
    {
        if (!database.empty())
            query_context.setCurrentDatabase(database);

        return executeQuery("/*" + comment + "*/ " + query_to_execute, query_context, true);
    }
    catch (...)
    {
        tryLogCurrentException(
            &Poco::Logger::get("MaterializeMySQLSyncThread(" + database + ")"),
            "Query " + query_to_execute + " wasn't finished successfully");
        throw;
    }
}

static inline DatabaseMaterializeMySQL & getDatabase(const String & database_name)
{
    DatabasePtr database = DatabaseCatalog::instance().getDatabase(database_name);

    if (DatabaseMaterializeMySQL * database_materialize = typeid_cast<DatabaseMaterializeMySQL *>(database.get()))
        return *database_materialize;

    throw Exception("LOGICAL_ERROR: cannot cast to DatabaseMaterializeMySQL, it is a bug.", ErrorCodes::LOGICAL_ERROR);
}

MaterializeMySQLSyncThread::~MaterializeMySQLSyncThread()
{
    try
    {
        stopSynchronization();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

static String checkVariableAndGetVersion(const mysqlxx::Pool::Entry & connection)
{
    Block variables_header{
        {std::make_shared<DataTypeString>(), "Variable_name"},
        {std::make_shared<DataTypeString>(), "Value"}
    };

    const String & check_query = "SHOW VARIABLES WHERE "
         "(Variable_name = 'log_bin' AND upper(Value) = 'ON') "
         "OR (Variable_name = 'binlog_format' AND upper(Value) = 'ROW') "
         "OR (Variable_name = 'binlog_row_image' AND upper(Value) = 'FULL') "
         "OR (Variable_name = 'default_authentication_plugin' AND upper(Value) = 'MYSQL_NATIVE_PASSWORD');";

    MySQLBlockInputStream variables_input(connection, check_query, variables_header, DEFAULT_BLOCK_SIZE);

    Block variables_block = variables_input.read();
    if (!variables_block || variables_block.rows() != 4)
    {
        std::unordered_map<String, String> variables_error_message{
            {"log_bin", "log_bin = 'ON'"},
            {"binlog_format", "binlog_format='ROW'"},
            {"binlog_row_image", "binlog_row_image='FULL'"},
            {"default_authentication_plugin", "default_authentication_plugin='mysql_native_password'"}
        };
        ColumnPtr variable_name_column = variables_block.getByName("Variable_name").column;

        for (size_t index = 0; index < variables_block.rows(); ++index)
        {
            const auto & error_message_it = variables_error_message.find(variable_name_column->getDataAt(index).toString());

            if (error_message_it != variables_error_message.end())
                variables_error_message.erase(error_message_it);
        }

        bool first = true;
        std::stringstream error_message;
        error_message << "Illegal MySQL variables, the MaterializeMySQL engine requires ";
        for (const auto & [variable_name, variable_error_message] : variables_error_message)
        {
            error_message << (first ? "" : ", ") << variable_error_message;

            if (first)
                first = false;
        }

        throw Exception(error_message.str(), ErrorCodes::ILLEGAL_MYSQL_VARIABLE);
    }

    Block version_header{{std::make_shared<DataTypeString>(), "version"}};
    MySQLBlockInputStream version_input(connection, "SELECT version() AS version;", version_header, DEFAULT_BLOCK_SIZE);

    Block version_block = version_input.read();
    if (!version_block || version_block.rows() != 1)
        throw Exception("LOGICAL ERROR: cannot get mysql version.", ErrorCodes::LOGICAL_ERROR);

    return version_block.getByPosition(0).column->getDataAt(0).toString();
}

MaterializeMySQLSyncThread::MaterializeMySQLSyncThread(
    const Context & context, const String & database_name_, const String & mysql_database_name_,
    mysqlxx::Pool && pool_, MySQLClient && client_, MaterializeMySQLSettings * settings_)
    : log(&Poco::Logger::get("MaterializeMySQLSyncThread")), global_context(context.getGlobalContext()), database_name(database_name_)
    , mysql_database_name(mysql_database_name_), pool(std::move(pool_)), client(std::move(client_)), settings(settings_)
{
    query_prefix = "EXTERNAL DDL FROM MySQL(" + backQuoteIfNeed(database_name) + ", " + backQuoteIfNeed(mysql_database_name) + ") ";
}

void MaterializeMySQLSyncThread::synchronization(const String & mysql_version)
{
    setThreadName(MYSQL_BACKGROUND_THREAD_NAME);

    try
    {
        if (std::optional<MaterializeMetadata> metadata = prepareSynchronized(mysql_version))
        {
            Stopwatch watch;
            Buffers buffers(database_name);

            while (!isCancelled())
            {
                /// TODO: add gc task for `sign = -1`(use alter table delete, execute by interval. need final state)
                UInt64 max_flush_time = settings->max_flush_data_time;
                BinlogEventPtr binlog_event = client.readOneBinlogEvent(std::max(UInt64(1), max_flush_time - watch.elapsedMilliseconds()));

                {
                    if (binlog_event)
                        onEvent(buffers, binlog_event, *metadata);

                    if (watch.elapsedMilliseconds() > max_flush_time || buffers.checkThresholds(
                            settings->max_rows_in_buffer, settings->max_bytes_in_buffer,
                            settings->max_rows_in_buffers, settings->max_bytes_in_buffers)
                        )
                    {
                        watch.restart();

                        if (!buffers.data.empty())
                            flushBuffersData(buffers, *metadata);
                    }
                }
            }
        }
    }
    catch (...)
    {
        tryLogCurrentException(log);
        getDatabase(database_name).setException(std::current_exception());
    }
}

void MaterializeMySQLSyncThread::stopSynchronization()
{
    if (!sync_quit && background_thread_pool)
    {
        sync_quit = true;
        background_thread_pool->join();
    }
}

void MaterializeMySQLSyncThread::startSynchronization()
{
    const auto & mysql_server_version = checkVariableAndGetVersion(pool.get());

    background_thread_pool = std::make_unique<ThreadFromGlobalPool>(
        [this, mysql_server_version = mysql_server_version]() { synchronization(mysql_server_version); });
}

static inline void cleanOutdatedTables(const String & database_name, const Context & context)
{
    auto ddl_guard = DatabaseCatalog::instance().getDDLGuard(database_name, "");
    const DatabasePtr & clean_database = DatabaseCatalog::instance().getDatabase(database_name);

    for (auto iterator = clean_database->getTablesIterator(context); iterator->isValid(); iterator->next())
    {
        Context query_context = createQueryContext(context);
        String comment = "Materialize MySQL step 1: execute MySQL DDL for dump data";
        String table_name = backQuoteIfNeed(database_name) + "." + backQuoteIfNeed(iterator->name());
        tryToExecuteQuery(" DROP TABLE " + table_name, query_context, database_name, comment);
    }
}

static inline BlockOutputStreamPtr getTableOutput(const String & database_name, const String & table_name, Context & query_context, bool insert_materialized = false)
{
    const StoragePtr & storage = DatabaseCatalog::instance().getTable(StorageID(database_name, table_name), query_context);

    std::stringstream insert_columns_str;
    const StorageInMemoryMetadata & storage_metadata = storage->getInMemoryMetadata();
    const ColumnsDescription & storage_columns = storage_metadata.getColumns();
    const NamesAndTypesList & insert_columns_names = insert_materialized ? storage_columns.getAllPhysical() : storage_columns.getOrdinary();


    for (auto iterator = insert_columns_names.begin(); iterator != insert_columns_names.end(); ++iterator)
    {
        if (iterator != insert_columns_names.begin())
            insert_columns_str << ", ";

        insert_columns_str << iterator->name;
    }


    String comment = "Materialize MySQL step 1: execute dump data";
    BlockIO res = tryToExecuteQuery("INSERT INTO " + backQuoteIfNeed(table_name) + "(" + insert_columns_str.str() + ")" + " VALUES",
        query_context, database_name, comment);

    if (!res.out)
        throw Exception("LOGICAL ERROR: It is a bug.", ErrorCodes::LOGICAL_ERROR);

    return res.out;
}

static inline void dumpDataForTables(
    mysqlxx::Pool::Entry & connection, MaterializeMetadata & master_info,
    const String & query_prefix, const String & database_name, const String & mysql_database_name,
    const Context & context, const std::function<bool()> & is_cancelled)
{
    auto iterator = master_info.need_dumping_tables.begin();
    for (; iterator != master_info.need_dumping_tables.end() && !is_cancelled(); ++iterator)
    {
        const auto & table_name = iterator->first;
        Context query_context = createQueryContext(context);
        String comment = "Materialize MySQL step 1: execute MySQL DDL for dump data";
        tryToExecuteQuery(query_prefix + " " + iterator->second, query_context, database_name, comment); /// create table.

        auto out = std::make_shared<CountingBlockOutputStream>(getTableOutput(database_name, table_name, query_context));
        MySQLBlockInputStream input(
            connection, "SELECT * FROM " + backQuoteIfNeed(mysql_database_name) + "." + backQuoteIfNeed(table_name),
            out->getHeader(), DEFAULT_BLOCK_SIZE);

        Stopwatch watch;
        copyData(input, *out, is_cancelled);
        const Progress & progress = out->getProgress();
        LOG_INFO(&Poco::Logger::get("MaterializeMySQLSyncThread(" + database_name + ")"),
            "Materialize MySQL step 1: dump {}, {} rows, {} in {} sec., {} rows/sec., {}/sec."
            , table_name, formatReadableQuantity(progress.written_rows), formatReadableSizeWithBinarySuffix(progress.written_bytes)
            , watch.elapsedSeconds(), formatReadableQuantity(static_cast<size_t>(progress.written_rows / watch.elapsedSeconds()))
            , formatReadableSizeWithBinarySuffix(static_cast<size_t>(progress.written_bytes / watch.elapsedSeconds())));
    }
}

static inline UInt32 randomNumber()
{
    std::mt19937 rng;
    rng.seed(std::random_device()());
    std::uniform_int_distribution<std::mt19937::result_type> dist6(std::numeric_limits<UInt32>::min(), std::numeric_limits<UInt32>::max());
    return dist6(rng);
}

std::optional<MaterializeMetadata> MaterializeMySQLSyncThread::prepareSynchronized(const String & mysql_version)
{
    bool opened_transaction = false;
    mysqlxx::PoolWithFailover::Entry connection;

    while (!isCancelled())
    {
        try
        {
            connection = pool.get();
            opened_transaction = false;

            MaterializeMetadata metadata(
                connection, getDatabase(database_name).getMetadataPath() + "/.metadata",
                mysql_database_name, opened_transaction, mysql_version);

            if (!metadata.need_dumping_tables.empty())
            {
                metadata.transaction(Position(metadata.binlog_position, metadata.binlog_file), [&]()
                {
                    cleanOutdatedTables(database_name, global_context);
                    dumpDataForTables(connection, metadata, query_prefix, database_name, mysql_database_name, global_context, [this] { return isCancelled(); });
                });
            }

            if (opened_transaction)
                connection->query("COMMIT").execute();

            client.connect();
            client.startBinlogDump(randomNumber(), mysql_database_name, metadata.binlog_file, metadata.binlog_position);
            return metadata;
        }
        catch (...)
        {
            tryLogCurrentException(log);

            if (opened_transaction)
                connection->query("ROLLBACK").execute();

            try
            {
                throw;
            }
            catch (const mysqlxx::ConnectionFailed &)
            {
                /// Avoid busy loop when MySQL is not available.
                sleepForMilliseconds(settings->max_wait_time_when_mysql_unavailable);
            }
        }
    }

    return {};
}

void MaterializeMySQLSyncThread::flushBuffersData(Buffers & buffers, MaterializeMetadata & metadata)
{
    metadata.transaction(client.getPosition(), [&]() { buffers.commit(global_context); });
}

static inline void fillSignAndVersionColumnsData(Block & data, Int8 sign_value, UInt64 version_value, size_t fill_size)
{
    MutableColumnPtr sign_mutable_column = IColumn::mutate(std::move(data.getByPosition(data.columns() - 2).column));
    MutableColumnPtr version_mutable_column = IColumn::mutate(std::move(data.getByPosition(data.columns() - 1).column));

    ColumnInt8::Container & sign_column_data = assert_cast<ColumnInt8 &>(*sign_mutable_column).getData();
    ColumnUInt64::Container & version_column_data = assert_cast<ColumnUInt64 &>(*version_mutable_column).getData();

    for (size_t index = 0; index < fill_size; ++index)
    {
        sign_column_data.emplace_back(sign_value);
        version_column_data.emplace_back(version_value);
    }

    data.getByPosition(data.columns() - 2).column = std::move(sign_mutable_column);
    data.getByPosition(data.columns() - 1).column = std::move(version_mutable_column);
}

template <bool assert_nullable = false>
static void writeFieldsToColumn(
    IColumn & column_to, const std::vector<Field> & rows_data, size_t column_index, const std::vector<bool> & mask, ColumnUInt8 * null_map_column = nullptr)
{
    if (ColumnNullable * column_nullable = typeid_cast<ColumnNullable *>(&column_to))
        writeFieldsToColumn<true>(column_nullable->getNestedColumn(), rows_data, column_index, mask, &column_nullable->getNullMapColumn());
    else
    {
        const auto & write_data_to_null_map = [&](const Field & field, size_t row_index)
        {
            if (!mask.empty() && !mask.at(row_index))
                return false;

            if constexpr (assert_nullable)
            {
                if (field.isNull())
                {
                    column_to.insertDefault();
                    null_map_column->insertValue(1);
                    return false;
                }

                null_map_column->insertValue(0);
            }

            return true;
        };

        const auto & write_data_to_column = [&](auto * casted_column, auto from_type, auto to_type)
        {
            for (size_t index = 0; index < rows_data.size(); ++index)
            {
                const Tuple & row_data = DB::get<const Tuple &>(rows_data[index]);
                const Field & value = row_data[column_index];

                if (write_data_to_null_map(value, index))
                    casted_column->insertValue(static_cast<decltype(to_type)>(value.template get<decltype(from_type)>()));
            }
        };

        if (ColumnInt8 * casted_int8_column = typeid_cast<ColumnInt8 *>(&column_to))
            write_data_to_column(casted_int8_column, UInt64(), Int8());
        else if (ColumnInt16 * casted_int16_column = typeid_cast<ColumnInt16 *>(&column_to))
            write_data_to_column(casted_int16_column, UInt64(), Int16());
        else if (ColumnInt64 * casted_int64_column = typeid_cast<ColumnInt64 *>(&column_to))
            write_data_to_column(casted_int64_column, UInt64(), Int64());
        else if (ColumnUInt8 * casted_uint8_column = typeid_cast<ColumnUInt8 *>(&column_to))
            write_data_to_column(casted_uint8_column, UInt64(), UInt8());
        else if (ColumnUInt16 * casted_uint16_column = typeid_cast<ColumnUInt16 *>(&column_to))
            write_data_to_column(casted_uint16_column, UInt64(), UInt16());
        else if (ColumnUInt32 * casted_uint32_column = typeid_cast<ColumnUInt32 *>(&column_to))
            write_data_to_column(casted_uint32_column, UInt64(), UInt32());
        else if (ColumnUInt64 * casted_uint64_column = typeid_cast<ColumnUInt64 *>(&column_to))
            write_data_to_column(casted_uint64_column, UInt64(), UInt64());
        else if (ColumnFloat32 * casted_float32_column = typeid_cast<ColumnFloat32 *>(&column_to))
            write_data_to_column(casted_float32_column, Float64(), Float32());
        else if (ColumnFloat64 * casted_float64_column = typeid_cast<ColumnFloat64 *>(&column_to))
            write_data_to_column(casted_float64_column, Float64(), Float64());
        else if (ColumnInt32 * casted_int32_column = typeid_cast<ColumnInt32 *>(&column_to))
        {
            for (size_t index = 0; index < rows_data.size(); ++index)
            {
                const Tuple & row_data = DB::get<const Tuple &>(rows_data[index]);
                const Field & value = row_data[column_index];

                if (write_data_to_null_map(value, index))
                {
                    if (value.getType() == Field::Types::UInt64)
                        casted_int32_column->insertValue(value.get<Int32>());
                    else if (value.getType() == Field::Types::Int64)
                    {
                        /// For MYSQL_TYPE_INT24
                        const Int32 & num = value.get<Int32>();
                        casted_int32_column->insertValue(num & 0x800000 ? num | 0xFF000000 : num);
                    }
                    else
                        throw Exception("LOGICAL ERROR: it is a bug.", ErrorCodes::LOGICAL_ERROR);
                }
            }
        }
        else if (ColumnString * casted_string_column = typeid_cast<ColumnString *>(&column_to))
        {
            for (size_t index = 0; index < rows_data.size(); ++index)
            {
                const Tuple & row_data = DB::get<const Tuple &>(rows_data[index]);
                const Field & value = row_data[column_index];

                if (write_data_to_null_map(value, index))
                {
                    const String & data = value.get<const String &>();
                    casted_string_column->insertData(data.data(), data.size());
                }
            }
        }
        else if (ColumnFixedString * casted_fixed_string_column = typeid_cast<ColumnFixedString *>(&column_to))
        {
            for (size_t index = 0; index < rows_data.size(); ++index)
            {
                const Tuple & row_data = DB::get<const Tuple &>(rows_data[index]);
                const Field & value = row_data[column_index];

                if (write_data_to_null_map(value, index))
                {
                    const String & data = value.get<const String &>();
                    casted_fixed_string_column->insertData(data.data(), data.size());
                }
            }
        }
        else
            throw Exception("Unsupported data type from MySQL.", ErrorCodes::NOT_IMPLEMENTED);
    }
}

template <Int8 sign>
static size_t onWriteOrDeleteData(const std::vector<Field> & rows_data, Block & buffer, size_t version)
{
    size_t prev_bytes = buffer.bytes();
    for (size_t column = 0; column < buffer.columns() - 2; ++column)
    {
        MutableColumnPtr col_to = IColumn::mutate(std::move(buffer.getByPosition(column).column));

        writeFieldsToColumn(*col_to, rows_data, column, {});
        buffer.getByPosition(column).column = std::move(col_to);
    }

    fillSignAndVersionColumnsData(buffer, sign, version, rows_data.size());
    return buffer.bytes() - prev_bytes;
}

static inline bool differenceSortingKeys(const Tuple & row_old_data, const Tuple & row_new_data, const std::vector<size_t> sorting_columns_index)
{
    for (const auto & sorting_column_index : sorting_columns_index)
        if (row_old_data[sorting_column_index] != row_new_data[sorting_column_index])
            return true;

    return false;
}

static inline size_t onUpdateData(const std::vector<Field> & rows_data, Block & buffer, size_t version, const std::vector<size_t> & sorting_columns_index)
{
    if (rows_data.size() % 2 != 0)
        throw Exception("LOGICAL ERROR: It is a bug.", ErrorCodes::LOGICAL_ERROR);

    size_t prev_bytes = buffer.bytes();
    std::vector<bool> writeable_rows_mask(rows_data.size());

    for (size_t index = 0; index < rows_data.size(); index += 2)
    {
        writeable_rows_mask[index + 1] = true;
        writeable_rows_mask[index] = differenceSortingKeys(
            DB::get<const Tuple &>(rows_data[index]), DB::get<const Tuple &>(rows_data[index + 1]), sorting_columns_index);
    }

    for (size_t column = 0; column < buffer.columns() - 2; ++column)
    {
        MutableColumnPtr col_to = IColumn::mutate(std::move(buffer.getByPosition(column).column));

        writeFieldsToColumn(*col_to, rows_data, column, writeable_rows_mask);
        buffer.getByPosition(column).column = std::move(col_to);
    }

    MutableColumnPtr sign_mutable_column = IColumn::mutate(std::move(buffer.getByPosition(buffer.columns() - 2).column));
    MutableColumnPtr version_mutable_column = IColumn::mutate(std::move(buffer.getByPosition(buffer.columns() - 1).column));

    ColumnInt8::Container & sign_column_data = assert_cast<ColumnInt8 &>(*sign_mutable_column).getData();
    ColumnUInt64::Container & version_column_data = assert_cast<ColumnUInt64 &>(*version_mutable_column).getData();

    for (size_t index = 0; index < rows_data.size(); index += 2)
    {
        if (likely(!writeable_rows_mask[index]))
        {
            sign_column_data.emplace_back(1);
            version_column_data.emplace_back(version);
        }
        else
        {
            /// If the sorting keys is modified, we should cancel the old data, but this should not happen frequently
            sign_column_data.emplace_back(-1);
            sign_column_data.emplace_back(1);
            version_column_data.emplace_back(version);
            version_column_data.emplace_back(version);
        }
    }

    buffer.getByPosition(buffer.columns() - 2).column = std::move(sign_mutable_column);
    buffer.getByPosition(buffer.columns() - 1).column = std::move(version_mutable_column);
    return buffer.bytes() - prev_bytes;
}

void MaterializeMySQLSyncThread::onEvent(Buffers & buffers, const BinlogEventPtr & receive_event, MaterializeMetadata & metadata)
{
    if (receive_event->type() == MYSQL_WRITE_ROWS_EVENT)
    {
        WriteRowsEvent & write_rows_event = static_cast<WriteRowsEvent &>(*receive_event);
        Buffers::BufferAndSortingColumnsPtr buffer = buffers.getTableDataBuffer(write_rows_event.table, global_context);
        size_t bytes = onWriteOrDeleteData<1>(write_rows_event.rows, buffer->first, ++metadata.version);
        buffers.add(buffer->first.rows(), buffer->first.bytes(), write_rows_event.rows.size(), bytes);
    }
    else if (receive_event->type() == MYSQL_UPDATE_ROWS_EVENT)
    {
        UpdateRowsEvent & update_rows_event = static_cast<UpdateRowsEvent &>(*receive_event);
        Buffers::BufferAndSortingColumnsPtr buffer = buffers.getTableDataBuffer(update_rows_event.table, global_context);
        size_t bytes = onUpdateData(update_rows_event.rows, buffer->first, ++metadata.version, buffer->second);
        buffers.add(buffer->first.rows(), buffer->first.bytes(), update_rows_event.rows.size(), bytes);
    }
    else if (receive_event->type() == MYSQL_DELETE_ROWS_EVENT)
    {
        DeleteRowsEvent & delete_rows_event = static_cast<DeleteRowsEvent &>(*receive_event);
        Buffers::BufferAndSortingColumnsPtr buffer = buffers.getTableDataBuffer(delete_rows_event.table, global_context);
        size_t bytes = onWriteOrDeleteData<-1>(delete_rows_event.rows, buffer->first, ++metadata.version);
        buffers.add(buffer->first.rows(), buffer->first.bytes(), delete_rows_event.rows.size(), bytes);
    }
    else if (receive_event->type() == MYSQL_QUERY_EVENT)
    {
        QueryEvent & query_event = static_cast<QueryEvent &>(*receive_event);
        flushBuffersData(buffers, metadata);

        try
        {
            Context query_context = createQueryContext(global_context);
            String comment = "Materialize MySQL step 2: execute MySQL DDL for sync data";
            String event_database = query_event.schema == mysql_database_name ? database_name : "";
            tryToExecuteQuery(query_prefix + query_event.query, query_context, event_database, comment);
        }
        catch (Exception & exception)
        {
            tryLogCurrentException(log);

            /// If some DDL query was not successfully parsed and executed
            /// Then replication may fail on next binlog events anyway
            if (exception.code() != ErrorCodes::SYNTAX_ERROR)
                throw;
        }
    }
    else if (receive_event->header.type != HEARTBEAT_EVENT)
    {
        const auto & dump_event_message = [&]()
        {
            std::stringstream ss;
            receive_event->dump(ss);
            return ss.str();
        };

        LOG_DEBUG(log, "Skip MySQL event: \n {}", dump_event_message());
    }
}

bool MaterializeMySQLSyncThread::isMySQLSyncThread()
{
    return getThreadName() == MYSQL_BACKGROUND_THREAD_NAME;
}

void MaterializeMySQLSyncThread::Buffers::add(size_t block_rows, size_t block_bytes, size_t written_rows, size_t written_bytes)
{
    total_blocks_rows += written_rows;
    total_blocks_bytes += written_bytes;
    max_block_rows = std::max(block_rows, max_block_rows);
    max_block_bytes = std::max(block_bytes, max_block_bytes);
}

bool MaterializeMySQLSyncThread::Buffers::checkThresholds(size_t check_block_rows, size_t check_block_bytes, size_t check_total_rows, size_t check_total_bytes) const
{
    return max_block_rows >= check_block_rows || max_block_bytes >= check_block_bytes || total_blocks_rows >= check_total_rows
        || total_blocks_bytes >= check_total_bytes;
}

void MaterializeMySQLSyncThread::Buffers::commit(const Context & context)
{
    try
    {
        for (auto & table_name_and_buffer : data)
        {
            Context query_context = createQueryContext(context);
            OneBlockInputStream input(table_name_and_buffer.second->first);
            BlockOutputStreamPtr out = getTableOutput(database, table_name_and_buffer.first, query_context, true);
            copyData(input, *out);
        }

        data.clear();
        max_block_rows = 0;
        max_block_bytes = 0;
        total_blocks_rows = 0;
        total_blocks_bytes = 0;
    }
    catch (...)
    {
        data.clear();
        throw;
    }
}

MaterializeMySQLSyncThread::Buffers::BufferAndSortingColumnsPtr MaterializeMySQLSyncThread::Buffers::getTableDataBuffer(
    const String & table_name, const Context & context)
{
    const auto & iterator = data.find(table_name);
    if (iterator == data.end())
    {
        StoragePtr storage = DatabaseCatalog::instance().getTable(StorageID(database, table_name), context);

        const StorageInMemoryMetadata & metadata = storage->getInMemoryMetadata();
        BufferAndSortingColumnsPtr & buffer_and_soring_columns = data.try_emplace(
            table_name, std::make_shared<BufferAndSortingColumns>(metadata.getSampleBlock(), std::vector<size_t>{})).first->second;

        Names required_for_sorting_key = metadata.getColumnsRequiredForSortingKey();

        for (const auto & required_name_for_sorting_key : required_for_sorting_key)
            buffer_and_soring_columns->second.emplace_back(
                buffer_and_soring_columns->first.getPositionByName(required_name_for_sorting_key));

        return buffer_and_soring_columns;
    }

    return iterator->second;
}

}

#endif