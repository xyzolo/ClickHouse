/* Copyright (c) 2018 BlackBerry Limited

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTWatchQuery.h>
#include <Parsers/ASTDropQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterDropQuery.h>
#include <DataStreams/NullBlockInputStream.h>
#include <DataStreams/LiveBlockInputStream.h>
#include <DataStreams/MaterializingBlockInputStream.h>
#include <Common/typeid_cast.h>

#include <Storages/StorageLiveView.h>
#include <Storages/StorageFactory.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Interpreters/DatabaseAndTableWithAlias.h>
#include <Interpreters/AddDefaultDatabaseVisitor.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int INCORRECT_QUERY;
    extern const int TABLE_WAS_NOT_DROPPED;
    extern const int QUERY_IS_NOT_SUPPORTED_IN_MATERIALIZED_VIEW;
}


static void extractDependentTable(ASTSelectQuery & query, String & select_database_name, String & select_table_name)
{
    auto db_and_table = getDatabaseAndTable(query, 0);
    ASTPtr subquery = getTableFunctionOrSubquery(query, 0);

    if (!db_and_table && !subquery)
        return;

    if (db_and_table)
    {
        select_table_name = db_and_table->table;

        if (db_and_table->database.empty())
        {
            db_and_table->database = select_database_name;
            AddDefaultDatabaseVisitor visitor(select_database_name);
            visitor.visit(query);
        }
        else
            select_database_name = db_and_table->database;
    }
    else if (auto ast_select = typeid_cast<ASTSelectWithUnionQuery *>(subquery.get()))
    {
        if (ast_select->list_of_selects->children.size() != 1)
            throw Exception("UNION is not supported for LIVE VIEW", ErrorCodes::QUERY_IS_NOT_SUPPORTED_IN_MATERIALIZED_VIEW);

        auto & inner_query = ast_select->list_of_selects->children.at(0);

        extractDependentTable(typeid_cast<ASTSelectQuery &>(*inner_query), select_database_name, select_table_name);
    }
    else
        throw Exception("Logical error while creating StorageLiveView."
                        " Could not retrieve table name from select query.",
                        DB::ErrorCodes::LOGICAL_ERROR);
}


static void checkAllowedQueries(const ASTSelectQuery & query)
{
    if (query.prewhere_expression || query.final() || query.sample_size())
        throw Exception("LIVE VIEW cannot have PREWHERE, SAMPLE or FINAL.", DB::ErrorCodes::QUERY_IS_NOT_SUPPORTED_IN_MATERIALIZED_VIEW);

    ASTPtr subquery = getTableFunctionOrSubquery(query, 0);
    if (!subquery)
        return;

    if (auto ast_select = typeid_cast<const ASTSelectWithUnionQuery *>(subquery.get()))
    {
        if (ast_select->list_of_selects->children.size() != 1)
            throw Exception("UNION is not supported for LIVE VIEW", ErrorCodes::QUERY_IS_NOT_SUPPORTED_IN_MATERIALIZED_VIEW);

        const auto & inner_query = ast_select->list_of_selects->children.at(0);

        checkAllowedQueries(typeid_cast<const ASTSelectQuery &>(*inner_query));
    }
}


StorageLiveView::StorageLiveView(
    const String & table_name_,
    const String & database_name_,
    Context & local_context,
    const ASTCreateQuery & query,
    const ColumnsDescription & columns)
    : IStorage(columns), table_name(table_name_),
    database_name(database_name_), global_context(local_context.getGlobalContext())
{
    if (!query.select)
        throw Exception("SELECT query is not specified for " + getName(), ErrorCodes::INCORRECT_QUERY);

    /// Default value, if only table name exist in the query
    select_database_name = local_context.getCurrentDatabase();
    if (query.select->list_of_selects->children.size() != 1)
        throw Exception("UNION is not supported for LIVE VIEW", ErrorCodes::QUERY_IS_NOT_SUPPORTED_IN_MATERIALIZED_VIEW);

    inner_query = query.select->list_of_selects->children.at(0);

    ASTSelectQuery & select_query = typeid_cast<ASTSelectQuery &>(*inner_query);
    extractDependentTable(select_query, select_database_name, select_table_name);

    if (!select_table_name.empty())
        global_context.addDependency(
            DatabaseAndTableName(select_database_name, select_table_name),
            DatabaseAndTableName(database_name, table_name));

    is_temporary = query.temporary;
    auto storage = local_context.getTable(select_database_name, select_table_name);
    sample_block = InterpreterSelectQuery(inner_query, local_context, storage).getSampleBlock();

    blocks_ptr = std::make_shared<BlocksPtr>();
    active_ptr = std::make_shared<bool>(true);
}

bool StorageLiveView::getNewBlocks()
{
    Block block;
    SipHash hash;
    UInt128 key;
    BlocksPtr new_blocks = std::make_shared<Blocks>();
    BlocksPtr new_mergeable_blocks = std::make_shared<Blocks>();

    InterpreterSelectQuery interpreter{inner_query->clone(), global_context, Names(), QueryProcessingStage::WithMergeableState};
    auto mergeable_stream = std::make_shared<MaterializingBlockInputStream>(interpreter.execute().in);

    while (Block block = mergeable_stream->read())
        new_mergeable_blocks->push_back(block);

    mergeable_blocks = std::make_shared<std::vector<BlocksPtr>>();
    mergeable_blocks->push_back(new_mergeable_blocks);

    BlockInputStreamPtr from = std::make_shared<BlocksBlockInputStream>(std::make_shared<BlocksPtr>(new_mergeable_blocks), sample_block);
    //auto proxy_storage = createProxyStorage(global_context.getTable(select_database_name, select_table_name), {from});
    InterpreterSelectQuery select(inner_query->clone(), global_context, from, QueryProcessingStage::Complete);
    BlockInputStreamPtr data = std::make_shared<MaterializingBlockInputStream>(select.execute().in);

    while (Block block = data->read())
    {
        block.updateHash(hash);
        new_blocks->push_back(block);
    }

    /// mark last block as end of frame
    if (!new_blocks->empty())
        new_blocks->back().info.is_end_frame = true;
    hash.get128(key.low, key.high);

    /// Update blocks only if hash keys do not match
    /// NOTE: hash could be different for the same result
    ///       if blocks are not in the same order
    bool updated = false;
    {
        if (hash_key != key.toHexString())
        {
            if (new_blocks->empty())
            {
                new_blocks->push_back(getSampleBlock());
                new_blocks->back().info.is_end_frame = true;
            }
            if (!new_blocks->empty())
                new_blocks->front().info.hash = key.toHexString();
            (*blocks_ptr) = new_blocks;
            hash_key = key.toHexString();
            updated = true;
        }
    }
    return updated;
}

void StorageLiveView::checkTableCanBeDropped() const
{
    Dependencies dependencies = global_context.getDependencies(database_name, table_name);
    if (!dependencies.empty())
    {
        DatabaseAndTableName database_and_table_name = dependencies.front();
        throw Exception("Table has dependency " + database_and_table_name.first + "." + database_and_table_name.second, ErrorCodes::TABLE_WAS_NOT_DROPPED);
    }
}

void StorageLiveView::noUsersThread()
{
    if (shutdown_called)
        return;

    bool drop_table = false;

    {
        Poco::FastMutex::ScopedLock lock(noUsersThreadMutex);
        while (1)
        {
            if (!noUsersThreadWakeUp && !noUsersThreadCondition.tryWait(noUsersThreadMutex, global_context.getSettingsRef().temporary_live_view_timeout.totalSeconds() * 1000))
            {
                noUsersThreadWakeUp = false;
                if (shutdown_called)
                    return;
                if (hasUsers())
                    return;
                if (!global_context.getDependencies(database_name, table_name).empty())
                    continue;
                drop_table = true;
            }
            break;
        }
    }

    if (drop_table)
    {
        if ( global_context.tryGetTable(database_name, table_name) )
        {
            try
            {
                /// We create and execute `drop` query for this table
                auto drop_query = std::make_shared<ASTDropQuery>();
                drop_query->database = database_name;
                drop_query->table = table_name;
                ASTPtr ast_drop_query = drop_query;
                InterpreterDropQuery drop_interpreter(ast_drop_query, global_context);
                drop_interpreter.execute();
            }
            catch(...)
            {
            }
        }
    }
}

void StorageLiveView::startNoUsersThread()
{
    bool expected = false;
    if (!startnousersthread_called.compare_exchange_strong(expected, true))
        return;

    if (is_dropped)
        return;

    if (is_temporary)
    {
        if (no_users_thread.joinable())
        {
            {
                Poco::FastMutex::ScopedLock lock(noUsersThreadMutex);
                noUsersThreadWakeUp = true;
                noUsersThreadCondition.signal();
            }
            no_users_thread.join();
        }
        {
            Poco::FastMutex::ScopedLock lock(noUsersThreadMutex);
            noUsersThreadWakeUp = false;
        }
        if (!is_dropped)
            no_users_thread = std::thread(&StorageLiveView::noUsersThread, this);
    }
    startnousersthread_called = false;
}

void StorageLiveView::startup()
{
    startNoUsersThread();
}

void StorageLiveView::shutdown()
{
    bool expected = false;
    if (!shutdown_called.compare_exchange_strong(expected, true))
        return;

    if (no_users_thread.joinable())
    {
        Poco::FastMutex::ScopedLock lock(noUsersThreadMutex);
        noUsersThreadWakeUp = true;
        noUsersThreadCondition.signal();
        /// Must detach the no users thread
        /// as we can't join it as it will result
        /// in a deadlock
        no_users_thread.detach();
    }
}

StorageLiveView::~StorageLiveView()
{
    shutdown();
}

void StorageLiveView::drop()
{
    global_context.removeDependency(
        DatabaseAndTableName(select_database_name, select_table_name),
        DatabaseAndTableName(database_name, table_name));
    Poco::FastMutex::ScopedLock lock(mutex);
    is_dropped = true;
    condition.broadcast();
}

BlockInputStreams StorageLiveView::read(
    const Names & /*column_names*/,
    const SelectQueryInfo & /*query_info*/,
    const Context & /*context*/,
    QueryProcessingStage::Enum /*processed_stage*/,
    const size_t /*max_block_size*/,
    const unsigned /*num_streams*/)
{
    /// add user to the blocks_ptr
    std::shared_ptr<BlocksPtr> stream_blocks_ptr = blocks_ptr;
    {
        Poco::FastMutex::ScopedLock lock(mutex);
        /// Always trigger new read of the blocks so that SELECT can be used
        /// to force blocks refresh if needed
        if ( getNewBlocks() )
            condition.broadcast();
    }
    return { std::make_shared<BlocksBlockInputStream>(stream_blocks_ptr, sample_block) };
}

