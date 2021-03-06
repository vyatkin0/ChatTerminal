/*
$Id: Commands.cpp 36 2011-08-09 07:35:21Z avyatkin $

Implementation of the Commands class

Copyright (c) 2010 VyPRESS Research, LLC. All rights reserved.
For conditions of distribution and use, see copyright notice in ChatTerminal.h
*/

#include "ChatTerminal.h"
#include "Commands.h"
#include "StrResources.h"

#ifdef CHATTERM_OS_WINDOWS
#ifdef _DEBUG
	#include <iostream>
#endif
#endif // CHATTERM_OS_WINDOWS

using namespace resources;

#define ACTIVE_CHANNEL_STATE '2'

#ifdef CHATTERM_OS_WINDOWS
HANDLE Commands::DatagramIdMonitor::m_ = {0};
HANDLE Commands::DelayedMsgsMonitor::m_ = {0};
#else
pthread_mutex_t Commands::DatagramIdMonitor::m_ = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Commands::DelayedMsgsMonitor::m_ = PTHREAD_MUTEX_INITIALIZER;
#endif // CHATTERM_OS_WINDOWS

bool Commands::debug_ = false;

//It is necessary to send commands with random packet signatures from this thread
// Seed the random-number generator with the current time so that
// the numbers will be different every time we run.
Commands::Commands() :
#ifndef CHATTERM_OS_WINDOWS
			fBe_(NixHlpr.isBigEndian()),
#endif
			datagramId_((srand(static_cast<unsigned int>(time(NULL))), rand()))
{
	if(datagramId_>999999999) datagramId_%=999999999;

	DatagramIdMonitor::Initialize();
	DelayedMsgsMonitor::Initialize();

#ifdef CHATTERM_OS_WINDOWS
#ifdef _DEBUG
	std::wcout << L"Generated datagramId_ is " << datagramId_ << std::endl;
#endif
#endif // CHATTERM_OS_WINDOWS
}

Commands::~Commands(void)
{
	DelayedMsgsMonitor::Delete();
	DatagramIdMonitor::Delete();
}

#ifdef CHATTERM_OS_WINDOWS
bool Commands::encryptData(unsigned char* data, size_t size, const CHANNEL_INFO* pcchinfo)
{
	DWORD dwLen = static_cast<DWORD>(size);
	return TRUE == CryptEncrypt(pcchinfo->hKey, NULL, TRUE, 0, data, &dwLen, dwLen);
}

bool Commands::getPasswordHash(const wchar_t* pass, size_t len, unsigned char* hash, unsigned long hash_len, HCRYPTKEY* phKey)
{
	HCRYPTHASH hHash = 0;
	bool bResult = false;

	try
	{
		// Acquire a hash object handle.
		if(!CryptCreateHash(theApp.hCryptProv_, CALG_MD5, 0, 0, &hHash))
		{
			throw -1;
		}

		if(!CryptHashData(hHash, (const unsigned char*)pass, static_cast<DWORD>(len*sizeof(wchar_t)), 0))
		{
			throw -1;
		}

		if(!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hash_len, NULL))
		{
			throw -1;
		}

		if(!CryptDeriveKey(theApp.hCryptProv_, CALG_RC4, hHash, 0x00280000/*40 bits key length*/, phKey))
		{
			throw -1;
		}

		bResult = true;
	}
	catch(int)
	{
	}

	if(hHash) CryptDestroyHash(hHash);

	return bResult;
}

int Commands::signMessage(const char* ptrMessage, size_t cMessageLen, unsigned char* pSignatute, size_t cSignatureLen)
{
	int nErr = 0;
	HCRYPTHASH hHash = 0;

	try
	{
		// Acquire a hash object handle.
		if(!CryptCreateHash(theApp.hCryptProv_, CALG_MD5, 0, 0, &hHash))
		{
			throw -1;
		}

		if(!CryptHashData(hHash, (const BYTE*)ptrMessage, static_cast<DWORD>(cMessageLen), 0))
		{
			throw -1;
		}

		const DWORD dwKeySpec = AT_KEYEXCHANGE;//AT_SIGNATURE

		if(!CryptSignHash(hHash, dwKeySpec, NULL, 0, pSignatute, reinterpret_cast<DWORD*>(&cSignatureLen)))
		{
			throw -1;
		}
	}
	catch(int nException)
	{
		nErr = nException;
	}

	if(hHash) CryptDestroyHash(hHash);
	return nErr;
}
#else
/*
To choose an appropriate key length, the following methods are recommended.

    * To enumerate the algorithms that the CSP supports and to get maximum and minimum key lengths for each algorithm, call CryptGetProvParam with PP_ENUMALGS_EX.
    * Use the minimum and maximum lengths to choose an appropriate key length. It is not always advisable to choose the maximum length because this can lead to performance issues.
    * After the desired key length has been chosen, use the upper 16 bits of the dwFlags parameter to specify the key length.

Let n be the required derived key length, in bytes. The derived key is the first n bytes of the hash value after the hash computation has been completed by CryptDeriveKey. If the hash is not a member of the SHA-2 family and the required key is for either 3DES or AES, the key is derived as follows:

   1. Form a 64-byte buffer by repeating the constant 0x36 64 times. Let k be the length of the hash value that is represented by the input parameter hBaseData. Set the first k bytes of the buffer to the result of an XOR operation of the first k bytes of the buffer with the hash value that is represented by the input parameter hBaseData.
   2. Form a 64-byte buffer by repeating the constant 0x5C 64 times. Set the first k bytes of the buffer to the result of an XOR operation of the first k bytes of the buffer with the hash value that is represented by the input parameter hBaseData.
   3. Hash the result of step 1 by using the same hash algorithm as that used to compute the hash value that is represented by the hBaseData parameter.
   4. Hash the result of step 2 by using the same hash algorithm as that used to compute the hash value that is represented by the hBaseData parameter.
   5. Concatenate the result of step 3 with the result of step 4.
   6. Use the first n bytes of the result of step 5 as the derived key.

The default RSA Full Cryptographic Service Provider is the Microsoft RSA Strong Cryptographic Provider. 
*/
bool Commands::encryptData(unsigned char* data, size_t size, const CHANNEL_INFO* pcchinfo)
{
	//We use salt value of 11 NULL bytes for compatibility with Windows
	RC4_KEY key = {0};

	unsigned char key_data[16] = {0};
	//set 40 bits key
	memcpy(key_data, pcchinfo->passwd_hash, 5);
	RC4_set_key(&key, 16, key_data);

	unsigned char* outdata = new unsigned char[size];
	RC4(&key, size, data, outdata);

	memcpy(data, outdata, size);
	delete[] outdata;
	return true;
}

bool Commands::getPasswordHash(const wchar_t* pass, size_t len, unsigned char* hash, unsigned long hash_len)
{
	if(hash_len<MD5_DIGEST_LENGTH) return false;

	size_t buflen = len*2;
	unsigned char* wpassbuf = new unsigned char[buflen];

	size_t ret_len = NixHlpr.convWcharToUtf16(const_cast<wchar_t*>(pass), len, wpassbuf, buflen);

	if(ret_len>0)
	{
		MD5(wpassbuf, buflen, hash);	
		delete[] wpassbuf;
		return true;
	}

	delete[] wpassbuf;
	return false;
}

