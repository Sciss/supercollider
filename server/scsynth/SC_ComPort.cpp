/*
    SuperCollider real time audio synthesis system
    Copyright (c) 2002 James McCartney. All rights reserved.
    http://www.audiosynth.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "SC_Endian.h"
#include "SC_HiddenWorld.h"
#include "SC_WorldOptions.h"
#include "sc_msg_iter.h"
#include "SC_OscUtils.hpp"

#include <ctype.h>
#include <stdexcept>
#include <stdarg.h>
#include <cerrno>

#include <sys/types.h>
#include "OSC_Packet.h"

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/bind/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/typeof/typeof.hpp>

#include "SC_Lock.h"

#include "nova-tt/semaphore.hpp"
#include "nova-tt/thread_priority.hpp"

#ifdef USE_RENDEZVOUS
#    include "Rendezvous.h"
#endif

#ifdef __EMSCRIPTEN__
#    include <emscripten.h>
#    include <emscripten/bind.h>
#endif


bool ProcessOSCPacket(World* inWorld, OSC_Packet* inPacket);

namespace scsynth {

//////////////////////////////////////////////////////////////////////////////////////////////////////////


static bool UnrollOSCPacket(World* inWorld, int inSize, char* inData, OSC_Packet* inPacket) {
    if (!strcmp(inData, "#bundle")) { // is a bundle
        char* data;
        char* dataEnd = inData + inSize;
        int len = 16;
        bool hasNestedBundle = false;

        // get len of nested messages only, without len of nested bundle(s)
        data = inData + 16; // skip bundle header
        while (data < dataEnd) {
            int32 msgSize = OSCint(data);
            data += sizeof(int32);
            if (strcmp(data, "#bundle")) // is a message
                len += sizeof(int32) + msgSize;
            else
                hasNestedBundle = true;
            data += msgSize;
        }

        if (hasNestedBundle) {
            if (len > 16) { // not an empty bundle
                // add nested messages to bundle buffer
                char* buf = (char*)malloc(len);
                inPacket->mSize = len;
                inPacket->mData = buf;

                memcpy(buf, inData, 16); // copy bundle header
                data = inData + 16; // skip bundle header
                while (data < dataEnd) {
                    int32 msgSize = OSCint(data);
                    data += sizeof(int32);
                    if (strcmp(data, "#bundle")) { // is a message
                        memcpy(buf, data - sizeof(int32), sizeof(int32) + msgSize);
                        buf += msgSize;
                    }
                    data += msgSize;
                }

                // process this packet without its nested bundle(s)
                if (!ProcessOSCPacket(inWorld, inPacket)) {
                    free(buf);
                    return false;
                }
            }

            // process nested bundle(s)
            data = inData + 16; // skip bundle header
            while (data < dataEnd) {
                int32 msgSize = OSCint(data);
                data += sizeof(int32);
                if (!strcmp(data, "#bundle")) { // is a bundle
                    OSC_Packet* packet = (OSC_Packet*)malloc(sizeof(OSC_Packet));
                    memcpy(packet, inPacket, sizeof(OSC_Packet)); // clone inPacket

                    if (!UnrollOSCPacket(inWorld, msgSize, data, packet)) {
                        free(packet);
                        return false;
                    }
                }
                data += msgSize;
            }
        } else { // !hasNestedBundle
            char* buf = (char*)malloc(inSize);
            inPacket->mSize = inSize;
            inPacket->mData = buf;
            memcpy(buf, inData, inSize);

            if (!ProcessOSCPacket(inWorld, inPacket)) {
                free(buf);
                return false;
            }
        }
    } else { // is a message
        char* buf = (char*)malloc(inSize);
        inPacket->mSize = inSize;
        inPacket->mData = buf;
        memcpy(buf, inData, inSize);

        if (!ProcessOSCPacket(inWorld, inPacket)) {
            free(buf);
            return false;
        }
    }

    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

SC_Thread gAsioThread;
boost::asio::io_service ioService;

const int kTextBufSize = 65536;


static void udp_reply_func(struct ReplyAddress* addr, char* msg, int size) {
    using namespace boost::asio;

    ip::udp::socket* socket = reinterpret_cast<ip::udp::socket*>(addr->mReplyData);
    ip::udp::endpoint endpoint(addr->mAddress, addr->mPort);

    boost::system::error_code errc;
    socket->send_to(buffer(msg, size), endpoint, 0, errc);

    if (errc)
        printf("%s\n", errc.message().c_str());
}

static void tcp_reply_func(struct ReplyAddress* addr, char* msg, int size) {
    // Write size as 32bit unsigned network-order integer
    uint32 u = sc_htonl(size);

    using namespace boost::asio;

    // FIXME: connection could be destroyed!
    ip::tcp::socket* socket = reinterpret_cast<ip::tcp::socket*>(addr->mReplyData);

#if 0
	ip::tcp::socket::message_flags flags = 0;
#    ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#    endif
#endif

    boost::system::error_code errc;
    write(*socket, buffer(&u, sizeof(uint32)), errc);
    if (errc)
        printf("%s\n", errc.message().c_str());

    write(*socket, buffer(msg, size), errc);
    if (errc)
        printf("%s\n", errc.message().c_str());
}

#ifdef __EMSCRIPTEN__

// this is always called on the same thread, although different
// from the main thread. In order to access `Module` and do
// anything useful on the JS side, execution has to be deferred
// to the main thread. The blocking function `MAIN_THREAD_EM_ASM`
// is used so that the `msg` can be safely used without copying.
static void web_reply_func(struct ReplyAddress* addr, char* msg, int size) {
    // clang-format off
    MAIN_THREAD_EM_ASM({
        var clientPort  = $0;
        var od          = Module.oscDriver;
        var ep          = od ? od[clientPort] : undefined;
        var rcv         = ep ? ep['receive' ] : undefined;
        if (typeof rcv == 'function') {
            var serverPort  = $1;
            var ptr         = $2;
            var dataSize    = $3;
            var data        = new Uint8Array(Module.HEAPU8.buffer, ptr, dataSize);
            try {
                rcv(serverPort, data);
            } catch (e) {
                console.log("Error in OSC reply handler: ", e.message);
            }
        }
    }, addr->mPort, addr->mSocket, msg, size);
    // clang-format on
}
#endif

class SC_UdpInPort {
    struct World* mWorld;
    int mPortNum;
    std::string mbindTo;
    boost::array<char, kTextBufSize> recvBuffer;

    boost::asio::ip::udp::endpoint remoteEndpoint;

#ifdef USE_RENDEZVOUS
    SC_Thread mRendezvousThread;
#endif

    void handleReceivedUDP(const boost::system::error_code& error, std::size_t bytes_transferred) {
        if (error == boost::asio::error::operation_aborted)
            return; /* we're done */

        if (error) {
            printf("SC_UdpInPort: received error - %s", error.message().c_str());
            startReceiveUDP();
            return;
        }

        if (mWorld->mDumpOSC)
            dumpOSC(mWorld->mDumpOSC, bytes_transferred, recvBuffer.data());

        OSC_Packet* packet = (OSC_Packet*)malloc(sizeof(OSC_Packet));

        packet->mReplyAddr.mProtocol = kUDP;
        packet->mReplyAddr.mAddress = remoteEndpoint.address();
        packet->mReplyAddr.mPort = remoteEndpoint.port();
        packet->mReplyAddr.mSocket = udpSocket.native_handle();
        packet->mReplyAddr.mReplyFunc = udp_reply_func;
        packet->mReplyAddr.mReplyData = (void*)&udpSocket;

        packet->mSize = bytes_transferred;

        if (!UnrollOSCPacket(mWorld, bytes_transferred, recvBuffer.data(), packet))
            free(packet);

        startReceiveUDP();
    }

    void startReceiveUDP() {
        using namespace boost;
        udpSocket.async_receive_from(asio::buffer(recvBuffer), remoteEndpoint,
                                     boost::bind(&SC_UdpInPort::handleReceivedUDP, this, asio::placeholders::error,
                                                 asio::placeholders::bytes_transferred));
    }

