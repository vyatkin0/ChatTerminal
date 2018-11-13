/*
$Id: NetworkIo.cpp 36 2011-08-09 07:35:21Z avyatkin $

Implementation of global functions and classes from the networkio namespace

Copyright (c) 2010 VyPRESS Research, LLC. All rights reserved.
For conditions of distribution and use, see copyright notice in ChatTerminal.h
*/

#include "ChatTerminal.h"
#include "NetworkIo.h"
#include "ProcessorMsgX.h"

#ifdef CHATTERM_OS_WINDOWS
#include <process.h>    /* _beginthread, _endthread */
#include <Iphlpapi.h>
#else

#include <pthread.h>
#include <errno.h>

#include <sys/utsname.h> //for uname
#include <sys/stat.h> //for lstat
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/route.h> //for NET_RT_IFLIST

#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#endif // CHATTERM_OS_WINDOWS

#include "StrResources.h"

#ifdef GTEST_PROJECT
extern bool fGTestGetAdaptersAddresses;
#endif

#define DEFAULT_PORT 8167
#define DEFAULT_TTL 1
#define DEFAULT_TTL_V6 2
#define DEFAULT_MCAST_ADDR L"227.0.0.2"
#define DEFAULT_MCAST_ADDR_V6 L"FFFE::2"

namespace networkio
{
	const double Receiver::nPacketsQueTimeInterval_ = 1*60;//1 min time interval of queue of packets
	unsigned int Receiver::nFloodRate_ = 15;//Maximum 15 packets per nPacketsQueTimeInterval_
	int Receiver::nFloodProtectionTimeInterval_ = 30;//30 sec is a flooding protection time
	bool Receiver::debug_ = false;
	bool Receiver::fAddressListChange_ = false;
	bool Sender::debug_ = false;

	//////////////////////////////////////////////////////////////////
	//Global functions
	/**
	Compares sockaddrs structs, if one of them has sin6_scope_id==0 then scope_id is ignored
	*/
	bool compare_sockaddrs(const sockaddr* pcsaddr1, size_t len1, const sockaddr* pcsaddr2, size_t len2)
	{
		if(pcsaddr1->sa_family != pcsaddr2->sa_family) return false;

		if(AF_INET6 == pcsaddr1->sa_family)
		{
			if(sizeof(sockaddr_in6) > len1) return false;
			if(sizeof(sockaddr_in6) > len2) return false;

			const sockaddr_in6* paddr1 = reinterpret_cast<const sockaddr_in6*>(pcsaddr1);
			const sockaddr_in6* paddr2 = reinterpret_cast<const sockaddr_in6*>(pcsaddr2);

			if(paddr1->sin6_port != paddr2->sin6_port) return false;
			if(paddr1->sin6_flowinfo != paddr2->sin6_flowinfo) return false;
			if(0!=memcmp(&paddr1->sin6_addr, &paddr2->sin6_addr, sizeof(in6_addr))) return false;

			if(paddr1->sin6_scope_id == paddr2->sin6_scope_id) return true;
			return (0 == paddr1->sin6_scope_id || 0 == paddr2->sin6_scope_id);
		}

		if(AF_INET == pcsaddr1->sa_family)
		{
			if(sizeof(sockaddr_in) > len1) return false;
			if(sizeof(sockaddr_in) > len2) return false;

			const sockaddr_in* paddr1 = reinterpret_cast<const sockaddr_in*>(pcsaddr1);
			const sockaddr_in* paddr2 = reinterpret_cast<const sockaddr_in*>(pcsaddr2);

			if(paddr1->sin_port != paddr2->sin_port) return false;

			return 0==memcmp(&paddr1->sin_addr, &paddr2->sin_addr, sizeof(in_addr));
		}

		if(len1 != len2) return false;
		return 0==memcmp(pcsaddr1, pcsaddr2, len1);
	}

	/*
	bool compare_sockaddr(const sockaddr* pa1, const sockaddr* pa2)
	{
		if(pa1->sa_family != pa->.sa_family) return false;

		switch(pa1->sa_family)
		{
		case AF_INET:
			{
				const sockaddr_in* p1 = reinterpret_cast<const sockaddr_in*>(&pa1->saddr_);
				const sockaddr_in* p2 = reinterpret_cast<const sockaddr_in*>(&pa2->from_addr_);

				return 0==memcmp(&p1->sin_addr, &p2->sin_addr, sizeof(p1->sin_addr));
			}
			break;

		case AF_INET6:
			{
				const sockaddr_in6* p1 = reinterpret_cast<const sockaddr_in6*>(&pa1->saddr_);
				const sockaddr_in6* p2 = reinterpret_cast<const sockaddr_in6*>(&pa2->from_addr_);

				return 0==memcmp(&p1->sin6_addr, &p2->sin6_addr, sizeof(p1->sin6_addr));
			}
			break;
		}

		return 0==memcmp(pa1, pa2, sizeof(*pa1));
	}
	*/

#ifdef CHATTERM_OS_WINDOWS
	std::wstring sockaddr_to_string(const sockaddr* pcaddr, size_t addrlen)
	{
		DWORD dwAddressStringLength = INET6_ADDRSTRLEN;
		std::unique_ptr<wchar_t[]> ptrAddress = std::make_unique<wchar_t[]>(dwAddressStringLength);
		memset(ptrAddress.get(), 0x00, dwAddressStringLength*sizeof(wchar_t));

		sockaddr* paddr = const_cast<sockaddr*>(pcaddr);

		//WSAAddressToString fails when lpszAddressString=NULL in Windows XP
		if(SOCKET_ERROR == WSAAddressToString(paddr, static_cast<DWORD>(addrlen), 0, ptrAddress.get(), &dwAddressStringLength))
		{
			int error = WSAGetLastError();
			if((WSAEFAULT == error) && dwAddressStringLength)
			{
				ptrAddress = std::make_unique<wchar_t[]>(dwAddressStringLength);
				memset(ptrAddress.get(), 0x00, dwAddressStringLength*sizeof(wchar_t));

				if(0 == WSAAddressToString(paddr, static_cast<DWORD>(addrlen), 0, ptrAddress.get(), &dwAddressStringLength))
					return std::wstring(ptrAddress.get());
			}
		}
		else
			return std::wstring(ptrAddress.get());

		return 0;
	}

	unsigned int get_interface_v6_index_by_addr(const sockaddr_in6* paddr)
	{
		if(paddr->sin6_scope_id && IN6_IS_ADDR_LINKLOCAL(&paddr->sin6_addr))
				return paddr->sin6_scope_id;

		//check whether GetAdaptersAddresses is presented
		typedef ULONG (WINAPI* GetAAProc)(ULONG,ULONG,PVOID,PIP_ADAPTER_ADDRESSES,PULONG);
		GetAAProc pGetAdaptersAddressesProc = 0;

		HINSTANCE hinstLib = LoadLibrary(L"Iphlpapi.dll");
		if(hinstLib)
			pGetAdaptersAddressesProc = (GetAAProc)GetProcAddress(hinstLib, "GetAdaptersAddresses");

		if(!pGetAdaptersAddressesProc)
		{
			FreeLibrary(hinstLib);
			return 0;
		}

		ULONG ulSize = 0;

		ULONG ulFlags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;

		if(ERROR_BUFFER_OVERFLOW == (pGetAdaptersAddressesProc)(AF_INET6, ulFlags, NULL, NULL, &ulSize))
		{
			std::unique_ptr<unsigned char[]> pAddressesBuf = std::make_unique<unsigned char[]>(ulSize);
			IP_ADAPTER_ADDRESSES* pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(pAddressesBuf.get());

			if(ERROR_SUCCESS == (pGetAdaptersAddressesProc)(AF_INET6, ulFlags, NULL, pAddresses, &ulSize))
			{
				while(pAddresses)
				{
					if(pAddresses->Flags&IP_ADAPTER_IPV6_ENABLED)
					{
						IP_ADAPTER_UNICAST_ADDRESS* pAddr = pAddresses->FirstUnicastAddress;

						while(pAddr)
						{
							if(compare_sockaddrs(pAddr->Address.lpSockaddr, pAddr->Address.iSockaddrLength, reinterpret_cast<const sockaddr*>(paddr), sizeof(*paddr)))
							{
								IF_INDEX index = pAddresses->Ipv6IfIndex;
								if(hinstLib) FreeLibrary(hinstLib);
								return index;
							}

							pAddr = pAddr->Next;
						}
					}

					pAddresses = pAddresses->Next;
				}
			}
		}

		if(hinstLib) FreeLibrary(hinstLib);
		return false;
	}