int Commands::signMessage(const char* ptrMessage, size_t cMessageLen, unsigned char* pSignatute, size_t cSignatureLen)
{
	unsigned int siglen = RSA_size(theApp.pRSA_);
	if(cSignatureLen < siglen) return -1;

	unsigned char md[MD5_DIGEST_LENGTH] = {0};
	MD5(reinterpret_cast<const unsigned char*>(ptrMessage), cMessageLen, md);

	int result = RSA_sign(NID_md5, md, MD5_DIGEST_LENGTH, pSignatute, &siglen, theApp.pRSA_);

	//int len_n = BN_num_bytes(theApp.pRSA_->n);

	//reverse bytes for Windows
	for(unsigned int i=0; i<siglen/2; ++i)
	{
		unsigned char b = pSignatute[i];
		pSignatute[i] = pSignatute[siglen-i-1];
		pSignatute[siglen-i-1] = b;
	}

	if(1 == result) return 0;

	return -1;
}
#endif // CHATTERM_OS_WINDOWS

bool Commands::getToInfo(const wchar_t* to, const USER_INFO** ppinfo)
{
	if(0 == to || 0 == *to)
	{
		consoleio::print_line( wszNoRecipient);
		return false;
	}

	std::shared_ptr<USER_INFO> pUserInfo;
	bool bInList = USER_INFO::isUserInList(to, pUserInfo);

	if(ppinfo) *ppinfo = pUserInfo.get();

	if(!bInList)
	{
		if(pUserInfo)
			consoleio::print_line(wszRecipientBlocked);
		else
			consoleio::print_line(wszRecipientNotInList);

		return false;
	}

	return true;
}

int Commands::sendBroadcastMsg(const char* buf, int len)
{
	if(buf == 0) return 0;
	if(len<1) return 0;

	/*
	class dest_sender
	{
	public:
		dest_sender(const char* buf, int len) : buf_(buf), len_(len) {};

		void operator()(const networkio::DESTADDR_INFO& destaddr_info) const
		{
			last_result_ = destaddr_info.psender_->sendTo(&destaddr_info.saddr_, buf_, len_);
			DBG_UNREFERENCED_LOCAL_VARIABLE(result);
		}

	private:
		const char* buf_;
		int len_;
	} d(buf, len);

	for_each(BroadcastAddrs_.begin(), BroadcastAddrs_.end(), d);
	*/

	std::vector< std::unique_ptr<networkio::DESTADDR_INFO>> ::const_iterator it = Destinations_.begin();
	std::vector< std::unique_ptr<networkio::DESTADDR_INFO>> ::const_iterator end = Destinations_.end();

	bool result = true;
	while(it!=end)
	{
		int send_result = (*it)->psender_->sendTo((*it)->get_saddr(), buf, len);
		if(send_result != len)
		{
			std::wstring wszToAddr = networkio::sockaddr_to_string((*it)->get_saddr(), sizeof(sockaddr_in6));
			consoleio::print_line( wszUnableToSendMsg, wszToAddr.c_str());

			result = false;
		}

		++it;
	}

	if(result) return len;
	return 0;
}

void Commands::sendDelayedMsgs()
{
	DelayedMsgsMonitor DELAYED_MSGS_MONITOR;

	std::vector<DELAYED_MSG_DATA>::iterator it = delayedMsgs_.begin();
	std::vector<DELAYED_MSG_DATA>::iterator end = delayedMsgs_.end();

	bool fClearAll = true;
#ifdef CHATTERM_OS_WINDOWS
	DWORD cur_tick = GetTickCount();
#else
	struct timeval now = {0};
	gettimeofday(&now, NULL);
#endif // CHATTERM_OS_WINDOWS

	while(it!=end)
	{
		DELAYED_MSG_DATA& pData = *it;

#ifdef CHATTERM_OS_WINDOWS
		if(cur_tick>pData.when_)
#else
		if(now.tv_sec>pData->when_.tv_sec || now.tv_usec>pData->when_.tv_usec)
#endif // CHATTERM_OS_WINDOWS
		{
			if (pData.when_ > 0)
			{
#ifdef _DEBUG
#ifdef CHATTERM_OS_WINDOWS
				consoleio::print_line(L"Send delayed message %d -> %d", pData.when_, cur_tick);
#else
				consoleio::print_line(L"Send delayed message %d:%d -> %d:%d", pData->when_.tv_sec, pData->when_.tv_usec, now.tv_sec, now.tv_usec);
#endif // CHATTERM_OS_WINDOWS
#endif
				sendMsgToAddr(pData.what_.get(), pData.what_len_, (const sockaddr*)&pData.to_, pData.by_);
				pData.when_ = 0;
			}
		}
		else
			fClearAll = false;

		++it;
	}

	if(fClearAll)
		delayedMsgs_.clear();
}

int Commands::sendMsgTo(const char* buf, int len, const USER_INFO* pinfo, int delay)
{
	if(delay>0)
	{
		{//scope for DelayedMsgsMonitor
			DelayedMsgsMonitor DELAYED_MSGS_MONITOR;

			//DELAYED_MSG_DATA* pData = new DELAYED_MSG_DATA(buf, len, pinfo, delay);
			delayedMsgs_.emplace_back(buf, len, pinfo, delay);
#ifdef _DEBUG
#ifdef CHATTERM_OS_WINDOWS
			consoleio::print_line(L"Add delayed message to the queue %d", delayedMsgs_.back().when_);
#else
			struct timeval now={0};
			gettimeofday(&now, NULL);

			consoleio::print_line(L"Add delayed message to the queue %d:%d %d:%d", now.tv_sec, now.tv_usec, pData->when_.tv_sec, pData->when_.tv_usec);
#endif // CHATTERM_OS_WINDOWS
#endif
		}

		theApp.resumeTimer();

		return len;
	}

	return sendMsgToAddr(buf, len, pinfo->naddr_info.get_saddr(), pinfo->naddr_info.preceiver_->getSender());
}

int Commands::sendMsgToAddr(const char* buf, int len, const sockaddr* paddr, const networkio::Sender* psndr)
{
	int result = psndr->sendTo(paddr, buf, len);
	if(result != len)
	{
		std::wstring wszToAddr = networkio::sockaddr_to_string(paddr, sizeof(sockaddr_in6));
		consoleio::print_line( wszUnableToSendMsg, wszToAddr.c_str());
	}
	return result;
}


DWORD Commands::byteToDwordColor(unsigned char color)
{
	DWORD dwColor = 0;

	//0xAA + 0x55 = 0xFF
	if(color&FOREGROUND_INTENSITY) dwColor = 0x00AAAAAA;

	if(color&FOREGROUND_RED) dwColor|=0x00000055;
	if(color&FOREGROUND_GREEN) dwColor|=0x00005500;
	if(color&FOREGROUND_BLUE) dwColor|=0x00550000;

	return dwColor;
}

