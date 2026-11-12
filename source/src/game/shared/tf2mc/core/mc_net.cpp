//========= TF2MC bridge - engine independent core =========//
#include "mc_net.h"

#ifdef _WIN32
	#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
	#endif
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#pragma comment( lib, "ws2_32.lib" )
	typedef int socklen_t;
	typedef SOCKET native_sock_t;
	#define MC_INVALID_SOCKET ((intptr_t)INVALID_SOCKET)
	static void mc_closesocket( intptr_t s ) { closesocket( (SOCKET)s ); }
#else
	#include <arpa/inet.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <netinet/tcp.h>
	#include <sys/socket.h>
	#include <unistd.h>
	#define MC_INVALID_SOCKET ((intptr_t)-1)
	typedef int native_sock_t;
	static void mc_closesocket( intptr_t s ) { ::close( (int)s ); }
#endif

namespace mc
{
	namespace
	{
		struct NetInit
		{
			NetInit()
			{
#ifdef _WIN32
				WSADATA d; WSAStartup( MAKEWORD( 2, 2 ), &d );
#endif
			}
		};

		bool RecvAll( intptr_t s, uint8_t *buf, size_t n, std::atomic<bool> &stop )
		{
			size_t got = 0;
			while ( got < n )
			{
				if ( stop.load() ) return false;
				int r = ::recv( (native_sock_t)s, (char *)buf + got, (int)( n - got ), 0 );
				if ( r <= 0 ) return false;
				got += (size_t)r;
			}
			return true;
		}

		bool SendAll( intptr_t s, const uint8_t *buf, size_t n )
		{
			size_t sent = 0;
			while ( sent < n )
			{
#ifdef MSG_NOSIGNAL
				int r = ::send( (native_sock_t)s, (const char *)buf + sent, (int)( n - sent ), MSG_NOSIGNAL );
#else
				int r = ::send( (native_sock_t)s, (const char *)buf + sent, (int)( n - sent ), 0 );
#endif
				if ( r <= 0 ) return false;
				sent += (size_t)r;
			}
			return true;
		}
	}

	Connection::Connection()
		: m_state( DISCONNECTED ), m_stop( false ), m_bytesIn( 0 ), m_bytesOut( 0 ), m_socket( MC_INVALID_SOCKET )
	{
		static NetInit s_init;
	}

	Connection::~Connection()
	{
		Disconnect();
	}

	void Connection::Connect( const std::string &host, int port )
	{
		Disconnect();
		m_stop = false;
		m_state = CONNECTING;
		{
			std::lock_guard<std::mutex> l( m_errMutex );
			m_error.clear();
		}
		m_reader = std::thread( &Connection::ThreadMain, this, host, port );
	}

	void Connection::CloseSocket()
	{
		intptr_t s = m_socket;
		m_socket = MC_INVALID_SOCKET;
		if ( s != MC_INVALID_SOCKET )
		{
#ifdef _WIN32
			shutdown( (SOCKET)s, SD_BOTH );
#else
			shutdown( (int)s, SHUT_RDWR );
#endif
			mc_closesocket( s );
		}
	}

	void Connection::Disconnect()
	{
		m_stop = true;
		m_outCv.notify_all();
		CloseSocket();
		if ( m_reader.joinable() ) m_reader.join();
		if ( m_writer.joinable() ) m_writer.join();
		if ( m_state != FAILED ) m_state = DISCONNECTED;
		std::lock_guard<std::mutex> l( m_outMutex );
		m_outbox.clear();
	}

	std::string Connection::GetError()
	{
		std::lock_guard<std::mutex> l( m_errMutex );
		return m_error;
	}

	void Connection::SetError( const std::string &e )
	{
		std::lock_guard<std::mutex> l( m_errMutex );
		m_error = e;
	}

	void Connection::Send( std::vector<uint8_t> frame )
	{
		{
			std::lock_guard<std::mutex> l( m_outMutex );
			m_outbox.push_back( std::move( frame ) );
		}
		m_outCv.notify_one();
	}

	size_t Connection::Poll( std::vector<Packet> &out, size_t maxPackets )
	{
		std::lock_guard<std::mutex> l( m_inMutex );
		size_t n = 0;
		while ( !m_inbox.empty() && n < maxPackets )
		{
			out.push_back( std::move( m_inbox.front() ) );
			m_inbox.pop_front();
			++n;
		}
		return n;
	}

	void Connection::ThreadMain( std::string host, int port )
	{
		addrinfo hints;
		memset( &hints, 0, sizeof( hints ) );
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		addrinfo *res = nullptr;
		char portStr[16];
		snprintf( portStr, sizeof( portStr ), "%d", port );
		if ( getaddrinfo( host.c_str(), portStr, &hints, &res ) != 0 || !res )
		{
			SetError( "could not resolve host " + host );
			m_state = FAILED;
			return;
		}

		intptr_t s = MC_INVALID_SOCKET;
		for ( addrinfo *a = res; a; a = a->ai_next )
		{
			intptr_t c = (intptr_t)::socket( a->ai_family, a->ai_socktype, a->ai_protocol );
			if ( c == MC_INVALID_SOCKET ) continue;
			if ( ::connect( (native_sock_t)c, a->ai_addr, (socklen_t)a->ai_addrlen ) == 0 ) { s = c; break; }
			mc_closesocket( c );
		}
		freeaddrinfo( res );

		if ( s == MC_INVALID_SOCKET )
		{
			SetError( "connection refused by " + host + ":" + portStr );
			m_state = FAILED;
			return;
		}

		int one = 1;
		setsockopt( (native_sock_t)s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof( one ) );
		int big = 4 * 1024 * 1024;
		setsockopt( (native_sock_t)s, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof( big ) );

		m_socket = s;
		if ( m_stop ) { CloseSocket(); return; }
		m_state = CONNECTED;
		m_writer = std::thread( &Connection::WriterMain, this );

		std::vector<uint8_t> buf;
		while ( !m_stop )
		{
			uint8_t hdr[4];
			if ( !RecvAll( s, hdr, 4, m_stop ) ) break;
			uint32_t len;
			memcpy( &len, hdr, 4 );
			if ( len == 0 || len > MAX_FRAME ) { SetError( "bad frame length" ); break; }
			buf.resize( len );
			if ( !RecvAll( s, buf.data(), len, m_stop ) ) break;
			m_bytesIn += len + 4;

			Packet p;
			p.type = buf[0];
			p.data.assign( buf.begin() + 1, buf.end() );
			std::lock_guard<std::mutex> l( m_inMutex );
			m_inbox.push_back( std::move( p ) );
		}

		if ( !m_stop )
		{
			if ( GetError().empty() ) SetError( "connection closed by Minecraft server" );
			m_state = FAILED;
		}
		m_stop = true;
		m_outCv.notify_all();
	}

	void Connection::WriterMain()
	{
		while ( true )
		{
			std::vector<uint8_t> frame;
			{
				std::unique_lock<std::mutex> l( m_outMutex );
				m_outCv.wait( l, [this] { return m_stop.load() || !m_outbox.empty(); } );
				if ( m_stop.load() ) return;
				frame = std::move( m_outbox.front() );
				m_outbox.pop_front();
			}
			intptr_t s = m_socket;
			if ( s == MC_INVALID_SOCKET || !SendAll( s, frame.data(), frame.size() ) )
				return;
			m_bytesOut += frame.size();
		}
	}
}