	std::unique_ptr<Interface> get_interface_by_adapter_name(const wchar_t* aname, const wchar_t* desired_addr, int ai_family)
	{
		if(!aname) return nullptr;
		if(ai_family != AF_INET && ai_family != AF_INET6) return nullptr;

		struct addrinfo hints = {0};
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = ai_family;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;

		struct addrinfo* pai = 0;
		if(desired_addr && *desired_addr)
		{
			std::unique_ptr<char[]> ptrAddress;
			size_t buflen = 0;
			if(0 == wcstombs_s(&buflen, NULL, 0, desired_addr, 0))
			{
				ptrAddress = std::make_unique<char[]>(buflen);
				wcstombs_s(&buflen, ptrAddress.get(), buflen, desired_addr, _TRUNCATE);
			}

			int res = getaddrinfo(ptrAddress.get(), "0"/*any port*/, &hints, &pai);
			if(0 != res) return nullptr;
			if(0 == pai) return nullptr;
		}

		/*
		// Create a locale object representing the active codepage locale
		_locale_t locale = _create_locale(LC_ALL, ".ACP");

		size_t buflen = 0;
		char* pszAName = 0;
		if(0 == _wcstombs_s_l(&buflen, NULL, 0, aname, 0, locale))
		{
			pszAName = new char[buflen];
			_wcstombs_s_l(&buflen, pszAName, buflen, aname, _TRUNCATE, locale);
		}

		_free_locale(locale);
		*/

		size_t buflen = 0;
		std::unique_ptr<char[]> ptrAName;
		if(0 == wcstombs_s(&buflen, NULL, 0, aname, 0))
		{
			ptrAName = std::make_unique<char[]>(buflen);
			wcstombs_s(&buflen, ptrAName.get(), buflen, aname, _TRUNCATE);
		}

		//check whether GetAdaptersAddresses is presented
		typedef ULONG (WINAPI* GetAAProc)(ULONG,ULONG,PVOID,PIP_ADAPTER_ADDRESSES,PULONG);
		GetAAProc pGetAdaptersAddressesProc = 0;

		HINSTANCE hinstLib = LoadLibrary(L"Iphlpapi.dll");
		if(hinstLib)
			pGetAdaptersAddressesProc = (GetAAProc)GetProcAddress(hinstLib, "GetAdaptersAddresses");

#ifdef GTEST_PROJECT
		if(!fGTestGetAdaptersAddresses) pGetAdaptersAddressesProc = 0;
#endif

		if(pGetAdaptersAddressesProc)
		{
			ULONG ulSize = 0;

			ULONG ulFlags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;

			if(ERROR_BUFFER_OVERFLOW == (pGetAdaptersAddressesProc)(ai_family, ulFlags, NULL, NULL, &ulSize))
			{
				std::unique_ptr<unsigned char[]> ptrAddressesBuf = std::make_unique<unsigned char[]>(ulSize);
				IP_ADAPTER_ADDRESSES* pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(ptrAddressesBuf.get());

				if(ERROR_SUCCESS == (pGetAdaptersAddressesProc)(ai_family, ulFlags, NULL, pAddresses, &ulSize))
				{
					while(pAddresses)
					{
						if((0==wcscmp(pAddresses->FriendlyName, aname))
							|| (0==wcscmp(pAddresses->Description, aname))
							|| (0==strcmp(pAddresses->AdapterName, ptrAName.get())))
						{
							switch(ai_family)
							{
							case AF_INET:
								if(!(pAddresses->Flags&IP_ADAPTER_IPV4_ENABLED))
								{
									pAddresses = pAddresses->Next;
									continue;
								}
								break;
							case AF_INET6:
								if(!(pAddresses->Flags&IP_ADAPTER_IPV6_ENABLED))
								{
									pAddresses = pAddresses->Next;
									continue;
								}
								break;
							}

							IP_ADAPTER_UNICAST_ADDRESS* pAddr = pAddresses->FirstUnicastAddress;

							while(pAddr)
							{
								if(pai)
								{
									if((static_cast<INT>(pai->ai_addrlen) == pAddr->Address.iSockaddrLength)
										&& (compare_sockaddrs(pAddr->Address.lpSockaddr, pAddr->Address.iSockaddrLength, pai->ai_addr, pai->ai_addrlen)))
									{
										unsigned long if6index = ai_family == AF_INET6 ? pAddresses->Ipv6IfIndex : 0;
										std::unique_ptr<Interface> ptrI = std::make_unique<Interface>(pai, if6index);

										if(hinstLib) FreeLibrary(hinstLib);
										return ptrI;
									}
								}
								else
									if(pAddr->Address.lpSockaddr->sa_family == ai_family)
									{
										const char* null_addr = (ai_family==AF_INET6) ? "::" : "0";
										//just for allocating addrinfo structure
										int res = getaddrinfo(null_addr, "0"/*any port*/, &hints, &pai);
										if((0 == res) && (static_cast<INT>(pai->ai_addrlen) == pAddr->Address.iSockaddrLength))
										{
											memcpy(pai->ai_addr, pAddr->Address.lpSockaddr,pai->ai_addrlen);

											unsigned long if6index = ai_family==AF_INET6 ? pAddresses->Ipv6IfIndex : 0;

											std::unique_ptr<Interface> ptrI = std::make_unique<Interface>(pai, if6index);

											if(hinstLib) FreeLibrary(hinstLib);
											return ptrI;
										}
									}

								pAddr = pAddr->Next;
							}
						}

						pAddresses = pAddresses->Next;
					}
				}
			}
		}
		else
			if(ai_family == AF_INET)
			{
				//for Windows 2000
				DWORD dwSize = 0;
				if(GetAdaptersInfo(NULL, &dwSize) == ERROR_BUFFER_OVERFLOW)
				{
					std::unique_ptr<unsigned char[]> ptrAdapterInfoBuf = std::make_unique<unsigned char[]>(dwSize);
					IP_ADAPTER_INFO* pAdapterInfo = reinterpret_cast<IP_ADAPTER_INFO*>(ptrAdapterInfoBuf.get());

					if(GetAdaptersInfo(pAdapterInfo, &dwSize) == ERROR_SUCCESS)
					{
						while(pAdapterInfo)
						{
							if((ptrAName && ptrAName[0] && (0==strcmp(pAdapterInfo->Description, ptrAName.get())))
								|| (0==strcmp(pAdapterInfo->AdapterName, ptrAName.get())))
							{
								//Doesn't work in Windows7
								//wchar_t wszAdapterName[MAX_ADAPTER_NAME_LENGTH + 4] = {0};
								//MultiByteToWideChar(CP_ACP, 0, pAdapterInfo->AdapterName, _ARRAYSIZE(pAdapterInfo->AdapterName), wszAdapterName, _ARRAYSIZE(wszAdapterName));
								//ULONG IfIndex = 0;
								//DWORD dwResult = GetAdapterIndex(wszAdapterName, &IfIndex);

								IP_ADDR_STRING* pAddrString = &pAdapterInfo->IpAddressList;
								if(pai)
								{
									while (pAddrString)
									{
										unsigned long ulIP = inet_addr(pAddrString->IpAddress.String);
										if(ulIP && (pai->ai_addrlen == sizeof(sockaddr_in)))
										{
											sockaddr_in* pin_addr = reinterpret_cast<sockaddr_in*>(pai->ai_addr);

											if(pin_addr->sin_addr.S_un.S_addr == ulIP)
											{
												std::unique_ptr<Interface> ptrI = std::make_unique<Interface>(pai, 0);

												if(hinstLib) FreeLibrary(hinstLib);
												return ptrI;
											}
										}

										pAddrString = pAddrString->Next;
									}
								}
								else
									if(pAddrString && *pAddrString->IpAddress.String)
									{
										int res = getaddrinfo(pAddrString->IpAddress.String, "0"/*any port*/, &hints, &pai);
										if(0 == res)
										{
											std::unique_ptr<Interface> ptrI = std::make_unique<Interface>(pai, 0);

											if(hinstLib) FreeLibrary(hinstLib);
											return ptrI;
										}
									}
							}
							pAdapterInfo = pAdapterInfo->Next;
						}
					}
				}
			}

		if(hinstLib) FreeLibrary(hinstLib);
		if(pai) freeaddrinfo(pai);
		return false;
	}
#else
	wchar_t* sockaddr_to_string(const sockaddr* pcaddr, size_t /*addrlen*/)
	{
		char dst[INET6_ADDRSTRLEN] = {0};
		const char* szAddress = "";

		switch(pcaddr->sa_family)
		{
		case AF_INET:
			{
				const sockaddr_in* pa = reinterpret_cast<const sockaddr_in*>(pcaddr);
				szAddress = inet_ntop(pa->sin_family, &pa->sin_addr, dst, INET6_ADDRSTRLEN);
			}
			break;
		case AF_INET6:
			{
				const sockaddr_in6* pa = reinterpret_cast<const sockaddr_in6*>(pcaddr);
				szAddress = inet_ntop(pa->sin6_family, &pa->sin6_addr, dst, INET6_ADDRSTRLEN);
			}
			break;
		default:
			szAddress = inet_ntop(pcaddr->sa_family, pcaddr->sa_data, dst, INET6_ADDRSTRLEN);
		}

		if(0 == szAddress || 0 == *szAddress) return NULL;

		wchar_t* pwszAddress = 0;

		size_t len = NixHlpr.assignWcharSz(&pwszAddress, szAddress);

		wchar_t* pwszAddressPort = 0;

		switch(pcaddr->sa_family)
		{
		case AF_INET:
		{
			const sockaddr_in* pa = reinterpret_cast<const sockaddr_in*>(pcaddr);
			if(pa->sin_port)
			{
				pwszAddressPort = new wchar_t[len+16];//16 for port number
				swprintf(pwszAddressPort, len+16, L"%ls:%u", pwszAddress, ntohs(pa->sin_port));
			}
		}
		break;

		case AF_INET6:
		{
			const sockaddr_in6* paddrin6 = reinterpret_cast<const sockaddr_in6*>(pcaddr);

			if(paddrin6->sin6_scope_id)
				paddrin6->sin6_scope_id;

			if(paddrin6->sin6_port || paddrin6->sin6_scope_id)
			{
				pwszAddressPort = new wchar_t[len+32];//32 for port number and scope_id
				if(paddrin6->sin6_port && paddrin6->sin6_scope_id)
					swprintf(pwszAddressPort, len+16, L"[%ls%%%u]:%u", pwszAddress, paddrin6->sin6_scope_id, ntohs(paddrin6->sin6_port));
				else
					if(paddrin6->sin6_port)
						swprintf(pwszAddressPort, len+16, L"[%ls]:%u", pwszAddress, ntohs(paddrin6->sin6_port));
					else
						swprintf(pwszAddressPort, len+16, L"%ls%%%u", pwszAddress, paddrin6->sin6_scope_id);
			}
		}
		break;
		}

		if(pwszAddressPort)
		{
			delete[] pwszAddress;
			return pwszAddressPort;
		}

		return pwszAddress;
	}

#ifdef NET_RT_IFLIST
	/*
	 * Round up 'a' to next multiple of 'size', which must be a power of 2
	 */
	#define ROUNDUP(a, size) (((a) & ((size)-1)) ? (1 + ((a) | ((size)-1))) : (a))

