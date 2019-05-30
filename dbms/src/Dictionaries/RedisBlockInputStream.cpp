#include <Common/config.h>
#if USE_POCO_REDIS

#    include <sstream>
#    include <string>
#    include <vector>

#    include <Poco/Redis/Array.h>
#    include <Poco/Redis/Client.h>
#    include <Poco/Redis/Command.h>
#    include <Poco/Redis/Type.h>

#    include <Columns/ColumnNullable.h>
#    include <Columns/ColumnString.h>
#    include <Columns/ColumnsNumber.h>
#    include <IO/ReadHelpers.h>
#    include <IO/WriteHelpers.h>
#    include <Common/FieldVisitors.h>
#    include <ext/range.h>
#    include "DictionaryStructure.h"
#    include "RedisBlockInputStream.h"


namespace DB
{
    namespace ErrorCodes
    {
        extern const int TYPE_MISMATCH;
        extern const int LOGICAL_ERROR;
        extern const int LIMIT_EXCEEDED;
        extern const int NUMBER_OF_COLUMNS_DOESNT_MATCH;
    }


    RedisBlockInputStream::RedisBlockInputStream(
            const std::shared_ptr<Poco::Redis::Client> & client_,
            const Poco::Redis::Array & keys_,
            const DB::Block & sample_block,
            const size_t max_block_size)
            : client(client_), keys(keys_), max_block_size{max_block_size}
    {
        description.init(sample_block);
    }

    RedisBlockInputStream::~RedisBlockInputStream() = default;


    namespace
    {
        using ValueType = ExternalResultDescription::ValueType;
        using RedisArray = Poco::Redis::Array;

        bool isNull(const Poco::Redis::RedisType::Ptr & value)
        {
            return value->isBulkString() &&
                static_cast<const Poco::Redis::Type<Poco::Redis::BulkString> *>(value.get())->value().isNull();
        }

        std::string getStringOrThrow(const Poco::Redis::RedisType::Ptr & value, const std::string & column_name)
        {
            switch (value->type())
            {
                case Poco::Redis::RedisTypeTraits<Poco::Redis::BulkString>::TypeId:
                {
                    const auto & bs = static_cast<const Poco::Redis::Type<Poco::Redis::BulkString> *>(value.get())->value();
                    if (bs.isNull())
                        throw Exception{"Type mismatch, expected not null String for column " + column_name,
                                        ErrorCodes::TYPE_MISMATCH};
                    return bs.value();
                }
                case Poco::Redis::RedisTypeTraits<std::string>::TypeId:
                    return static_cast<const Poco::Redis::Type<std::string> *>(value.get())->value();
                default:
                    throw Exception{"Type mismatch, expected std::string, got type id = " + toString(value->type()) + " for column " + column_name,
                                    ErrorCodes::TYPE_MISMATCH};
            }
        }

        template <typename T>
        inline void insert(IColumn & column, const String & stringValue)
        {
            static_cast<ColumnVector<T> &>(column).insertValue(parse<T>(stringValue));
        }

        void insertValue(IColumn & column, const ValueType type, const Poco::Redis::RedisType::Ptr & value, const std::string & name)
        {
            String stringValue = getStringOrThrow(value, name);

            switch (type)
            {
                case ValueType::UInt8:
                    insert<UInt8>(column, stringValue);
                    break;
                case ValueType::UInt16:
                    insert<UInt16>(column, stringValue);
                    break;
                case ValueType::UInt32:
                    insert<UInt32>(column, stringValue);
                    break;
                case ValueType::UInt64:
                    insert<UInt64>(column, stringValue);
                    break;
                case ValueType::Int8:
                    insert<Int8>(column, stringValue);
                    break;
                case ValueType::Int16:
                    insert<Int16>(column, stringValue);
                    break;
                case ValueType::Int32:
                    insert<Int32>(column, stringValue);
                    break;
                case ValueType::Int64:
                    insert<Int64>(column, stringValue);
                    break;
                case ValueType::Float32:
                    insert<Float32>(column, stringValue);
                    break;
                case ValueType::Float64:
                    insert<Float64>(column, stringValue);
                    break;
                case ValueType::String:
                    static_cast<ColumnString &>(column).insert(parse<String>(stringValue));
                    break;
                case ValueType::Date:
                    static_cast<ColumnUInt16 &>(column).insertValue(parse<LocalDate>(stringValue).getDayNum());
                    break;
                case ValueType::DateTime:
                    static_cast<ColumnUInt32 &>(column).insertValue(static_cast<UInt32>(parse<LocalDateTime>(stringValue)));
                    break;
                case ValueType::UUID:
                    static_cast<ColumnUInt128 &>(column).insertValue(parse<UUID>(stringValue));
                    break;
            }
        }

        void insertDefaultValue(IColumn & column, const IColumn & sample_column) { column.insertFrom(sample_column, 0); }
    }


