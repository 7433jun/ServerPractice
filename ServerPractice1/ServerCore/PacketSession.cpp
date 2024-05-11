#include "pch.h"
#include "PacketSession.h"

//[0]..........................[25][26]..........................[50]....
//[packet                         ][packet                          ]
//[size(2byte)][id(2byte)][data...][size(2byte)][id(2bytes)][data...]...
int PacketSession::OnRecv(BYTE* buffer, int len)
{
	// ó���� ����
	int processLen = 0;
	while (true)
	{
		int dataSize = len - processLen;

		if (dataSize < sizeof(PacketHeader))
			break;

		// [size(2)][id(2)] = *(4byte  point )(buffer[0]]��°�� �ּ�)
		PacketHeader header = *(PacketHeader*)(&buffer[processLen]);

		// OnRecvPacket���ٰ� (��Ŷ�� ���� �ּ�, ũ��)
		// ū�����͸� �ɰ��� �Ѱ�
		// �����ּ�->[size(2byte)][id(2byte)][data...] <- ��ü ũ�⸸
		OnRecvPacket(&buffer[processLen], header.size);

		// ����� ���̴� header�� size�� ����ִ� �ش� ��Ŷ�� ũ�⸸ŭ ����
		processLen += header.size;
	}

	return processLen;
}