	/*
	 * Step to next socket address structure;
	 * if sa_len is 0, assume it is sizeof(u_long).
	 */
	#define NEXT_SA(ap) ap = (const struct sockaddr *) ((caddr_t) ap + (ap->sa_len ? ROUNDUP(ap->sa_len, sizeof (u_long)) : sizeof(u_long)))

	void get_rtaddrs(int addrs, const struct sockaddr* sa, const struct sockaddr **rti_info)
	{
		for (int i = 0; i < RTAX_MAX; i++)
		{
			if (addrs & (1 << i))
			{
				rti_info[i] = sa;
				NEXT_SA(sa);
			} else
				rti_info[i] = NULL;
		}
	}

	bool get_rt_inet6_info( const char* aname, const sockaddr_in6* paddr, unsigned int* pifIndex, struct in6_addr* pin6addr)
	{
		unsigned int ifIndex = 0; //if then information about all interfaces will be returned
		if(aname)
			ifIndex = if_nametoindex (aname);

		int mib[6] = {CTL_NET, AF_ROUTE, 0, AF_INET6/* only addresses of this family */, NET_RT_IFLIST, ifIndex/* interface index or 0 */};

		size_t len = 0;
		if (sysctl(mib, 6, NULL, &len, NULL, 0) < 0) return false;

		char *buf = new char[len];

		if (sysctl(mib, 6, buf, &len, NULL, 0) < 0)
		{
			delete buf;
			return false;
		}

		const struct sockaddr* rti_info[RTAX_MAX]={0};

		for (char* next = buf; next < buf + len; )
		{
			struct if_msghdr* ifm = (struct if_msghdr *) next;
			if (ifm->ifm_type == RTM_IFINFO)
			{
				if(ifm->ifm_addrs & (1<<RTAX_IFA))
				{
					const struct sockaddr* sa = (const struct sockaddr*) (ifm + 1);
					get_rtaddrs(ifm->ifm_addrs, sa, rti_info);

					const struct sockaddr_in6* psa6 = reinterpret_cast<const struct sockaddr_in6*>(rti_info[RTAX_IFA]);

					if(psa6)
					{
						if(0==paddr)
						{
							*pifIndex = ifm->ifm_index; 
							if(pin6addr) memcpy(pin6addr, &psa6->sin6_addr, sizeof(psa6->sin6_addr));
							delete[] buf;
							return true;
						}

						if(0==memcmp(&paddr->sin6_addr, &psa6->sin6_addr, sizeof(psa6->sin6_addr)))
						{
							*pifIndex = ifm->ifm_index;
							delete[] buf;
							return true;
						}
					}
				}
			}
			else
				if (ifm->ifm_type == RTM_NEWADDR)
				{
					struct ifa_msghdr* ifam = (struct ifa_msghdr*) next;
					if(ifam->ifam_addrs & (1<<RTAX_IFA))
					{
						const struct sockaddr* sa = (const struct sockaddr*) (ifam + 1);
						get_rtaddrs(ifam->ifam_addrs, sa, rti_info);

						const struct sockaddr_in6* psa6 = reinterpret_cast<const struct sockaddr_in6*>(rti_info[RTAX_IFA]);
						if(psa6)
						{
							if(0==paddr)
							{
								*pifIndex = ifam->ifam_index; 
								if(pin6addr) memcpy(pin6addr, &psa6->sin6_addr, sizeof(psa6->sin6_addr));
								delete[] buf;
								return true;
							}

							if(0==memcmp(&paddr->sin6_addr, &psa6->sin6_addr, sizeof(psa6->sin6_addr)))
							{
								*pifIndex = ifam->ifam_index;
								delete[] buf;
								return true;
							}
						}
					}
				}

				next += ifm->ifm_msglen;
		}

		delete[] buf;

		return false;
	}
#endif // NET_RT_IFLIST

	bool get_proc_net_if_inet6_info( const char* aname, const sockaddr_in6* paddr, unsigned int* pifIndex, struct in6_addr* pin6addr)
	{
		//It will be done using /proc/net/if_inet6
		//http://www.linuxjournal.com/article/8381

		const char *filePath = "/proc/net/if_inet6";
		
		struct stat linkAttrs = {0};//link attributes.
		if( ( lstat( filePath, &linkAttrs ) ) != 0 )
#ifdef NET_RT_IFLIST
			return get_rt_inet6_info(aname, paddr, pifIndex, pin6addr);
#else
			return 0;
#endif
		if( S_ISLNK( linkAttrs.st_mode ) ) return 0;//ERROR: /proc/net/if_inet6 is a symbolic link

		FILE *file = fopen(filePath, "r");
		if(NULL == file) return 0;

		struct stat fileAttrs = {0};//file attributes.
		if( ( fstat( fileno( file ), &fileAttrs ) ) != 0 )
		{
			fclose(file);
			return 0;
		}

		if( ( ( linkAttrs.st_ino ) != ( fileAttrs.st_ino ) ) ||
			( ( linkAttrs.st_dev ) != ( fileAttrs.st_dev ) ) )
		{
			fclose(file);
			return 0;
		}

		if( ! ( S_ISREG( fileAttrs.st_mode ) ) )
		{
			fclose(file);
			return 0;
		}

		if( ( ( fileAttrs.st_size ) || ( fileAttrs.st_blocks ) ) != 0 )
		{
			fclose(file);
			return 0;
		}

		if( ( ( fileAttrs.st_uid ) || ( fileAttrs.st_gid ) ) != 0 )
		{
			fclose(file);
			return 0;
		}

		if( (fileAttrs.st_mode & ALLPERMS) != 0x124 )//0444
		{
			fclose(file);
			return 0;
		}

		unsigned int ip6addr[4] = {0};
		unsigned int ifIndex = 0;
		unsigned int ifPrefix = 0;
		unsigned int ifScope = 0;
		unsigned int ifFlag = 0;
		char szIfName[IF_NAMESIZE] = {0};

		while(EOF != fscanf(file, "%08x%08x%08x%08x %x %x %x %x %s",
			&ip6addr[0],&ip6addr[1],&ip6addr[2],&ip6addr[3], &ifIndex, &ifPrefix, &ifScope, &ifFlag, szIfName))
		{
			uint32_t haddr[4] = {htonl(ip6addr[0]), htonl(ip6addr[1]), htonl(ip6addr[2]), htonl(ip6addr[3])};
			bool bAddr = (0==paddr);
			if(!bAddr)
			{
				if(0==memcmp(&paddr->sin6_addr, haddr, sizeof(haddr)))
				{
					bAddr = true;
				}
				else
					continue;
			}

			bool bName = (0==aname);
			if(!bName)
				bName = (0==strcmp(aname, szIfName));

			if(bName && bAddr)
			{
				fclose(file);

				if(pifIndex)
					*pifIndex = ifIndex;

				if(pin6addr)
				{
					memcpy(pin6addr, haddr, sizeof(haddr));
				}

				return true;
			}
		}

		fclose(file);
		return false;
	}
	
	unsigned int get_interface_v6_index_by_addr(const sockaddr_in6* paddr)
	{
		if(paddr->sin6_scope_id && IN6_IS_ADDR_LINKLOCAL(&paddr->sin6_addr)) return paddr->sin6_scope_id;

		//It will be done using /proc/net/if_inet6
		//http://www.linuxjournal.com/article/8381

		unsigned int ifIndex = 0;
		if(get_proc_net_if_inet6_info(NULL, paddr, &ifIndex, NULL))
			return ifIndex;

		return 0;
	}

	bool get_interface_by_adapter_name4(const wchar_t* aname, const wchar_t* desired_addr, int ai_family, Interface*& piref)
	{
		if(!aname) return false;
		if(ai_family != AF_INET) return false;

		char* pszAName = 0;
		size_t buflen = NixHlpr.assignCharSz(&pszAName, aname);
		DBG_UNREFERENCED_LOCAL_VARIABLE(buflen);
		if(0==pszAName) return false;

		struct ifreq ifr = {0};
		strncpy(ifr.ifr_name, pszAName, _ARRAYSIZE(ifr.ifr_name)-1);

		delete[] pszAName;

		struct addrinfo hints = {0};
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = ai_family;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;

		struct addrinfo* pai = 0;
		if(desired_addr && *desired_addr)
		{
			char* address = 0;
			NixHlpr.assignCharSz(&address, desired_addr);
			if(0==address) return false;

			int res = getaddrinfo(address, "0"/*any port*/, &hints, &pai);
			delete[] address;
			if(0 != res) return false;
			if(0 == pai) return false;
		}

		int sck = socket(ai_family, SOCK_DGRAM, 0);
		if(sck < 0)
		{
			if(pai) freeaddrinfo(pai);
			return false;
		}

		/* Query available interface by name. */
		int result = ioctl(sck, SIOCGIFADDR, &ifr);
		close(sck);

		if(result!=0)
		{
			if(pai) freeaddrinfo(pai);
			return false;
		}

		if(pai)
		{
			if((pai->ai_addrlen == sizeof(ifr.ifr_addr))
				&& (compare_sockaddrs(&ifr.ifr_addr, sizeof(ifr.ifr_addr), pai->ai_addr, pai->ai_addrlen)))
			{
				piref = new Interface(pai, 0);

				freeaddrinfo(pai);
				return true;
			}

			freeaddrinfo(pai);
		}
		else
		{
			piref = new Interface(&ifr.ifr_addr,0);
			return true;
		}

		return false;
	}