    Block RedisBlockInputStream::readImpl()
    {
        if (description.sample_block.rows() == 0 || keys.size() == 0)
            all_read = true;

        if (all_read)
            return {};

        const size_t size = description.sample_block.columns();

        MutableColumns columns(description.sample_block.columns());

        for (const auto i : ext::range(0, size))
            columns[i] = description.sample_block.getByPosition(i).column->cloneEmpty();

        const auto insertValueByIdx = [this, &columns](size_t idx, const auto & value)
        {
            const auto & name = description.sample_block.getByPosition(idx).name;
            if (description.types[idx].second)
            {
                ColumnNullable & column_nullable = static_cast<ColumnNullable &>(*columns[idx]);
                insertValue(column_nullable.getNestedColumn(), description.types[idx].first, value, name);
                column_nullable.getNullMapData().emplace_back(0);
            }
            else
                insertValue(*columns[idx], description.types[idx].first, value, name);
        };

        if (keys.begin()->get()->isArray())
        {
            size_t num_rows = 0;
            while (num_rows < max_block_size && !all_read)
            {
                if (cursor >= keys.size())
                {
                    all_read = true;
                    break;
                }

                const auto & primary_with_secondary = *(keys.begin() + cursor);
                const auto & keys_array =
                        static_cast<const Poco::Redis::Type<Poco::Redis::Array> *>(primary_with_secondary.get())->value();
                if (keys_array.size() < 2)
                {
                    throw Exception{"Too low keys in request to source: " + DB::toString(keys_array.size())
                                    + ", expected 2 or more",
                                    ErrorCodes::LOGICAL_ERROR};
                }
                if (num_rows + keys_array.size() - 1 > max_block_size)
                {
                    if (num_rows == 0)
                        throw Exception{"Too many (" + DB::toString(keys_array.size()) + ") key attributes",
                                        ErrorCodes::LIMIT_EXCEEDED};
                    break;
                }

                Poco::Redis::Command commandForValues("HMGET");
                const auto & primary_key = *keys_array.begin();
                commandForValues.addRedisType(primary_key);
                for (size_t i = 1; i < keys_array.size(); ++i)
                {
                    const auto & secondary_key = *(keys_array.begin() + i);
                    insertValueByIdx(0, primary_key);
                    insertValueByIdx(1, secondary_key);
                    commandForValues.addRedisType(secondary_key);
                }
                ++cursor;

                Poco::Redis::Array values = client->execute<Poco::Redis::Array>(commandForValues);
                if (commandForValues.size() != values.size() + 2) // 'HMGET' primary_key secondary_keys
                    throw Exception{"Inconsistent sizes of keys and values in Redis request",
                                    ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH};

                for (size_t i = 0; i < values.size(); ++i)
                {
                    const auto & secondary_key = *(keys_array.begin() + i + 1);
                    const auto & value = *(values.begin() + i);
                    if (value.isNull())
                    {
                        insertValueByIdx(0, primary_key);
                        insertValueByIdx(1, secondary_key);
                        insertDefaultValue(*columns[2], *description.sample_block.getByPosition(2).column);
                        ++num_rows;
                    }
                    else if (!isNull(value)) // null string means 'no value for requested key'
                    {
                        insertValueByIdx(0, primary_key);
                        insertValueByIdx(1, secondary_key);
                        insertValueByIdx(2, value);
                        ++num_rows;
                    }
                }
            }
        }
        else
        {
            size_t num_rows = 0;
            while (num_rows < max_block_size && !all_read)
            {
                Poco::Redis::Command commandForValues("MGET");

                // keys.size() > 0
                for (size_t i = 0; i < max_block_size && cursor < keys.size(); ++i)
                {
                    const auto & key = *(keys.begin() + cursor);
                    commandForValues.addRedisType(key);
                    ++cursor;
                }

                if (commandForValues.size() == 1) // only 'MGET'
                {
                    all_read = true;
                    break;
                }

                Poco::Redis::Array values = client->execute<Poco::Redis::Array>(commandForValues);
                if (commandForValues.size() != values.size() + 1) // 'MGET' keys
                    throw Exception{"Inconsistent sizes of keys and values in Redis request",
                                    ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH};

                for (size_t i = 0; i < values.size(); ++i)
                {
                    const auto & key = *(keys.begin() + cursor - i - 1);
                    const auto & value = *(values.begin() + values.size() - i - 1);
                    if (value.isNull())
                    {
                        insertValueByIdx(0, key);
                        insertDefaultValue(*columns[1], *description.sample_block.getByPosition(1).column);
                        ++num_rows;
                    }
                    else if (!isNull(value)) // null string means 'no value for requested key'
                    {
                        insertValueByIdx(0, key);
                        insertValueByIdx(1, value);
                        ++num_rows;
                    }
                }
            }
        }

        return description.sample_block.cloneWithColumns(std::move(columns));
    }
}

#endif
