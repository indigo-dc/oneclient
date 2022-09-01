/**
 * @file fslogicWrapper.cc
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "nullHelper.h"

#include "communication/communicator.h"
#include "context.h"
#include "events/manager.h"
#include "fslogic/fsLogic.h"
#include "fslogic/withUuids.h"
#include "messages/configuration.h"
#include "options/options.h"
#include "scheduler.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/make_shared.hpp>
#include <boost/python.hpp>
#include <boost/python/suite/indexing/vector_indexing_suite.hpp>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/fibers/ForEach.h>
#if FUSE_USE_VERSION > 30
#include <fuse3/fuse.h>
#else
#include <fuse/fuse.h>
#endif

#include <atomic>
#include <memory>

using namespace one;
using namespace one::client;
using namespace one::communication;
using namespace boost::python;
using namespace std::literals;

struct Stat {
    time_t atime;
    time_t mtime;
    time_t ctime;
    int gid;
    int uid;
    int mode;
    size_t size;

    bool operator==(const Stat &o)
    {
        return atime == o.atime && mtime == o.mtime && ctime == o.ctime &&
            gid == o.gid && uid == o.uid && mode == o.mode && size == o.size;
    }
};

struct Ubuf {
    time_t actime;
    time_t modtime;
};

struct Xattr {
    std::string name;
    std::string value;
};

class ReleaseGIL {
public:
    ReleaseGIL()
        : threadState{PyEval_SaveThread(), PyEval_RestoreThread}
    {
    }

private:
    std::unique_ptr<PyThreadState, decltype(&PyEval_RestoreThread)> threadState;
};

class HelpersCacheProxy : public one::client::cache::HelpersCache {
public:
    using HelpersCache::HelpersCache;

    std::shared_ptr<NullHelperMock> m_helper =
        std::make_shared<NullHelperMock>();

    folly::Future<HelperPtr> get(const folly::fbstring &,
        const folly::fbstring &, const folly::fbstring &, const bool,
        const bool) override
    {
        m_helper->m_real.setNeedsDataConsistencyCheck(
            m_needsDataConsistencyCheck);
        return folly::makeFuture<HelperPtr>(m_helper);
    }

    bool m_needsDataConsistencyCheck{false};
};

constexpr auto FSLOGIC_PROXY_RETRY_COUNT = 2;

class FsLogicProxy {
public:
    FsLogicProxy(std::shared_ptr<Context> context,
        unsigned int metadataCacheSize = 10000,
        unsigned int dropDirectoryCacheAfter = 60)
        : m_helpersCache{new HelpersCacheProxy(*context->communicator(),
              context->scheduler(), *context->options())}
        , m_fsLogic{context, std::make_shared<messages::Configuration>(),
              std::unique_ptr<HelpersCacheProxy>{m_helpersCache},
              metadataCacheSize, false, false,
              context->options()->getProviderTimeout(),
              std::chrono::seconds{dropDirectoryCacheAfter},
              makeRunInFiber() /*[](auto f) { f(); }*/, false}
        , m_context{context}
    {
        m_fsLogic.setMaxRetryCount(FSLOGIC_PROXY_RETRY_COUNT);
        m_thread = std::thread{[this] {
            folly::setThreadName("InFiber");
            m_eventBase.loopForever();
        }};
    }

    void start()
    {
        ReleaseGIL guard;
        m_fsLogic.start();
    }

    void stop()
    {
        if (!m_stopped.test_and_set()) {
            ReleaseGIL guard;

            folly::Promise<folly::Unit> stopped;
            auto stoppedFuture = stopped.getFuture();

            m_fiberManager.addTaskRemote(
                [this, stopped = std::move(stopped)]() mutable {
                    m_fsLogic.stop();
                    stopped.setValue();
                });

            std::move(stoppedFuture).get();
        }
    }

    ~FsLogicProxy()
    {
        stop();

        ReleaseGIL guard;
        m_eventBase.terminateLoopSoon();
        m_thread.join();
    }

    void failHelper()
    {
        m_helpersCache->m_helper->set_ec(
            std::make_error_code(std::errc::owner_dead));
    }

    struct statvfs statfs(std::string uuid)
    {
        ReleaseGIL guard;

        return m_fiberManager
            .addTaskRemoteFuture(
                [this, uuid]() { return m_fsLogic.statfs(uuid); })
            .get();
    }

    Stat lookup(std::string parentUuid, std::string name)
    {
        ReleaseGIL guard;

        auto attr = m_fiberManager
                        .addTaskRemoteFuture([this, parentUuid, name]() {
                            return m_fsLogic.lookup(parentUuid, name);
                        })
                        .get();

        auto statbuf = one::client::fslogic::detail::toStatbuf(attr, 123);
        Stat stat;

        stat.atime = statbuf.st_atime;
        stat.mtime = statbuf.st_mtime;
        stat.ctime = statbuf.st_ctime;
        stat.gid = statbuf.st_gid;
        stat.uid = statbuf.st_uid;
        stat.mode = statbuf.st_mode;
        stat.size = statbuf.st_size;

        return stat;
    }

    Stat getattr(std::string uuid)
    {
        ReleaseGIL guard;

        auto attr = m_fiberManager
                        .addTaskRemoteFuture(
                            [this, uuid]() { return m_fsLogic.getattr(uuid); })
                        .get();

        auto statbuf = one::client::fslogic::detail::toStatbuf(attr, 123);
        Stat stat;

        stat.atime = statbuf.st_atime;
        stat.mtime = statbuf.st_mtime;
        stat.ctime = statbuf.st_ctime;
        stat.gid = statbuf.st_gid;
        stat.uid = statbuf.st_uid;
        stat.mode = statbuf.st_mode;
        stat.size = statbuf.st_size;

        return stat;
    }

    void mkdir(std::string parentUuid, std::string name, int mode)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, parentUuid, name, mode]() {
                m_fsLogic.mkdir(parentUuid, name, mode);
            })
            .get();
    }

    void unlink(std::string parentUuid, std::string name)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, parentUuid, name]() {
                m_fsLogic.unlink(parentUuid, name);
            })
            .get();
    }

    void rmdir(std::string parentUuid, std::string name)
    {
        unlink(parentUuid, name);
    }

    void rename(std::string parentUuid, std::string name,
        std::string newParentUuid, std::string newName)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture(
                [this, parentUuid, name, newParentUuid, newName]() {
                    m_fsLogic.rename(parentUuid, name, newParentUuid, newName);
                })
            .get();
    }

    void chmod(std::string uuid, int mode)
    {
        ReleaseGIL guard;

        m_fiberManager
            .addTaskRemoteFuture([this, uuid, mode]() {
                struct stat statbuf = {};
                statbuf.st_mode = mode;
                m_fsLogic.setattr(uuid, statbuf, FUSE_SET_ATTR_MODE);
            })
            .get();
    }

    void utime(std::string uuid)
    {
        ReleaseGIL guard;

#if defined(FUSE_SET_ATTR_ATIME_NOW) && defined(FUSE_SET_ATTR_MTIME_NOW)

        m_fiberManager
            .addTaskRemoteFuture([this, uuid]() {
                struct stat statbuf = {};
                m_fsLogic.setattr(uuid, statbuf,
                    FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW);
            })
            .get();
#endif
    }

    void utime_buf(std::string uuid, Ubuf ubuf)
    {
        ReleaseGIL guard;

        m_fiberManager
            .addTaskRemoteFuture([this, uuid, ubuf]() {
                struct stat statbuf = {};
                statbuf.st_atime = ubuf.actime;
                statbuf.st_mtime = ubuf.modtime;

                m_fsLogic.setattr(
                    uuid, statbuf, FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME);
            })
            .get();
    }

    int opendir(std::string uuid)
    {
        ReleaseGIL guard;

        return m_fiberManager
            .addTaskRemoteFuture(
                [this, uuid]() { return m_fsLogic.opendir(uuid); })
            .get();
    }

    void releasedir(std::string uuid, int fuseHandleId)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, uuid, fuseHandleId]() {
                m_fsLogic.releasedir(uuid, fuseHandleId);
            })
            .get();
    }

    std::vector<std::string> readdir(
        std::string uuid, int chunkSize, int offset)
    {
        ReleaseGIL guard;

        return m_fiberManager
            .addTaskRemoteFuture([this, uuid, chunkSize, offset]() {
                std::vector<std::string> children;
                for (auto &name : m_fsLogic.readdir(uuid, chunkSize, offset))
                    children.emplace_back(name.toStdString());

                return children;
            })
            .get();
    }

    void mknod(std::string parentUuid, std::string name, int mode)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, parentUuid, name, mode]() {
                m_fsLogic.mknod(parentUuid, name, mode);
            })
            .get();
    }

    void link(std::string uuid, std::string parentUuid, std::string name)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, uuid, parentUuid, name]() {
                m_fsLogic.link(uuid, parentUuid, name);
            })
            .get();
    }

    void symlink(std::string parentUuid, std::string name, std::string link)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, parentUuid, name, link]() {
                m_fsLogic.symlink(parentUuid, name, link);
            })
            .get();
    }

    std::string readlink(std::string uuid)
    {
        ReleaseGIL guard;
        return m_fiberManager
            .addTaskRemoteFuture([this, uuid]() {
                return m_fsLogic.readlink(uuid).toStdString();
            })
            .get();
    }

    int open(std::string uuid, int flags)
    {
        ReleaseGIL guard;
        return m_fiberManager
            .addTaskRemoteFuture(
                [this, uuid, flags]() { return m_fsLogic.open(uuid, flags); })
            .get();
    }

    std::string read(std::string uuid, int fileHandleId, int offset, int size)
    {
        ReleaseGIL guard;
        return m_fiberManager
            .addTaskRemoteFuture([this, uuid, fileHandleId, offset, size]() {
                auto buf = m_fsLogic.read(uuid, fileHandleId, offset, size, {},
                    FSLOGIC_PROXY_RETRY_COUNT);

                std::string data;
                buf.appendToString(data);

                return data;
            })
            .get();
    }

    int write(std::string uuid, int fuseHandleId, int offset, int size)
    {
        ReleaseGIL guard;

        return m_fiberManager
            .addTaskRemoteFuture([this, uuid, fuseHandleId, offset, size]() {
                auto buf = folly::IOBuf::create(size);
                buf->append(size);

                return m_fsLogic.write(uuid, fuseHandleId, offset,
                    std::move(buf), FSLOGIC_PROXY_RETRY_COUNT);
            })
            .get();
    }

    void release(std::string uuid, int fuseHandleId)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, uuid, fuseHandleId]() {
                m_fsLogic.release(uuid, fuseHandleId);
            })
            .get();
    }

    void truncate(std::string uuid, int size)
    {
        ReleaseGIL guard;

        m_fiberManager
            .addTaskRemoteFuture([this, uuid, size]() {
                struct stat statbuf = {};
                statbuf.st_size = size;
                m_fsLogic.setattr(uuid, statbuf, FUSE_SET_ATTR_SIZE);
            })
            .get();
    }

    std::vector<std::string> listxattr(std::string uuid)
    {
        ReleaseGIL guard;

        return m_fiberManager
            .addTaskRemoteFuture([this, uuid]() {
                std::vector<std::string> xattrs;
                for (auto &xattrName : m_fsLogic.listxattr(uuid))
                    xattrs.emplace_back(xattrName.toStdString());

                return xattrs;
            })
            .get();
    }

    Xattr getxattr(std::string uuid, std::string name)
    {
        ReleaseGIL guard;

        return m_fiberManager
            .addTaskRemoteFuture([this, uuid, name]() {
                auto xattrValue = m_fsLogic.getxattr(uuid, name);

                Xattr xattr;
                xattr.name = name;
                xattr.value = xattrValue.toStdString();

                return xattr;
            })
            .get();
    }

    void setxattr(std::string uuid, std::string name, std::string value,
        bool create, bool replace)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture([this, uuid, name, value, create, replace]() {
                m_fsLogic.setxattr(uuid, name, value, create, replace);
            })
            .get();
    }

    void removexattr(std::string uuid, std::string name)
    {
        ReleaseGIL guard;
        m_fiberManager
            .addTaskRemoteFuture(
                [this, uuid, name]() { m_fsLogic.removexattr(uuid, name); })
            .get();
    }

    int metadataCacheSize()
    {
        ReleaseGIL guard;
        return m_fsLogic.metadataCache().size();
    }

    bool metadataCacheContains(std::string uuid)
    {
        ReleaseGIL guard;
        return m_fiberManager
            .addTaskRemoteFuture([this, uuid]() {
                return m_fsLogic.metadataCache().contains(uuid);
            })
            .get();
    }

    void expect_call_sh_open(std::string uuid, int times)
    {
        m_helpersCache->m_helper->expect_call_sh_open(uuid, times);
    }

    void expect_call_sh_release(std::string uuid, int times)
    {
        m_helpersCache->m_helper->expect_call_sh_release(uuid, times);
    }

    bool verify_and_clear_expectations()
    {
        return m_helpersCache->m_helper->verify_and_clear_expectations();
    }

    void setNeedsDataConsistencyCheck(bool needsDataConsistencyCheck)
    {
        m_helpersCache->m_needsDataConsistencyCheck = needsDataConsistencyCheck;
    }