	bool get_interface_by_adapter_name(const wchar_t* aname, const wchar_t* desired_addr, int ai_family, Interface*& piref)
	{
		if(!aname) return false;
		if(ai_family == AF_INET)
			return get_interface_by_adapter_name4(aname, desired_addr, ai_family, piref);

		if(ai_family != AF_INET6) return false;

		struct addrinfo hints = {0};
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = ai_family;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;

		struct addrinfo* pai = 0;
		if(desired_addr && *desired_addr)
		{
			char* address = 0;
			NixHlpr.assignCharSz(&address, desired_addr);
			if(0==address) return false;
			int res = getaddrinfo(address, "0"/*any port*/, &hints, &pai);
			delete[] address;
			if(0 != res) return false;
			if(0 == pai) return false;

			if(pai->ai_addrlen < sizeof(sockaddr_in6))
			{
				freeaddrinfo(pai);
				return false;
			}
		}

		char* pszAName = 0;
		size_t buflen = NixHlpr.assignCharSz(&pszAName, aname);
		DBG_UNREFERENCED_LOCAL_VARIABLE(buflen);
		if(0==pszAName)
		{
			if(pai) freeaddrinfo(pai);
			return false;
		}

		unsigned int ifIndex = 0;

		if(pai)
		{
			const sockaddr_in6* pcaddr6 = reinterpret_cast<sockaddr_in6*> (pai->ai_addr);
			if(get_proc_net_if_inet6_info(pszAName, pcaddr6, &ifIndex, NULL))
			{
				piref = new Interface(pai, ifIndex);
				freeaddrinfo(pai);
				return true;
			}
		}
		else
		{
			//just for allocating addrinfo structure
			int res = getaddrinfo("::", NULL/*any port*/, &hints, &pai);
			if((0 == res) && (pai->ai_addrlen >= sizeof(struct sockaddr_in6)))
			{
				sockaddr_in6* paddr6 = reinterpret_cast<sockaddr_in6*> (pai->ai_addr);
				if(get_proc_net_if_inet6_info(pszAName, NULL, &ifIndex, &paddr6->sin6_addr))
				{
					piref = new Interface(pai, ifIndex);
					freeaddrinfo(pai);
					return true;
				}
			}
		}

		freeaddrinfo(pai);
		return false;
	}

	/*
	The new fashioned one is to query them using rtnetlink. You use a RTM_GETADDR
	NLM_F_REQUEST query with wildcard (NLM_F_ROOT) to get a full list.
	See the netlink,rtnetlink, libnetlink manpages and iproute2 as an example.
	It is easier when you use libnetlink.
	*/
	bool get_default_netconfig_ioctl(Interface*& piref, Sender*& psref, Receiver*& prref, DESTADDR_INFO*& pdref)
	{
		if(Receiver::debug_)
		{
			consoleio::print_line( resources::wszDbgNetIfs );
		}

		int sck = socket(AF_INET, SOCK_DGRAM, 0);
		if(sck < 0) return false;

		char ifreq_buf[sizeof(struct ifreq)*128] = {0};

		// Query available interfaces.
		struct ifconf ifc = {0};
		ifc.ifc_len = sizeof(ifreq_buf);
		ifc.ifc_buf = ifreq_buf;

		int result = ioctl(sck, SIOCGIFCONF, &ifc);

		if( result != 0)
		{
			close(sck);
			return false;
		}

		if(Receiver::debug_)
		{
			for (char* ptr = ifreq_buf; ptr < ifreq_buf + ifc.ifc_len; )
			{
				struct ifreq *ifr = (struct ifreq *) ptr;
#ifdef _SIZEOF_ADDR_IFREQ
				ptr += _SIZEOF_ADDR_IFREQ(*ifr);
#else
				ptr += sizeof(struct ifreq);
#endif
				wchar_t* wszAddress = sockaddr_to_string(&ifr->ifr_addr, 0);
				if(wszAddress)
				{
					consoleio::print_line(wszAddress);
					delete[] wszAddress;
				}
			}
		}

		unsigned long if6index = 0;

		const struct ifreq* pifreq4 = 0;
		const struct ifreq* pifreq6 = 0;
		// Iterate through the list of interfaces.
		for (char* ptr = ifreq_buf; (ptr < ifreq_buf + ifc.ifc_len) && (0==pifreq4); )
		{
			struct ifreq *ifr = (struct ifreq *) ptr;
#ifdef _SIZEOF_ADDR_IFREQ
			ptr += _SIZEOF_ADDR_IFREQ(*ifr);
#else
			ptr += sizeof(struct ifreq);
#endif
			struct ifreq ifrcopy = *ifr;
			int result = ioctl(sck, SIOCGIFFLAGS, &ifrcopy);

			if( 0!=result ) continue;

			if ((ifrcopy.ifr_flags & IFF_UP) == 0) continue;	/* ignore if interface not up */
			if ((ifrcopy.ifr_flags & IFF_MULTICAST) == 0) continue;	/* ignore if interface not supported for multicast */
			if ((ifrcopy.ifr_flags & IFF_LOOPBACK) != 0) continue;	/* ignore loopback interfaces */

			switch(ifr->ifr_addr.sa_family)
			{
			case AF_INET:
				if(0==pifreq4)//not necessary, for testing only
				{
					//INADDR_ANY              (ULONG)0x00000000
					//INADDR_LOOPBACK         0x7f000001
					//INADDR_BROADCAST        (ULONG)0xffffffff
					//INADDR_NONE             0xffffffff
					const sockaddr_in* pa = reinterpret_cast<const sockaddr_in*>(&ifr->ifr_addr);
#ifdef  CHATTERM_OS_WINDOWS
					if(INADDR_ANY == pa->sin_addr.S_un.S_addr
						|| INADDR_LOOPBACK == ntohl(pa->sin_addr.S_un.S_addr)
						|| INADDR_BROADCAST == pa->sin_addr.S_un.S_addr
						|| INADDR_NONE == pa->sin_addr.S_un.S_addr
						|| IN_MULTICAST( ntohl(pa->sin_addr.S_un.S_addr)))
#else
					if(INADDR_ANY == pa->sin_addr.s_addr
						|| INADDR_LOOPBACK == ntohl(pa->sin_addr.s_addr)
						|| INADDR_BROADCAST == pa->sin_addr.s_addr
						|| INADDR_NONE == pa->sin_addr.s_addr
						|| IN_MULTICAST( ntohl(pa->sin_addr.s_addr)))
#endif // CHATTERM_OS_WINDOWS
					{
					}
					else
					{
						pifreq4 = ifr;
						if6index = 0;
					}
				}
				break;

			case AF_INET6:
				if(0==pifreq6)
				{
					const sockaddr_in6* pa = reinterpret_cast<const sockaddr_in6*>(&ifr->ifr_addr);
					if(IN6_IS_ADDR_UNSPECIFIED(&pa->sin6_addr)
						//|| IN6_IS_ADDR_LINKLOCAL(&pa->sin6_addr)
						|| IN6_IS_ADDR_LOOPBACK(&pa->sin6_addr)
						|| IN6_IS_ADDR_MULTICAST(&pa->sin6_addr)
#ifdef  CHATTERM_OS_WINDOWS
						|| IN6_IS_ADDR_ANYCAST(&pa->sin6_addr)
						|| IN6_IS_ADDR_V4TRANSLATED(&pa->sin6_addr)
#endif // CHATTERM_OS_WINDOWS
						|| IN6_IS_ADDR_V4MAPPED(&pa->sin6_addr)
						|| IN6_IS_ADDR_V4COMPAT(&pa->sin6_addr))
					{
					}
					else
					{
						pifreq6 = ifr;
						if6index = if_nametoindex(ifr->ifr_name);
					}
				}
				break;
			}
		}

		close(sck);

		const struct ifreq* pifreq = pifreq4 ? pifreq4 : pifreq6;

		if(!pifreq) return false;

		Interface* pi = new Interface(&pifreq->ifr_addr, if6index);

		if(0 == pi) return false;

		piref = pi;

		psref = new Sender();
		//unsigned short port = DEFAULT_PORT;
		unsigned short port = 0;

		if(0!=psref->bindToInterface(piref, port, DWORD(-1)))
		{
			wchar_t* wszAddress = piref->getStringAddress();
			consoleio::print_line(resources::wszErrNotBindSender, wszAddress);
			delete[] wszAddress;
		}

		const wchar_t* wszMcastAddress =  pifreq->ifr_addr.sa_family == AF_INET6 ? DEFAULT_MCAST_ADDR_V6 : DEFAULT_MCAST_ADDR;

		prref = new Receiver(psref);
		port = DEFAULT_PORT;
		int bind_result = prref->bindToInterface(piref, port, wszMcastAddress);
		wchar_t* wszIfAddress = piref->getStringAddress(port);

		if(0 != bind_result)
			consoleio::print_line(resources::wszErrNotBindRcvr, wszIfAddress);

		pdref = new DESTADDR_INFO(psref);

		if(0 != pdref->bindToAddress(wszMcastAddress, port))
			consoleio::print_line(resources::wszErrNotBindDestAddr, wszMcastAddress);

		consoleio::print_line(resources::wszDefaultNetIf, wszIfAddress, wszMcastAddress);

		delete[] wszIfAddress;
		return true;
	}
#endif // CHATTERM_OS_WINDOWS