std::tuple<std::unique_ptr<char[]>, int> Commands::createMessageFields(char chType, MSG_FIELD_IN* pFields, int nFields)
{
	const size_t nSignatureLength = 128;//0x80;
	const size_t nRequestOffset = 10;//10 - 'X'+DatagramID
	size_t buf_size = nRequestOffset+1; //'X'+DatagramID + Id
	size_t q_signature_offset = 0;
	size_t q_signature_len_size = 0;
	size_t signature_len_size = 0;

	for (int i = 0; i < nFields; i++)
	{
		buf_size += pFields[i].size;
		switch (pFields[i].type)
		{
		case STRING_FIELD:
			buf_size++;//terminating null
			break;
		case QSIGNATURE_FIELD:
		case SIGNATURE_FIELD:
			buf_size += nSignatureLength;
			break;
		default:
			break;
		}
	}

	std::unique_ptr<char[]> ptrMessage = std::make_unique<char[]>(buf_size);
	memset(ptrMessage.get(), 0x00, buf_size);

	{//scope for DatagramIdMonitor
		DatagramIdMonitor DATAGRAM_ID_MONITOR;
	
		if(++datagramId_>999999999) datagramId_ = 1;
	
		sprintf_s(ptrMessage.get(), buf_size, "X%09.9d", datagramId_);
	
	#ifdef _DEBUG
	#ifndef GTEST_PROJECT
		if(debug_)
			consoleio::print_line(L"datagramId_ is %d, packet signature is %hs", datagramId_, ptrMessage.get());
	#endif
	#endif
	}

	char* seek = ptrMessage.get() + nRequestOffset;

	*seek++ = chType;
	
	for(int i=0; i<nFields; i++)
	{
		switch(pFields[i].type)
		{
			case SIGNATURE_LEN_FIELD:
				signature_len_size = pFields[i].size;
				pFields[i].data.bytes_=(unsigned char*)&nSignatureLength;
#ifndef CHATTERM_OS_WINDOWS
				if(fBe_ && (pFields[i].size<sizeof(nSignatureLength)))
					pFields[i].data.bytes_+=(sizeof(nSignatureLength)-pFields[i].size);
#endif
			case LEN_OF_STRING_FIELD:
			case NUMBER_FIELD:
				if(pFields[i].size<1) break;
#ifndef CHATTERM_OS_WINDOWS
				if(fBe_)
				{
					for(size_t j=pFields[i].size-1; j!=size_t(-1); --j)
					*seek++ = pFields[i].data.bytes_[j];
					break;
				}
#endif				
			case BYTES_FIELD:
				{
					memcpy(seek, pFields[i].data.bytes_, pFields[i].size);
					seek+=pFields[i].size;
				}
				break;

			case STRING_FIELD:
				{
#ifdef CHATTERM_OS_WINDOWS
					int nSizeInBytes = WideCharToMultiByte(CP_UTF8, 0, pFields[i].data.cwsz_, -1, seek, static_cast<int>(pFields[i].size), 0, 0);
#else
					size_t len = wcslen(pFields[i].data.wsz_);
					size_t nSizeInBytes = NixHlpr.convWcharToUtf8(pFields[i].data.wsz_, len+1, reinterpret_cast<unsigned char*>(seek), pFields[i].size);
#endif
					if(i>0 && (LEN_OF_STRING_FIELD == pFields[i-1].type))
					{
#ifndef CHATTERM_OS_WINDOWS
						if(fBe_)
						{
						    unsigned char* pbytes = (unsigned char*)&nSizeInBytes;
						    if(pFields[i-1].size<sizeof(nSizeInBytes))
							pbytes+=(sizeof(nSizeInBytes)-pFields[i-1].size);

						    for(size_t j=0; j<pFields[i-1].size; ++j)
							*(seek - pFields[i-1].size +j) = pbytes[pFields[i-1].size-1-j];
						}
						else
#endif
							memcpy(seek - pFields[i-1].size, &nSizeInBytes, pFields[i-1].size);
					}
					
					pFields[i].size = nSizeInBytes;
					seek+=nSizeInBytes;
				}
				break;

			case CHAR_FIELD:
				{
					*seek++ = pFields[i].data.ch_;
				}
				break;

			case QSIGNATURE_FIELD:
				q_signature_offset = 1;
				q_signature_len_size = signature_len_size;
			case SIGNATURE_FIELD:
				{
					int nErr = signMessage(ptrMessage.get()+nRequestOffset+q_signature_offset, (seek-ptrMessage.get())-nRequestOffset-q_signature_offset-q_signature_len_size, (unsigned char*)seek, nSignatureLength);
					if(0 == nErr)
					{
					}

					pFields[i].size = nSignatureLength;
					seek+=nSignatureLength;
				}
				break;
			default:
				break;
		}
	}

	int msg_size = static_cast<int>(seek - ptrMessage.get());
	return std::make_tuple(std::move(ptrMessage), msg_size);
}

int Commands::SecureNewTopicQ3(const std::wstring& channel, const CHANNEL_INFO* pcchinfo, const wchar_t* topic)
{
	/* The command takes only one parameter - topic;
	The channels is always a current channel
	A caller responsible for these checks
	if(0==channel || 0==*channel)
	{
		consoleio::print_line( wszNoChannel, channel);
		return 0;
	}

	if(!CHANNEL_INFO::isMyChannel(channel, 0))
	{
		consoleio::print_line( wszYouNotInChannel, channel);
		return 0;
	}
	*/

	int send_err = -1;

	if(0 == topic || 0 == *topic)
	{
		consoleio::print_line( wszNoNewTopic);
		return 0;
	}

	WORD TopicLentgh = (WORD)wcslen(topic);
	/*'Q' '3' Channel h00 TopicLength Topic ' (From) ' SignatureSize Signature*/
	MSG_FIELD_IN fieldsQ3[6] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{LEN_OF_STRING_FIELD, sizeof(TopicLentgh),0}
							,{STRING_FIELD,(TopicLentgh+1)*sizeof(wchar_t),topic}
							,{SIGNATURE_LEN_FIELD, sizeof(WORD),0}, {QSIGNATURE_FIELD, 0,0}};

	fieldsQ3[0].data.ch_ = '3';
	fieldsQ3[2].data.bytes_ = (unsigned char*)&TopicLentgh;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ3, _ARRAYSIZE(fieldsQ3));

	if(ptrMessage)
	{
		size_t offset = 10/*'X'+DatagramID*/+sizeof(char)/*'Q'*/+fieldsQ3[0].size+fieldsQ3[1].size+fieldsQ3[2].size;

		encryptData((unsigned char*)ptrMessage.get()+offset, fieldsQ3[3].size, pcchinfo);

		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);
	}

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::ReplySecureTopicQ2(const std::wstring&  channel, const CHANNEL_INFO* pcchinfo, const wchar_t* topic, const USER_INFO* pinfo)
{
	int send_err = -1;

	WORD TopicLentgh = (WORD)wcslen(topic);
	/*'Q' '2' Channel h00 TopicLength Topic SignatureSize Signature*/
	MSG_FIELD_IN fieldsQ2[6] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{LEN_OF_STRING_FIELD, sizeof(TopicLentgh),0}
							,{STRING_FIELD,(wcslen(topic)+1)*sizeof(wchar_t),topic}
							,{SIGNATURE_LEN_FIELD, sizeof(WORD),0}, {QSIGNATURE_FIELD, 0,0}};

	fieldsQ2[0].data.ch_ = '2';
	fieldsQ2[2].data.bytes_ = (unsigned char*)&TopicLentgh;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ2, _ARRAYSIZE(fieldsQ2));

	if(ptrMessage)
	{
		size_t offset = 10/*'X'+DatagramID*/+sizeof(char)/*'Q'*/+fieldsQ2[0].size+fieldsQ2[1].size+fieldsQ2[2].size;

		encryptData((unsigned char*)ptrMessage.get()+offset, fieldsQ2[3].size, pcchinfo);

		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);
	}

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::ReplySecureHereQ4(const std::wstring& channel, const USER_INFO* pinfo, int delay)
{
	int send_err = -1;

	/*'Q' '4' To h00 Channel h00 From h00 RemoteActive SignatureSize Signature*/
	MSG_FIELD_IN fieldsQ4[8] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t),pinfo->getNick()}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{CHAR_FIELD,1,0}
							,{QSIGNATURE_FIELD, 0,0},{SIGNATURE_LEN_FIELD, sizeof(WORD),0}};

	fieldsQ4[0].data.ch_ = '4';
	fieldsQ4[4].data.ch_ = ACTIVE_CHANNEL_STATE;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ4, _ARRAYSIZE(fieldsQ4));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size,pinfo,delay);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::SecureHereQ8(const std::wstring& channel)
{
	int send_err = -1;

	if(channel.length()<1)
	{
		consoleio::print_line( wszNoChannel, channel.c_str());
		return 0;
	}

	std::shared_ptr<CHANNEL_INFO> ptrChInfo;
	if(!CHANNEL_INFO::isMyChannel(channel, ptrChInfo))
	{
		consoleio::print_line( wszYouNotInChannel, channel.c_str());
		return 0;
	}

	/*'Q' '8' From h00 Channel h00 Signature SignatureSize*/
	MSG_FIELD_IN fieldsQ8[5] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{QSIGNATURE_FIELD, 0,0}, {SIGNATURE_LEN_FIELD, sizeof(WORD),0}};

	fieldsQ8[0].data.ch_ = '8';

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ8, _ARRAYSIZE(fieldsQ8));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::SecureLeaveQ7(const std::wstring& channel)
{
	int send_err = -1;

	if(channel.empty())
	{
		consoleio::print_line( wszNoChannel, channel);
		return 0;
	}

	std::shared_ptr<CHANNEL_INFO> ptrChInfo;
	if(!CHANNEL_INFO::isMyChannel(channel, ptrChInfo))
	{
		consoleio::print_line( wszYouNotInChannel, channel);
		return 0;
	}

	send_err = SecureLeaveQ7(ptrChInfo.get());

	/*
	if(0 == send_err)
	{
		if (ptrChInfo->removeMember(theApp.ptrMe_.get()) && ptrChInfo->users.size() < 1)
			CHANNEL_INFO::SetOfChannels_.erase(ptrChInfo);

		consoleio::print_line(nullptr);
	}
	*/

	return send_err;
}

