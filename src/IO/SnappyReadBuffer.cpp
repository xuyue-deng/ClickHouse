#include "config.h"

#if USE_SNAPPY
#include <memory>
#include <fcntl.h>
#include <sys/types.h>

#include <snappy.h>

#include <IO/copyData.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

#include <IO/SnappyReadBuffer.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int SNAPPY_UNCOMPRESS_FAILED;
    extern const int SEEK_POSITION_OUT_OF_BOUND;
}


SnappyReadBuffer::SnappyReadBuffer(std::unique_ptr<ReadBuffer> in_, size_t buf_size, char * existing_memory, size_t alignment)
    : BufferWithOwnMemory<SeekableReadBuffer>(buf_size, existing_memory, alignment), in(std::move(in_))
{
}

bool SnappyReadBuffer::nextImpl()
{
    if (compress_buffer.empty() && uncompress_buffer.empty())
    {
        {
            WriteBufferFromString wb(compress_buffer);
            copyData(*in, wb);
        }

        bool success = snappy::Uncompress(compress_buffer.data(), compress_buffer.size(), &uncompress_buffer);
        if (!success)
        {
            throw Exception(ErrorCodes::SNAPPY_UNCOMPRESS_FAILED, "snappy uncomress failed: ");
        }
        BufferBase::set(const_cast<char *>(uncompress_buffer.data()), uncompress_buffer.size(), 0);
        return true;
    }
    return false;
}

SnappyReadBuffer::~SnappyReadBuffer() = default;

off_t SnappyReadBuffer::seek(off_t off, int whence)
{
    off_t new_pos;
    if (whence == SEEK_SET)
        new_pos = off;
    else if (whence == SEEK_CUR)
        new_pos = count() + off;
    else
        throw Exception(ErrorCodes::SEEK_POSITION_OUT_OF_BOUND, "Only SEEK_SET and SEEK_CUR seek modes allowed.");

    working_buffer = internal_buffer;
    if (new_pos < 0 || new_pos > off_t(working_buffer.size()))
        throw Exception(ErrorCodes::SEEK_POSITION_OUT_OF_BOUND,
                        "Cannot seek through buffer because seek position ({}) is out of bounds [0, {}]",
                        new_pos, working_buffer.size());
    position() = working_buffer.begin() + new_pos;
    return new_pos;
}

off_t SnappyReadBuffer::getPosition()
{
    return count();
}

}
#endif
