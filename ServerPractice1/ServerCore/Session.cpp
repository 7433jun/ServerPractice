#include "pch.h"
#include "Session.h"
#include "SocketHelper.h"
#include "Service.h"

Session::Session() : recvBuffer(BUFFER_SIZE)
{
	socket = SocketHelper::CreateSocket();
}

Session::~Session()
{
	SocketHelper::CloseSocket(socket);
}

bool Session::Connect()
{
	return RegisterConnect();
}

void Session::Send(shared_ptr<SendBuffer> sendData)
{
	// 연결 안되어 있음 return
	if (!IsConnected())
		return;

	// 모든 스레드들의 기본값
	bool registerSend = false;

	{
		// Lock을 잡고
		unique_lock<shared_mutex> lock(rwLock);
		// 경합해서 이긴애가 push
		sendQueue.push(sendData);
		// 최초의 sendRegistered 값을 바꾸는 애 하나만
		if (sendRegistered.exchange(true) == false)
		{
			// registerSend를 true 할수 있음
			registerSend = true;
		}
	}// Lock 풀림

	if (registerSend)
	{
		RegisterSend();
	}
}

void Session::Disconnect(const WCHAR* cause)
{
	if (isConnected.exchange(false) == false)
		return;

	wprintf(L"Disconnect reason : %ls\n", cause);

	//OnDisconnected();
	//GetService()->RemoveSession(GetSession());

	RegisterDisconnect();
}

bool Session::RegisterConnect()
{
	if (IsConnected())
		return false;

	if (GetService()->GetServiceType() != ServiceType::CLIENT)
		return false;

	if (!SocketHelper::SetReuseAddress(socket, true))
		return false;

	if (SocketHelper::BindAny(socket, 0) == false)
		return false;

	connectEvent.Init();
	connectEvent.iocpObj = shared_from_this();

	DWORD numOfBytes = 0;
	SOCKADDR_IN sockAddr = GetService()->GetSockAddr();
	if (SocketHelper::ConnectEx(socket, (SOCKADDR*)&sockAddr, sizeof(sockAddr), nullptr, 0, &numOfBytes, &connectEvent))
	{
		int errorCode = WSAGetLastError();
		if (errorCode != ERROR_IO_PENDING)
		{
			HandleError(errorCode);
			connectEvent.iocpObj = nullptr;
			return false;
		}
	}

	return true;
}

void Session::RegisterRecv()
{
	if (!IsConnected())
		return;

	recvEvent.Init();
	recvEvent.iocpObj = shared_from_this();

	WSABUF wsaBuf;
	wsaBuf.buf = (char*)recvBuffer.WritePos();
	wsaBuf.len = recvBuffer.FreeSize();

	DWORD recvLen = 0;
	DWORD flags = 0;

	if (WSARecv(socket, &wsaBuf, 1, &recvLen, &flags, &recvEvent, nullptr) == SOCKET_ERROR)
	{
		int errorCode = WSAGetLastError();
		if (errorCode != WSA_IO_PENDING)
		{
			HandleError(errorCode);
			recvEvent.iocpObj = nullptr;
		}

	}

}

