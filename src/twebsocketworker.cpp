/* Copyright (c) 2015, AOYAMA Kazuharu
 * All rights reserved.
 *
 * This software may be used and distributed according to the terms of
 * the New BSD License, which is incorporated herein by reference.
 */

#include <TWebApplication>
#include <TDispatcher>
#include <TWebSocketEndpoint>
#include "twebsocketworker.h"
#include "tepoll.h"
#include "tsystemglobal.h"
#include "turlroute.h"


TWebSocketWorker::TWebSocketWorker(const QByteArray &socket, const TSession &session, QObject *parent)
    : QThread(parent), socketUuid(socket), sessionStore(session), requestPath(),
      opcode(TWebSocketFrame::Continuation), requestData()
{
    tSystemDebug("TWebSocketWorker::TWebSocketWorker");
}


TWebSocketWorker::TWebSocketWorker(const QByteArray &socket, const QByteArray &path, TWebSocketFrame::OpCode opCode, const QByteArray &data, QObject *parent)
    : QThread(parent), socketUuid(socket), sessionStore(), requestPath(path), opcode(opCode), requestData(data)
{
    tSystemDebug("TWebSocketWorker::TWebSocketWorker");
}


TWebSocketWorker::~TWebSocketWorker()
{
    tSystemDebug("TWebSocketWorker::~TWebSocketWorker");
}


void TWebSocketWorker::run()
{
    QString es = TUrlRoute::splitPath(requestPath).value(0).toLower() + "endpoint";
    TDispatcher<TWebSocketEndpoint> dispatcher(es);
    TWebSocketEndpoint *endpoint = dispatcher.object();

    if (endpoint) {
        tSystemDebug("Found endpoint: %s", qPrintable(es));
        tSystemDebug("TWebSocketWorker opcode: %d", opcode);

        switch (opcode) {
        case TWebSocketFrame::Continuation:
            if (sessionStore.id().isEmpty()) {
                endpoint->onOpen(sessionStore);
            } else {
                tError("Invalid logic  [%s:%d]",  __FILE__, __LINE__);
            }
            break;

        case TWebSocketFrame::TextFrame:
            endpoint->onTextReceived(QString::fromUtf8(requestData));
            break;

        case TWebSocketFrame::BinaryFrame:
            endpoint->onBinaryReceived(requestData);
            break;

        case TWebSocketFrame::Close:
            endpoint->onClose();
            endpoint->closeWebSocket();
            break;

        case TWebSocketFrame::Ping:
            endpoint->onPing();
            endpoint->sendPong();
            break;

        case TWebSocketFrame::Pong:
            endpoint->onPong();
            break;

        default:
            tWarn("Invalid opcode: 0x%x  [%s:%d]", (int)opcode, __FILE__, __LINE__);
            break;
        }

        // Sends payload
        for (QListIterator<QVariant> it(endpoint->payloadList); it.hasNext(); ) {
            const QVariant &var = it.next();
            switch (var.type()) {
            case QVariant::String:
                TEpollWebSocket::sendText(socketUuid, var.toString());
                break;

            case QVariant::ByteArray:
                TEpollWebSocket::sendBinary(socketUuid, var.toByteArray());
                break;

            case QVariant::Int: {

                int opcode = var.toInt();
                switch (opcode) {
                case TWebSocketFrame::Close:
                    TEpollWebSocket::disconnect(socketUuid);
                    break;

                case TWebSocketFrame::Ping:
                    TEpollWebSocket::sendPing(socketUuid);
                    break;

                case TWebSocketFrame::Pong:
                    TEpollWebSocket::sendPong(socketUuid);
                    break;

                default:
                    tError("Invalid logic  [%s:%d]",  __FILE__, __LINE__);
                    break;
                }

                break; }

            default:
                tError("Invalid logic  [%s:%d]",  __FILE__, __LINE__);
                break;
            }
        }
    }
}
