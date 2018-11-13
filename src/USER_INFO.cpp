/**
$Id: USER_INFO.cpp 35 2010-09-28 11:34:51Z avyatkin $

Implementation of USER_INFO and PERSONAL_INFO classes

Copyright (c) 2010 VyPRESS Research, LLC. All rights reserved.
For conditions of distribution and use, see copyright notice in ChatTerminal.h
*/

#include <algorithm>

#include "ChatTerminal.h"
#include "USER_INFO.h"
#include "StrResources.h"

#ifdef CHATTERM_OS_WINDOWS
#include <lm.h>
#else
//#include <unistd.h> // for char *getlogin(void);
#include <pwd.h> //for getpwuid(geteuid());
#include <sys/utsname.h> //for uname
#endif // CHATTERM_OS_WINDOWS

using namespace resources;

const wchar_t* const USER_INFO::NullNick_ = L"_NULL_";

/*0 = Black 8 = Gray
1 = Blue 9 = Light Blue
2 = Green A = Light Green
3 = Aqua B = Light Aqua
4 = Red C = Light Red
5 = Purple D = Light Purple
6 = Yellow E = Light Yellow
7 = White F = Bright White*/
const wchar_t* const USER_INFO::colors_[16]={L"black", L"blue",L"green",L"aqua"
						,L"red",L"purple",L"yellow",L"white"
						,L"gray",L"light blue",L"light green",L"light aqua"
						,L"light red",L"light purple",L"light yellow",L"bright white"};

//It must be accessed in ContainersMonitor CONTAINERS_MONITOR Critical Section
std::set< std::shared_ptr<USER_INFO>, USER_INFO::Less > USER_INFO::SetOfUsers_;

USER_INFO::ConstIteratorOfUsers USER_INFO::findUsersByReceiver(ConstIteratorOfUsers it, const networkio::Receiver* pcrcvr)
{
	ConstIteratorOfUsers end = SetOfUsers_.end();
	while(it != end)
	{
		if(theApp.ptrMe_ = *it)
		{
			if(pcrcvr->isEchoedMessage())
				return it;
		}
		else
			if(*it && networkio::NETADDR_INFO::compare_with_receiver((*it)->naddr_info, pcrcvr))
				return it;

		++it;
	}

	return SetOfUsers_.end();
}

bool USER_INFO::isUserInList(const wchar_t* name, std::shared_ptr<USER_INFO>& refPtrUserInfo)
{
	ConstIteratorOfUsers it = std::find_if(SetOfUsers_.begin(), SetOfUsers_.end(), NameComparator(name));

	bool result = ((it != SetOfUsers_.end()) && (*it));
	if(result)
	{
		if(*it!=theApp.ptrMe_ && ((*it)->flood>0))
		{
			time_t curt = 0;
			time(&curt);
			if( (*it)->flood > difftime( curt, (*it)->last_activity))
			{
				result = false;//user is blocked
			}
			else
			{
				(*it)->flood = 0;
				(*it)->last_activity = curt;
			}
		}
		else
			time(&(*it)->last_activity);

		refPtrUserInfo = *it;
	}

	return result;
}