	bool get_default_netconfig(std::shared_ptr<Sender>& psref, std::shared_ptr<Receiver>& prref, std::unique_ptr<DESTADDR_INFO>& pdref)
	{
		struct addrinfo hints = {0};
		hints.ai_flags = AI_CANONNAME;
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;

		struct addrinfo* pres_addr = 0;
#ifdef  CHATTERM_OS_WINDOWS
		/*If the pNodeName parameter points to a computer name, the permanent addresses for the computer are returned.
		If pNodeName parameter points to a string equal to "localhost", all loopback addresses on the local
		computer are returned.
		If the pNodeName parameter contains an empty string, all registered addresses on the local computer are returned.
		On Windows Server 2003 and later if the pNodeName parameter points to a string equal to "..localmachine",
		all registered addresses on the local computer are returned.
		If the pNodeName parameter refers to a cluster virtual server name, only virtual server addresses are returned.*/
		int result = getaddrinfo("", "0"/*any port*/, &hints, &pres_addr);
#else
		struct utsname info = {0};
		
		if(0 != uname(&info)) return get_default_netconfig_ioctl(piref, psref, prref, pdref);

		//resolves info.nodename using a DNS server and /etc/hosts
		//local addresses that are not specified in the /etc/hosts are not returned here 
		int result = getaddrinfo(info.nodename, NULL/*any port*/, &hints, &pres_addr);//"..localmachine" only for Win2003
#endif // CHATTERM_OS_WINDOWS

		if(0 != result) return false;

		struct addrinfo* ai_next = pres_addr;
		struct addrinfo* ai_next4 = 0;
		struct addrinfo* ai_next6 = 0;

		if(Receiver::debug_)
		{
			consoleio::print_line( resources::wszDbgNetIfs );

			while(ai_next)
			{
				std::wstring wszAddress = sockaddr_to_string(ai_next->ai_addr, ai_next->ai_addrlen);
				consoleio::print_line(wszAddress.c_str());

				ai_next = ai_next->ai_next;
			}

			ai_next = pres_addr;
		}

		unsigned long if6index = 0;
		//try to find an IPv4 binding address first
		while(ai_next && (0 == ai_next4))
		{
			//In Mac OS X ai_socktype and ai_protocol are always 0
			//if(ai_next->ai_socktype == SOCK_DGRAM && ai_next->ai_protocol == IPPROTO_UDP)
			{
				switch(ai_next->ai_family)
				{
				case AF_INET:
					if(0==ai_next4)//not necessary, for testing only
					{
						//INADDR_ANY              (ULONG)0x00000000
						//INADDR_LOOPBACK         0x7f000001
						//INADDR_BROADCAST        (ULONG)0xffffffff
						//INADDR_NONE             0xffffffff
						const sockaddr_in* pa = reinterpret_cast<const sockaddr_in*>(ai_next->ai_addr);
#ifdef  CHATTERM_OS_WINDOWS
						if(INADDR_ANY == pa->sin_addr.S_un.S_addr
							|| INADDR_LOOPBACK == ntohl(pa->sin_addr.S_un.S_addr)
							|| INADDR_BROADCAST == pa->sin_addr.S_un.S_addr
							|| INADDR_NONE == pa->sin_addr.S_un.S_addr
							|| IN_MULTICAST(ntohl(pa->sin_addr.S_un.S_addr)))
#else
						if(INADDR_ANY == pa->sin_addr.s_addr
							|| INADDR_LOOPBACK == ntohl(pa->sin_addr.s_addr)
							|| INADDR_BROADCAST == pa->sin_addr.s_addr
							|| INADDR_NONE == pa->sin_addr.s_addr
							|| IN_MULTICAST(ntohl(pa->sin_addr.s_addr)))
#endif // CHATTERM_OS_WINDOWS
						{
						}
						else
							ai_next4 = ai_next;
					}
					break;

				case AF_INET6:
					if(0==ai_next6)
					{
						const sockaddr_in6* pa = reinterpret_cast<const sockaddr_in6*>(ai_next->ai_addr);
						if(IN6_IS_ADDR_UNSPECIFIED(&pa->sin6_addr)
	//							|| IN6_IS_ADDR_LINKLOCAL(&pa->sin6_addr)
							|| IN6_IS_ADDR_LOOPBACK(&pa->sin6_addr)
							|| IN6_IS_ADDR_MULTICAST(&pa->sin6_addr)
#ifdef  CHATTERM_OS_WINDOWS
							|| IN6_IS_ADDR_ANYCAST(&pa->sin6_addr)
							|| IN6_IS_ADDR_V4TRANSLATED(&pa->sin6_addr)
#endif // CHATTERM_OS_WINDOWS
							|| IN6_IS_ADDR_V4MAPPED(&pa->sin6_addr)
							|| IN6_IS_ADDR_V4COMPAT(&pa->sin6_addr))
						{
						}
						else
							ai_next6 = ai_next;
					}
					break;
				}
			}

			ai_next = ai_next->ai_next;
		}

		if(ai_next4)
		{
			ai_next = ai_next4;
		}
		else
			if(ai_next6)
			{
				ai_next = ai_next6;
				if6index = get_interface_v6_index_by_addr(reinterpret_cast<const sockaddr_in6*>(ai_next6->ai_addr));
			}

		if(!ai_next) return false;

		std::shared_ptr<Interface> piref = std::make_shared<Interface>(ai_next, if6index);

		const wchar_t* wszMcastAddress =  ai_next->ai_family == AF_INET6 ? DEFAULT_MCAST_ADDR_V6 : DEFAULT_MCAST_ADDR;
		
		if(ai_next!= pres_addr)
			freeaddrinfo(pres_addr);

		psref = std::make_shared<Sender>();
		//unsigned short port = DEFAULT_PORT;
		unsigned short port = 0;

		if(0!=psref->bindToInterface(piref, port, DWORD(-1)))
		{
			std::wstring wszAddress = piref->getStringAddress();
			consoleio::print_line(resources::wszErrNotBindSender, wszAddress.c_str());
		}

		prref = std::make_shared<Receiver>(psref.get());
		port = DEFAULT_PORT;
		int bind_result = prref->bindToInterface(piref, port, wszMcastAddress);
		std::wstring wszIfAddress = piref->getStringAddress(port);

		if(0 != bind_result)
			consoleio::print_line(resources::wszErrNotBindRcvr, wszIfAddress.c_str());

		pdref = std::make_unique<DESTADDR_INFO>(psref.get());

		if(0 != pdref->bindToAddress(wszMcastAddress, port))
			consoleio::print_line(resources::wszErrNotBindDestAddr, wszMcastAddress);

		consoleio::print_line(resources::wszDefaultNetIf, wszIfAddress.c_str(), wszMcastAddress);

		return true;
	}

	//End of Global functions
	//////////////////////////////////////////////////////////////////

	Interface::Interface(const wchar_t* wszAddress, int ai_family) : pai_(nullptr), ipv6_if_index_(0)
	{
		std::unique_ptr<char[]> ptrAddress;
		size_t buflen = 0;
#ifdef CHATTERM_OS_WINDOWS
		if(0 == wcstombs_s(&buflen, NULL, 0, wszAddress, 0))
		{
			ptrAddress = std::make_unique<char[]>(buflen);
			wcstombs_s(&buflen, ptrAddress.get(), buflen, wszAddress, _TRUNCATE);
		}
#else
		buflen = NixHlpr.assignCharSz(&address, wszAddress);
#endif // CHATTERM_OS_WINDOWS

		struct addrinfo hints = {0};
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = ai_family;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;

		// Resolve the server address and port
		getaddrinfo(ptrAddress.get(), "0"/*any port*/, &hints, &pai_);

		if(pai_ && (AF_INET6==pai_->ai_family) && pai_->ai_addr)
		{
			sockaddr_in6* paddrin6 = reinterpret_cast<sockaddr_in6*>(pai_->ai_addr);

			ipv6_if_index_ = get_interface_v6_index_by_addr(paddrin6);

			if((0==paddrin6->sin6_scope_id) && IN6_IS_ADDR_LINKLOCAL(&paddrin6->sin6_addr))
				paddrin6->sin6_scope_id = ipv6_if_index_;
		}
	}

	Interface::Interface(const addrinfo* cpai, unsigned int if6index) : pai_(nullptr), ipv6_if_index_(if6index)
	{
		//reserve buffer for addrinfo data
		paibufPtr_ = std::make_unique<unsigned char[]>(sizeof(addrinfo)+ __max(sizeof(sockaddr_in6), cpai->ai_addrlen));
		memcpy(paibufPtr_.get(), cpai, sizeof(addrinfo));

		unsigned char* addr_buf = paibufPtr_.get() + sizeof(addrinfo);

		memcpy(addr_buf, cpai->ai_addr, cpai->ai_addrlen);

		pai_ = reinterpret_cast<addrinfo*>(paibufPtr_.get());

		pai_->ai_addr = reinterpret_cast<sockaddr*>(addr_buf);
		pai_->ai_next = 0;
		pai_->ai_socktype = SOCK_DGRAM;
		pai_->ai_protocol = IPPROTO_UDP;

		if(pai_ && (AF_INET6==pai_->ai_family) && pai_->ai_addr)
		{
			sockaddr_in6* paddrin6 = reinterpret_cast<sockaddr_in6*>(pai_->ai_addr);
			if((0==paddrin6->sin6_scope_id) && IN6_IS_ADDR_LINKLOCAL(&paddrin6->sin6_addr))
				paddrin6->sin6_scope_id = ipv6_if_index_;
		}
	}

#ifndef CHATTERM_OS_WINDOWS
	Interface::Interface(const sockaddr* pcaddr, unsigned int if6index) : pai_(0), paibuf_(0), ipv6_if_index_(if6index)
	{
		paibuf_ = new addrinfo;
		memset(paibuf_, 0x00, sizeof(*paibuf_));
		paibuf_->ai_family = pcaddr->sa_family;
		paibuf_->ai_addrlen = AF_INET6==pcaddr->sa_family ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
		unsigned char* addr_buf = new unsigned char[sizeof(struct sockaddr_in6)];
		memcpy(addr_buf, pcaddr, paibuf_->ai_addrlen);
		paibuf_->ai_addr = reinterpret_cast<sockaddr*>(addr_buf);
		paibuf_->ai_socktype = SOCK_DGRAM;
		paibuf_->ai_protocol = IPPROTO_UDP;
		paibuf_->ai_next = 0;

		pai_ = paibuf_;

		if(pai_ && (AF_INET6==pai_->ai_family) && pai_->ai_addr)
		{
			sockaddr_in6* paddrin6 = reinterpret_cast<sockaddr_in6*>(pai_->ai_addr);
			if((0==paddrin6->sin6_scope_id) && IN6_IS_ADDR_LINKLOCAL(&paddrin6->sin6_addr))
				paddrin6->sin6_scope_id = ipv6_if_index_;
		}
	}
#endif // CHATTERM_OS_WINDOWS