private:
    folly::fibers::FiberManager::Options makeFiberManagerOpts()
    {
        folly::fibers::FiberManager::Options opts;
        opts.stackSize = FIBER_STACK_SIZE;
        return opts;
    }

    std::function<void(folly::Function<void()>)> makeRunInFiber()
    {
        return [this](folly::Function<void()> fun) mutable {
            m_fiberManager.addTaskRemote(std::move(fun));
        };
    }

    folly::EventBase m_eventBase;
    folly::fibers::FiberManager &m_fiberManager{
        folly::fibers::getFiberManager(m_eventBase, makeFiberManagerOpts())};

    std::thread m_thread;
    std::atomic_flag m_stopped = ATOMIC_FLAG_INIT;

    HelpersCacheProxy *m_helpersCache;
    fslogic::FsLogic m_fsLogic;
    std::shared_ptr<Context> m_context;
};

namespace {
boost::shared_ptr<FsLogicProxy> create(std::string ip, int port,
    unsigned int metadataCacheSize, unsigned int dropDirectoryCacheAfter,
    std::string cliOptions = "")
{
    FLAGS_minloglevel = 1;

    auto communicator = std::make_shared<Communicator>(/*connections*/ 10,
        /*threads*/ 1, ip, port,
        /*verifyServerCertificate*/ false, /*upgrade to clproto*/ true,
        /*perform handshake*/ false);

    auto context = std::make_shared<Context>();
    context->setScheduler(std::make_shared<Scheduler>(1));
    context->setCommunicator(communicator);

    const auto globalConfigPath = boost::filesystem::unique_path();

    auto options = std::make_shared<options::Options>();
    std::vector<std::string> optionsTokens;
    std::string optionsString = std::string("oneclient -H ") + ip +
        " -t TOKEN --provider-timeout 5 " + cliOptions + " mountpoint";
    boost::split(optionsTokens, optionsString, boost::is_any_of(" "),
        boost::token_compress_on);

    std::vector<const char *> cmdArgs;
    std::transform(optionsTokens.begin(), optionsTokens.end(),
        std::back_inserter(cmdArgs), [](auto &s) { return s.c_str(); });

    options->parse(cmdArgs.size(), cmdArgs.data());
    context->setOptions(std::move(options));

    communicator->setScheduler(context->scheduler());

    communicator->connect();

    return boost::make_shared<FsLogicProxy>(
        context, metadataCacheSize, dropDirectoryCacheAfter);
}

int regularMode() { return S_IFREG; }

void translate(const std::errc &err)
{
    PyErr_SetString(
        PyExc_RuntimeError, std::make_error_code(err).message().c_str());
}
}

