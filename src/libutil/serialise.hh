#pragma once
///@file

#include <concepts>
#include <memory>

#include "generator.hh"
#include "types.hh"
#include "util.hh"

namespace boost::context { struct stack_context; }

namespace nix {


/**
 * Abstract destination of binary data.
 */
struct Sink
{
    virtual ~Sink() { }
    virtual void operator () (std::string_view data) = 0;
};

/**
 * Just throws away data.
 */
struct NullSink : Sink
{
    void operator () (std::string_view data) override
    { }
};


struct FinishSink : virtual Sink
{
    virtual void finish() = 0;
};


/**
 * A buffered abstract sink. Warning: a BufferedSink should not be
 * used from multiple threads concurrently.
 */
struct BufferedSink : virtual Sink
{
    size_t bufSize, bufPos;
    std::unique_ptr<char[]> buffer;

    BufferedSink(size_t bufSize = 32 * 1024)
        : bufSize(bufSize), bufPos(0), buffer(nullptr) { }

    void operator () (std::string_view data) override;

    void flush();

protected:

    virtual void writeUnbuffered(std::string_view data) = 0;
};


/**
 * Abstract source of binary data.
 */
struct Source
{
    virtual ~Source() { }

    /**
     * Store exactly ‘len’ bytes in the buffer pointed to by ‘data’.
     * It blocks until all the requested data is available, or throws
     * an error if it is not going to be available.
     */
    void operator () (char * data, size_t len);

    /**
     * Store up to ‘len’ in the buffer pointed to by ‘data’, and
     * return the number of bytes stored.  It blocks until at least
     * one byte is available.
     */
    virtual size_t read(char * data, size_t len) = 0;

    void drainInto(Sink & sink);

    std::string drain();
};


/**
 * A buffered abstract source. Warning: a BufferedSource should not be
 * used from multiple threads concurrently.
 */
struct BufferedSource : Source
{
    size_t bufSize, bufPosIn, bufPosOut;
    std::unique_ptr<char[]> buffer;

    BufferedSource(size_t bufSize = 32 * 1024)
        : bufSize(bufSize), bufPosIn(0), bufPosOut(0), buffer(nullptr) { }

    size_t read(char * data, size_t len) override;

    bool hasData();

protected:
    /**
     * Underlying read call, to be overridden.
     */
    virtual size_t readUnbuffered(char * data, size_t len) = 0;
};


/**
 * A sink that writes data to a file descriptor.
 */
struct FdSink : BufferedSink
{
    int fd;
    size_t written = 0;

    FdSink() : fd(-1) { }
    FdSink(int fd) : fd(fd) { }
    FdSink(FdSink&&) = default;

    FdSink & operator=(FdSink && s)
    {
        flush();
        fd = s.fd;
        s.fd = -1;
        written = s.written;
        return *this;
    }

    ~FdSink();

    void writeUnbuffered(std::string_view data) override;
};


/**
 * A source that reads data from a file descriptor.
 */
struct FdSource : BufferedSource
{
    int fd;
    size_t read = 0;
    BackedStringView endOfFileError{"unexpected end-of-file"};

    FdSource() : fd(-1) { }
    FdSource(int fd) : fd(fd) { }
    FdSource(FdSource &&) = default;

