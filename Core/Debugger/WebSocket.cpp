// Copyright (c) 2017- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <mutex>
#include <condition_variable>
#include "thread/threadutil.h"
#include "Core/Debugger/WebSocket.h"
#include "Core/Debugger/WebSocket/WebSocketUtils.h"

// This WebSocket (connected through the same port as disc sharing) allows API/debugger access to PPSSPP.
// Currently, the only subprotocol "debugger.ppsspp.org" uses a simple JSON based interface.
//
// Messages to and from PPSSPP follow the same basic format:
//    { "event": "NAME", ... }
//
// And are primarily of these types:
//  * Events from the debugger/client (you) to PPSSPP
//    If there's a response, it will generally use the same name.  It may not be immedate - it's an event.
//  * Spontaneous events from PPSSPP
//    Things like logs, breakpoint hits, etc. not directly requested.
//
// Otherwise you may see error events which indicate PPSSPP couldn't understand or failed internally:
//  - "event": "error"
//  - "message": A string describing what happened.
//  - "level": Integer severity level. (1 = NOTICE, 2 = ERROR, 3 = WARN, 4 = INFO, 5 = DEBUG, 6 = VERBOSE)
//  - "ticket": Optional, present if in response to an event with a "ticket" field, simply repeats that value.
//
// At start, please send a "version" event.  See WebSocket/GameSubscriber.cpp for more details.
//
// For other events, look inside Core/Debugger/WebSocket/ for details on each event.

#include "Core/Debugger/WebSocket/GameBroadcaster.h"
#include "Core/Debugger/WebSocket/LogBroadcaster.h"
#include "Core/Debugger/WebSocket/SteppingBroadcaster.h"

#include "Core/Debugger/WebSocket/CPUCoreSubscriber.h"
#include "Core/Debugger/WebSocket/DisasmSubscriber.h"
#include "Core/Debugger/WebSocket/GameSubscriber.h"
#include "Core/Debugger/WebSocket/SteppingSubscriber.h"

typedef void *(*SubscriberInit)(DebuggerEventHandlerMap &map);
typedef void (*Subscribershutdown)(void *p);
struct SubscriberInfo {
	SubscriberInit init;
	Subscribershutdown shutdown;
};

static const std::vector<SubscriberInfo> subscribers({
	{ &WebSocketCPUCoreInit, nullptr },
	{ &WebSocketDisasmInit, &WebSocketDisasmShutdown },
	{ &WebSocketGameInit, nullptr },
	{ &WebSocketSteppingInit, &WebSocketSteppingShutdown },
});

// To handle webserver restart, keep track of how many running.
static volatile int debuggersConnected = 0;
static volatile bool stopRequested = false;
static std::mutex stopLock;
static std::condition_variable stopCond;

// Prevent threading surprises and obscure crashes by locking startup/shutdown.
static bool lifecycleLockSetup = false;
static std::mutex lifecycleLock;

static void UpdateConnected(int delta) {
	std::lock_guard<std::mutex> guard(stopLock);
	debuggersConnected += delta;
	stopCond.notify_all();
}

static void WebSocketNotifyLifecycle(CoreLifecycle stage) {
	switch (stage) {
	case CoreLifecycle::STARTING:
	case CoreLifecycle::STOPPING:
		if (debuggersConnected > 0) {
			DEBUG_LOG(SYSTEM, "Waiting for debugger to complete on shutdown");
		}
		lifecycleLock.lock();
		break;

	case CoreLifecycle::START_COMPLETE:
	case CoreLifecycle::STOPPED:
		lifecycleLock.unlock();
		if (debuggersConnected > 0) {
			DEBUG_LOG(SYSTEM, "Debugger ready for shutdown");
		}
		break;
	}
}

static void SetupDebuggerLock() {
	if (!lifecycleLockSetup) {
		Core_ListenLifecycle(&WebSocketNotifyLifecycle);
		lifecycleLockSetup = true;
	}
}

void HandleDebuggerRequest(const http::Request &request) {
	net::WebSocketServer *ws = net::WebSocketServer::CreateAsUpgrade(request, "debugger.ppsspp.org");
	if (!ws)
		return;

	setCurrentThreadName("Debugger");
	UpdateConnected(1);
	SetupDebuggerLock();

	LogBroadcaster logger;
	GameBroadcaster game;
	SteppingBroadcaster stepping;

	std::unordered_map<std::string, DebuggerEventHandler> eventHandlers;
	std::vector<void *> subscriberData;
	for (auto info : subscribers) {
		std::lock_guard<std::mutex> guard(lifecycleLock);
		subscriberData.push_back(info.init(eventHandlers));
	}

	// There's a tradeoff between responsiveness to incoming events, and polling for changes.
	int highActivity = 0;
	ws->SetTextHandler([&](const std::string &t) {
		JsonReader reader(t.c_str(), t.size());
		if (!reader.ok()) {
			ws->Send(DebuggerErrorEvent("Bad message: invalid JSON", LogTypes::LERROR));
			return;
		}

		const JsonGet root = reader.root();
		const char *event = root ? root.getString("event", nullptr) : nullptr;
		if (!event) {
			ws->Send(DebuggerErrorEvent("Bad message: no event property", LogTypes::LERROR, root));
			return;
		}

		DebuggerRequest req(event, ws, root);
		auto eventFunc = eventHandlers.find(event);
		if (eventFunc != eventHandlers.end()) {
			std::lock_guard<std::mutex> guard(lifecycleLock);
			eventFunc->second(req);
			if (!req.Finish()) {
				// Poll more frequently for a second in case this triggers something.
				highActivity = 1000;
			}
		} else {
			req.Fail("Bad message: unknown event");
		}
	});
	ws->SetBinaryHandler([&](const std::vector<uint8_t> &d) {
		ws->Send(DebuggerErrorEvent("Bad message", LogTypes::LERROR));
	});

	while (ws->Process(highActivity ? 1.0f / 1000.0f : 1.0f / 60.0f)) {
		std::lock_guard<std::mutex> guard(lifecycleLock);
		// These send events that aren't just responses to requests.
		logger.Broadcast(ws);
		game.Broadcast(ws);
		stepping.Broadcast(ws);

		if (stopRequested) {
			ws->Close(net::WebSocketClose::GOING_AWAY);
		}
		if (highActivity > 0) {
			highActivity--;
		}
	}

	std::lock_guard<std::mutex> guard(lifecycleLock);
	for (size_t i = 0; i < subscribers.size(); ++i) {
		if (subscribers[i].shutdown) {
			subscribers[i].shutdown(subscriberData[i]);
		} else {
			assert(!subscriberData[i]);
		}
	}

	delete ws;
	UpdateConnected(-1);
}

void StopAllDebuggers() {
	std::unique_lock<std::mutex> guard(stopLock);
	while (debuggersConnected != 0) {
		stopRequested = true;
		stopCond.wait(guard);
	}

	// Reset it back for next time.
	stopRequested = false;
}
