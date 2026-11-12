//========= TF2MC bridge - engine independent core =========//
// Threaded TCP client. A reader thread parses frames into an inbox; a writer
// thread drains the outbox. The game thread only touches Poll()/Send().
//==========================================================//
#pragma once

#include "mc_protocol.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mc
{
	class Connection
	{
	public:
		enum State { DISCONNECTED, CONNECTING, CONNECTED, FAILED };

		Connection();
		~Connection();

		// Starts an asynchronous connect. Returns immediately.
		void Connect( const std::string &host, int port );
		void Disconnect();

		State GetState() const { return m_state.load(); }
		bool IsConnected() const { return m_state.load() == CONNECTED; }
		std::string GetError();

		// Thread safe. Frames sent before the socket connects are queued.
		void Send( std::vector<uint8_t> frame );
		void Send( Writer &w ) { Send( std::move( w.finish() ) ); }

		// Moves all received packets into out (appends). Returns count.
		size_t Poll( std::vector<Packet> &out, size_t maxPackets = 100000 );

		uint64_t BytesIn() const { return m_bytesIn.load(); }
		uint64_t BytesOut() const { return m_bytesOut.load(); }

	private:
		void ThreadMain( std::string host, int port );
		void WriterMain();
		void SetError( const std::string &e );
		void CloseSocket();

		std::atomic<State> m_state;
		std::atomic<bool> m_stop;
		std::atomic<uint64_t> m_bytesIn, m_bytesOut;
		intptr_t m_socket;

		std::thread m_reader;
		std::thread m_writer;

		std::mutex m_inMutex;
		std::deque<Packet> m_inbox;

		std::mutex m_outMutex;
		std::condition_variable m_outCv;
		std::deque<std::vector<uint8_t>> m_outbox;

		std::mutex m_errMutex;
		std::string m_error;
	};
}
