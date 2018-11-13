/*
$Id: NetworkIo.h 26 2010-08-26 19:01:37Z avyatkin $

Declaration of global functions and classes from the networkio namespace, that works with the TCP/IP network

Copyright (c) 2010 VyPRESS Research, LLC. All rights reserved.
For conditions of distribution and use, see copyright notice in ChatTerminal.h
*/

#pragma once

#ifdef CHATTERM_OS_WINDOWS
	//#include <winsock.h>
	//#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <Wspiapi.h> /* for getaddrinfo() in Windows 2000 */
#else
	#include <sys/types.h>
	#include <sys/socket.h>
        #include <netinet/in.h>
	#include <netdb.h>

	#define SOCKET int
	#define INVALID_SOCKET -1
	#define closesocket close
#endif // CHATTERM_OS_WINDOWS

namespace networkio
{
	// Forward class declarations
	class Interface;
	class Sender;
	class Receiver;
	class NETADDR_INFO;
	struct DESTADDR_INFO;

	/**
	Creates string representation of an IP address
	@pcaddr - const pointer to the address structure
	@addrlen - size of the address structure
	@return - a buffer that contains the string
	*/
	std::wstring sockaddr_to_string(const sockaddr* pcaddr, size_t addrlen);

	/**
	Searches network interface by adapter name or friendly name and address.
	@aname - adapter name or friendly name
	@desired_addr - string representation of the IP address
	@ai_family - desired family of IP addresses
	@piref - reference to a pointer that receives the interface object which meets specified criteria;
	         The object must be freed using delete operator
	@return - true is successful, false otherwise
	*/
	std::unique_ptr<Interface> get_interface_by_adapter_name(const wchar_t* aname, const wchar_t* desired_addr, int ai_family);

	/**
	Suggests default network configuration for communications.
	@psref - reference to a pointer that receives the sender object;
	@prref - reference to a pointer that receives the receiver object;
	@pdref - reference to a pointer that receives the destination address object;
	@return - true is successful, false otherwise
	*/
	bool get_default_netconfig(std::shared_ptr<Sender>& psref, std::shared_ptr<Receiver>& prref, std::unique_ptr<DESTADDR_INFO>& pdref);

	/**
	Class describes an object which represents a network interface
	*/
	class Interface
	{
	public:
		Interface(const Interface&) = delete;
		Interface& operator = (const Interface&) = delete;

		Interface(const wchar_t* wszAddress, int ai_family);
		Interface(const addrinfo* pai, unsigned int if6index);
#ifndef CHATTERM_OS_WINDOWS
		Interface(const sockaddr* pcaddr, unsigned int if6index);
#endif // CHATTERM_OS_WINDOWS

		~Interface(void)
		{
			if(pai_ && (pai_ != reinterpret_cast<addrinfo*>(paibufPtr_.get())) ) freeaddrinfo(pai_);

			//if(paibufPtr_)
			//{
			///	delete[] reinterpret_cast<unsigned char*>(paibufPtr_->ai_addr);
			//	//delete paibuf_;
			//}
		}

		std::wstring getStringAddress(unsigned short port);
		std::wstring getStringAddress();

	private:
		addrinfo* pai_;
		std::unique_ptr<unsigned char[]> paibufPtr_;
		unsigned int ipv6_if_index_;

	friend class Receiver;
	friend class Sender;

#ifdef GTEST_PROJECT
	friend class NetworkTest_AdapterNames_Test;
#endif
	};

	/**
	Class describes an object which is used to send UDP datagrams
	*/
	class Sender
	{
	public:
		Sender(void) : sock_(INVALID_SOCKET), port_(0), pif_(0) {}
		~Sender(void)
		{
			if(INVALID_SOCKET !=sock_ ) closesocket(sock_);
		}

		/**
		Binds socket to specified network interface, port; Sets necessary socket options
		*/
		int bindToInterface(const std::shared_ptr<Interface>& refPtrIf, unsigned short port, DWORD dwTTL);

		int sendTo(const sockaddr* paddr, const char* buf, int len) const;

		int getIfFamily() const
		{
			return pif_->pai_->ai_family;
		}

		static bool debug_ ;//display packets as a hexstring (debug)

