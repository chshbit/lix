#include "filetransfer.hh"
#include "namespaces.hh"
#include "globals.hh"
#include "store-api.hh"
#include "s3.hh"
#include "signals.hh"
#include "strings.hh"
#include <cstddef>

#include <cstdio>
#include <kj/encoding.h>

#if ENABLE_S3
#include <aws/core/client/ClientConfiguration.h>
#endif

#include <unistd.h>
#include <fcntl.h>

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <regex>

using namespace std::string_literals;

namespace nix {

FileTransferSettings fileTransferSettings;

static GlobalConfig::Register rFileTransferSettings(&fileTransferSettings);

struct curlFileTransfer : public FileTransfer
{
    CURLM * curlm = 0;

    const unsigned int baseRetryTimeMs;

    struct TransferItem : public std::enable_shared_from_this<TransferItem>
    {
        struct DownloadState
        {
            bool done = false;
            std::exception_ptr exc;
            std::string data;
        };

        curlFileTransfer & fileTransfer;
        std::string uri;
        FileTransferResult result;
        Activity act;
        std::unique_ptr<FILE, decltype([](FILE * f) { fclose(f); })> uploadData;
        Sync<DownloadState> downloadState;
        std::condition_variable downloadEvent;
        enum {
            /// nothing has been transferred yet
            initialSetup,
            /// data transfer in progress
            transferring,
            /// transfer complete, result or failure reported
            transferComplete,
        } phase = initialSetup;
        std::promise<FileTransferResult> metadataPromise;
        CURL * req; // must never be nullptr
        std::string statusMsg;

        uint64_t bodySize = 0;

        struct curl_slist * requestHeaders = 0;

        inline static const std::set<long> successfulStatuses {200, 201, 204, 206, 304, 0 /* other protocol */};
        /* Get the HTTP status code, or 0 for other protocols. */
        long getHTTPStatus()
        {
            long httpStatus = 0;
            long protocol = 0;
            curl_easy_getinfo(req, CURLINFO_PROTOCOL, &protocol);
            if (protocol == CURLPROTO_HTTP || protocol == CURLPROTO_HTTPS)
                curl_easy_getinfo(req, CURLINFO_RESPONSE_CODE, &httpStatus);
            return httpStatus;
        }

        std::string verb() const
        {
            return uploadData ? "upload" : "download";
        }