public:
    boost::asio::ip::udp::socket udpSocket;

    SC_UdpInPort(struct World* world, std::string bindTo, int inPortNum):
        mWorld(world),
        mPortNum(inPortNum),
        mbindTo(bindTo),
        udpSocket(ioService) {
        using namespace boost::asio;
        BOOST_AUTO(protocol, ip::udp::v4());
        udpSocket.open(protocol);

        udpSocket.bind(ip::udp::endpoint(boost::asio::ip::address::from_string(bindTo), inPortNum));

        boost::asio::socket_base::send_buffer_size option(65536);
        udpSocket.set_option(option);

#ifdef USE_RENDEZVOUS
        if (world->mRendezvous) {
            SC_Thread thread(boost::bind(PublishPortToRendezvous, kSCRendezvous_UDP, sc_htons(mPortNum)));
            mRendezvousThread = std::move(thread);
        }
#endif

        startReceiveUDP();
    }
};


class SC_TcpConnection : public boost::enable_shared_from_this<SC_TcpConnection> {
public:
    struct World* mWorld;
    typedef boost::shared_ptr<SC_TcpConnection> pointer;
    boost::asio::ip::tcp::socket socket;

    SC_TcpConnection(struct World* world, boost::asio::io_service& ioService, class SC_TcpInPort* parent):
        mWorld(world),
        socket(ioService),
        mParent(parent) {}