USER_INFO::ConstIteratorOfUsers USER_INFO::removeUserFromList(ConstIteratorOfUsers it_u)
{
	if(it_u == SetOfUsers_.end())
		return SetOfUsers_.end();

	if(0 == *it_u)
	{
#ifdef CHATTERM_OS_WINDOWS
		return SetOfUsers_.erase(it_u);
#else
		size_t n = std::distance(SetOfUsers_.begin(), it_u);
		SetOfUsers_.erase(it_u);
		it_u = SetOfUsers_.begin();
		ConstIteratorOfUsers end = SetOfUsers_.end();
		while(n-- && ++it_u!=end);
		return it_u;
#endif // CHATTERM_OS_WINDOWS
	}

	CHANNEL_INFO::ConstIteratorOfChannels it_ch = CHANNEL_INFO::SetOfChannels_.begin();
	//CHANNEL_INFO::ConstIteratorOfChannels it_ch_end = CHANNEL_INFO::SetOfChannels_.end();

	while(it_ch != CHANNEL_INFO::SetOfChannels_.end())
	{
		if(*it_ch)
		{
			CHANNEL_INFO::IteratorOfChannelUsers it = std::find_if((*it_ch)->users.begin(), (*it_ch)->users.end(), USER_INFO::Comparator((*it_u).get()));
			if(it != (*it_ch)->users.end())
				(*it_ch)->users.erase(it);
		}

		if((*it_ch)->users.size()<1)//remove channel
		{
#ifdef CHATTERM_OS_WINDOWS
			it_ch = CHANNEL_INFO::SetOfChannels_.erase(it_ch);
#else
			size_t n = std::distance(CHANNEL_INFO::SetOfChannels_.begin(), it_ch);
			CHANNEL_INFO::SetOfChannels_.erase(it_ch);

			it_ch = CHANNEL_INFO::SetOfChannels_.begin();
			CHANNEL_INFO::ConstIteratorOfChannels end = CHANNEL_INFO::SetOfChannels_.end();
			while(n-- && ++it_ch!=end);
#endif // CHATTERM_OS_WINDOWS
		}
		else
			++it_ch;
	}

#ifdef CHATTERM_OS_WINDOWS
	return SetOfUsers_.erase(it_u);
#else
	size_t n = std::distance(SetOfUsers_.begin(), it_u);
	SetOfUsers_.erase(it_u);
	it_u = SetOfUsers_.begin();
	ConstIteratorOfUsers end = SetOfUsers_.end();
	while(n-- && ++it_u!=end);
	return it_u;
#endif // CHATTERM_OS_WINDOWS
}

void USER_INFO::removeUserFromList(const wchar_t* name)
{
	ConstIteratorOfUsers it_u = std::find_if(SetOfUsers_.begin(), SetOfUsers_.end(), NameComparator(name));
	//ConstIteratorOfUsers it_u = SetOfUsers_.find(name);

	removeUserFromList(it_u);
}

#ifdef CHATTERM_OS_WINDOWS
bool PERSONAL_INFO::getOS(std::wstring& strOS)
{
	//Not cached yet.
	OSVERSIONINFOEX osvi  = {0};
	BOOL bOsVersionInfoEx = FALSE;

	// Try calling GetVersionEx using the OSVERSIONINFOEX structure.
	// If that fails, try using the OSVERSIONINFO structure.

	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);

#pragma warning(push)
#pragma warning ( disable: 4996 )

	if(FALSE == (bOsVersionInfoEx = GetVersionEx ((OSVERSIONINFO *) &osvi)))
	{
		// If OSVERSIONINFOEX doesn't work, try OSVERSIONINFO.
		osvi.dwOSVersionInfoSize = sizeof (OSVERSIONINFO);
		if (! GetVersionEx ( (OSVERSIONINFO *) &osvi) )
			return false;
	}