int Commands::SecureLeaveQ7(const CHANNEL_INFO* pcchinfo)
{
	int send_err = -1;

	/*'Q' '7' From h00 Channel h00 Gender Signature SignatureSize*/
	MSG_FIELD_IN fieldsQ7[6] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(pcchinfo->name.length()+1)*sizeof(wchar_t),pcchinfo->name.c_str()}
							,{CHAR_FIELD,1,0}
							,{QSIGNATURE_FIELD, 0,0}, {SIGNATURE_LEN_FIELD, sizeof(WORD),0}};

	fieldsQ7[0].data.ch_ = '7';
	fieldsQ7[3].data.ch_ = theApp.ptrMe_->gender;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ7, _ARRAYSIZE(fieldsQ7));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::SecureChannelMsgQ01(const std::wstring& channel, const CHANNEL_INFO* pcchinfo, const wchar_t* text, bool fMe)
{
	/* The command takes only one parameter - text;
	The channels is always a current channel
	A caller responsible for these checks
	if(0==channel || 0==*channel)
	{
		consoleio::print_line( wszNoChannel, channel);
		return 0;
	}

	if(!CHANNEL_INFO::isMyChannel(channel, 0))
	{
		consoleio::print_line( wszYouNotInChannel, channel);
		return 0;
	}
	*/

	int send_err = -1;

	if(0==text || 0==*text)
	{
		consoleio::print_line( wszNoMsgText, channel);
		return 0;
	}

	/*'Q' '0' Channel h00 From h00 MessageTextLentgh MessageText h00 SignatureSize Signature*/
	WORD MessageTextLentgh = static_cast<WORD>(wcslen(text));
	MSG_FIELD_IN fieldsQ10[7] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{LEN_OF_STRING_FIELD, sizeof(MessageTextLentgh),0}
							,{STRING_FIELD,(MessageTextLentgh+1)*sizeof(wchar_t),text}
							,{SIGNATURE_LEN_FIELD, sizeof(WORD),0},{QSIGNATURE_FIELD, 0,0}};

	fieldsQ10[0].data.ch_ = fMe ? '1' : '0';
	fieldsQ10[3].data.bytes_ = (unsigned char*)&MessageTextLentgh;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ10, _ARRAYSIZE(fieldsQ10));

	if(ptrMessage)
	{
		size_t offset = 10/*'X'+DatagramID*/+sizeof(char)/*'Q'*/+fieldsQ10[0].size+fieldsQ10[1].size+fieldsQ10[2].size+fieldsQ10[3].size;

		encryptData((unsigned char*)ptrMessage.get()+offset, fieldsQ10[4].size, pcchinfo);

		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);
	}

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::SecureJoinQ5(const std::wstring& channel, const wchar_t* passwd)
{
	int send_err = -1;

	if(channel.empty())
	{
		consoleio::print_line( wszNoChannel, channel);
		return 0;
	}

	if(0==passwd) passwd = wszEmptyString;

	unsigned char hash[16]={0};
#ifdef CHATTERM_OS_WINDOWS
	HCRYPTKEY hKey = 0;
	if(!getPasswordHash(passwd, wcslen(passwd), hash, sizeof(hash), &hKey)) return -1;
#else
	if(!getPasswordHash(passwd, wcslen(passwd), hash, sizeof(hash))) return -1;
#endif
	int msg_size = 0;

	/*'Q' '5' From h00 Channel h00 Status Gender (16)MD5Hash SignatureSize Signature*/
	MSG_FIELD_IN fieldsQ5[8] = {{CHAR_FIELD,1,0}, {STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{CHAR_FIELD,1,0},{CHAR_FIELD,1,0}
							,{BYTES_FIELD,sizeof(hash),0}
							,{QSIGNATURE_FIELD, 0,0},{SIGNATURE_LEN_FIELD, sizeof(WORD),0}};

	fieldsQ5[0].data.ch_ = '5';
	fieldsQ5[3].data.ch_ = theApp.ptrMe_->status;
	fieldsQ5[4].data.ch_ = theApp.ptrMe_->gender;
	fieldsQ5[5].data.bytes_ = hash;

	std::unique_ptr<char[]> ptrMessage;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ5, _ARRAYSIZE(fieldsQ5));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size)
	{
		const CHANNEL_INFO* pcchinfo = CHANNEL_INFO::addChannelMember(channel.c_str(), theApp.ptrMe_.get(), CHANNEL_INFO::SECURED);

		if(0 == pcchinfo || !pcchinfo->joined || !pcchinfo->secured)
		{
#ifdef CHATTERM_OS_WINDOWS
			if(hKey) CryptDestroyKey(hKey);
#endif
			consoleio::print_line(wszYouNotInChannel, channel);
			return 0;
		}

		memcpy(pcchinfo->passwd_hash, hash, sizeof(pcchinfo->passwd_hash));
#ifdef CHATTERM_OS_WINDOWS
		pcchinfo->hKey = hKey;
#endif
		CHANNEL_INFO::setActiveChannel(pcchinfo);

		consoleio::print_line(NULL);
		send_err = 0;
	}
	else
	{
#ifdef CHATTERM_OS_WINDOWS
		if(hKey) CryptDestroyKey(hKey);
#endif
	}

	return send_err;
}