    FdSource & operator=(FdSource && s)
    {
        fd = s.fd;
        s.fd = -1;
        read = s.read;
        return *this;
    }

protected:
    size_t readUnbuffered(char * data, size_t len) override;
};


/**
 * A sink that writes data to a string.
 */
struct StringSink : Sink
{
    std::string s;
    StringSink() { }
    explicit StringSink(const size_t reservedSize)
    {
      s.reserve(reservedSize);
    };
    StringSink(std::string && s) : s(std::move(s)) { };
    void operator () (std::string_view data) override;
};


/**
 * A source that reads data from a string.
 */
struct StringSource : Source
{
    std::string_view s;
    size_t pos;
    StringSource(std::string_view s) : s(s), pos(0) { }
    size_t read(char * data, size_t len) override;
};


/**
 * A sink that writes all incoming data to two other sinks.
 */
struct TeeSink : Sink
{
    Sink & sink1, & sink2;
    TeeSink(Sink & sink1, Sink & sink2) : sink1(sink1), sink2(sink2) { }
    virtual void operator () (std::string_view data)
    {
        sink1(data);
        sink2(data);
    }
};


/**
 * Adapter class of a Source that saves all data read to a sink.
 */
struct TeeSource : Source
{
    Source & orig;
    Sink & sink;
    TeeSource(Source & orig, Sink & sink)
        : orig(orig), sink(sink) { }
    size_t read(char * data, size_t len)
    {
        size_t n = orig.read(data, len);
        sink({data, n});
        return n;
    }
};

/**
 * A reader that consumes the original Source until 'size'.
 */
struct SizedSource : Source
{
    Source & orig;
    size_t remain;
    SizedSource(Source & orig, size_t size)
        : orig(orig), remain(size) { }
    size_t read(char * data, size_t len)
    {
        if (this->remain <= 0) {
            throw EndOfFile("sized: unexpected end-of-file");
        }
        len = std::min(len, this->remain);
        size_t n = this->orig.read(data, len);
        this->remain -= n;
        return n;
    }

    /**
     * Consume the original source until no remain data is left to consume.
     */
    size_t drainAll()
    {
        std::vector<char> buf(8192);
        size_t sum = 0;
        while (this->remain > 0) {
            size_t n = read(buf.data(), buf.size());
            sum += n;
        }
        return sum;
    }
};

/**
 * A sink that that just counts the number of bytes given to it
 */
struct LengthSink : Sink
{
    uint64_t length = 0;

    void operator () (std::string_view data) override
    {
        length += data.size();
    }
};

/**
 * Convert a function into a sink.
 */
struct LambdaSink : Sink
{
    typedef std::function<void(std::string_view data)> lambda_t;

    lambda_t lambda;

    LambdaSink(const lambda_t & lambda) : lambda(lambda) { }

    void operator () (std::string_view data) override
    {
        lambda(data);
    }
};


/**
 * Convert a function into a source.
 */
struct LambdaSource : Source
{
    typedef std::function<size_t(char *, size_t)> lambda_t;

    lambda_t lambda;

    LambdaSource(const lambda_t & lambda) : lambda(lambda) { }

    size_t read(char * data, size_t len) override
    {
        return lambda(data, len);
    }
};

/**
 * Chain a number of sources together, exhausting them all in turn.
 */
template<typename... Sources>
    requires (std::derived_from<Sources, Source> && ...)
struct ChainSource : Source
{
private:
    std::tuple<Sources...> sources;
    std::array<Source *, sizeof...(Sources)> ptrs;
    size_t sourceIdx = 0;

    template<size_t... N>
    void fillPtrs(std::index_sequence<N...>)
    {
        ((ptrs[N] = &std::get<N>(sources)), ...);
    }

public:
    ChainSource(Sources && ... sources)
        : sources(std::move(sources)...)
    {
        fillPtrs(std::index_sequence_for<Sources...>{});
    }

    ChainSource(ChainSource && other)
        : sources(std::move(other.sources))
        , sourceIdx(other.sourceIdx)
    {
        fillPtrs(std::index_sequence_for<Sources...>{});
        other.sourceIdx = sizeof...(Sources);
    }

    ChainSource & operator=(ChainSource && other)
    {
        std::swap(sources, other.sources);
        // since Sources... are the same the tuple type and offsets
        // are the same, so pointers remain valid on both sides.
        std::swap(sourceIdx, other.sourceIdx);
        return *this;
    }