        TransferItem(curlFileTransfer & fileTransfer,
            const std::string & uri,
            const Headers & headers,
            ActivityId parentAct,
            std::optional<std::string_view> uploadData,
            bool noBody,
            curl_off_t writtenToSink
        )
            : fileTransfer(fileTransfer)
            , uri(uri)
            , act(*logger, lvlTalkative, actFileTransfer,
                fmt(uploadData ? "uploading '%s'" : "downloading '%s'", uri),
                {uri}, parentAct)
            , req(curl_easy_init())
        {
            if (req == nullptr) {
                throw FileTransferError(Misc, {}, "could not allocate curl handle");
            }
            for (auto it = headers.begin(); it != headers.end(); ++it){
                requestHeaders = curl_slist_append(requestHeaders, fmt("%s: %s", it->first, it->second).c_str());
            }

            if (verbosity >= lvlVomit) {
                curl_easy_setopt(req, CURLOPT_VERBOSE, 1);
                curl_easy_setopt(req, CURLOPT_DEBUGFUNCTION, TransferItem::debugCallback);
            }

            curl_easy_setopt(req, CURLOPT_URL, uri.c_str());
            curl_easy_setopt(req, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(req, CURLOPT_ACCEPT_ENCODING, ""); // all of them!
            curl_easy_setopt(req, CURLOPT_MAXREDIRS, 10);
            curl_easy_setopt(req, CURLOPT_NOSIGNAL, 1);
            curl_easy_setopt(req, CURLOPT_USERAGENT,
                ("curl/" LIBCURL_VERSION " Lix/" + nixVersion +
                    (fileTransferSettings.userAgentSuffix != "" ? " " + fileTransferSettings.userAgentSuffix.get() : "")).c_str());
            curl_easy_setopt(req, CURLOPT_PIPEWAIT, 1);
            if (fileTransferSettings.enableHttp2)
                curl_easy_setopt(req, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
            else
                curl_easy_setopt(req, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(req, CURLOPT_WRITEFUNCTION, TransferItem::writeCallbackWrapper);
            curl_easy_setopt(req, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(req, CURLOPT_HEADERFUNCTION, TransferItem::headerCallbackWrapper);
            curl_easy_setopt(req, CURLOPT_HEADERDATA, this);

            curl_easy_setopt(req, CURLOPT_PROGRESSFUNCTION, progressCallbackWrapper);
            curl_easy_setopt(req, CURLOPT_PROGRESSDATA, this);
            curl_easy_setopt(req, CURLOPT_NOPROGRESS, 0);

            curl_easy_setopt(req, CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");

            curl_easy_setopt(req, CURLOPT_HTTPHEADER, requestHeaders);

            if (settings.downloadSpeed.get() > 0)
                curl_easy_setopt(req, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t) (settings.downloadSpeed.get() * 1024));

            if (noBody)
                curl_easy_setopt(req, CURLOPT_NOBODY, 1);

            if (uploadData) {
                this->uploadData.reset(fmemopen(const_cast<char *>(uploadData->data()), uploadData->size(), "r"));
                curl_easy_setopt(req, CURLOPT_UPLOAD, 1L);
                curl_easy_setopt(req, CURLOPT_READDATA, this->uploadData.get());
                curl_easy_setopt(req, CURLOPT_INFILESIZE_LARGE, (curl_off_t) uploadData->length());
            }

            if (settings.caFile != "")
                curl_easy_setopt(req, CURLOPT_CAINFO, settings.caFile.get().c_str());

            curl_easy_setopt(req, CURLOPT_CONNECTTIMEOUT, fileTransferSettings.connectTimeout.get());

            curl_easy_setopt(req, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(req, CURLOPT_LOW_SPEED_TIME, fileTransferSettings.stalledDownloadTimeout.get());

            /* If no file exist in the specified path, curl continues to work
               anyway as if netrc support was disabled. */
            curl_easy_setopt(req, CURLOPT_NETRC_FILE, settings.netrcFile.get().c_str());
            curl_easy_setopt(req, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);

            if (writtenToSink)
                curl_easy_setopt(req, CURLOPT_RESUME_FROM_LARGE, writtenToSink);
        }

        ~TransferItem()
        {
            curl_easy_cleanup(req);
            if (requestHeaders) curl_slist_free_all(requestHeaders);
            try {
                if (phase != transferComplete)
                    fail(FileTransferError(Interrupted, {}, "download of '%s' was interrupted", uri));
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        }

        bool acceptsRanges()
        {
            curl_header * h;
            if (curl_easy_header(req, "accept-ranges", 0, CURLH_HEADER, -1, &h)) {
                // treat any error as the remote not accepting range requests. the only
                // interesting local error is out-of-memory, which we can't even handle
                return false;
            }

            return toLower(trim(h->value)) == "bytes";
        }

        void failEx(std::exception_ptr ex)
        {
            assert(phase != transferComplete);
            if (phase == initialSetup) {
                metadataPromise.set_exception(ex);
            }
            phase = transferComplete;
            downloadState.lock()->exc = ex;
            downloadEvent.notify_all();
        }

        template<class T>
        void fail(T && e)
        {
            failEx(std::make_exception_ptr(std::forward<T>(e)));
        }

        void maybeFinishSetup()
        {
            if (phase > initialSetup) {
                return;
            }

            char * effectiveUriCStr = nullptr;
            curl_easy_getinfo(req, CURLINFO_EFFECTIVE_URL, &effectiveUriCStr);
            if (effectiveUriCStr) {
                result.effectiveUri = effectiveUriCStr;
            }

            result.cached = getHTTPStatus() == 304;

            if (phase == initialSetup) {
                metadataPromise.set_value(result);
            }
            phase = transferring;
        }

        std::exception_ptr callbackException;

        size_t writeCallback(void * contents, size_t size, size_t nmemb)
        {
            const size_t realSize = size * nmemb;

            try {
                maybeFinishSetup();

                auto state = downloadState.lock();

                // when the buffer is full (as determined by a historical magic value) we
                // pause the transfer and wait for the receiver to unpause it when ready.
                if (successfulStatuses.count(getHTTPStatus()) && state->data.size() > 1024 * 1024) {
                    return CURL_WRITEFUNC_PAUSE;
                }

                state->data.append(static_cast<const char *>(contents), realSize);
                downloadEvent.notify_all();
                bodySize += realSize;
                return realSize;
            } catch (...) {
                callbackException = std::current_exception();
                return CURL_WRITEFUNC_ERROR;
            }
        }

        static size_t writeCallbackWrapper(void * contents, size_t size, size_t nmemb, void * userp)
        {
            return static_cast<TransferItem *>(userp)->writeCallback(contents, size, nmemb);
        }

        size_t headerCallback(void * contents, size_t size, size_t nmemb)
        try {
            size_t realSize = size * nmemb;
            std::string line(static_cast<char *>(contents), realSize);
            printMsg(lvlVomit, "got header for '%s': %s", uri, trim(line));

            static std::regex statusLine("HTTP/[^ ]+ +[0-9]+(.*)", std::regex::extended | std::regex::icase);
            if (std::smatch match; std::regex_match(line, match, statusLine)) {
                statusMsg = trim(match.str(1));
            } else {
                auto i = line.find(':');
                if (i != std::string::npos) {
                    std::string name = toLower(trim(line.substr(0, i)));

                    if (name == "etag") {
                        // NOTE we don't check that the etag hasn't gone *missing*. technically
                        // this is not an error as long as we get the same data from the remote.
                        auto etag = trim(line.substr(i + 1));
                        result.etag = std::move(etag);
                    }

                    else if (name == "link" || name == "x-amz-meta-link") {
                        auto value = trim(line.substr(i + 1));
                        static std::regex linkRegex("<([^>]*)>; rel=\"immutable\"", std::regex::extended | std::regex::icase);
                        if (std::smatch match; std::regex_match(value, match, linkRegex)) {
                            result.immutableUrl = match.str(1);
                        } else
                            debug("got invalid link header '%s'", value);
                    }
                }
            }
            return realSize;
        } catch (...) {
            callbackException = std::current_exception();
            return CURL_WRITEFUNC_ERROR;
        }

        static size_t headerCallbackWrapper(void * contents, size_t size, size_t nmemb, void * userp)
        {
            return static_cast<TransferItem *>(userp)->headerCallback(contents, size, nmemb);
        }

        int progressCallback(double dltotal, double dlnow)
        {
            try {
              act.progress(dlnow, dltotal);
            } catch (nix::Interrupted &) {
              assert(_isInterrupted);
            }
            return _isInterrupted;
        }

        static int progressCallbackWrapper(void * userp, double dltotal, double dlnow, double ultotal, double ulnow)
        {
            return static_cast<TransferItem *>(userp)->progressCallback(dltotal, dlnow);
        }

        static int debugCallback(CURL * handle, curl_infotype type, char * data, size_t size, void * userptr)
        {
            if (type == CURLINFO_TEXT)
                vomit("curl: %s", chomp(std::string(data, size)));
            return 0;
        }

        void finish(CURLcode code)
        {
            auto httpStatus = getHTTPStatus();

            maybeFinishSetup();

            debug("finished %s of '%s'; curl status = %d, HTTP status = %d, body = %d bytes",
                verb(), uri, code, httpStatus, bodySize);

            if (callbackException)
                failEx(callbackException);

            else if (code == CURLE_OK && successfulStatuses.count(httpStatus))
            {
                act.progress(bodySize, bodySize);
                phase = transferComplete;
                downloadState.lock()->done = true;
                downloadEvent.notify_all();
            }

            else {
                // We treat most errors as transient, but won't retry when hopeless
                Error err = Transient;

                if (httpStatus == 404 || httpStatus == 410 || code == CURLE_FILE_COULDNT_READ_FILE) {
                    // The file is definitely not there
                    err = NotFound;
                } else if (httpStatus == 401 || httpStatus == 403 || httpStatus == 407) {
                    // Don't retry on authentication/authorization failures
                    err = Forbidden;
                } else if (httpStatus >= 400 && httpStatus < 500 && httpStatus != 408 && httpStatus != 429) {
                    // Most 4xx errors are client errors and are probably not worth retrying:
                    //   * 408 means the server timed out waiting for us, so we try again
                    //   * 429 means too many requests, so we retry (with a delay)
                    err = Misc;
                } else if (httpStatus == 501 || httpStatus == 505 || httpStatus == 511) {
                    // Let's treat most 5xx (server) errors as transient, except for a handful:
                    //   * 501 not implemented
                    //   * 505 http version not supported
                    //   * 511 we're behind a captive portal
                    err = Misc;
                } else {
                    // Don't bother retrying on certain cURL errors either

                    // Allow selecting a subset of enum values
                    #pragma GCC diagnostic push
                    #pragma GCC diagnostic ignored "-Wswitch-enum"
                    switch (code) {
                        case CURLE_FAILED_INIT:
                        case CURLE_URL_MALFORMAT:
                        case CURLE_NOT_BUILT_IN:
                        case CURLE_REMOTE_ACCESS_DENIED:
                        case CURLE_FILE_COULDNT_READ_FILE:
                        case CURLE_FUNCTION_NOT_FOUND:
                        case CURLE_ABORTED_BY_CALLBACK:
                        case CURLE_BAD_FUNCTION_ARGUMENT:
                        case CURLE_INTERFACE_FAILED:
                        case CURLE_UNKNOWN_OPTION:
                        case CURLE_SSL_CACERT_BADFILE:
                        case CURLE_TOO_MANY_REDIRECTS:
                        case CURLE_WRITE_ERROR:
                        case CURLE_UNSUPPORTED_PROTOCOL:
                            err = Misc;
                            break;
                        default: // Shut up warnings
                            break;
                    }
                    #pragma GCC diagnostic pop
                }

                std::optional<std::string> response;
                if (!successfulStatuses.count(httpStatus))
                    response = std::move(downloadState.lock()->data);
                auto exc =
                    code == CURLE_ABORTED_BY_CALLBACK && _isInterrupted
                    ? FileTransferError(Interrupted, std::move(response), "%s of '%s' was interrupted", verb(), uri)
                    : httpStatus != 0
                    ? FileTransferError(err,
                        std::move(response),
                        "unable to %s '%s': HTTP error %d (%s)%s",
                        verb(), uri, httpStatus, statusMsg,
                        code == CURLE_OK ? "" : fmt(" (curl error: %s)", curl_easy_strerror(code)))
                    : FileTransferError(err,
                        std::move(response),
                        "unable to %s '%s': %s (%d)",
                        verb(), uri, curl_easy_strerror(code), code);

                fail(std::move(exc));
            }
        }

        void unpause()
        {
            auto lock = fileTransfer.state_.lock();
            lock->unpause.push_back(shared_from_this());
            fileTransfer.wakeup();
        }

        void cancel()
        {
            std::promise<void> promise;
            auto wait = promise.get_future();
            {
                auto lock = fileTransfer.state_.lock();
                if (lock->quit) {
                    return;
                }
                lock->cancel[shared_from_this()] = std::move(promise);
            }
            fileTransfer.wakeup();
            wait.get();
        }
    };

    struct State
    {
        bool quit = false;
        std::vector<std::shared_ptr<TransferItem>> incoming;
        std::vector<std::shared_ptr<TransferItem>> unpause;
        std::map<std::shared_ptr<TransferItem>, std::promise<void>> cancel;
    };

    Sync<State> state_;

    std::thread workerThread;

    curlFileTransfer(unsigned int baseRetryTimeMs)
        : baseRetryTimeMs(baseRetryTimeMs)
    {
        static std::once_flag globalInit;
        std::call_once(globalInit, curl_global_init, CURL_GLOBAL_ALL);

        curlm = curl_multi_init();

        curl_multi_setopt(curlm, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        curl_multi_setopt(curlm, CURLMOPT_MAX_TOTAL_CONNECTIONS,
            fileTransferSettings.httpConnections.get());

        workerThread = std::thread([&]() { workerThreadEntry(); });
    }

    ~curlFileTransfer()
    {
        try {
            stopWorkerThread();
        } catch (nix::Error & e) {
            // This can only fail if a socket to our own process cannot be
            // written to, so it is always a bug in the program if it fails.
            //
            // Joining the thread would probably only cause a deadlock if this
            // happened, so just die on purpose.
            printError("failed to join curl file transfer worker thread: %1%", e.what());
            std::terminate();
        }
        workerThread.join();

        if (curlm) curl_multi_cleanup(curlm);
    }

    void wakeup()
    {
        if (auto mc = curl_multi_wakeup(curlm))
            throw nix::Error("unexpected error from curl_multi_wakeup(): %s", curl_multi_strerror(mc));
    }

    void stopWorkerThread()
    {
        /* Signal the worker thread to exit. */
        {
            auto state(state_.lock());
            state->quit = true;
        }
        wakeup();
    }

    void workerThreadMain()
    {
        /* Cause this thread to be notified on SIGINT. */
        auto callback = createInterruptCallback([&]() {
            stopWorkerThread();
        });

        unshareFilesystem();

        std::map<CURL *, std::shared_ptr<TransferItem>> items;

        bool quit = false;

        // NOTE: we will need to use CURLMOPT_TIMERFUNCTION to integrate this
        // loop with kj. until then curl will handle its timeouts internally.
        int64_t timeoutMs = INT64_MAX;

        while (!quit) {
            checkInterrupt();

            {
                auto cancel = [&] { return std::move(state_.lock()->cancel); }();
                for (auto & [item, promise] : cancel) {
                    curl_multi_remove_handle(curlm, item->req);
                    promise.set_value();
                }
            }

            /* Let curl do its thing. */
            int running;
            CURLMcode mc = curl_multi_perform(curlm, &running);
            if (mc != CURLM_OK)
                throw nix::Error("unexpected error from curl_multi_perform(): %s", curl_multi_strerror(mc));

            /* Set the promises of any finished requests. */
            CURLMsg * msg;
            int left;
            while ((msg = curl_multi_info_read(curlm, &left))) {
                if (msg->msg == CURLMSG_DONE) {
                    auto i = items.find(msg->easy_handle);
                    assert(i != items.end());
                    i->second->finish(msg->data.result);
                    curl_multi_remove_handle(curlm, i->second->req);
                    items.erase(i);
                }
            }

            /* Wait for activity, including wakeup events. */
            mc = curl_multi_poll(curlm, nullptr, 0, std::min<int64_t>(timeoutMs, INT_MAX), nullptr);
            if (mc != CURLM_OK)
                throw nix::Error("unexpected error from curl_multi_poll(): %s", curl_multi_strerror(mc));

            /* Add new curl requests from the incoming requests queue,
               except for requests that are embargoed (waiting for a
               retry timeout to expire). */

            std::vector<std::shared_ptr<TransferItem>> incoming;

            timeoutMs = INT64_MAX;

            {
                auto unpause = [&] { return std::move(state_.lock()->unpause); }();
                for (auto & item : unpause) {
                    curl_easy_pause(item->req, CURLPAUSE_CONT);
                }
            }

            {
                auto state(state_.lock());
                incoming = std::move(state->incoming);
                quit = state->quit;
            }

            for (auto & item : incoming) {
                debug("starting %s of %s", item->verb(), item->uri);
                curl_multi_add_handle(curlm, item->req);
                items[item->req] = item;
            }
        }

        debug("download thread shutting down");
    }

    void workerThreadEntry()
    {
        try {
            workerThreadMain();
        } catch (nix::Interrupted & e) {
        } catch (std::exception & e) {
            printError("unexpected error in download thread: %s", e.what());
        }

        {
            auto state(state_.lock());
            state->incoming.clear();
            state->quit = true;
        }
    }

    void enqueueItem(std::shared_ptr<TransferItem> item)
    {
        if (item->uploadData
            && !item->uri.starts_with("http://")
            && !item->uri.starts_with("https://"))
            throw nix::Error("uploading to '%s' is not supported", item->uri);

        {
            auto state(state_.lock());
            if (state->quit)
                throw nix::Error("cannot enqueue download request because the download thread is shutting down");
            state->incoming.push_back(item);
        }
        wakeup();
    }

#if ENABLE_S3
    static std::tuple<std::string, std::string, Store::Params> parseS3Uri(std::string uri)
    {
        auto [path, params] = splitUriAndParams(uri);

        auto slash = path.find('/', 5); // 5 is the length of "s3://" prefix
            if (slash == std::string::npos)
                throw nix::Error("bad S3 URI '%s'", path);

        std::string bucketName(path, 5, slash - 5);
        std::string key(path, slash + 1);

        return {bucketName, key, params};
    }
#endif

    void upload(const std::string & uri, std::string data, const Headers & headers) override
    {
        enqueueFileTransfer(uri, headers, std::move(data), false);
    }

    std::optional<std::pair<FileTransferResult, box_ptr<Source>>> tryEagerTransfers(
        const std::string & uri,
        const Headers & headers,
        const std::optional<std::string> & data,
        bool noBody
    )
    {
        // curl transfers using file:// urls cannot be paused, and are a bit unruly
        // in other ways too. since their metadata is trivial and we already have a
        // backend for simple file system reads we can use that instead. we'll pass
        // uploads to files to curl even so, those will fail in enqueueItem anyway.
        // on all other decoding failures we also let curl fail for us a bit later.
        //
        // note that we use kj to decode the url, not curl. curl uses only the path
        // component of the url to determine the file name, but it does note expose
        // the decoding method it uses for this. for file:// transfers curl forbids
        // only \0 characters in the urldecoded path, not all control characters as
        // it does in the public curl_url_get(CURLUPART_PATH, CURLU_URLDECODE) api.
        //
        // also note: everything weird you see here is for compatibility with curl.
        // we can't even fix it because nix-channel relies on this. even reading of
        // directories being allowed and returning something (though hopefully it's
        // enough to return anything instead of a directory listing like curl does)
        if (uri.starts_with("file://") && !data.has_value()) {
            if (!uri.starts_with("file:///")) {
                throw FileTransferError(NotFound, std::nullopt, "file not found");
            }
            auto url = curl_url();
            if (!url) {
                throw std::bad_alloc();
            }
            KJ_DEFER(curl_url_cleanup(url));
            curl_url_set(url, CURLUPART_URL, uri.c_str(), 0);
            char * path = nullptr;
            curl_url_get(url, CURLUPART_PATH, &path, 0);
            auto decoded = kj::decodeUriComponent(kj::arrayPtr(path, path + strlen(path)));
            if (!decoded.hadErrors && decoded.findFirst(0) == nullptr) {
                Path fsPath(decoded.cStr(), decoded.size());
                FileTransferResult metadata{.effectiveUri = std::string("file://") + path};
                struct stat st;
                AutoCloseFD fd(open(fsPath.c_str(), O_RDONLY));
                if (!fd || fstat(fd.get(), &st) != 0) {
                    throw FileTransferError(
                        NotFound, std::nullopt, "%s: file not found (%s)", fsPath, strerror(errno)
                    );
                }
                if (S_ISDIR(st.st_mode)) {
                    return {{std::move(metadata), make_box_ptr<StringSource>("")}};
                }
                struct OwningFdSource : FdSource
                {
                    AutoCloseFD fd;
                    OwningFdSource(AutoCloseFD fd) : FdSource(fd.get()), fd(std::move(fd)) {}
                };
                return {{std::move(metadata), make_box_ptr<OwningFdSource>(std::move(fd))}};
            }
        }

        /* Ugly hack to support s3:// URIs. */
        if (uri.starts_with("s3://")) {
            // FIXME: do this on a worker thread
#if ENABLE_S3
            auto [bucketName, key, params] = parseS3Uri(uri);

            std::string profile = getOr(params, "profile", "");
            std::string region = getOr(params, "region", Aws::Region::US_EAST_1);
            std::string scheme = getOr(params, "scheme", "");
            std::string endpoint = getOr(params, "endpoint", "");

            S3Helper s3Helper(profile, region, scheme, endpoint);

            // FIXME: implement ETag
            auto s3Res = s3Helper.getObject(bucketName, key);
            FileTransferResult res;
            if (!s3Res.data)
                throw FileTransferError(NotFound, "S3 object '%s' does not exist", uri);
            return {{res, make_box_ptr<StringSource>(std::move(*s3Res.data))}};
#else
            throw nix::Error(
                "cannot download '%s' because Lix is not built with S3 support", uri
            );
#endif
        }

        return std::nullopt;
    }

    std::pair<FileTransferResult, box_ptr<Source>> enqueueFileTransfer(
        const std::string & uri,
        const Headers & headers,
        std::optional<std::string> data,
        bool noBody
    )
    {
        if (auto eager = tryEagerTransfers(uri, headers, data, noBody)) {
            return std::move(*eager);
        }

        auto source = make_box_ptr<TransferSource>(*this, uri, headers, std::move(data), noBody);
        source->awaitData();
        return {source->metadata, std::move(source)};
    }

    struct TransferSource : Source
    {
        curlFileTransfer & parent;
        std::string uri;
        Headers headers;
        std::optional<std::string> data;
        bool noBody;
        ActivityId parentAct = getCurActivity();

        std::shared_ptr<TransferItem> transfer;
        FileTransferResult metadata;
        std::string chunk;
        std::string_view buffered;

        unsigned int attempt = 0;
        const size_t tries = fileTransferSettings.tries;
        curl_off_t totalReceived = 0;

        TransferSource(
            curlFileTransfer & parent,
            const std::string & uri,
            const Headers & headers,
            std::optional<std::string> data,
            bool noBody
        )
            : parent(parent)
            , uri(uri)
            , headers(headers)
            , data(std::move(data))
            , noBody(noBody)
        {
            metadata = withRetries([&] { return startTransfer(uri); });
        }

        ~TransferSource()
        {
            // wake up the download thread if it's still going and have it abort
            try {
                if (transfer) {
                    transfer->cancel();
                }
            } catch (...) {
                ignoreExceptionInDestructor();
            }
        }

        auto withRetries(auto fn) -> decltype(fn())
        {
            std::optional<std::string> retryContext;
            while (true) {
                try {
                    if (retryContext) {
                        attemptRetry(*retryContext);
                    }
                    return fn();
                } catch (FileTransferError & e) {
                    // If this is a transient error, then maybe retry after a while. after any
                    // bytes have been received we require range support to proceed, otherwise
                    // we'd need to start from scratch and discard everything we already have.
                    if (e.error != Transient || data.has_value() || attempt >= tries
                        || (totalReceived > 0 && !transfer->acceptsRanges()))
                    {
                        throw;
                    }
                    retryContext = e.what();
                }
            }
        }

        FileTransferResult startTransfer(const std::string & uri, curl_off_t offset = 0)
        {
            attempt += 1;
            auto uploadData = data ? std::optional(std::string_view(*data)) : std::nullopt;
            transfer = std::make_shared<TransferItem>(
                parent, uri, headers, parentAct, uploadData, noBody, offset
            );
            parent.enqueueItem(transfer);
            return transfer->metadataPromise.get_future().get();
        }

        void throwChangedTarget(std::string_view what, std::string_view from, std::string_view to)
        {
            if (!from.empty() && from != to) {
                throw FileTransferError(
                    Misc, {}, "uri %s changed %s from %s to %s during transfer", uri, what, from, to
                );
            }
        }

        bool attemptRetry(const std::string & context)
        {
            auto state(transfer->downloadState.lock());

            assert(state->data.empty());

            thread_local std::minstd_rand random{std::random_device{}()};
            std::uniform_real_distribution<> dist(0.0, 0.5);
            int ms = parent.baseRetryTimeMs * std::pow(2.0f, attempt - 1 + dist(random));
            if (totalReceived) {
                warn("%s; retrying from offset %d in %d ms", context, totalReceived, ms);
            } else {
                warn("%s; retrying in %d ms", context, ms);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(ms));

            state->exc = nullptr;

            // use the effective URI of the previous transfer for retries. this avoids
            // some silent corruption if a redirect changes between starting and retry
            const auto & uri = metadata.effectiveUri.empty() ? this->uri : metadata.effectiveUri;

            auto newMeta = startTransfer(uri, totalReceived);
            throwChangedTarget("final destination", metadata.effectiveUri, newMeta.effectiveUri);
            throwChangedTarget("ETag", metadata.etag, newMeta.etag);
            throwChangedTarget(
                "immutable url",
                metadata.immutableUrl.value_or(""),
                newMeta.immutableUrl.value_or("")
            );

            return true;
        }

        void awaitData()
        {
            withRetries([&] {
                /* Grab data if available, otherwise wait for the download
                   thread to wake us up. */
                while (buffered.empty()) {
                    auto state(transfer->downloadState.lock());

                    if (state->data.empty()) {
                        if (state->exc) {
                            std::rethrow_exception(state->exc);
                        } else if (state->done) {
                            return;
                        }

                        transfer->unpause();
                        state.wait(transfer->downloadEvent);
                    }

                    chunk = std::move(state->data);
                    buffered = chunk;
                    totalReceived += chunk.size();
                    transfer->unpause();
                }
            });
        }

        size_t read(char * data, size_t len) override
        {
            auto readPartial = [this](char * data, size_t len) -> size_t {
                const auto available = std::min(len, buffered.size());
                if (available == 0u) return 0u;

                memcpy(data, buffered.data(), available);
                buffered.remove_prefix(available);
                return available;
            };
            size_t total = readPartial(data, len);

            while (total < len) {
                awaitData();
                const auto current = readPartial(data + total, len - total);
                total += current;
                if (total == 0 || current == 0) {
                    break;
                }
            }

            if (total == 0) {
                throw EndOfFile("transfer finished");
            }

            return total;
        }
    };

    bool exists(const std::string & uri, const Headers & headers) override
    {
        try {
            enqueueFileTransfer(uri, headers, std::nullopt, true);
            return true;
        } catch (FileTransferError & e) {
            /* S3 buckets return 403 if a file doesn't exist and the
                bucket is unlistable, so treat 403 as 404. */
            if (e.error == FileTransfer::NotFound || e.error == FileTransfer::Forbidden)
                return false;
            throw;
        }
    }

    std::pair<FileTransferResult, box_ptr<Source>>
    download(const std::string & uri, const Headers & headers) override
    {
        return enqueueFileTransfer(uri, headers, std::nullopt, false);
    }
};

ref<curlFileTransfer> makeCurlFileTransfer(std::optional<unsigned int> baseRetryTimeMs)
{
    return make_ref<curlFileTransfer>(baseRetryTimeMs.value_or(250));
}

ref<FileTransfer> getFileTransfer()
{
    static ref<curlFileTransfer> fileTransfer = makeCurlFileTransfer({});

    if (fileTransfer->state_.lock()->quit)
        fileTransfer = makeCurlFileTransfer({});

    return fileTransfer;
}

ref<FileTransfer> makeFileTransfer(std::optional<unsigned int> baseRetryTimeMs)
{
    return makeCurlFileTransfer(baseRetryTimeMs);
}

template<typename... Args>
FileTransferError::FileTransferError(FileTransfer::Error error, std::optional<std::string> response, const Args & ... args)
    : Error(args...), error(error), response(response)
{
    const auto hf = HintFmt(args...);
    // FIXME: Due to https://github.com/NixOS/nix/issues/3841 we don't know how
    // to print different messages for different verbosity levels. For now
    // we add some heuristics for detecting when we want to show the response.
    if (response && (response->size() < 1024 || response->find("<html>") != std::string::npos))
        err.msg = HintFmt("%1%\n\nresponse body:\n\n%2%", Uncolored(hf.str()), chomp(*response));
    else
        err.msg = hf;
}

}