	std::wstring Interface::getStringAddress(unsigned short port)
	{
		sockaddr_in6 saddr = {0};
		memcpy(&saddr, pai_->ai_addr, __min(pai_->ai_addrlen, sizeof(saddr)));
		NETADDR_INFO::set_port(reinterpret_cast<sockaddr*>(&saddr), htons(port));

		return sockaddr_to_string(reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
	}

	std::wstring Interface::getStringAddress()
	{
		return sockaddr_to_string(pai_->ai_addr, pai_->ai_addrlen);
	}

	int Sender::bindToInterface(const std::shared_ptr<Interface>& refPtrIf, unsigned short port, DWORD dwTTL)
	{
		pif_ = refPtrIf;

		addrinfo* const& pai = refPtrIf->pai_;

		// Create a SOCKET for sending UDP datagrams
		sock_ = socket(pai->ai_family, pai->ai_socktype, pai->ai_protocol);

		_ASSERTE(INVALID_SOCKET != sock_);

		if(INVALID_SOCKET == sock_) return -1;

		switch(pai->ai_family)
		{
		case AF_INET:
			{
				DWORD dwVal = 1;
				int brcast = setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (char*)&dwVal, sizeof (dwVal));
				DBG_UNREFERENCED_LOCAL_VARIABLE(brcast);
				_ASSERTE(0 == brcast);
			}
			break;

		case AF_INET6:
			{
				DWORD dwVal = 1;//IPV6_V6ONLY is supported only since Vista, so do not check the result
				int ipv6oly = setsockopt (sock_, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&dwVal, sizeof (dwVal));
				DBG_UNREFERENCED_LOCAL_VARIABLE(ipv6oly);
				_ASSERTE(0 == ipv6oly);
			}
			break;
		default:
			_ASSERTE(!"Sender::bindToInterface pai->ai_family!");
			return -1;
		}

		int result = 0;
		if(port)
		{
			BOOL on=1;
			int reuseaddr = setsockopt(sock_,SOL_SOCKET,SO_REUSEADDR,(const char*)&on,sizeof(on));
			DBG_UNREFERENCED_LOCAL_VARIABLE(reuseaddr);
			_ASSERTE(0 == reuseaddr);

			sockaddr_in6 saddr = {0};
			memcpy(&saddr, pai->ai_addr, __min(pai->ai_addrlen, sizeof(saddr)));

			//test for Linux to enable using one socket for sending and receiving multicast datagrams
			//saddr.sin6_addr = in6addr_any;

			NETADDR_INFO::set_port(reinterpret_cast<sockaddr*>(&saddr), htons(port));

			result = bind(sock_, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
		}
		else
			result = bind(sock_, pai->ai_addr, static_cast<int>(pai->ai_addrlen));

		_ASSERTE(0 == result);

		if(0 == result)
		{
			//get bound port number
			sockaddr_in6 saddr = {0};
#ifdef CHATTERM_OS_WINDOWS
			int saddr_len = sizeof(saddr);
#else
			socklen_t saddr_len = sizeof(saddr);
#endif // CHATTERM_OS_WINDOWS

			if(0 == getsockname(sock_, reinterpret_cast<sockaddr*>(&saddr), &saddr_len))
				port_ = saddr.sin6_port;
			else
				port_ = htons(port);

			_ASSERTE((port==0) || ( htons(port) == saddr.sin6_port));

			switch(pai->ai_family)
			{
			case AF_INET:
				{
					if(dwTTL == DWORD(-1)) dwTTL = DEFAULT_TTL;
					int result1 = setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&dwTTL, sizeof (dwTTL));
					_ASSERTE(0 == result1);

					const sockaddr_in* paddr_in = reinterpret_cast<const sockaddr_in*>(pai->ai_addr);
					int result2 = setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_IF, (char*)&paddr_in->sin_addr, sizeof (paddr_in->sin_addr));
					_ASSERTE(0 == result2);
					result = result1|result2;
				}
				break;

			case AF_INET6:
				{
					if(dwTTL == DWORD(-1)) dwTTL = DEFAULT_TTL_V6;
					int result1 = setsockopt(sock_, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char*)&dwTTL, sizeof (dwTTL));
					_ASSERTE(0 == result1);
					int result2 = setsockopt(sock_, IPPROTO_IPV6, IPV6_UNICAST_HOPS, (char*)&dwTTL, sizeof (dwTTL));
					_ASSERTE(0 == result2);
					unsigned int ifs = pif_->ipv6_if_index_;
					int result3 = setsockopt(sock_, IPPROTO_IPV6, IPV6_MULTICAST_IF, (char*)&ifs, sizeof (ifs));
					_ASSERTE(0 == result3);
					result = result1|result2|result3;
				}
				break;
			default:
				result = -1;
			}
		}

		_ASSERTE(0 == result);
		return result;
	}

	int Sender::sendTo(const sockaddr* paddr, const char* buf, int len) const
	{
		if(pif_->pai_->ai_family != paddr->sa_family) return -1;

		//Mac OS X returns error if size of paddr is sizeof(sockaddr_in6) in case of AF_INET
		int result = sendto(sock_, buf, len, 0, paddr, AF_INET6 == paddr->sa_family ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));

		if(debug_)
		{
			//to keep always together print_line and print_bytes
			consoleio::PrintLinesMonitor PRINT_LINES_MONITOR;
			consoleio::print_line_selected(resources::wszDbgPacket, len, theApp.getStrTime(true));

			std::wstring wszToAddr = sockaddr_to_string(paddr, sizeof(sockaddr_in6));
			std::wstring wszFromAddr = sockaddr_to_string(pif_->pai_->ai_addr, pif_->pai_->ai_addrlen);

			if(pif_->pai_->ai_family == AF_INET6)
				consoleio::print_line_selected(resources::wszDbgPacketSentToFrom6, wszToAddr.c_str(), wszFromAddr.c_str(), ntohs(port_));
			else
				consoleio::print_line_selected(resources::wszDbgPacketSentToFrom, wszToAddr.c_str(), wszFromAddr.c_str(), ntohs(port_));

			consoleio::print_bytes(reinterpret_cast<const unsigned char*>(buf), len);
		}

		_ASSERTE(len == result);
		return result;
	}

	int Receiver::bindToInterface(std::shared_ptr<Interface>& ptrIf, unsigned short port, const wchar_t* wszMcastGroups)
	{
		addrinfo* const& pai = ptrIf->pai_;
		int result = 0;

		if(nullptr!=pcSender_ && (htons(port) == pcSender_->port_))
		{
			//Using the same socket for receiving and sending is not good idea
			//I'm not sure that sendto() and recvrfom() are thread safe
			sock_ = pcSender_->sock_;

			/* It is not necessary
			switch(pai->ai_family)
			{
			case AF_INET:
				{
					DWORD dwVal = 1;
					result = setsockopt(sock_,IPPROTO_IP,IP_MULTICAST_LOOP,(const char*)&dwVal,sizeof(dwVal));
					if(result)
						_ASSERTE(!"Failed to set IP_MULTICAST_LOOP socket option!");
				}
				break;

			case AF_INET6:
				{
					DWORD dwVal = 1;
					result = setsockopt(sock_,IPPROTO_IPV6,IPV6_MULTICAST_LOOP,(const char*)&dwVal,sizeof(dwVal));
					if(result)
						_ASSERTE(!"Failed to set IPV6_MULTICAST_LOOP socket option!");
				}
				break;
			default:
				_ASSERTE(!"Receiver::bindToInterface pai->ai_family!");
				return -1;
			}*/
		}
		else
		{
			// Create a SOCKET for sending UDP datagrams
			sock_ = socket(pai->ai_family, pai->ai_socktype, pai->ai_protocol);

			_ASSERTE(INVALID_SOCKET != sock_);
			if(INVALID_SOCKET == sock_) return -1;

			DWORD dwVal = 1;
			int reuseaddr = setsockopt(sock_,SOL_SOCKET,SO_REUSEADDR,(const char*)&dwVal,sizeof(dwVal));
			DBG_UNREFERENCED_LOCAL_VARIABLE(reuseaddr);
			_ASSERTE(0 == reuseaddr);

			switch(pai->ai_family)
			{
			case AF_INET:
				{
					sockaddr_in saddr = {0};
					saddr.sin_family = AF_INET;
					saddr.sin_port = htons(port);
		#ifdef CHATTERM_OS_WINDOWS
					const sockaddr_in* paddr = reinterpret_cast<const sockaddr_in*>(pai->ai_addr);
					saddr.sin_addr.s_addr = paddr->sin_addr.s_addr;
		#else
					saddr.sin_addr.s_addr = INADDR_ANY;
		#endif // CHATTERM_OS_WINDOWS
		
					result = bind(sock_, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
				}
				break;

			case AF_INET6:
				{
					dwVal = 1;//IPV6_V6ONLY is supported only since Vista, so do not check the result
					int ipv6oly = setsockopt (sock_, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&dwVal, sizeof (dwVal));
					DBG_UNREFERENCED_LOCAL_VARIABLE(ipv6oly);
					_ASSERTE(0 == ipv6oly);

					const sockaddr_in6* paddr = reinterpret_cast<const sockaddr_in6*>(pai->ai_addr);

					sockaddr_in6 saddr = {0};
					saddr.sin6_family = AF_INET6;
					saddr.sin6_port = htons(port);
					saddr.sin6_flowinfo = paddr->sin6_flowinfo;
					saddr.sin6_scope_id = paddr->sin6_scope_id;

		#ifdef CHATTERM_OS_WINDOWS
					saddr.sin6_addr = paddr->sin6_addr;
		#else
					saddr.sin6_addr = in6addr_any;
		#endif // CHATTERM_OS_WINDOWS
		
					result = bind(sock_, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr));
				}
				break;
			default:
				_ASSERTE(!"Receiver::bindToInterface pai->ai_family!");
				return -1;
			}
		}

		_ASSERTE(0 == result);

		if(0 == result)
		{
			ptrIf_ = ptrIf;

			//get bound port number to from_addr_ for ptrMe_->naddr_info.assign_from_receiver(Receivers_.front());
			//in ChatTerminalApp::run()
#ifdef CHATTERM_OS_WINDOWS
			int saddr_len = sizeof(from_addr_);
#else
			socklen_t saddr_len = sizeof(from_addr_);
#endif // CHATTERM_OS_WINDOWS
			if(0 == getsockname(sock_, reinterpret_cast<sockaddr*>(&from_addr_), &saddr_len))
				port_ = from_addr_.sin6_port;
			else
				port_ = htons(port);

			_ASSERTE(htons(port) == from_addr_.sin6_port);

			//other possible options here
			//IP_PKTINFO
			//IPV6_PKTINFO

			if(0==wszMcastGroups || 0==*wszMcastGroups)
			{
				if(AF_INET==pai->ai_family)
				{
					//receive broadcast messages only
#ifdef CHATTERM_OS_WINDOWS
					DWORD value = 1;
					result = setsockopt (sock_, IPPROTO_IP, IP_RECEIVE_BROADCAST, (char*)&value, sizeof (value));
#else
					//DWORD value = 1;
					//result = setsockopt (sock_, SOL_SOCKET, SO_BROADCAST, (char*)&value, sizeof (value));
#endif // CHATTERM_OS_WINDOWS
				}
				else
					result = -1;

				_ASSERTE(0 == result);
			}
			else
			{
				std::unique_ptr<char[]> mcast_groups;
				size_t buflen = 0;
#ifdef CHATTERM_OS_WINDOWS
				if(0 == wcstombs_s(&buflen, NULL, 0, wszMcastGroups, 0))
				{
					mcast_groups = std::make_unique<char[]>(buflen);
					wcstombs_s(&buflen, mcast_groups.get(), buflen, wszMcastGroups, _TRUNCATE);
				}
#else
				buflen = NixHlpr.assignCharSz(&mcast_groups, wszMcastGroups);
#endif // CHATTERM_OS_WINDOWS

				const char* seek = mcast_groups.get();
				while(*seek)
				{
					while(iswspace(*seek)) ++seek;

					const char* start = seek;

					while(*seek && !iswspace(*seek)) ++seek;

					if(seek<=start) break;

					std::string mcast_addr(start, seek-start);

					switch(pai->ai_family)
					{
					case AF_INET:
						{
							struct ip_mreq mreq = {0};// Used in adding or dropping multicasting addresses
							// Join the multicast group from which to receive datagrams.
							mreq.imr_multiaddr.s_addr = inet_addr(mcast_addr.c_str());

							const sockaddr_in* psin = reinterpret_cast<const sockaddr_in*>(ptrIf_->pai_->ai_addr);
#ifdef CHATTERM_OS_WINDOWS
							mreq.imr_interface.S_un.S_addr = psin->sin_addr.S_un.S_addr;
#else
							mreq.imr_interface.s_addr = psin->sin_addr.s_addr;
#endif // CHATTERM_OS_WINDOWS
							result = setsockopt (sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof (mreq));
							_ASSERTE(0 == result);
						}
						break;

					case AF_INET6:
						{
							struct ipv6_mreq mreq = {0};// Used in adding or dropping multicasting addresses
							// Join the multicast group from which to receive datagrams.

							struct addrinfo hints = {0};
							hints.ai_flags = AI_NUMERICHOST;
							hints.ai_family = pai->ai_family;
							hints.ai_socktype = SOCK_DGRAM;
							hints.ai_protocol = IPPROTO_UDP;

							struct addrinfo* pres_addr = 0;
							// Resolve the server address and port
							if(0 == getaddrinfo(mcast_addr.c_str(), "0"/*any port*/, &hints, &pres_addr))
							{
								const sockaddr_in6* pa = reinterpret_cast<const sockaddr_in6*>(pres_addr->ai_addr);

								memcpy(&mreq.ipv6mr_multiaddr, &pa->sin6_addr, sizeof(mreq.ipv6mr_multiaddr));
								mreq.ipv6mr_interface = ptrIf_->ipv6_if_index_;

								result = setsockopt (sock_, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*)&mreq, sizeof (mreq));
								freeaddrinfo(pres_addr);
							}
							else
								result = -1;

							_ASSERTE(0 == result);
						}
						break;

					default:
						result = -1;
					}
				}
			}
		}

		_ASSERTE(0 == result);