int Commands::ReplySecureJoinQ6(const std::wstring& channel, char result, const USER_INFO* pinfo, int delay)
{
	int send_err = -1;

	/*'Q' '6' To h00 Channel h00 Result Signature SignatureSize*/
	MSG_FIELD_IN fieldsQ6[6] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t),pinfo->getNick()}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{CHAR_FIELD, 1,0}
							,{QSIGNATURE_FIELD, 0,0}, {SIGNATURE_LEN_FIELD, sizeof(WORD),0}};

	fieldsQ6[0].data.ch_ = '6';
	fieldsQ6[3].data.ch_ = result;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Q', fieldsQ6, _ARRAYSIZE(fieldsQ6));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo,delay);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::ReplyList1(const USER_INFO* pinfo, int delay)
{
	int send_err = -1;

	DWORD dwColor = byteToDwordColor(theApp.ptrMe_->color);
	const wchar_t wszMsgBoardMessageIDs[]=L"";
	/*
	'1' To h00 From h00 Status WndActive
	h00 Version Gender UUID
	h00 LicenseID CodePage
	Color h00
	LastMsgBoardMessage TotalMsgBoardMessages MsgBoardMessageIDs h00
	PubKeySize PubKey
	Icon
	Signature SignatureSize
	*/
	MSG_FIELD_IN fields1[20] = {{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t), pinfo->getNick()}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{CHAR_FIELD,1,0},{CHAR_FIELD,1,0}
							,{BYTES_FIELD,1,0},{NUMBER_FIELD,sizeof(theApp.ptrMe_->ver),0}, {CHAR_FIELD,1,0},{BYTES_FIELD,sizeof(theApp.ptrMe_->uuid),0}
							,{BYTES_FIELD,1,0},{NUMBER_FIELD,sizeof(theApp.ptrMe_->license_id),0}, {CHAR_FIELD,1,0}
							,{NUMBER_FIELD,sizeof(dwColor),0},{BYTES_FIELD,1,0}
							,{BYTES_FIELD,sizeof(SYSTEMTIME)+sizeof(WORD),0}, {STRING_FIELD,sizeof(wszMsgBoardMessageIDs),wszMsgBoardMessageIDs}//No Message Board messages
							,{NUMBER_FIELD, sizeof(WORD),0}, {BYTES_FIELD, theApp.ptrMe_->pub_key_size,0}
							,{BYTES_FIELD, sizeof(theApp.ptrMe_->icon),0}
							,{SIGNATURE_FIELD, 0,0}, {SIGNATURE_LEN_FIELD, sizeof(WORD),0}};

	fields1[2].data.ch_ = theApp.ptrMe_->status;
	fields1[3].data.ch_ = theApp.ptrMe_->wnd_state;
	fields1[4].data.bytes_ = pNullBytes;
	fields1[5].data.bytes_ = (unsigned char*)&theApp.ptrMe_->ver;
	fields1[6].data.ch_ = theApp.ptrMe_->gender;
	fields1[7].data.bytes_ = (unsigned char*)&theApp.ptrMe_->uuid;
	fields1[8].data.bytes_ = pNullBytes;
	fields1[9].data.bytes_ = (unsigned char*)&theApp.ptrMe_->license_id;
	fields1[10].data.ch_ = theApp.ptrMe_->codepage;
	fields1[11].data.bytes_ = (unsigned char*)&dwColor;
	fields1[12].data.bytes_ = pNullBytes;
	fields1[13].data.bytes_ = pNullBytes;
	fields1[15].data.bytes_ = (unsigned char*)&theApp.ptrMe_->pub_key_size;
	fields1[16].data.bytes_ = theApp.ptrMe_->pub_key.get();
	fields1[17].data.bytes_ = &theApp.ptrMe_->icon;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('1', fields1, _ARRAYSIZE(fields1));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo, delay);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::ReplyConfirmMassTextMsg7(const wchar_t* datagramId, const USER_INFO* pinfo, int delay)
{
	int send_err = -1;

	/*'7' Status To h00 From h00 Gender CurrentAA h00
			DatagramID h00
	*/
	//CurrentAA must contain a terminating null, so one more h00 is added as BYTES_FIELD
	//AutoAnswers are not supported
	const wchar_t wszCurrentAA[]=L"";

	MSG_FIELD_IN fields7[7] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t), pinfo->getNick()}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{CHAR_FIELD,1,0}
							,{STRING_FIELD,sizeof(wszCurrentAA),wszCurrentAA}, {BYTES_FIELD, 1,0}
							,{STRING_FIELD,(wcslen(datagramId)+1)*sizeof(wchar_t),datagramId}};

	fields7[0].data.ch_ = theApp.ptrMe_->status;
	fields7[3].data.ch_ = theApp.ptrMe_->gender;
	fields7[5].data.bytes_ = pNullBytes;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('7', fields7, _ARRAYSIZE(fields7));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo, delay);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::ReplyTopicC(const std::wstring& channel, const wchar_t* topic, const USER_INFO* pinfo, int delay)
{
	int send_err = -1;

	/*'C' To h00 Channel h00 Topic h00*/

	MSG_FIELD_IN fieldsC[3] = {{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t), pinfo->getNick()}
					,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t), channel.c_str()}
					,{STRING_FIELD,(wcslen(topic)+1)*sizeof(wchar_t), topic}};

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('C', fieldsC, _ARRAYSIZE(fieldsC));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo, delay);

	if(send_err == msg_size) return 0;

	return send_err;
}


int Commands::ReplyInfoG(const USER_INFO* pinfo)
{
	int send_err = -1;

	//Info

	//Do not reply the IP address because TCP connections are not supported
	//wchar_t* wszFromAddr = networkio::sockaddr_to_string(theApp.ptrMe_->naddr_info.psaddr_, sizeof(sockaddr_in6));
	//const wchar_t* wszIpAddresses = wszFromAddr ? wszFromAddr : wszEmptyString;

	const wchar_t wszIpAddresses[] = L"";
	const wchar_t wszCurrentAA[]=L"";

	std::wstring strChannels;
	CHANNEL_INFO::getChannelsList(strChannels);

	/*'G' To h00 From h00 Computer name h00 User name h00
	IP addresses h00 ListOfChannels '#' h00 CurrentAA h00 Domain name h00
	OS h00 Chat software h00
	FullName h00 Job h00 Department h00 Work phone h00
	Mobile phone h00 www h00 e-mail h00 address h00*/

	MSG_FIELD_IN fieldsG[18] = {{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t), pinfo->getNick()}
		,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.computer_name.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.computer_name.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.user_name.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.user_name.c_str()}
		,{STRING_FIELD, sizeof(wszIpAddresses), wszIpAddresses}
		,{STRING_FIELD, (strChannels.length()+1)*sizeof(wchar_t), strChannels.c_str()}
		,{STRING_FIELD, sizeof(wszCurrentAA),wszCurrentAA}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.domain_name.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.domain_name.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.os.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.os.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.chat_software.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.chat_software.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.full_name.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.full_name.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.job.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.job.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.department.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.department.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.phone_work.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.phone_work.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.phone_mob.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.phone_mob.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.www.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.www.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.email.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.email.c_str()}
		,{STRING_FIELD, (theApp.MyPersonalInfo_.address.length()+1)*sizeof(wchar_t), theApp.MyPersonalInfo_.address.c_str()}};

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('G', fieldsG, _ARRAYSIZE(fieldsG));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);

	if(send_err == msg_size) return 0;
	return send_err;
}

