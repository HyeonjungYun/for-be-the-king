// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "Containers/Queue.h"
#include "S1.h"

class FSocket;

struct S1_API FPacketHeader
{
	FPacketHeader() : PacketSize(0), PacketID(0)
	{
	}

	FPacketHeader(uint16 PacketSize, uint16 PacketID) : PacketSize(PacketSize), PacketID(PacketID)
	{
	}

	friend FArchive& operator<<(FArchive& Ar, FPacketHeader& Header)
	{
		Ar << Header.PacketSize;
		Ar << Header.PacketID;
		return Ar;
	}

	uint16 PacketSize;
	uint16 PacketID;
};

/*---------------
	RecvWorker
---------------*/

class S1_API RecvWorker : public FRunnable
{
public:
	RecvWorker(FSocket* Socket, TSharedPtr<class PacketSession> Session);
	~RecvWorker();

	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Exit() override;

	void Destroy();

private:
	bool ReceivePacket(TArray<uint8>& OutPacket);
	bool ReceiveDesiredBytes(uint8* Results, int32 Size);

protected:
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> Running = true;
	FSocket* Socket;
	TWeakPtr<class PacketSession> SessionRef;	
	// 세션의 큐를 세션에서 직접 받아오면 세션이 종료되었을 때 큐의 포인터가 null이 되므로 세션을 직접 받아와 레퍼런스 카운트를 1 늘려준다
};

/*---------------
	SendWorker
---------------*/

class S1_API SendWorker : public FRunnable
{
public:
	SendWorker(FSocket* Socket, TSharedPtr<class PacketSession> Session);
	~SendWorker();

	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Exit() override;

	void Destroy();

private:
	bool SendPacket(SendBufferRef SendBuffer);
	bool SendDesiredBytes(const uint8* Buffer, int32 Size);

protected:
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> Running = true;
	FSocket* Socket;
	TWeakPtr<class PacketSession> SessionRef;
};