    size_t read(char * data, size_t len) override
    {
        if (sourceIdx == sizeof...(Sources))
            throw EndOfFile("reached end of chained sources");
        try {
            return ptrs[sourceIdx]->read(data, len);
        } catch (EndOfFile &) {
            sourceIdx++;
            return this->read(data, len);
        }
    }
};

std::unique_ptr<FinishSink> sourceToSink(std::function<void(Source &)> fun);

/**
 * Convert a function that feeds data into a Sink into a Source. The
 * Source executes the function as a coroutine.
 */
std::unique_ptr<Source> sinkToSource(
    std::function<void(Sink &)> fun,
    std::function<void()> eof = []() {
        throw EndOfFile("coroutine has finished");
    });

struct SerializingTransform;
using WireFormatGenerator = Generator<std::span<const char>, SerializingTransform>;

inline void drainGenerator(WireFormatGenerator g, std::derived_from<Sink> auto & into)
{
    while (g) {
        auto bit = g();
        into(std::string_view(bit.data(), bit.size()));
    }
}

struct SerializingTransform
{
    std::array<char, 8> buf;

    static std::span<const char> raw(auto... args)
    {
        return std::span<const char>(args...);
    }

    std::span<const char> operator()(uint64_t n)
    {
        buf[0] = n & 0xff;
        buf[1] = (n >> 8) & 0xff;
        buf[2] = (n >> 16) & 0xff;
        buf[3] = (n >> 24) & 0xff;
        buf[4] = (n >> 32) & 0xff;
        buf[5] = (n >> 40) & 0xff;
        buf[6] = (n >> 48) & 0xff;
        buf[7] = (unsigned char) (n >> 56) & 0xff;
        return {buf.begin(), 8};
    }

    // only choose this for *exactly* char spans, do not allow implicit
    // conversions. this would cause ambiguities with strings literals,
    // and resolving those with more string-like overloads needs a lot.
    template<typename Span>
        requires std::same_as<Span, std::span<char>> || std::same_as<Span, std::span<const char>>
    std::span<const char> operator()(Span s)
    {
        return s;
    }
    WireFormatGenerator operator()(std::string_view s);
    WireFormatGenerator operator()(const Strings & s);
    WireFormatGenerator operator()(const StringSet & s);
    WireFormatGenerator operator()(const Error & s);
};

inline Sink & operator<<(Sink & sink, WireFormatGenerator && g)
{
    while (g) {
        auto bit = g();
        sink(std::string_view(bit.data(), bit.size()));
    }
    return sink;
}

void writePadding(size_t len, Sink & sink);

inline Sink & operator<<(Sink & sink, uint64_t u)
{
    return sink << [&]() -> WireFormatGenerator { co_yield u; }();
}

inline Sink & operator<<(Sink & sink, std::string_view s)
{
    return sink << [&]() -> WireFormatGenerator { co_yield s; }();
}

inline Sink & operator<<(Sink & sink, const Strings & s)
{
    return sink << [&]() -> WireFormatGenerator { co_yield s; }();
}

inline Sink & operator<<(Sink & sink, const StringSet & s)
{
    return sink << [&]() -> WireFormatGenerator { co_yield s; }();
}

inline Sink & operator<<(Sink & sink, const Error & ex)
{
    return sink << [&]() -> WireFormatGenerator { co_yield ex; }();
}

MakeError(SerialisationError, Error);

template<typename T>
T readNum(Source & source)
{
    unsigned char buf[8];
    source((char *) buf, sizeof(buf));

    auto n = readLittleEndian<uint64_t>(buf);

    if (n > (uint64_t) std::numeric_limits<T>::max())
        throw SerialisationError("serialised integer %d is too large for type '%s'", n, typeid(T).name());

    return (T) n;
}


inline unsigned int readInt(Source & source)
{
    return readNum<unsigned int>(source);
}


inline uint64_t readLongLong(Source & source)
{
    return readNum<uint64_t>(source);
}


void readPadding(size_t len, Source & source);
size_t readString(char * buf, size_t max, Source & source);
std::string readString(Source & source, size_t max = std::numeric_limits<size_t>::max());
template<class T> T readStrings(Source & source);

Source & operator >> (Source & in, std::string & s);

template<typename T>
Source & operator >> (Source & in, T & n)
{
    n = readNum<T>(in);
    return in;
}

template<typename T>
Source & operator >> (Source & in, bool & b)
{
    b = readNum<uint64_t>(in);
    return in;
}

Error readError(Source & source);


/**
 * An adapter that converts a std::basic_istream into a source.
 */
struct StreamToSourceAdapter : Source
{
    std::shared_ptr<std::basic_istream<char>> istream;