	private:
		SOCKET sock_;
		unsigned short port_;
		std::shared_ptr<Interface> pif_;

	friend class Receiver;
	};

	/**
	Class describes an object which is used to receive UDP datagrams
	*/
	class Receiver
	{
		/**
		Class describes an object that represents a piece of a UDP datagram for checking purposes
		*/
		struct PKT_DATA
		{
			// Size of a datagram Id
			static const int sig_len = 9;

			// Piece of the datagram: Id and IP address
			unsigned char data[sig_len + sizeof(sockaddr_in6)];

			// Time when the datagram was received
			time_t ct;

			PKT_DATA() : ct(time(NULL))
			{
				memset(data, 0, sizeof(data));
			}

			PKT_DATA(const unsigned char* pSig, const sockaddr* pAddr) : ct(time(NULL))
			{
				memcpy(data, pSig, sig_len);
				memcpy(data+sig_len, pAddr, sizeof(sockaddr_in6));
			}

			bool operator== (const PKT_DATA& p) const
			{
				return 0==memcmp(&p.data, data, sizeof(data));
			}

			bool is_from(const PKT_DATA& p) const
			{
				return 0==memcmp(p.data+sig_len, data+sig_len, sizeof(sockaddr_in6));
			}
		};

	public:
		explicit Receiver(const Sender* pcSender)
			: sock_(INVALID_SOCKET)
			,port_(0)
			,pcSender_(pcSender)
			,fStop_(false)
#ifdef CHATTERM_OS_WINDOWS
			,eventRead_(WSACreateEvent())
			,thread_(INVALID_HANDLE_VALUE)
#else
			,thread_(0)
#endif
		{
			memset(&from_addr_, 0x00, sizeof(from_addr_));
		}

		~Receiver(void)
		{
			stop();
#ifdef CHATTERM_OS_WINDOWS
			if(WSA_INVALID_EVENT!=eventRead_)
				WSACloseEvent(eventRead_);
#endif
			if(INVALID_SOCKET != sock_ && (!pcSender_ || sock_ != pcSender_->sock_))
			{
				closesocket(sock_);
				sock_ = INVALID_SOCKET;
			}
		}

		/**
		Binds socket to specified network interface, port; Sets necessary socket options; Joins to the multicast group(s)
		*/
		int bindToInterface(std::shared_ptr<Interface>& ptrIf, unsigned short port, const wchar_t* wszMcastGroups);

		/**
		Starts the receiver thread.
		@return - 0 is successful, error code otherwise
		*/
		int start();

		/**
		Stops the receiver thread.
		@return - 0 is successful, error code otherwise
		*/
		int stop();

		/**
		Accessor to psender_ member
		*/
		const Sender* getSender() const
		{
			return pcSender_;
		}

		/**
		Checks whether last packet was received from one of the application's senders
		*/
		bool isEchoedMessage() const;

		// Maximum number of packets per nPacketsQueTimeInterval_(default per 1 min) if 0 check for flooding is disabled
		static unsigned int nFloodRate_;

		// Flooding protection user blocking time
		static int nFloodProtectionTimeInterval_;

		// Debugging flag; If true then displays packets as a hexstring
		static bool debug_ ;

		// Flag that is become true whet the list of local transport addresses was changed
		static bool fAddressListChange_;

	private:
#ifdef CHATTERM_OS_WINDOWS
		int recvFromWSA(char* buf, int len);
#else
		int recvFrom(char* buf, int len);
#endif // CHATTERM_OS_WINDOWS

		void processPacket(char* pPacket, int len);
		void removeOldPackets(const PKT_DATA& pkt);
		int checkDuplAndFlooding(const PKT_DATA& pkt);

		bool isFromSender(Sender* const& ps) const;

		SOCKET sock_;
		unsigned short port_;

		//Pointer to the interface object where the socket bind
		std::shared_ptr<Interface> ptrIf_;

		// Temporary buffer for storing an address from the latest datagram was received
		// It is also used in bindToInterface() to obtain a really bound address and port
		sockaddr_in6 from_addr_;

		//Pointer to the sender object which is used to send reply messages to recipients
		const Sender* pcSender_;