void Session::RegisterSend()
{
	if (!IsConnected())
		return;

	// SendEvent 초기화
	sendEvent.Init();
	// sendEvent의 iocp를 session으로
	sendEvent.iocpObj = shared_from_this();

	int writeSize = 0;
	// sendQueue의 데이터가 남아 있지 않을때까지 돌림
	while (!sendQueue.empty())
	{
		// sendQueue의 앞부분부터 pop시키기 위해서 앞부분 데이터 캐싱
		shared_ptr<SendBuffer> sendBuffer = sendQueue.front();
		// 얼마나 사용했는지 크기 추가
		writeSize += sendBuffer->WriteSize();
		// 앞부분 날림
		sendQueue.pop();
		// SendEvent에 sendQueue에 들어 있는 값들을 밀어 넣음
		sendEvent.sendBuffers.push_back(sendBuffer);
	}

	// 한꺼번에 데이터를 보내기 위해
	vector<WSABUF> wsaBufs;

	// SendEvent의 sendBuffers크기 만큼 공간 예약
	wsaBufs.reserve(sendEvent.sendBuffers.size());

	// SendEvent의 sendBuffers 순회하면서 등록
	for (auto sendBuffer : sendEvent.sendBuffers)
	{
		WSABUF wsaBuf;
		wsaBuf.buf = (char*)sendBuffer->GetBuffer();
		wsaBuf.len = sendBuffer->WriteSize();
		wsaBufs.push_back(wsaBuf);
	}

	DWORD recvLen = 0;
	DWORD flags = 0;

	// 한꺼번에 여러개 보내기
	if (WSASend(socket, wsaBufs.data(), (DWORD)wsaBufs.size(), &recvLen, flags, &sendEvent, nullptr) == SOCKET_ERROR)
	{
		int errorCode = WSAGetLastError();
		if (errorCode != WSA_IO_PENDING)
		{
			HandleError(errorCode);
			// Session에 있으니까
			sendEvent.iocpObj = nullptr;
			// SendEvent의 sendBuffers 깨끗이
			sendEvent.sendBuffers.clear();
			// sendRegistered 상태를 false
			sendRegistered.store(false);
		}
	}
}

bool Session::RegisterDisconnect()
{
	disconnectEvent.Init();
	disconnectEvent.iocpObj = shared_from_this();

	if (SocketHelper::DisconnectEx(socket, &disconnectEvent, TF_REUSE_SOCKET, 0))
	{
		int errorCode = WSAGetLastError();
		if (errorCode != WSA_IO_PENDING)
		{
			HandleError(errorCode);
			disconnectEvent.iocpObj = nullptr;
			return false;
		}
	}

	return true;
}

void Session::ObserveIO(IocpEvent* iocpEvent, DWORD bytesTransferred)
{
	switch (iocpEvent->eventType)
	{
	case EventType::CONNECT:
		ProcessConnect();
		break;
	case EventType::RECV:
		ProcessRecv(bytesTransferred);
		break;
	case EventType::SEND:
		// 보낼 크기만 보내기
		ProcessSend(bytesTransferred);
		break;
	case EventType::DISCONNECT:
		ProcessDisConnect();
		break;
	default:
		break;
	}
}

void Session::ProcessConnect()
{
	connectEvent.iocpObj = nullptr;

	isConnected.store(true);

	GetService()->AddSession(GetSession());

	OnConnected();

	RegisterRecv();
}

void Session::ProcessSend(int numOfBytes)
{
	// null로 밀고
	sendEvent.iocpObj = nullptr;
	// SendEvent의 sendBuffers 깨끗이
	sendEvent.sendBuffers.clear();

	if (numOfBytes == 0)
	{
		Disconnect(L"Send 0 bytes");
	}

	OnSend(numOfBytes);

	unique_lock<shared_mutex> lock(rwLock);
	if (sendQueue.empty())
		// sendRegistered 상태를 false
		sendRegistered.store(false);
	else
		// RegisterSend를 한번 더 실행
		RegisterSend();
}

void Session::ProcessRecv(int numOfBytes)
{
	recvEvent.iocpObj = nullptr;

	if (numOfBytes == 0)
	{
		Disconnect(L"Recv 0 bytes");
		return;
	}

	if (recvBuffer.OnWrite(numOfBytes) == false)
	{
		Disconnect(L"On write overflow");
		return;
	}

	int dataSize = recvBuffer.DataSize();

	int processLen = OnRecv(recvBuffer.ReadPos(), numOfBytes);
	if (processLen < 0 || dataSize < processLen || recvBuffer.OnRead(processLen) == false)
	{
		Disconnect(L"On read overflow");
		return;
	}

	recvBuffer.Clear();

	RegisterRecv();
}

void Session::ProcessDisConnect()
{
	disconnectEvent.iocpObj = nullptr;

	OnDisconnected();
	GetService()->RemoveSession(GetSession());
}

void Session::HandleError(int errorCode)
{
	switch (errorCode)
	{
	case WSAECONNRESET:
	case WSAECONNABORTED:
		Disconnect(L"Handle Error");
		break;
	default:
		printf("Error Code : %d\n", errorCode);
		break;
	}
}