BOOST_PYTHON_MODULE(fslogic)
{
    PyEval_InitThreads();
    register_exception_translator<std::errc>(&translate);

    class_<struct statvfs>("StatVFS")
        .def_readonly("bsize", &statvfs::f_bsize)
        .def_readonly("frsize", &statvfs::f_frsize)
        .def_readonly("blocks", &statvfs::f_blocks)
        .def_readonly("bfree", &statvfs::f_bfree)
        .def_readonly("bavail", &statvfs::f_bavail)
        .def_readonly("files", &statvfs::f_files)
        .def_readonly("ffree", &statvfs::f_ffree)
        .def_readonly("favail", &statvfs::f_favail)
        .def_readonly("fsid", &statvfs::f_fsid)
        .def_readonly("flag", &statvfs::f_flag)
        .def_readonly("namemax", &statvfs::f_namemax);

    class_<Stat>("Stat")
        .def_readonly("atime", &Stat::atime)
        .def_readonly("mtime", &Stat::mtime)
        .def_readonly("ctime", &Stat::ctime)
        .def_readonly("gid", &Stat::gid)
        .def_readonly("uid", &Stat::uid)
        .def_readonly("mode", &Stat::mode)
        .def_readonly("size", &Stat::size)
        .def("__eq__", &Stat::operator==);

    class_<Ubuf>("Ubuf")
        .def_readwrite("actime", &Ubuf::actime)
        .def_readwrite("modtime", &Ubuf::modtime);

    class_<Xattr>("Xattr")
        .def_readwrite("name", &Xattr::name)
        .def_readwrite("value", &Xattr::value);

    class_<std::vector<std::string>>("vector").def(
        vector_indexing_suite<std::vector<std::string>>());

    class_<FsLogicProxy, boost::noncopyable>("FsLogicProxy", no_init)
        .def("__init__", make_constructor(create))
        .def("failHelper", &FsLogicProxy::failHelper)
        .def("start", &FsLogicProxy::start)
        .def("stop", &FsLogicProxy::stop)
        .def("statfs", &FsLogicProxy::statfs)
        .def("getattr", &FsLogicProxy::getattr)
        .def("lookup", &FsLogicProxy::lookup)
        .def("mkdir", &FsLogicProxy::mkdir)
        .def("unlink", &FsLogicProxy::unlink)
        .def("rmdir", &FsLogicProxy::rmdir)
        .def("rename", &FsLogicProxy::rename)
        .def("chmod", &FsLogicProxy::chmod)
        .def("utime", &FsLogicProxy::utime)
        .def("utime_buf", &FsLogicProxy::utime_buf)
        .def("opendir", &FsLogicProxy::opendir)
        .def("releasedir", &FsLogicProxy::releasedir)
        .def("readdir", &FsLogicProxy::readdir)
        .def("mknod", &FsLogicProxy::mknod)
        .def("link", &FsLogicProxy::link)
        .def("symlink", &FsLogicProxy::symlink)
        .def("readlink", &FsLogicProxy::readlink)
        .def("open", &FsLogicProxy::open)
        .def("read", &FsLogicProxy::read)
        .def("write", &FsLogicProxy::write)
        .def("release", &FsLogicProxy::release)
        .def("truncate", &FsLogicProxy::truncate)
        .def("listxattr", &FsLogicProxy::listxattr)
        .def("getxattr", &FsLogicProxy::getxattr)
        .def("setxattr", &FsLogicProxy::setxattr)
        .def("removexattr", &FsLogicProxy::removexattr)
        .def("metadata_cache_size", &FsLogicProxy::metadataCacheSize)
        .def("metadata_cache_contains", &FsLogicProxy::metadataCacheContains)
        .def("set_needs_data_consistency_check",
            &FsLogicProxy::setNeedsDataConsistencyCheck)
        .def("expect_call_sh_open", &FsLogicProxy::expect_call_sh_open)
        .def("expect_call_sh_release", &FsLogicProxy::expect_call_sh_release)
        .def("verify_and_clear_expectations",
            &FsLogicProxy::verify_and_clear_expectations);

    def("regularMode", &regularMode);
}
