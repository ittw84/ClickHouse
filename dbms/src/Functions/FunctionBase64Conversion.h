#include <Common/config.h>
#if USE_BASE64
#include <Columns/ColumnConst.h>
#include <Columns/ColumnString.h>
#include <DataTypes/DataTypeString.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/GatherUtils/Algorithms.h>
#include <IO/WriteHelpers.h>
#include <libbase64.h> // Y_IGNORE


namespace DB
{
using namespace GatherUtils;

namespace ErrorCodes
{
    extern const int ILLEGAL_COLUMN;
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int INCORRECT_DATA;
}

struct Base64Encode
{
    static constexpr auto name = "base64Encode";
    static size_t getBufferSize(size_t string_length, size_t string_count)
    {
        return ((string_length - string_count) / 3 + string_count) * 4 + string_count;
    }
};

struct Base64Decode
{
    static constexpr auto name = "base64Decode";

    static size_t getBufferSize(size_t string_length, size_t string_count)
    {
        return ((string_length - string_count) / 4 + string_count) * 3 + string_count;
    }
};

struct TryBase64Decode
{
    static constexpr auto name = "tryBase64Decode";

    static size_t getBufferSize(size_t string_length, size_t string_count)
    {
        return Base64Decode::getBufferSize(string_length, string_count);
    }
};

template <typename Func>
class FunctionBase64Conversion : public IFunction
{
public:
    static constexpr auto name = Func::name;

    static FunctionPtr create(const Context &)
    {
        return std::make_shared<FunctionBase64Conversion>();
    }

    String getName() const override
    {
        return Func::name;
    }

    size_t getNumberOfArguments() const override
    {
        return 1;
    }

    bool useDefaultImplementationForConstants() const override
    {
        return true;
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (!WhichDataType(arguments[0].type).isString())
            throw Exception(
                "Illegal type " + arguments[0].type->getName() + " of 1 argument of function " + getName() + ". Must be String.",
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

        return std::make_shared<DataTypeString>();
    }

    void executeImpl(Block & block, const ColumnNumbers & arguments, size_t result, size_t input_rows_count) override
    {
        const ColumnPtr column_string = block.getByPosition(arguments[0]).column;
        const ColumnString * input = checkAndGetColumn<ColumnString>(column_string.get());

        if (!input)
            throw Exception(
                "Illegal column " + block.getByPosition(arguments[0]).column->getName() + " of first argument of function " + getName(),
                ErrorCodes::ILLEGAL_COLUMN);

        auto dst_column = ColumnString::create();
        auto & dst_data = dst_column->getChars();
        auto & dst_offsets = dst_column->getOffsets();

        size_t reserve = Func::getBufferSize(input->getChars().size(), input->size());
        dst_data.resize(reserve);
        dst_offsets.resize(input_rows_count);

        const ColumnString::Offsets & src_offsets = input->getOffsets();

        auto source = reinterpret_cast<const char *>(input->getChars().data());
        auto dst = reinterpret_cast<char *>(dst_data.data());
        auto dst_pos = dst;

        size_t src_offset_prev = 0;

        int codec = getCodec();
        for (size_t row = 0; row < input_rows_count; ++row)
        {
            size_t srclen = src_offsets[row] - src_offset_prev - 1;
            size_t outlen = 0;

            if constexpr (std::is_same_v<Func, Base64Encode>)
            {
                base64_encode(source, srclen, dst_pos, &outlen, codec);
            }
            else if constexpr (std::is_same_v<Func, Base64Decode>)
            {
                if (!base64_decode(source, srclen, dst_pos, &outlen, codec))
                {
                    throw Exception("Failed to " + getName() + " input '" + String(source, srclen) + "'", ErrorCodes::INCORRECT_DATA);
                }
            }
            else
            {
                // during decoding character array can be partially polluted
                // if fail, revert back and clean
                auto savepoint = dst_pos;
                if (!base64_decode(source, srclen, dst_pos, &outlen, codec))
                {
                    outlen = 0;
                    dst_pos = savepoint;
                    // clean the symbol
                    dst_pos[0] = 0;
                }
            }

            source += srclen + 1;
            dst_pos += outlen + 1;

            dst_offsets[row] = dst_pos - dst;
            src_offset_prev = src_offsets[row];
        }

        dst_data.resize(dst_pos - dst);

        block.getByPosition(result).column = std::move(dst_column);
    }

private:
    static int getCodec()
    {
        /// You can provide different value if you want to test specific codecs.
        /// Due to poor implementation of "base64" library (it will write to a global variable),
        ///  it doesn't scale for multiple threads. Never use non-zero values in production.
        return 0;
    }
};
}
#endif