#pragma warning( pop )

	switch (osvi.dwPlatformId)
	{
	case VER_PLATFORM_WIN32_NT:

		// Test for the product.
		if ( osvi.dwMajorVersion <= 4 )
			strOS = L"Microsoft Windows NT ";
		else
		if ( osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 0 )
			strOS = L"Microsoft Windows 2000 ";
		else
		if ( osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 1 )
			strOS = L"Microsoft Windows XP ";
		else
		if ( osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 2 )
			strOS = L"Microsoft Windows 2003 ";
		else
		if ( osvi.dwMajorVersion == 6 )
			strOS = L"Microsoft Windows ";//7, Vista or 2008
		else
			strOS = L"Microsoft Windows ";

		// Test for product type.

		if( bOsVersionInfoEx )
		{
			if ( osvi.wProductType == VER_NT_WORKSTATION )
			{
				if( osvi.wSuiteMask & VER_SUITE_PERSONAL )
				{
					if ( osvi.dwMajorVersion == 6 && osvi.dwMinorVersion > 0)
					{
						if(osvi.dwMinorVersion < 1)
							strOS.append(L"Vista Home ");//Vista
						else
							strOS.append(L"7 Home ");//Windows 7
					}
					else
						strOS.append(L"Home ");
				}
				else
					if ( osvi.dwMajorVersion == 6 )
					{
						if(osvi.dwMinorVersion < 1)
							strOS.append(L"Vista Professional ");//Vista
						else
							strOS.append(L"7 Professional ");//Windows 7
					}
					else
						strOS.append(L"Professional ");
			}
			else
				if ( osvi.wProductType == VER_NT_SERVER || osvi.wProductType == VER_NT_DOMAIN_CONTROLLER)
				{
					if( osvi.wSuiteMask & VER_SUITE_DATACENTER )
						strOS.append(L"DataCenter Server ");
					else
						if( osvi.wSuiteMask & VER_SUITE_ENTERPRISE )
							strOS.append(L"Advanced Server ");
						else
							strOS.append(L"Server ");

					if ( osvi.dwMajorVersion == 6 )
					{
						strOS.append(L"2008 ");
						if(osvi.dwMinorVersion > 0)
							strOS.append(L"R2 ");
					}
				}
		}
		else
		{
			HKEY hKey = 0;
			TCHAR szProductType[80]={0};
			DWORD dwBufLen = sizeof(szProductType);

			RegOpenKeyEx( HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\ProductOptions", 0, KEY_QUERY_VALUE, &hKey );
			RegQueryValueEx( hKey, L"ProductType", NULL, NULL, (LPBYTE) szProductType, &dwBufLen);
			RegCloseKey( hKey );

			if ( lstrcmpi( L"WINNT", szProductType) == 0 )
				strOS.append(L"Professional ");

			if ( lstrcmpi( L"LANMANNT", szProductType) == 0 )
				strOS.append(L"Server ");

			if ( lstrcmpi( L"SERVERNT", szProductType) == 0 )
				strOS.append(L"Advanced Server ");
		}

		// Display version, service pack (if any)
		strOS.append(osvi.szCSDVersion);
		break;

	case VER_PLATFORM_WIN32_WINDOWS:

		if (osvi.dwMajorVersion == 4 && osvi.dwMinorVersion == 0)
		{
			strOS.append(L"Microsoft Windows 95 ");
			if ( osvi.szCSDVersion[1] == L'C' || osvi.szCSDVersion[1] == L'B' )
				strOS.append(L"OSR2 ");
		} 

		if (osvi.dwMajorVersion == 4 && osvi.dwMinorVersion == 10)
		{
			strOS.append(L"Microsoft Windows 98 ");
			if ( osvi.szCSDVersion[1] == L'A' )
				strOS.append(L"SE ");
		} 

		if (osvi.dwMajorVersion == 4 && osvi.dwMinorVersion == 90)
		{
			strOS.append(L"Microsoft Windows Me ");
		} 
		break;

	case VER_PLATFORM_WIN32s:
		strOS.append(L"Microsoft Win32s ");
		break;
	}

	return true;
}

bool PERSONAL_INFO::getDomainName(std::wstring& strDomain)
{
	bool result = true;
	wchar_t wszUser[UNLEN + 1] = {0};
	wchar_t wszDomain[DNLEN + 1] = {0};
	DWORD dwUNLen = _ARRAYSIZE(wszUser);
	DWORD dwDNLen = _ARRAYSIZE(wszDomain);

	HANDLE hToken   = NULL;
	PTOKEN_USER ptiUser  = NULL;
	SID_NAME_USE snu = SidTypeUnknown;

	try
	{
		// Get the calling thread's access token.
		if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &hToken))
		{
			if (GetLastError() != ERROR_NO_TOKEN)
				throw 1;

			// Retry against process token if no thread token exists.
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
				throw 2;
		}

		DWORD cbti = 0;

		// Obtain the size of the user information in the token.
		if (GetTokenInformation(hToken, TokenUser, NULL, 0, &cbti)) 
		{
			// Call should have failed due to zero-length buffer.
			throw 3;
		}
		else
		{
			// Call should have failed due to zero-length buffer.
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
				throw 4;
		}

		// Allocate buffer for user information in the token.
		ptiUser = (PTOKEN_USER) HeapAlloc(GetProcessHeap(), 0, cbti);
		if (!ptiUser)
			throw 5;

		// Retrieve the user information from the token.
		if (!GetTokenInformation(hToken, TokenUser, ptiUser, cbti, &cbti))
			throw 6;

		// Retrieve user name and domain name based on user's SID.
		if (!LookupAccountSid(NULL, ptiUser->User.Sid, wszUser, &dwUNLen, wszDomain, &dwDNLen, &snu))
			throw 7;
	}
	catch(...)
	{
		result = false;
	}

	// Free resources.
	if (hToken)
		CloseHandle(hToken);

	if (ptiUser)
		HeapFree(GetProcessHeap(), 0, ptiUser);

	strDomain = wszDomain;
	return result;
}