		// Flag to stop the receiver thread
		bool fStop_;
#ifdef CHATTERM_OS_WINDOWS
		HANDLE thread_;
		WSAEVENT eventRead_;
		static unsigned int __stdcall recvProc(void* pParam);
#else
		pthread_t thread_;
		static void* recvProc(void* pParam);
#endif // CHATTERM_OS_WINDOWS

		// Queue of packets to prevent processing duplicated packets and flooding
		std::deque<PKT_DATA> queOfPackets_;

		// Time interval of queue of packets (maximum time difference between the newest and the oldest packets), 1 min
		static const double nPacketsQueTimeInterval_;

		friend class NETADDR_INFO;
	};

	/**
	Class describes a network address of a recipient
	*/
	class NETADDR_INFO
	{
	public:
		NETADDR_INFO() : preceiver_(0)
		{
			memset(&saddr_, 0x00, sizeof(saddr_));
		}

		//network address structure represents network address of the recipient;
		sockaddr_in6 saddr_;

		// Pointer to a receiver object which UDP datagrams from the recipient received
		const Receiver* preceiver_;

		// Return a pointer to network address structure represents network address of the recipient;
		// Network address can be a link local or site local address
		const sockaddr* get_saddr() const
		{
			return reinterpret_cast<const sockaddr*>(&saddr_);
		};

		void assign_from_receiver(const Receiver* pcrcvr)
		{
			preceiver_ = pcrcvr;
			memcpy(&saddr_, &pcrcvr->from_addr_, sizeof(sockaddr_in6));
			set_port(reinterpret_cast<sockaddr*>(&saddr_), pcrcvr->port_);
		}

		static bool compare_with_receiver(NETADDR_INFO& naddr_info, const Receiver* prcvr)
		{
			if(naddr_info.preceiver_ != prcvr) return false;

			//return compare_sockaddr(&naddr_info.saddr_,& prcvr->from_addr_);
			switch(naddr_info.get_saddr()->sa_family)
			{
			case AF_INET:
				{
					const sockaddr_in* p1 = reinterpret_cast<const sockaddr_in*>(naddr_info.get_saddr());
					const sockaddr_in* p2 = reinterpret_cast<const sockaddr_in*>(&prcvr->from_addr_);

					return 0==memcmp(&p1->sin_addr, &p2->sin_addr, sizeof(p1->sin_addr));
				}
				break;

			case AF_INET6:
				{
					const sockaddr_in6* p1 = reinterpret_cast<const sockaddr_in6*>(naddr_info.get_saddr());
					const sockaddr_in6* p2 = reinterpret_cast<const sockaddr_in6*>(&prcvr->from_addr_);

					return 0==memcmp(&p1->sin6_addr, &p2->sin6_addr, sizeof(p1->sin6_addr));
				}
				break;
			}

			return 0==memcmp(naddr_info.get_saddr(), &prcvr->from_addr_, sizeof(sockaddr_in6));
		}

		static void set_port(sockaddr* paddr, unsigned short port)
		{
			switch(paddr->sa_family)
			{
			case AF_INET:
				{
					sockaddr_in* p = reinterpret_cast<sockaddr_in*>(paddr);
					p->sin_port = port;
				}
				break;

			case AF_INET6:
				{
					sockaddr_in6* p = reinterpret_cast<sockaddr_in6*>(paddr);
					p->sin6_port = port;
				}
				break;
			}
		}
	};

	/**
	Struct describes an object which is used for sending broadcast
	and multicast UDP datagrams by specified sender object
	*/
	struct DESTADDR_INFO
	{
		DESTADDR_INFO(const Sender* psender) : psender_(psender)
		{
			memset(&saddr_, 0x00, sizeof(sockaddr_in6));
		}

		sockaddr_in6 saddr_;

		int bindToAddress(const wchar_t* wszAddress, unsigned short port);

		// Pointer to a sender object for sending broadcast or multicast UDP datagrams
		const Sender* psender_;

		// returns a pointer to network address structure represents broadcast or multicast address
		// Network address can be a link local or site local address
		sockaddr* get_saddr()
		{
			return reinterpret_cast<sockaddr*>(&saddr_);
		}
	};
}