    StreamToSourceAdapter(std::shared_ptr<std::basic_istream<char>> istream)
        : istream(istream)
    { }

    size_t read(char * data, size_t len) override
    {
        if (!istream->read(data, len)) {
            if (istream->eof()) {
                if (istream->gcount() == 0)
                    throw EndOfFile("end of file");
            } else
                throw Error("I/O error in StreamToSourceAdapter");
        }
        return istream->gcount();
    }
};


/**
 * A source that reads a distinct format of concatenated chunks back into its
 * logical form, in order to guarantee a known state to the original stream,
 * even in the event of errors.
 *
 * Use with FramedSink, which also allows the logical stream to be terminated
 * in the event of an exception.
 */
struct FramedSource : Source
{
    Source & from;
    bool eof = false;
    std::vector<char> pending;
    size_t pos = 0;

    FramedSource(Source & from) : from(from)
    { }

    ~FramedSource()
    {
        if (!eof) {
            while (true) {
                auto n = readInt(from);
                if (!n) break;
                std::vector<char> data(n);
                from(data.data(), n);
            }
        }
    }

    size_t read(char * data, size_t len) override
    {
        if (eof) throw EndOfFile("reached end of FramedSource");

        if (pos >= pending.size()) {
            size_t len = readInt(from);
            if (!len) {
                eof = true;
                return 0;
            }
            pending = std::vector<char>(len);
            pos = 0;
            from(pending.data(), len);
        }

        auto n = std::min(len, pending.size() - pos);
        memcpy(data, pending.data() + pos, n);
        pos += n;
        return n;
    }
};

/**
 * Write as chunks in the format expected by FramedSource.
 *
 * The exception_ptr reference can be used to terminate the stream when you
 * detect that an error has occurred on the remote end.
 */
struct FramedSink : nix::BufferedSink
{
    BufferedSink & to;
    std::exception_ptr & ex;

    FramedSink(BufferedSink & to, std::exception_ptr & ex) : to(to), ex(ex)
    { }

    ~FramedSink()
    {
        try {
            to << 0;
            to.flush();
        } catch (...) {
            ignoreException();
        }
    }

    void writeUnbuffered(std::string_view data) override
    {
        /* Don't send more data if the remote has
            encountered an error. */
        if (ex) {
            auto ex2 = ex;
            ex = nullptr;
            std::rethrow_exception(ex2);
        }
        to << data.size();
        to(data);
    };
};

/**
 * Stack allocation strategy for sinkToSource.
 * Mutable to avoid a boehm gc dependency in libutil.
 *
 * boost::context doesn't provide a virtual class, so we define our own.
 */
struct StackAllocator {
    virtual boost::context::stack_context allocate() = 0;
    virtual void deallocate(boost::context::stack_context sctx) = 0;

    /**
     * The stack allocator to use in sinkToSource and potentially elsewhere.
     * It is reassigned by the initGC() method in libexpr.
     */
    static StackAllocator *defaultAllocator;
};

/* Disabling GC when entering a coroutine (without the boehm patch).
   mutable to avoid boehm gc dependency in libutil.
 */
extern std::shared_ptr<void> (*create_coro_gc_hook)();


}