void PERSONAL_INFO::initialize()
{
	wchar_t wszBufferUN[UNLEN + 1]={0};
	DWORD dwSize = _ARRAYSIZE(wszBufferUN);
	if(GetUserName(wszBufferUN, &dwSize))
		user_name = wszBufferUN;

	wchar_t wszBufferCN[MAX_COMPUTERNAME_LENGTH + 1]={0};
	dwSize = _ARRAYSIZE(wszBufferCN);

	if(GetComputerName(wszBufferCN, &dwSize))
		computer_name = wszBufferCN;

	getDomainName(domain_name);
	getOS(os);

	chat_software = wszVersionInfo;
}


bool USER_INFO::loadFromXml(const wchar_t* file_path)
{
	/*
	<chatterminal>
	  <user_info>
	  <nick>Vyatkin</nick>
	  <uuid></uuid>
	  <gender>man</gender>
	  <color>green</color>
	  <icon>2</icon>
	  <personal_info>
		<full_name>Full Name</full_name>
		<job>Job</job>
		<department>Department</department>
		<phone_work>Work Phone</phone_work>
		<phone_mob>Mobile Phone</phone_mob>
		<www>Web address</www>
		<email>E-Mail address</email>
		<address>Post Address</address>
	  </personal_info>
	  </user_info>
	</chatterminal>
	*/
	bool result = false;

	IXMLDOMDocument* pXMLDoc = 0;
	HRESULT hr = xmlhelper::create_DOM_instance(&pXMLDoc);
	if(S_OK != hr)
	{
		if(pXMLDoc) pXMLDoc->Release();
		return false;
	}

	VARIANT xmlSource ={0};
	VariantInit(&xmlSource);
	xmlSource.vt = VT_BSTR;
	xmlSource.bstrVal = SysAllocString(file_path);

	VARIANT_BOOL bIssuccessful = {0};
	hr = pXMLDoc->load(xmlSource, &bIssuccessful);
	VariantClear(&xmlSource);

	if(S_OK!=hr || bIssuccessful!=VARIANT_TRUE)
	{
		consoleio::print_line(wszErrLoadXmlFile, file_path);

		IXMLDOMParseError *pIParseError = NULL;
		hr = pXMLDoc->get_parseError(&pIParseError);

		if(pIParseError)
		{
			BSTR reasonString = 0;
			hr = pIParseError->get_reason(&reasonString);
			if(SUCCEEDED(hr))
			{
				consoleio::print_line(reasonString);
				SysFreeString(reasonString);
			}

			pIParseError->Release();
		}

		if(pXMLDoc) pXMLDoc->Release();
		return false;
	}

	IXMLDOMDocument2* pXMLDoc2 = 0;
	if(S_OK == pXMLDoc->QueryInterface(IID_IXMLDOMDocument2, (void**)&pXMLDoc2))
	{
		VARIANT value = {0};
		VariantInit(&value);
		value.vt = VT_BSTR;
		value.bstrVal = SysAllocString(L"XPath");
		BSTR bstrProp = SysAllocString(L"SelectionLanguage");
		hr = pXMLDoc2->setProperty(bstrProp, value);

		SysReAllocString(&bstrProp, L"SelectionNamespaces");
		SysReAllocString(&value.bstrVal, L"xmlns:n='http://www.vypress.com/chatterm10'");

		hr = pXMLDoc2->setProperty (bstrProp, value);

		SysFreeString(bstrProp);
		VariantClear(&value);

		/*
		IXMLDOMSchemaCollection * namespaceCollection = 0;
		hr = pXMLDoc2->get_namespaces (&namespaceCollection);

		BSTR length1 = 0;
		hr = namespaceCollection->get_namespaceURI(0, &length1);
		BSTR length2 = 0;
		hr = namespaceCollection->get_namespaceURI(1, &length2);
		BSTR length3 = 0;
		hr = namespaceCollection->get_namespaceURI(2, &length3);

		BSTR namespaceURI = 0;
		hr = pXMLDoc2->get_namespaceURI(&namespaceURI);
		*/

		pXMLDoc2->Release();
	}

	IXMLDOMNode* pUserInfoNode = 0;
	BSTR bstrUserInfo = SysAllocString(L"/n:chatterminal/n:user_info");
	hr = pXMLDoc->selectSingleNode(bstrUserInfo, &pUserInfoNode);
	if(S_OK == hr)
	{
		IXMLDOMNodeList *childsList = 0;
		hr = pUserInfoNode->get_childNodes(&childsList);
		long index = 0;

		long listLength = 0;
		if(S_OK == hr)
			hr = childsList->get_length(&listLength);

		if(S_OK == hr && listLength >= 6)
		{
			BSTR bstrText = 0;
			hr = xmlhelper::get_xml_item_text(childsList, index, L"nick", &bstrText);
			if(S_OK==hr)
			{
				setNick(bstrText, SysStringLen(bstrText));
				SysFreeString(bstrText);
			}

			hr = xmlhelper::get_xml_item_text(childsList, index, L"uuid", &bstrText);
			if(S_OK==hr)
			{
				RPC_WSTR rpcwstrUuid = reinterpret_cast<RPC_WSTR>(bstrText);
				if(RPC_S_OK != UuidFromString(rpcwstrUuid, &uuid))
					memset(&uuid, 0x00, sizeof(uuid));

				SysFreeString(bstrText);
			}

			hr = xmlhelper::get_xml_item_text(childsList, index, L"gender", &bstrText);
			if(S_OK==hr)
			{
				if(0 == _wcsicmp(bstrText, L"female") || 0 == _wcsicmp(bstrText, L"woman"))
					gender = '1';
				SysFreeString(bstrText);
			}

			hr = xmlhelper::get_xml_item_text(childsList, index, L"color", &bstrText);
			if(S_OK==hr)
			{
				int num_color = _wtoi(bstrText);
				if (num_color == 0)//not numeric string
				{
					BYTE i = 0;
					for( i ; i<_ARRAYSIZE(colors_); i++)
					{
						if(0 == _wcsicmp(colors_[i], bstrText))
						{
							color = i;
							break;
						}
					}

					if(i >= _ARRAYSIZE(colors_) && *bstrText == L'0')
					{
						color = 0;
					}
				}
				else
					color = static_cast<BYTE>(num_color);

				SysFreeString(bstrText);
			}

			hr = xmlhelper::get_xml_item_text(childsList, index, L"icon", &bstrText);
			if(S_OK==hr)
			{
				icon = static_cast<unsigned char>(_wtoi(bstrText));
				SysFreeString(bstrText);
			}
		}
		else
		{
			_ASSERTE(!"Wrong /chatterminal/user_info childs count!");
		}

		IXMLDOMNode* pPersonalInfoNode = 0;
		hr = xmlhelper::get_xml_item(childsList, index, L"personal_info", &pPersonalInfoNode);
		if(S_OK == hr)
		{
			IXMLDOMNodeList *childsPersonalInfoList = 0;
			hr = pPersonalInfoNode->get_childNodes(&childsPersonalInfoList);

			long listPersonalInfoLength = 0;
			if(S_OK == hr)
				hr = childsPersonalInfoList->get_length(&listPersonalInfoLength);

			if(S_OK == hr && listPersonalInfoLength >= 8)
			{
				long childs_index = 0;
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"full_name", theApp.MyPersonalInfo_.full_name);
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"job", theApp.MyPersonalInfo_.job);
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"department", theApp.MyPersonalInfo_.department);
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"phone_work", theApp.MyPersonalInfo_.phone_work);
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"phone_mob", theApp.MyPersonalInfo_.phone_mob);
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"www", theApp.MyPersonalInfo_.www);
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"email", theApp.MyPersonalInfo_.email);
				xmlhelper::get_xml_item_text(childsPersonalInfoList, childs_index, L"address", theApp.MyPersonalInfo_.address);

				result = true;
			}
			else
			{
				_ASSERTE(!"Wrong /chatterminal/user_info/personal_info childs count!");
			}

			if(childsPersonalInfoList) childsPersonalInfoList->Release();
		}

		if(pPersonalInfoNode) pPersonalInfoNode->Release();

		if(childsList) childsList->Release();
	}

	if(pUserInfoNode) pUserInfoNode->Release();
	if(bstrUserInfo) SysFreeString(bstrUserInfo);

	if(pXMLDoc) pXMLDoc->Release();

	return result;
}
#else
void PERSONAL_INFO::initialize()
{
	//L_cuserid
	//char* szUserName = cuserid(NULL);
	struct passwd* ppwd = getpwuid(geteuid());
	char* szUserName = ppwd->pw_name;
	
	NixHlpr.assignWstr(user_name, szUserName);

	struct utsname info = {0};

	if(0 == uname(&info))
	{
		NixHlpr.assignWstr(computer_name, info.nodename);
#ifdef _GNU_SOURCE
		NixHlpr.assignWstr(domain_name, info.domainname);
#endif
		char szOS[sizeof(info.release)+sizeof(info.sysname)] = {0};
		size_t sysname_len = strlen(info.sysname);
		strcpy(szOS, info.sysname);
		szOS[sysname_len] = ' ';
		strcpy(szOS+sysname_len+1, info.release);
		NixHlpr.assignWstr(os, szOS);
	}

	chat_software = wszVersionInfo;
}