#ifndef CHATTERM_OS_WINDOWS
		if(0 == result)
		{
			struct timeval tv = {30, 0};//30 seconds timeout
			result = setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		}

		_ASSERTE(0 == result);
#endif // CHATTERM_OS_WINDOWS
		return result;
	}

#ifdef CHATTERM_OS_WINDOWS
	int Receiver::start()
	{
		if(INVALID_SOCKET == sock_) return -1;
		if(INVALID_HANDLE_VALUE != thread_) return 0;
		fStop_ = false;
		thread_ = (HANDLE)_beginthreadex(NULL, 0, recvProc, this, 0, NULL);
		_ASSERTE(thread_);
		return 0;
	}

	int Receiver::stop()
	{
		fStop_ = true;

		if(WSA_INVALID_EVENT!=eventRead_)
			WSASetEvent(eventRead_);

		if(INVALID_HANDLE_VALUE != thread_)
		{
			WaitForSingleObject( thread_, INFINITE );
			CloseHandle(thread_);

			thread_ = INVALID_HANDLE_VALUE;
		}

		return 0;
	}

	int Receiver::recvFromWSA(char* buf, int len)
	{
		DWORD dwWaitResult = WSAWaitForMultipleEvents(1, &eventRead_, FALSE, INFINITE, FALSE);

		int wsaerr=WSAGetLastError();
		if(wsaerr==WSAEINTR || wsaerr==WSAENOTSOCK)
		{
			_ASSERTE(!"WSAWaitForMultipleEvents failed");
			return -1;
		}

		switch(dwWaitResult)
		{
		case WSA_WAIT_TIMEOUT:
			break;

		case WSA_WAIT_EVENT_0:
			{
				if(fStop_) break;

				WSANETWORKEVENTS NetworkEvents={0};
				if(0 == WSAEnumNetworkEvents(sock_,eventRead_,&NetworkEvents))
				{
					switch(NetworkEvents.lNetworkEvents)
					{
					case FD_READ:
						{
							int from_len = sizeof(from_addr_);
							//WSARecvMsg() ???
							int result = recvfrom(sock_, buf, len, 0, reinterpret_cast<sockaddr*>(&from_addr_), &from_len);

							if(SOCKET_ERROR == result)
								consoleio::print_line(resources::wszErrRecvFrom, WSAGetLastError());

							return result;
						}

					case FD_ADDRESS_LIST_CHANGE:
						fAddressListChange_ = true;
						consoleio::console_generate_event();
						break;
					}
				}
			}
			break;
		}

		return -1;
	}

	unsigned int __stdcall Receiver::recvProc(void* pParam)
	{
		Receiver* pr = reinterpret_cast<Receiver*>(pParam);

		char chBuffer[3001]={0};//Packet have to always have a terminating null symbol, for normal work of parse_message_fields(STRING_FIELD)
		int recv_len = 0;

		WSAEventSelect(pr->sock_, pr->eventRead_, FD_READ|FD_ADDRESS_LIST_CHANGE);

		{//to remove cbBytesReturned from stack
			DWORD cbBytesReturned = 0;
			//to continue receive FD_ADDRESS_LIST_CHANGE events
			WSAIoctl(pr->sock_,SIO_ADDRESS_LIST_CHANGE,0,0,0,0,&cbBytesReturned,0,0);
		}

		while(!pr->fStop_)
		{
			if( -1 == (recv_len = pr->recvFromWSA(chBuffer, sizeof(chBuffer)-1)))
			{
				break;
			}
			else
			{
				pr->processPacket(chBuffer, recv_len);
			}

			memset(chBuffer, 0x00, sizeof(chBuffer));
			recv_len = 0;
		}

		return 0;
	}