int Commands::ReplyConfirmBeepH(const USER_INFO* pinfo)
{
	int send_err = -1;

	//Beep
	/*'H' '1' To h00 From h00 Gender*/
	MSG_FIELD_IN fieldsH[4] = {{CHAR_FIELD,1,0},{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t),pinfo->getNick()},{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()},{CHAR_FIELD,1,0}};
	fieldsH[0].data.ch_ = '1';
	fieldsH[3].data.ch_ = theApp.ptrMe_->gender;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('H', fieldsH, _ARRAYSIZE(fieldsH));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);

	if(send_err == msg_size) return 0;
	return send_err;
}

int Commands::ReplyHereK(const std::wstring& channel, const USER_INFO* pinfo, int delay)
{
	int send_err = -1;

	/*'K' To h00 Channel h00 From h00 RemoteActive*/
	MSG_FIELD_IN fieldsK[4] = {{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t),pinfo->getNick()}
				,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
				,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
				,{CHAR_FIELD,1,0}};

	fieldsK[3].data.ch_ = ACTIVE_CHANNEL_STATE;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('K', fieldsK, _ARRAYSIZE(fieldsK));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo, delay);

	if(send_err == msg_size) return 0;
	return send_err;
}

int Commands::ReplyChannelsO(const USER_INFO* pinfo)
{
	int send_err = -1;

	std::wstring strChannels;

	CHANNEL_INFO::getChannelsList(strChannels);

	/*'O' To h00 ListOfChannels '#'*/
	MSG_FIELD_IN fieldsO[2] = {{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t),pinfo->getNick()},{STRING_FIELD,sizeof(wchar_t)*(strChannels.length()+1),strChannels.c_str()}};
	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('O', fieldsO, _ARRAYSIZE(fieldsO));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);

	if(send_err == msg_size) return 0;
	return send_err;
}

int Commands::Join4(const std::wstring& channel)
{
	int msg_size = 0, send_err = -1;

	if(channel.empty())
	{
		consoleio::print_line( wszNoChannel, channel);
		return 0;
	}

	size_t name_len = channel.length();

	if(name_len > CHANNEL_INFO::MaxNameLength)
	{
		consoleio::print_line(wszChannelNameLimit, CHANNEL_INFO::MaxNameLength-1);
		return 0;
	}

	if(0 == _wcsicmp(channel.c_str(), CHANNEL_INFO::wszMainChannel))
	{
		//send JoinMain
		DWORD dwColor = byteToDwordColor(theApp.ptrMe_->color);
		const wchar_t wszMsgBoardMessageIDs[]=L"";
		/*
		'4'
		From h00
		Channel h00
		Status Gender h00
		Version UUID h00
		CodePage Color h00
		LastMsgBoardMessage TotalMsgBoardMessages MsgBoardMessageIDs h00
		PubKeySize PubKey
		Icon
		Signature SignatureSize
		*/
		MSG_FIELD_IN fields4Main[18] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
								,{STRING_FIELD,sizeof(CHANNEL_INFO::wszMainChannel),CHANNEL_INFO::wszMainChannel}
								,{CHAR_FIELD,1,0},{CHAR_FIELD,1,0},{BYTES_FIELD,1,0}
								,{NUMBER_FIELD,sizeof(theApp.ptrMe_->ver),0}, {BYTES_FIELD,sizeof(theApp.ptrMe_->uuid),0},{BYTES_FIELD,1,0}
								,{CHAR_FIELD,1,0},{NUMBER_FIELD,sizeof(dwColor),0},{BYTES_FIELD,1,0}
								,{BYTES_FIELD,sizeof(SYSTEMTIME)+sizeof(WORD),0}, {STRING_FIELD,sizeof(wszMsgBoardMessageIDs),wszMsgBoardMessageIDs}//No Message Board messages
								,{NUMBER_FIELD, sizeof(WORD),0}, {BYTES_FIELD, theApp.ptrMe_->pub_key_size,0}
								,{BYTES_FIELD, sizeof(theApp.ptrMe_->icon),0}
								,{SIGNATURE_FIELD, 0,0}, {SIGNATURE_LEN_FIELD, sizeof(WORD),0}};
		
		fields4Main[2].data.ch_ = theApp.ptrMe_->status;
		fields4Main[3].data.ch_ = theApp.ptrMe_->gender;
		fields4Main[4].data.bytes_ = pNullBytes;
		fields4Main[5].data.bytes_ = (unsigned char*)&theApp.ptrMe_->ver;
		fields4Main[6].data.bytes_ = (unsigned char*)&theApp.ptrMe_->uuid;
		fields4Main[7].data.bytes_ = pNullBytes;
		fields4Main[8].data.ch_ = theApp.ptrMe_->codepage;
		fields4Main[9].data.bytes_ = (unsigned char*)&dwColor;
		fields4Main[10].data.bytes_ = pNullBytes;
		fields4Main[11].data.bytes_ = pNullBytes;
		fields4Main[13].data.bytes_ = (unsigned char*)&theApp.ptrMe_->pub_key_size;
		fields4Main[14].data.bytes_ = theApp.ptrMe_->pub_key.get();
		fields4Main[15].data.bytes_ = &theApp.ptrMe_->icon;

		std::unique_ptr<char[]> ptrMessage;
		std::tie(ptrMessage, msg_size) = createMessageFields('4', fields4Main, _ARRAYSIZE(fields4Main));

		if(ptrMessage)
			send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);
	}
	else
	{
		//send Join
		/*'4' From h00 Channel h00 Status Gender*/
		MSG_FIELD_IN fields4[4] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
								,{STRING_FIELD,(name_len+1)*sizeof(wchar_t),channel.c_str()}
								,{CHAR_FIELD,1,0},{CHAR_FIELD,1,0}};
		
		fields4[2].data.ch_ = theApp.ptrMe_->status;
		fields4[3].data.ch_ = theApp.ptrMe_->gender;

		std::unique_ptr<char[]> ptrMessage;
		std::tie(ptrMessage, msg_size) = createMessageFields('4', fields4, _ARRAYSIZE(fields4));

		if(ptrMessage)
			send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);
	}

	if(send_err == msg_size)
	{
		CHANNEL_INFO::addChannelMember(channel, theApp.ptrMe_.get(), CHANNEL_INFO::NOT_SECURED);

		std::shared_ptr<CHANNEL_INFO> ptrChInfo;
		if(!CHANNEL_INFO::isMyChannel(channel, ptrChInfo))
		{
			consoleio::print_line( wszYouNotInChannel, channel);
			return 0;
		}

		CHANNEL_INFO::setActiveChannel(ptrChInfo.get());

		consoleio::print_line(NULL);
		send_err = 0;
	}

	return send_err;
}