bool USER_INFO::loadFromXml(const wchar_t* file_path)
{
	/*
	<chatterminal>
	  <user_info>
	  <nick>Vyatkin</nick>
	  <uuid></uuid>
	  <gender>man</gender>
	  <color>green</color>
	  <icon>2</icon>
	  <personal_info>
		<full_name>Full Name</full_name>
		<job>Job</job>
		<department>Department</department>
		<phone_work>Work Phone</phone_work>
		<phone_mob>Mobile Phone</phone_mob>
		<www>Web address</www>
		<email>E-Mail address</email>
		<address>Post Address</address>
	  </personal_info>
	  </user_info>
	</chatterminal>
	*/

	xmlDocPtr doc = 0;
	if(!xmlhelper::create_Document_instance(&doc, file_path))
	{
		consoleio::print_line(wszErrLoadXmlFile, file_path);
		return false;
	}

	if (doc == NULL)
	{
		consoleio::print_line(wszErrLoadXmlFile, file_path);
		return false;
	}

	/* Init libxml */
	//xmlInitParser();

	/* Create xpath evaluation context */
	xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
	if(xpathCtx == NULL)
	{
		xmlFreeDoc(doc); 
		return false;
	}
	
	/* Register namespaces from list (if any) */
	/* do register namespace */
	if(xmlXPathRegisterNs(xpathCtx, BAD_CAST "n", BAD_CAST "http://www.vypress.com/chatterm10") != 0)
	{
		xmlXPathFreeContext(xpathCtx);
		xmlFreeDoc(doc);
		return false;
	}
	
	/* Evaluate xpath expression */
	xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(BAD_CAST "/n:chatterminal/n:user_info", xpathCtx);
	if(xpathObj == NULL)
	{
		xmlXPathFreeContext(xpathCtx); 
		xmlFreeDoc(doc); 
		return false;
	}

	/* process results */
	if(xpathObj->nodesetval)
	{
		xmlNodePtr user_info_nodePtr = xpathObj->nodesetval->nodeTab[0];

		if(user_info_nodePtr && user_info_nodePtr->type == XML_ELEMENT_NODE)
		{
			xmlNodePtr childrenPtr = user_info_nodePtr->children;

			wchar_t* wszText = 0;
			int buflen = xmlhelper::get_xml_item_text(childrenPtr, BAD_CAST"nick", &wszText);
			if(buflen>0)
				setNick(wszText, buflen-1);

			delete[] wszText;
			wszText = 0;

			xmlChar* xmlText = xmlhelper::get_xml_item_text(childrenPtr, BAD_CAST"uuid");
			if(xmlText)
			{
				//RPC_WSTR rpcwstrUuid = reinterpret_cast<RPC_WSTR>(bstrText);
				//if(RPC_S_OK != UuidFromString(rpcwstrUuid, &uuid))
				//	memset(&uuid, 0x00, sizeof(uuid));
				if(0 != uuid_parse((const char*) xmlText, uuid))
					uuid_clear(uuid);

				xmlFree(xmlText);
			}

			xmlText = xmlhelper::get_xml_item_text(childrenPtr, BAD_CAST"gender");
			if(xmlText)
			{
				if(0 == xmlStrcasecmp(xmlText, BAD_CAST"female") || 0 == xmlStrcasecmp(xmlText, BAD_CAST"woman"))
				{
					gender = '1';
				}

				xmlFree(xmlText);
			}

			xmlText = xmlhelper::get_xml_item_text(childrenPtr, BAD_CAST"color");
			if(xmlText)
			{
				int num_color = atoi((const char*)xmlText);
				if (num_color == 0)//not numeric string
				{
					/*0 = Black 8 = Gray
					1 = Blue 9 = Light Blue
					2 = Green A = Light Green
					3 = Aqua B = Light Aqua
					4 = Red C = Light Red
					5 = Purple D = Light Purple
					6 = Yellow E = Light Yellow
					7 = White F = Bright White*/
					const xmlChar* colors[]={BAD_CAST"black", BAD_CAST"blue",BAD_CAST"green",BAD_CAST"aqua"
											,BAD_CAST"red",BAD_CAST"purple",BAD_CAST"yellow",BAD_CAST"white"
											,BAD_CAST"gray",BAD_CAST"light blue",BAD_CAST"light green",BAD_CAST"light aqua"
											,BAD_CAST"light red",BAD_CAST"light purple",BAD_CAST"light yellow",BAD_CAST"bright white"};

					unsigned char i = 0;
					for( i ; i<_ARRAYSIZE(colors); i++)
					{
						if(0 == xmlStrcasecmp(colors[i], xmlText))
						{
							color = i;
							break;
						}
					}

					if(i >= _ARRAYSIZE(colors) && *xmlText == '0')
					{
						color = 0;
					}
				}
				else
					color = static_cast<unsigned char>(num_color);

				xmlFree(xmlText);
			}

			xmlText = xmlhelper::get_xml_item_text(childrenPtr, BAD_CAST"icon");
			if(xmlText)
			{
				icon = static_cast<unsigned char>(atoi(reinterpret_cast<const char*>(xmlText)));
				xmlFree(xmlText);
			}

			//skip comments
			while(childrenPtr && childrenPtr->type == XML_COMMENT_NODE) childrenPtr = childrenPtr->next;
		
			if(childrenPtr && childrenPtr->type == XML_ELEMENT_NODE)
			{
				if(xmlStrEqual(BAD_CAST"personal_info", childrenPtr->name))
				{
					xmlNodePtr personalInfoNodePtr = childrenPtr->children;

					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"full_name", pinfo->full_name);
					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"job", pinfo->job);
					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"department", pinfo->department);
					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"phone_work", pinfo->phone_work);
					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"phone_mob", pinfo->phone_mob);
					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"www", pinfo->www);
					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"email", pinfo->email);
					xmlhelper::get_xml_item_text(personalInfoNodePtr, BAD_CAST"address", pinfo->address);
				}
			}
		}
	}
	
	/* Cleanup */
	xmlXPathFreeObject(xpathObj);
	xmlXPathFreeContext(xpathCtx); 
	xmlFreeDoc(doc); 

	/* Shutdown libxml */
	//xmlCleanupParser();
	
	/*
	* this is to debug memory for regression tests
	*/
	xmlMemoryDump();
	return true;
}
#endif // CHATTERM_OS_WINDOWS