    ~SC_TcpConnection();

    void start() {
        const int kMaxPasswordLen = 32;
        char buf[kMaxPasswordLen];
        int32 size;
        int32 msglen;

        boost::system::error_code error;
        boost::asio::ip::tcp::no_delay noDelayOption(true);
        socket.set_option(noDelayOption, error);

        // first message must be the password. 4 tries.
        bool validated = mWorld->hw->mPassword[0] == 0;
        for (int i = 0; !validated && i < 4; ++i) {
            // FIXME: error handling!
            size = boost::asio::read(socket, boost::asio::buffer((void*)&msglen, sizeof(int32)));
            if (size < 0)
                return;

            msglen = sc_ntohl(msglen);
            if (msglen > kMaxPasswordLen)
                break;

            size = boost::asio::read(socket, boost::asio::buffer((void*)buf, msglen));
            if (size < 0)
                return;

            validated = strcmp(buf, mWorld->hw->mPassword) == 0;

            std::this_thread::sleep_for(std::chrono::seconds(i + 1)); // thwart cracking.
        }

        if (validated)
            startReceiveMessage();
    }

private:
    void startReceiveMessage() {
        namespace ba = boost::asio;
        async_read(socket, ba::buffer(&OSCMsgLength, sizeof(OSCMsgLength)),
                   boost::bind(&SC_TcpConnection::handleLengthReceived, shared_from_this(), ba::placeholders::error,
                               ba::placeholders::bytes_transferred));
    }

    int32 OSCMsgLength;
    char* data;
    class SC_TcpInPort* mParent;

    void handleLengthReceived(const boost::system::error_code& error, size_t bytes_transferred) {
        if (error) {
            if (error == boost::asio::error::eof)
                return; // connection closed

            printf("handleLengthReceived: error %s", error.message().c_str());
            return;
        }

        namespace ba = boost::asio;
        // msglen is in network byte order
        OSCMsgLength = sc_ntohl(OSCMsgLength);

        data = (char*)malloc(OSCMsgLength);

        async_read(socket, ba::buffer(data, OSCMsgLength),
                   boost::bind(&SC_TcpConnection::handleMsgReceived, shared_from_this(), ba::placeholders::error,
                               ba::placeholders::bytes_transferred));
    }

    void handleMsgReceived(const boost::system::error_code& error, size_t bytes_transferred) {
        if (error) {
            free(data);
            if (error == boost::asio::error::eof)
                return; // connection closed

            printf("handleMsgReceived: error %s", error.message().c_str());
            return;
        }

        assert(bytes_transferred == OSCMsgLength);

        if (mWorld->mDumpOSC)
            dumpOSC(mWorld->mDumpOSC, bytes_transferred, data);

        OSC_Packet* packet = (OSC_Packet*)malloc(sizeof(OSC_Packet));

        packet->mReplyAddr.mProtocol = kTCP;
        packet->mReplyAddr.mReplyFunc = tcp_reply_func;
        packet->mReplyAddr.mReplyData = (void*)&socket;

        packet->mSize = OSCMsgLength;

        if (!UnrollOSCPacket(mWorld, bytes_transferred, data, packet))
            free(packet);

        startReceiveMessage();
    }
};

class SC_TcpInPort {
    struct World* mWorld;
    boost::asio::ip::tcp::acceptor acceptor;

#ifdef USE_RENDEZVOUS
    SC_Thread mRendezvousThread;
#endif

    std::atomic<int> mAvailableConnections;
    friend class SC_TcpConnection;

public:
    SC_TcpInPort(struct World* world, const std::string& bindTo, int inPortNum, int inMaxConnections, int inBacklog):
        mWorld(world),
        acceptor(ioService, boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string(bindTo), inPortNum)),
        mAvailableConnections(inMaxConnections) {
        // FIXME: backlog???

#ifdef USE_RENDEZVOUS
        if (world->mRendezvous) {
            SC_Thread thread(boost::bind(PublishPortToRendezvous, kSCRendezvous_TCP, sc_htons(inPortNum)));
            mRendezvousThread = std::move(thread);
        }
#endif

        startAccept();
    }

    void startAccept() {
        if (mAvailableConnections > 0) {
            --mAvailableConnections;
            SC_TcpConnection::pointer newConnection(new SC_TcpConnection(mWorld, ioService, this));

            acceptor.async_accept(
                newConnection->socket,
                boost::bind(&SC_TcpInPort::handleAccept, this, newConnection, boost::asio::placeholders::error));
        }
    }

    void handleAccept(SC_TcpConnection::pointer newConnection, const boost::system::error_code& error) {
        if (!error)
            newConnection->start();
        startAccept();
    }

    void connectionDestroyed() {
        if (!mWorld->mRunning)
            return;
        mAvailableConnections += 1;
        startAccept();
    }
};