int Commands::ChannelMsg2A(const std::wstring& channel, const wchar_t* text, bool fMe)
{
	/* The command takes only one parameter - text;
	The channels is always a current channel
	A caller responsible for these checks
	if(0==channel || 0==*channel)
	{
		consoleio::print_line( wszNoChannel, channel);
		return 0;
	}

	if(!CHANNEL_INFO::isMyChannel(channel, 0))
	{
		consoleio::print_line( wszYouNotInChannel, channel);
		return 0;
	}
	*/

	int send_err = -1;

	if(0==text || 0==*text)
	{
		consoleio::print_line( wszNoMsgText, channel);
		return 0;
	}

	size_t textlen = wcslen(text);

	if(textlen > MaxMsgLength)
	{
		consoleio::print_line(wszLineMsgLimit, MaxMsgLength);
		return 0;
	}

	//send ChannelMsg
	/*'2' Channel h00 From h00 MessageText h00 Signature*/
	//send ChannelMeMsg
	/*'A' Channel h00 From h00 MessageText h00 Signature*/
	MSG_FIELD_IN fields2[4] = {{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(textlen+1)*sizeof(wchar_t),text}
							,{SIGNATURE_FIELD,0,0}};

	char msg_type = fMe ? 'A' : '2';
	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields(msg_type, fields2, _ARRAYSIZE(fields2));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size) return 0;
	return send_err;
}

int Commands::MassTextMsgE(const wchar_t* text)
{
	int send_err = -1;

	if(USER_INFO::SetOfUsers_.size()<1)
	{
		consoleio::print_line( wszNoUsers);
		return 0;
	}

	if(0==text || 0==*text)
	{
		consoleio::print_line( wszNoMsgText, text);
		return 0;
	}

	size_t textlen = wcslen(text);

	if(textlen > MaxMassMsgLength)
	{
		consoleio::print_line(wszMassMsgLimit, MaxMassMsgLength);
		return 0;
	}

	USER_INFO::ConstIteratorOfUsers it = USER_INFO::SetOfUsers_.begin();
	while(it != USER_INFO::SetOfUsers_.end())
	{
		//send MassTextMsg
		/*'E' From h00 To h00 MessageText h00*/
		MSG_FIELD_IN fieldsE[3] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
								,{STRING_FIELD,sizeof(wchar_t)*(wcslen((*it)->getNick())+1),(*it)->getNick()}
								,{STRING_FIELD,(textlen+1)*sizeof(wchar_t),text}};
		std::unique_ptr<char[]> ptrMessage;
		int msg_size = 0;
		std::tie(ptrMessage, msg_size) = createMessageFields('E', fieldsE, _ARRAYSIZE(fieldsE));

		if(ptrMessage)
			send_err = sendMsgTo(ptrMessage.get(), msg_size, (*it).get());

		++it;

		if(send_err != msg_size) return send_err;
	}

	return 0;
}

int Commands::FloodZ(const wchar_t* to, int nsecs)
{
	if(nsecs<1)
	{
		consoleio::print_line( wszIncorrectTimeOut);
		return 0;
	}

	const USER_INFO* pinfo = 0;
	if(!getToInfo(to, &pinfo)) return 0;

	if(pinfo == theApp.ptrMe_.get())
	{
		consoleio::print_line( wszNoBlockYourself);
		return 0;
	}

	return FloodZ(pinfo, nsecs);
}

int Commands::FloodZ(const USER_INFO* pinfo, int nsecs)
{
	int send_err = -1;

	wchar_t wszSeconds[32] = {0};
#ifdef CHATTERM_OS_WINDOWS
	_itow_s(nsecs, wszSeconds, _ARRAYSIZE(wszSeconds), 10);
#else
	swprintf(wszSeconds, _ARRAYSIZE(wszSeconds), L"%d", nsecs);
#endif // CHATTERM_OS_WINDOWS



	//send Flood
	/*'Z' To h00 From h00 seconds h00*/
	MSG_FIELD_IN fieldsZ[3] = {{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t),pinfo->getNick()}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(wcslen(wszSeconds)+1)*sizeof(wchar_t),wszSeconds}};
	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('Z', fieldsZ, _ARRAYSIZE(fieldsZ));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);

	if(send_err == msg_size)
	{
		pinfo->flood = nsecs;

		consoleio::print_line(pinfo->color, false, wszFloodProtection, theApp.getStrTime(false), pinfo->getNick(), wszSeconds);
		return 0;
	}

	return send_err;
}

int Commands::MassTextMsgToE(const wchar_t* to, const wchar_t* text)
{
	int send_err = -1;

	if(0==text || 0==*text)
	{
		consoleio::print_line( wszNoMsgText, text);
		return 0;
	}

	const USER_INFO* pinfo = 0;
	if(!getToInfo(to, &pinfo)) return 0;

	size_t textlen = wcslen(text);

	if(textlen > MaxMassMsgLength)
	{
		consoleio::print_line(wszMassMsgLimit, MaxMassMsgLength);
		return 0;
	}

	//send MassTextMsg
	/*'E' From h00 To h00 MessageText h00*/
	MSG_FIELD_IN fieldsE[3] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(wcslen(to)+1)*sizeof(wchar_t),to}
							,{STRING_FIELD,(textlen+1)*sizeof(wchar_t),text}};
	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('E', fieldsE, _ARRAYSIZE(fieldsE));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::NickName3(const wchar_t* nick)
{
	int send_err = -1;

	if(0 == nick || 0 == *nick)
	{
		consoleio::print_line( wszNoNewNick);
		return 0;
	}

	size_t nick_len = wcslen(nick);
	if(nick_len > USER_INFO::MaxNickLength)
	{
		consoleio::print_line(wszNickNameLimit, USER_INFO::MaxNickLength);
		return 0;
	}

	//send NickName
	/*'3' From h00 NewNick h00 Gender Signature*/
	MSG_FIELD_IN fields3[4] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(nick_len+1)*sizeof(wchar_t),nick}
							,{CHAR_FIELD,1,0}
							,{SIGNATURE_FIELD,0,0}};
	fields3[2].data.ch_ = theApp.ptrMe_->gender;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('3', fields3, _ARRAYSIZE(fields3));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size)
	{
		//ProcessorMsgX::processNickName3 does these steps
		//theApp.ptrMe_->setNick(nick, wcslen(nick));
		return 0;
	}

	return send_err;
}

int Commands::NewTopicB(const std::wstring& channel, const wchar_t* topic)
{
	/* The command takes only one parameter - topic;
	The channels is always a current channel
	A caller responsible for these checks
	if(0==channel || 0==*channel)
	{
		consoleio::print_line( wszNoChannel, channel);
		return 0;
	}

	if(!CHANNEL_INFO::isMyChannel(channel, 0))
	{
		consoleio::print_line( wszYouNotInChannel, channel);
		return 0;
	}
	*/

	int send_err = -1;

	if(0 == topic || 0 == *topic)
	{
		consoleio::print_line( wszNoNewTopic);
		return 0;
	}

	size_t topic_len = wcslen(topic);

	if(topic_len > CHANNEL_INFO::MaxTopicLength)
	{
		consoleio::print_line(wszChannelNameLimit, CHANNEL_INFO::MaxTopicLength);
		return 0;
	}

	//send NewTopic
	/*'B' Channel h00 Topic ' (From) ' h00 Signature*/
	MSG_FIELD_IN fieldsB[3] = {{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t), channel.c_str()}
							,{STRING_FIELD,(topic_len+1)*sizeof(wchar_t),topic}
							,{SIGNATURE_FIELD,0,0}};

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('B', fieldsB, _ARRAYSIZE(fieldsB));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::PingPongP(const wchar_t* to, bool fPong)
{
	const USER_INFO* pinfo = 0;
	if(!getToInfo(to, &pinfo)) return 0;

	return PingPongP(pinfo, fPong);
}