#else
	int Receiver::start()
	{
		if(INVALID_SOCKET == sock_) return -1;
		if(0 != thread_) return 0;
		fStop_ = false;
		//recvProc(this);
		int errcode = pthread_create(&thread_, NULL, recvProc, this);
		DBG_UNREFERENCED_LOCAL_VARIABLE(errcode);
		assert(0==errcode);
		return 0;
	}

	int Receiver::stop()
	{
		fStop_ = true;

		//send data to socket to unblock recvfrom call
		char null_data[1]={0};
		sockaddr_in6 saddr = {0};
		/*
		socklen_t saddr_len = sizeof(saddr);
		//In Mac OS X getsockname() returns 0::0 address for the Ipv6 so it is impossible to use it here
		if(0 == getsockname(sock_, reinterpret_cast<sockaddr*>(&saddr), &saddr_len))
		{
			int result = sendto(sock_, null_data, sizeof(null_data), 0, reinterpret_cast<sockaddr*>(&saddr), saddr_len);

			assert(sizeof(null_data) == result);
		}
		else
			assert(!"getsockname() failed!");
		*/
		
		if(psender_)
		{
			memcpy(&saddr, pif_->pai_->ai_addr, __min(pif_->pai_->ai_addrlen, sizeof(saddr)));
			NETADDR_INFO::set_port(reinterpret_cast<sockaddr*>(&saddr), port_);
			psender_->sendTo(reinterpret_cast<sockaddr*>(&saddr), null_data, sizeof(null_data));
		}

		if(thread_)
		{
			int *status = 0; // holds return code
			pthread_join(thread_, reinterpret_cast<void**>(&status));
			thread_ = 0;
			assert(0==status);
		}

		return 0;
	}

	int Receiver::recvFrom(char* buf, int len)
	{
		socklen_t from_len = sizeof(from_addr_);
		return recvfrom(sock_, buf, len, 0, reinterpret_cast<sockaddr*>(&from_addr_), &from_len);
	}

	void* Receiver::recvProc(void* pParam)
	{
		Receiver* pr = reinterpret_cast<Receiver*>(pParam);

		char chBuffer[3001]={0};//Packet have to always have a terminating null symbol, for normal work of parse_message_fields(STRING_FIELD)
		int recv_len = 0;

		while(!pr->fStop_)
		{
			if( -1 == (recv_len = pr->recvFrom(chBuffer, sizeof(chBuffer)-1)))
			{
				if(EWOULDBLOCK == errno) continue;//recv timeout elapsed
				consoleio::print_line(resources::wszErrRecvFrom, errno);
				break;
			}
			else
			{
				pr->processPacket(chBuffer, recv_len);
			}

			memset(chBuffer, 0x00, sizeof(chBuffer));
			recv_len = 0;
		}

		return 0;
	}
#endif // CHATTERM_OS_WINDOWS
	void Receiver::removeOldPackets(const PKT_DATA& pkt)
	{
		std::deque<PKT_DATA>::const_iterator end = queOfPackets_.end();
		std::deque<PKT_DATA>::const_iterator it = queOfPackets_.begin();

		while((it!=end) && (nPacketsQueTimeInterval_ < difftime(pkt.ct, it->ct)))
			++it;

		queOfPackets_.erase(queOfPackets_.begin(), it);
	}

	int Receiver::checkDuplAndFlooding(const PKT_DATA& pkt)
	{
		unsigned int from_count = 0;

		std::deque<PKT_DATA>::iterator begin = queOfPackets_.begin();
		std::deque<PKT_DATA>::iterator it = queOfPackets_.end();
		while(it!=begin)
		{
			if(*--it==pkt)
			{
				//it brakes the time sequence of packets
				//(*rit).ct = pkt.ct;//refresh a packet

				//remove an old packet
				queOfPackets_.erase(it);
				//add new pkt to the end of queue
				queOfPackets_.push_back(pkt);

				_ASSERTE(!"Duplicated UDP packet was discarded");
				if(debug_)
					consoleio::print_line( resources::wszDbgPacketDuplicated);
				return 1;
			}

			if(nFloodRate_>0)
			{
				if(pkt.is_from(*it)) ++from_count;

				if(from_count>nFloodRate_)
				{
					_ASSERTE(!"Flooding detected");

					ContainersMonitor CONTAINERS_MONITOR;

					USER_INFO::ConstIteratorOfUsers it_user = USER_INFO::findUsersByReceiver(USER_INFO::SetOfUsers_.begin(), this);

					while(it_user != USER_INFO::SetOfUsers_.end())
					{
						std::shared_ptr<USER_INFO> const& refPtrUserInfo = *it_user;

						//if it is my packet then flood is impossible
						if(refPtrUserInfo && refPtrUserInfo->flood<1)
						{
							theApp.Commands_.FloodZ(refPtrUserInfo.get(), nFloodProtectionTimeInterval_);
						}

						it_user = USER_INFO::findUsersByReceiver(++it_user, this);
					}

					return 2;
				}
			}
		}

		return 0;
	}

	void Receiver::processPacket(char* pPacket, int len)
	{
		if(debug_)
		{
			//to keep always together print_line and print_bytes
			consoleio::PrintLinesMonitor PRINT_LINES_MONITOR;

			consoleio::print_line_selected(resources::wszDbgPacket, len, theApp.getStrTime(true));

			std::wstring wszFromAddr = sockaddr_to_string(reinterpret_cast<const sockaddr*>(&from_addr_), sizeof(sockaddr_in6));
			std::wstring wszToAddr = sockaddr_to_string(ptrIf_->pai_->ai_addr, ptrIf_->pai_->ai_addrlen);

			if(ptrIf_->pai_->ai_family == AF_INET6)
				consoleio::print_line_selected(resources::wszDbgPacketRcvdFromTo6, wszFromAddr.c_str(), wszToAddr.c_str(), ntohs(port_));
			else
				consoleio::print_line_selected(resources::wszDbgPacketRcvdFromTo, wszFromAddr.c_str(), wszToAddr.c_str(), ntohs(port_));

			consoleio::print_bytes(reinterpret_cast<unsigned char*>(pPacket), len);
		}

		if(len<13 || *pPacket!='X') return;

		//check packet signature
		int k = 0;
		for(k; k<11 && k<len && isalnum(pPacket[k]); k++);
		if(k<11) return;

#ifdef _DEBUG
		bool br = theApp.ptrMe_->naddr_info.preceiver_ == this;
		bool becho = isEchoedMessage();
		if(!br && becho)
#else
		if((theApp.ptrMe_->naddr_info.preceiver_ != this) && isEchoedMessage())
#endif
			return;

		if(nFloodRate_>0)
		{
			PKT_DATA pkt((const unsigned char*)pPacket+1, reinterpret_cast<const sockaddr*>(&from_addr_));

			removeOldPackets(pkt);

			if(0 != checkDuplAndFlooding(pkt))
			{
				return;
			}

			//Just to keep the queue size limited
			const unsigned int nPacketsQueSize = nFloodRate_*4;//maximum size of a queue of UDP packets

			if(queOfPackets_.size() > nPacketsQueSize-1)
				queOfPackets_.pop_front();

			queOfPackets_.push_back(pkt);
		}

		char PacketId[10]={0};
		memcpy(PacketId, pPacket+1, 9);

		ContainersMonitor CONTAINERS_MONITOR;

		static ProcessorMsgX ProcessorX;
		ProcessorX.process(pPacket+10, len-10, this, PacketId);
	}

	bool Receiver::isFromSender(Sender* const& ps) const
	{
		if(from_addr_.sin6_family != ps->pif_->pai_->ai_family) return false;
		if(from_addr_.sin6_port != ps->port_) return false;

		if(AF_INET6 == ps->pif_->pai_->ai_family)
		{
			if(sizeof(sockaddr_in6) > ps->pif_->pai_->ai_addrlen) return false;

			const sockaddr_in6* paddr = reinterpret_cast<const sockaddr_in6*>(ps->pif_->pai_->ai_addr);

			if(from_addr_.sin6_flowinfo != paddr->sin6_flowinfo) return false;
			if(from_addr_.sin6_scope_id != paddr->sin6_scope_id) return false;
			return 0==memcmp(&paddr->sin6_addr, &from_addr_.sin6_addr, sizeof(in6_addr));
		}

		if(AF_INET == ps->pif_->pai_->ai_family)
		{
			if(sizeof(sockaddr_in) > ps->pif_->pai_->ai_addrlen) return false;

			const sockaddr_in* paddr = reinterpret_cast<const sockaddr_in*>(ps->pif_->pai_->ai_addr);
			const sockaddr_in* paddr_from = reinterpret_cast<const sockaddr_in*>(ps->pif_->pai_->ai_addr);

			return 0==memcmp(&paddr->sin_addr, &paddr_from->sin_addr, sizeof(in_addr));
		}

		return false;
	}

	//test if a message send by myself
	bool Receiver::isEchoedMessage() const
	{
		std::vector< std::shared_ptr<networkio::Sender> >::const_iterator it_senders = theApp.Senders_.begin();
		std::vector< std::shared_ptr<networkio::Sender> >::const_iterator end_senders = theApp.Senders_.end();

		while( (it_senders!=end_senders) && !isFromSender((*it_senders).get()) )
			++it_senders;

		return (it_senders != end_senders);
	}

	int DESTADDR_INFO::bindToAddress(const wchar_t* wszAddress, unsigned short port)
	{
		if( 0 == psender_) return -1;

		size_t buflen = 0;
		std::unique_ptr<char[]> ptrAddress;
#ifdef CHATTERM_OS_WINDOWS
		if(0 == wcstombs_s(&buflen, NULL, 0, wszAddress, 0))
		{
			ptrAddress = std::make_unique<char[]>(buflen);
			wcstombs_s(&buflen, ptrAddress.get(), buflen, wszAddress, _TRUNCATE);
		}
#else
		buflen = NixHlpr.assignCharSz(&address, wszAddress);
#endif // CHATTERM_OS_WINDOWS

		struct addrinfo hints = {0};
		hints.ai_flags = AI_NUMERICHOST;
		hints.ai_family = psender_->getIfFamily();
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;

		struct addrinfo* pres_addr = 0;

		char szPort[16]={0};
#ifdef CHATTERM_OS_WINDOWS
		_itoa_s(port, szPort, _ARRAYSIZE(szPort), 10);
#else
		sprintf(szPort, "%u", port);
#endif // CHATTERM_OS_WINDOWS
		int result = getaddrinfo(ptrAddress.get(), szPort, &hints, &pres_addr);

		if(0 == result)
		{
			memcpy(get_saddr(), pres_addr->ai_addr, __min(pres_addr->ai_addrlen, sizeof(sockaddr_in6)));

			freeaddrinfo(pres_addr);
		}

		return result;
	}
}