SC_TcpConnection::~SC_TcpConnection() { mParent->connectionDestroyed(); }


#ifdef __EMSCRIPTEN__

// #define SC_WEB_IN_PORT_DEBUG

static const char* kWebInPortIdent = "SC_WebInPort";

class SC_WebInPort {
    struct World* mWorld;
    int mPortNum;
    std::string mBindTo;
    char* mBufPtr;

public:
    SC_WebInPort(struct World* world, std::string bindTo, int inPortNum):
        mWorld(world),
        mPortNum(inPortNum),
        mBindTo(bindTo) {
#    ifdef SC_WEB_IN_PORT_DEBUG
        scprintf("%s: new ip %s port %d.\n", kWebInPortIdent, bindTo.c_str(), inPortNum);
#    endif

        if (SC_WebInPort::current != NULL) {
            throw std::runtime_error("SC_WebInPort: concurrent modification\n");
        }
        SC_WebInPort::current = this;

        // clang-format off
        EM_ASM({
            var serverPort      = $0;
            var maxNumBytes     = $1;
            if (!Module.oscDriver) Module.oscDriver = {};
            var self            = Module.web_in_port();
            var od              = Module.oscDriver;
            var ep              = {};
            ep.instance         = self;
            ep.bufPtr           = Module._malloc(maxNumBytes);
            ep.byteBuf          = new Uint8Array(Module.HEAPU8.buffer, ep.bufPtr, maxNumBytes);
            ep.receive = function(addr, data) {
                if (!addr) addr = 0;
                var sz = data.byteLength;
                if (sz < maxNumBytes) {
                    ep.byteBuf.set(data);
                    ep.instance.Receive(addr, sz);
                } else {
                    throw new Error('oscDriver.send: message size exceeded: ' + sz);
                }
            };
            od[serverPort]      = ep;
            self.InitBuffer(ep.bufPtr);

        }, inPortNum, kTextBufSize);
        // clang-format on

        SC_WebInPort::current = NULL;
    }

    ~SC_WebInPort() {
        mBufPtr = NULL;
        // clang-format off
        EM_ASM({
            var serverPort  = $0;
            var od          = Module.oscDriver;
            var ep          = od ? od[server] : undefined;
            if (ep) {
                if (ep.bufPtr) {
                    Module._free(ep.bufPtr);
                }
                od[serverPort] = undefined;
            }
        }, mPortNum);
        // clang-format on
    }

    void InitBuffer(uintptr_t bufPtr) {
#    ifdef SC_WEB_IN_PORT_DEBUG
        scprintf("%s: InitBuffer.\n", kWebInPortIdent);
#    endif
        // cf. https://stackoverflow.com/questions/20355880/#27364643
        this->mBufPtr = reinterpret_cast<char*>(bufPtr);
    }

    void Receive(int remotePort, int bytes_transferred) {
#    ifdef SC_WEB_IN_PORT_DEBUG
        scprintf("%s: Receive(%d, %d).\n", kWebInPortIdent, remotePort, bytes_transferred);
#    endif

        if (mWorld->mDumpOSC)
            dumpOSC(mWorld->mDumpOSC, bytes_transferred, mBufPtr);

        OSC_Packet* packet = (OSC_Packet*)malloc(sizeof(OSC_Packet));

        packet->mReplyAddr.mProtocol = kWeb;
        packet->mReplyAddr.mAddress = boost::asio::ip::make_address("127.0.0.1"); // not used
        packet->mReplyAddr.mPort = remotePort;
        packet->mReplyAddr.mSocket = mPortNum;
        packet->mReplyAddr.mReplyFunc = web_reply_func;
        packet->mReplyAddr.mReplyData = NULL;

        packet->mSize = bytes_transferred;

        bool ok = UnrollOSCPacket(mWorld, bytes_transferred, mBufPtr, packet);

#    ifdef SC_WEB_IN_PORT_DEBUG
        scprintf("%s: Receive result %d.\n", kWebInPortIdent, ok);
#    endif

        if (!ok)
            free(packet);
    }

    // access to instances via temporary singleton which is needed from JS.
    static SC_WebInPort* current;
};

SC_WebInPort* SC_WebInPort::current = NULL;