int Commands::PingPongP(const USER_INFO* pinfo, bool fPong)
{
	int send_err = -1;

	wchar_t wszTime[9]={0};//hh:mm:ss
	wcscpy_s(wszTime, _ARRAYSIZE(wszTime), theApp.getStrTime(true));

	const wchar_t wszMsgBoardMessageIDs[]=L"";

	//send Ping
	/*'P' '0' To h00 From h00 CurrentTime h00
	LastMsgBoardMessage TotalMsgBoardMessages MsgBoardMessageIDs h00 PubKeySize PubKey*/
	MSG_FIELD_IN fieldsP[8] = {{CHAR_FIELD,1,0}
							,{STRING_FIELD,(wcslen(pinfo->getNick())+1)*sizeof(wchar_t),pinfo->getNick()}
							,{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,sizeof(wszTime),wszTime}
							,{BYTES_FIELD,sizeof(SYSTEMTIME)+sizeof(WORD),0}, {STRING_FIELD,sizeof(wszMsgBoardMessageIDs),wszMsgBoardMessageIDs}//No Message Board messages
							,{NUMBER_FIELD, sizeof(WORD),0}, {BYTES_FIELD, theApp.ptrMe_->pub_key_size,0}};

	fieldsP[0].data.ch_ = fPong ? '1' : '0';
	fieldsP[4].data.bytes_ = pNullBytes;
	fieldsP[6].data.bytes_ = (unsigned char*)&theApp.ptrMe_->pub_key_size;
	fieldsP[7].data.bytes_ = theApp.ptrMe_->pub_key.get();

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('P', fieldsP, _ARRAYSIZE(fieldsP));

	if(ptrMessage)
	{
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);


		if(!fPong && (msg_size==send_err))
		{
			++pinfo->pings;
			return 0;
		}
	}

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::BeepH(const wchar_t* to)
{
	int send_err = -1;

	const USER_INFO* pinfo = 0;
	if(!getToInfo(to, &pinfo)) return 0;

	//Beep
	/*'H' '0' To h00 From h00*/
	MSG_FIELD_IN fieldsH[3] = {{CHAR_FIELD,1,0},{STRING_FIELD,(wcslen(to)+1)*sizeof(wchar_t),to},{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}};
	fieldsH[0].data.ch_ = '0';

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('H', fieldsH, _ARRAYSIZE(fieldsH));

	if(ptrMessage)
	{
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);


		if(msg_size==send_err)
		{
			++pinfo->beeps;
			return 0;
		}
	}

	return send_err;
}

int Commands::InfoF(const wchar_t* to)
{
	int send_err = -1;

	const USER_INFO* pinfo = 0;
	if(!getToInfo(to, &pinfo)) return 0;

	//Info
	/*'F' To h00 From h00*/
	MSG_FIELD_IN fieldsF[2] = {{STRING_FIELD,(wcslen(to)+1)*sizeof(wchar_t),to},{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}};

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('F', fieldsF, _ARRAYSIZE(fieldsF));

	if(ptrMessage)
	{
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);

	}

	if(msg_size==send_err)
	{
		++pinfo->infos;
		return 0;
	}

	return send_err;
}

int Commands::HereL(const std::wstring& channel)
{
	int send_err = -1;

	if(channel.empty())
	{
		consoleio::print_line( wszNoChannel, channel.c_str());
		return 0;
	}

	std::shared_ptr<CHANNEL_INFO> ptrChInfo;
	if(!CHANNEL_INFO::isMyChannel(channel, ptrChInfo))
	{
		consoleio::print_line( wszYouNotInChannel, channel.c_str());
		return 0;
	}

	//send Here
	/*'L' From h00 Channel h00*/
	MSG_FIELD_IN fieldsL[2] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
							,{STRING_FIELD,(channel.length()+1)*sizeof(wchar_t),channel.c_str()}};

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('L', fieldsL, _ARRAYSIZE(fieldsL));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::ChannelsN(const wchar_t* to)
{
	int send_err = -1;

	const USER_INFO* pinfo = 0;
	if(!getToInfo(to, &pinfo)) return 0;

	//send Channels
	/*'N' From h00*/
	MSG_FIELD_IN fieldsN[1] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}};
	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('N', fieldsN, _ARRAYSIZE(fieldsN));

	if(ptrMessage)
		send_err = sendMsgTo(ptrMessage.get(), msg_size, pinfo);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::List0()
{
	int send_err = -1;
	//send List
	/*'0' From h00 CodePage h00*/
	MSG_FIELD_IN fields0[3] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}, {CHAR_FIELD,1,0}, {BYTES_FIELD,1,0}};
	fields0[1].data.ch_ = theApp.ptrMe_->codepage;
	fields0[2].data.bytes_ = pNullBytes;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('0', fields0, _ARRAYSIZE(fields0));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size) return 0;

	return send_err;
}

int Commands::Leave5(const std::wstring& channel)
{
	int send_err = -1;

	if(channel.empty())
	{
		consoleio::print_line( wszNoChannel, channel.c_str());
		return 0;
	}

	std::shared_ptr<CHANNEL_INFO> ptrChInfo;
	if(!CHANNEL_INFO::isMyChannel(channel, ptrChInfo))
	{
		consoleio::print_line( wszYouNotInChannel, channel);
		return 0;
	}

	send_err = Leave5(ptrChInfo.get());

	/*
	if (0 == send_err)
	{
		if (ptrChInfo->removeMember(theApp.ptrMe_.get()) && ptrChInfo->users.size()< 1)
			CHANNEL_INFO::SetOfChannels_.erase(ptrChInfo);

		consoleio::print_line(nullptr);
	}
	*/

	return send_err;
}

int Commands::Leave5(const CHANNEL_INFO* pcchinfo)
{
	int send_err = -1;

	//send Leave
	/*'5' From h00 Channel h00 Gender Signature*/
	MSG_FIELD_IN fields5[4] = {{STRING_FIELD,(wcslen(theApp.ptrMe_->getNick())+1)*sizeof(wchar_t),theApp.ptrMe_->getNick()}
						,{STRING_FIELD,(pcchinfo->name.length()+1)*sizeof(wchar_t),pcchinfo->name.c_str()}
						,{CHAR_FIELD,1,0}
						,{SIGNATURE_FIELD,0,0}};

	fields5[2].data.ch_ = theApp.ptrMe_->gender;

	std::unique_ptr<char[]> ptrMessage;
	int msg_size = 0;
	std::tie(ptrMessage, msg_size) = createMessageFields('5', fields5, _ARRAYSIZE(fields5));

	if(ptrMessage)
		send_err = sendBroadcastMsg(ptrMessage.get(), msg_size);

	if(send_err == msg_size) return 0;

	return send_err;
}