BlockInputStreams StorageLiveView::watch(
    const Names & /*column_names*/,
    const SelectQueryInfo & query_info,
    const Context & /*context*/,
    QueryProcessingStage::Enum & processed_stage,
    size_t /*max_block_size*/,
    const unsigned /*num_streams*/)
{
    ASTWatchQuery & query = typeid_cast<ASTWatchQuery &>(*query_info.query);

    /// By default infinite stream of updates
    int64_t length = -2;

    if (query.limit_length)
        length = (int64_t)safeGet<UInt64>(typeid_cast<ASTLiteral &>(*query.limit_length).value);

    auto reader = std::make_shared<LiveBlockInputStream>(*this, blocks_ptr, active_ptr, condition, mutex, length);

    if (no_users_thread.joinable())
    {
        Poco::FastMutex::ScopedLock lock(noUsersThreadMutex);
        noUsersThreadWakeUp = true;
        noUsersThreadCondition.signal();
    }

    {
        Poco::FastMutex::ScopedLock lock(mutex);
        if (!(*blocks_ptr))
        {
           if (getNewBlocks())
               condition.broadcast();
        }
    }

    processed_stage = QueryProcessingStage::Complete;

    return { reader };
}

BlockOutputStreamPtr StorageLiveView::write(const ASTPtr & /*query*/, const Settings & /*settings*/)
{
    return std::make_shared<LiveBlockOutputStream>(*this);
}

void registerStorageLiveView(StorageFactory & factory)
{
    factory.registerStorage("LiveView", [](const StorageFactory::Arguments & args)
    {
        return StorageLiveView::create(args.table_name, args.database_name, args.local_context, args.query, args.columns);
    });
}

}