// function callable from JS to obtain the singleton instance
extern "C" SC_WebInPort* web_in_port() {
    if (SC_WebInPort::current == NULL) {
        throw std::runtime_error("SC_WebInPort: instance currently not set\n");
    }
    return SC_WebInPort::current;
}

EMSCRIPTEN_BINDINGS(Web_Audio) {
    emscripten::class_<SC_WebInPort>("SC_WebInPort")
        .function("Receive", &SC_WebInPort::Receive, emscripten::allow_raw_pointers())
        .function("InitBuffer", &SC_WebInPort::InitBuffer, emscripten::allow_raw_pointers());
    emscripten::function("web_in_port", &web_in_port, emscripten::allow_raw_pointers());
}

#endif

//////////////////////////////////////////////////////////////////////////////////////////////////////////

static void asioFunction() {
#ifdef NOVA_TT_PRIORITY_RT
    std::pair<int, int> priorities = nova::thread_priority_interval_rt();
    nova::thread_set_priority_rt((priorities.first + priorities.second) / 2);
#else
    std::pair<int, int> priorities = nova::thread_priority_interval();
    nova::thread_set_priority(priorities.second);
#endif

    boost::asio::io_service::work work(ioService);
    ioService.run();
}

void startAsioThread() {
    SC_Thread asioThread(&asioFunction);
    gAsioThread = std::move(asioThread);
}

void stopAsioThread() {
    ioService.stop();
    gAsioThread.join();
}

bool asioThreadStarted() { return gAsioThread.joinable(); }

}

using namespace scsynth;

//////////////////////////////////////////////////////////////////////////////////////////////////////////


SCSYNTH_DLLEXPORT_C bool World_SendPacketWithContext(World* inWorld, int inSize, char* inData, ReplyFunc inFunc,
                                                     void* inContext) {
    if (inSize > 0) {
        if (inWorld->mDumpOSC)
            dumpOSC(inWorld->mDumpOSC, inSize, inData);

        OSC_Packet* packet = (OSC_Packet*)malloc(sizeof(OSC_Packet));

        packet->mReplyAddr.mAddress = boost::asio::ip::address();
        packet->mReplyAddr.mReplyFunc = inFunc;
        packet->mReplyAddr.mReplyData = inContext;
        packet->mReplyAddr.mSocket = 0;

        if (!UnrollOSCPacket(inWorld, inSize, inData, packet)) {
            free(packet);
            return false;
        }
    }
    return true;
}

SCSYNTH_DLLEXPORT_C bool World_SendPacket(World* inWorld, int inSize, char* inData, ReplyFunc inFunc) {
    return World_SendPacketWithContext(inWorld, inSize, inData, inFunc, nullptr);
}

template <typename T, typename... Args> static bool protectedOpenPort(const char* socketType, Args&&... args) noexcept {
    try {
        new T(std::forward<Args>(args)...);
        return true;
    } catch (const boost::system::system_error& exc) {
        // Special verbose message to help with common issue. Issue #3969
        if (exc.code() == boost::system::errc::address_in_use) {
            scprintf("\n*** ERROR: failed to open %s socket: address in use.\n"
                     "This could be because another instance of scsynth is already using it.\n"
                     "You can use SuperCollider (sclang) to kill all running servers by running `Server.killAll`.\n"
                     "You can also kill scsynth using a terminal or your operating system's task manager.\n",
                     socketType);
        } else {
            scprintf("\n*** ERROR: failed to open %s socket: %s\n", socketType, exc.what());
        }
    } catch (const std::exception& exc) {
        scprintf("\n*** ERROR: failed to open %s socket: %s\n", socketType, exc.what());
    } catch (...) {
        scprintf("\n*** ERROR: failed to open %s socket: Unknown error\n", socketType);
    }
    return false;
}

SCSYNTH_DLLEXPORT_C int World_OpenUDP(struct World* inWorld, const char* bindTo, int inPort) {
#ifdef __EMSCRIPTEN__
    // when running in the browser, a special 'web' protocol is used in place of 'udp'.
    // that way scsynth can be started as usual with the '-u' switch
    return protectedOpenPort<SC_WebInPort>("Web", inWorld, bindTo, inPort);
#else
    return protectedOpenPort<SC_UdpInPort>("UDP", inWorld, bindTo, inPort);
#endif
}

SCSYNTH_DLLEXPORT_C int World_OpenTCP(struct World* inWorld, const char* bindTo, int inPort, int inMaxConnections,
                                      int inBacklog) {
    return protectedOpenPort<SC_TcpInPort>("TCP", inWorld, bindTo, inPort, inMaxConnections, inBacklog);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
