#include "http-binary-cache-store.hh"
#include "binary-cache-store.hh"
#include "filetransfer.hh"
#include "globals.hh"
#include "nar-info-disk-cache.hh"

namespace nix {

MakeError(UploadToHTTP, Error);

struct HttpBinaryCacheStoreConfig : virtual BinaryCacheStoreConfig
{
    using BinaryCacheStoreConfig::BinaryCacheStoreConfig;

    const std::string name() override { return "HTTP Binary Cache Store"; }

    std::string doc() override
    {
        return
          #include "http-binary-cache-store.md"
          ;
    }
};

class HttpBinaryCacheStore : public virtual HttpBinaryCacheStoreConfig, public virtual BinaryCacheStore
{
private:

    Path cacheUri;

    struct State
    {
        bool enabled = true;
        std::chrono::steady_clock::time_point disabledUntil;
    };

    Sync<State> _state;

public:

    HttpBinaryCacheStore(
        const std::string & scheme,
        const Path & _cacheUri,
        const Params & params)
        : StoreConfig(params)
        , BinaryCacheStoreConfig(params)
        , HttpBinaryCacheStoreConfig(params)
        , Store(params)
        , BinaryCacheStore(params)
        , cacheUri(scheme + "://" + _cacheUri)
    {
        if (cacheUri.back() == '/')
            cacheUri.pop_back();

        diskCache = getNarInfoDiskCache();
    }

    std::string getUri() override
    {
        return cacheUri;
    }

    void init() override
    {
        // FIXME: do this lazily?
        if (auto cacheInfo = diskCache->upToDateCacheExists(cacheUri)) {
            wantMassQuery.setDefault(cacheInfo->wantMassQuery);
            priority.setDefault(cacheInfo->priority);
        } else {
            try {
                BinaryCacheStore::init();
            } catch (UploadToHTTP &) {
                throw Error("'%s' does not appear to be a binary cache", cacheUri);
            }
            diskCache->createCache(cacheUri, storeDir, wantMassQuery, priority);
        }
    }

    static std::set<std::string> uriSchemes()
    {
        static bool forceHttp = getEnv("_NIX_FORCE_HTTP") == "1";
        auto ret = std::set<std::string>({"http", "https"});
        if (forceHttp) ret.insert("file");
        return ret;
    }

protected:

    void maybeDisable()
    {
        auto state(_state.lock());
        if (state->enabled && settings.tryFallback) {
            int t = 60;
            printError("disabling binary cache '%s' for %s seconds", getUri(), t);
            state->enabled = false;
            state->disabledUntil = std::chrono::steady_clock::now() + std::chrono::seconds(t);
        }
    }

    void checkEnabled()
    {
        auto state(_state.lock());
        if (state->enabled) return;
        if (std::chrono::steady_clock::now() > state->disabledUntil) {
            state->enabled = true;
            debug("re-enabling binary cache '%s'", getUri());
            return;
        }
        throw SubstituterDisabled("substituter '%s' is disabled", getUri());
    }

    bool fileExists(const std::string & path) override
    {
        checkEnabled();

        try {
            FileTransferRequest request{makeURI(path)};
            request.head = true;
            getFileTransfer()->transfer(request);
            return true;
        } catch (FileTransferError & e) {
            /* S3 buckets return 403 if a file doesn't exist and the
               bucket is unlistable, so treat 403 as 404. */
            if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
                return false;
            maybeDisable();
            throw;
        }
    }

    void upsertFile(const std::string & path,
        std::shared_ptr<std::basic_iostream<char>> istream,
        const std::string & mimeType) override
    {
        FileTransferRequest req{makeURI(path)};
        req.data = StreamToSourceAdapter(istream).drain();
        req.headers = {{"Content-Type", mimeType}};
        try {
            getFileTransfer()->transfer(req);
        } catch (FileTransferError & e) {
            throw UploadToHTTP("while uploading to HTTP binary cache at '%s': %s", cacheUri, e.msg());
        }
    }

    std::string makeURI(const std::string & path)
    {
        return path.starts_with("https://") || path.starts_with("http://")
                || path.starts_with("file://")
            ? path
            : cacheUri + "/" + path;
    }

    box_ptr<Source> getFile(const std::string & path) override
    {
        checkEnabled();
        FileTransferRequest request{makeURI(path)};
        try {
            return getFileTransfer()->download(std::move(request));
        } catch (FileTransferError & e) {
            if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
                throw NoSuchBinaryCacheFile("file '%s' does not exist in binary cache '%s'", path, getUri());
            maybeDisable();
            throw;
        }
    }

    std::optional<std::string> getFileContents(const std::string & path) override
    {
        checkEnabled();

        FileTransferRequest request{makeURI(path)};

        try {
            return std::move(getFileTransfer()->enqueueFileTransfer(request).get().data);
        } catch (FileTransferError & e) {
            if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
                return {};
            maybeDisable();
            throw;
        }
    }

    /**
     * This isn't actually necessary read only. We support "upsert" now, so we
     * have a notion of authentication via HTTP POST/PUT.
     *
     * For now, we conservatively say we don't know.
     *
     * \todo try to expose our HTTP authentication status.
     */
    std::optional<TrustedFlag> isTrustedClient() override
    {
        return std::nullopt;
    }
};

void registerHttpBinaryCacheStore() {
    StoreImplementations::add<HttpBinaryCacheStore, HttpBinaryCacheStoreConfig>();
}

}
