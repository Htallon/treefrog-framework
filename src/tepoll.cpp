/* Copyright (c) 2013, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include <QByteArray>
#include <QFileInfo>
#include <QBuffer>
#include <sys/types.h>
#include <sys/epoll.h>
#include <THttpRequestHeader>
#include <TSession>
#include "tepoll.h"
#include "tepollsocket.h"
#include "tsendbuffer.h"
#include "tepollwebsocket.h"
#include "tsessionmanager.h"
#include "tsystemglobal.h"
#include "tfcore_unix.h"

const int MaxEvents = 128;

static TEpoll *staticInstance;


class TSendData
{
public:
    enum Method {
        Disconnect,
        Send,
        SwitchToWebSocket,
    };

    int method;
    QByteArray uuid;
    TSendBuffer *buffer;
    THttpRequestHeader header;

    TSendData(Method m, const QByteArray &u, TSendBuffer *buf = 0)
        : method(m), uuid(u), buffer(buf), header()
    { }

    TSendData(Method m, const QByteArray &u, const THttpRequestHeader &h)
        : method(m), uuid(u), buffer(0), header(h)
    { }
};



TEpoll::TEpoll()
    : epollFd(0), events(new struct epoll_event[MaxEvents]),
      polling(false), numEvents(0), eventIterator(0), pollingSockets()
{
    epollFd = epoll_create(1);
    if (epollFd < 0) {
        tSystemError("Failed epoll_create()");
    }
}


TEpoll::~TEpoll()
{
    delete events;

    if (epollFd > 0)
        TF_CLOSE(epollFd);
}


int TEpoll::wait(int timeout)
{
    eventIterator = 0;
    polling = true;
    numEvents = tf_epoll_wait(epollFd, events, MaxEvents, timeout);
    int err = errno;
    polling = false;

    if (Q_UNLIKELY(numEvents < 0)) {
        tSystemError("Failed epoll_wait() : errno:%d", err);
    }

    return numEvents;
}


TEpollSocket *TEpoll::next()
{
    return (eventIterator < numEvents) ? (TEpollSocket *)events[eventIterator++].data.ptr : 0;
}

bool TEpoll::canReceive() const
{
    if (Q_UNLIKELY(eventIterator <= 0))
        return false;

    return (events[eventIterator - 1].events & EPOLLIN);
}


bool TEpoll::canSend() const
{
    if (Q_UNLIKELY(eventIterator <= 0))
        return false;

    return (events[eventIterator - 1].events & EPOLLOUT);
}


int TEpoll::recv(TEpollSocket *socket) const
{
    return socket->recv();
}


int TEpoll::send(TEpollSocket *socket) const
{
    return socket->send();
}


TEpoll *TEpoll::instance()
{
    if (Q_UNLIKELY(!staticInstance)) {
        staticInstance = new TEpoll();
    }
    return staticInstance;
}


bool TEpoll::addPoll(TEpollSocket *socket, int events)
{
    if (Q_UNLIKELY(!events))
        return false;

    struct epoll_event ev;
    ev.events  = events;
    ev.data.ptr = socket;

    int ret = tf_epoll_ctl(epollFd, EPOLL_CTL_ADD, socket->socketDescriptor(), &ev);
    int err = errno;
    if (Q_UNLIKELY(ret < 0)){
        if (err != EEXIST) {
            tSystemError("Failed epoll_ctl (EPOLL_CTL_ADD)  sd:%d errno:%d", socket->socketDescriptor(), err);
        }
    } else {
        tSystemDebug("OK epoll_ctl (EPOLL_CTL_ADD) (events:%u)  sd:%d", events, socket->socketDescriptor());
        pollingSockets.insert(socket->socketUuid(), socket);
    }
    return !ret;

}


bool TEpoll::modifyPoll(TEpollSocket *socket, int events)
{
   if (!events)
        return false;

    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = socket;

    int ret = tf_epoll_ctl(epollFd, EPOLL_CTL_MOD, socket->socketDescriptor(), &ev);
    int err = errno;
    if (Q_UNLIKELY(ret < 0)) {
        tSystemError("Failed epoll_ctl (EPOLL_CTL_MOD)  sd:%d errno:%d ev:0x%x", socket->socketDescriptor(), err, events);
    } else {
        tSystemDebug("OK epoll_ctl (EPOLL_CTL_MOD)  sd:%d", socket->socketDescriptor());
    }
    return !ret;

}


bool TEpoll::deletePoll(TEpollSocket *socket)
{
    if (pollingSockets.remove(socket->socketUuid()) == 0) {
        return false;
    }

    int ret = tf_epoll_ctl(epollFd, EPOLL_CTL_DEL, socket->socketDescriptor(), NULL);
    int err = errno;

    if (Q_UNLIKELY(ret < 0 && err != ENOENT)) {
        tSystemError("Failed epoll_ctl (EPOLL_CTL_DEL)  sd:%d errno:%d", socket->socketDescriptor(), err);
    } else {
        tSystemDebug("OK epoll_ctl (EPOLL_CTL_DEL)  sd:%d", socket->socketDescriptor());
    }

    return !ret;
}


bool TEpoll::waitSendData(int msec)
{
    return sendRequests.wait(msec);
}


void TEpoll::dispatchSendData()
{
    QList<TSendData *> dataList = sendRequests.dequeue();

    for (QListIterator<TSendData *> it(dataList); it.hasNext(); ) {
        TSendData *sd = it.next();
        TEpollSocket *sock = pollingSockets[sd->uuid];

        if (Q_LIKELY(sock && sock->socketDescriptor() > 0)) {
            switch (sd->method) {
            case TSendData::Send:
                sock->enqueueSendData(sd->buffer);
                modifyPoll(sock, (EPOLLIN | EPOLLOUT | EPOLLET));  // reset
                break;

            case TSendData::Disconnect:
                deletePoll(sock);
                sock->close();
                sock->deleteLater();
                break;

            case TSendData::SwitchToWebSocket: {
                tSystemDebug("Switch to WebSocket");
                Q_ASSERT(sd->buffer == NULL);

                QByteArray secKey = sd->header.rawHeader("Sec-WebSocket-Key");
                tSystemDebug("secKey: %s", secKey.data());
                TEpollWebSocket *ws = new TEpollWebSocket(sock->socketDescriptor(), sock->clientAddress(), sd->header);

                deletePoll(sock);
                sock->setSocketDescpriter(0);  // Delegates to new websocket
                sock->deleteLater();

                // Switch to WebSocket
                THttpResponseHeader response = ws->handshakeResponse();
                ws->enqueueSendData(TEpollSocket::createSendBuffer(response.toByteArray()));
                addPoll(ws, (EPOLLIN | EPOLLOUT | EPOLLET));  // reset

                // WebSocket opening
                TSession session;
                QByteArray sessionId = sd->header.cookie(TSession::sessionName());
                if (!sessionId.isEmpty()) {
                    // Finds a session
                    session = TSessionManager::instance().findSession(sessionId);
                }
                ws->startWorkerForOpening(session);
                break; }

            default:
                tSystemError("Logic error [%s:%d]", __FILE__, __LINE__);
                if (sd->buffer) {
                    delete sd->buffer;
                }
                break;
            }
        }

        delete sd;
    }
}


void TEpoll::releaseAllPollingSockets()
{
    for (QMapIterator<QByteArray, TEpollSocket *> it(pollingSockets); it.hasNext(); ) {
        it.next();
        it.value()->deleteLater();
    }
    pollingSockets.clear();
}


void TEpoll::setSendData(const QByteArray &uuid, const QByteArray &header, QIODevice *body, bool autoRemove, const TAccessLogger &accessLogger)
{
    QByteArray response = header;
    QFileInfo fi;

    if (Q_LIKELY(body)) {
        QBuffer *buffer = qobject_cast<QBuffer *>(body);
        if (buffer) {
            response += buffer->data();
        } else {
            fi.setFile(*qobject_cast<QFile *>(body));
        }
    }

    TSendBuffer *sendbuf = TEpollSocket::createSendBuffer(response, fi, autoRemove, accessLogger);
    sendRequests.enqueue(new TSendData(TSendData::Send, uuid, sendbuf));
}


void TEpoll::setSendData(const QByteArray &uuid, const QByteArray &data)
{
    TSendBuffer *sendbuf = TEpollSocket::createSendBuffer(data);
    sendRequests.enqueue(new TSendData(TSendData::Send, uuid, sendbuf));
}


void TEpoll::setDisconnect(const QByteArray &uuid)
{
    sendRequests.enqueue(new TSendData(TSendData::Disconnect, uuid));
}


void TEpoll::setSwitchToWebSocket(const QByteArray &uuid, const THttpRequestHeader &header)
{
    sendRequests.enqueue(new TSendData(TSendData::SwitchToWebSocket, uuid, header));
}
