#include "pch.h"
#include "BufferReader.h"
#include "BufferWriter.h"

TEST(Buffer, 쓴_값을_그대로_읽는다)
{
	BYTE raw[64] = {};

	uint16 a = 0x1234;
	uint32 b = 0xDEADBEEF;
	float  c = 3.14f;

	BufferWriter w(raw, sizeof(raw));
	w << a << b << c;

	uint16 ra = 0;
	uint32 rb = 0;
	float  rc = 0.f;

	BufferReader r(raw, sizeof(raw));
	r >> ra >> rb >> rc;

	EXPECT_EQ(ra, a);
	EXPECT_EQ(rb, b);
	EXPECT_FLOAT_EQ(rc, c);
}

TEST(Buffer, 쓴_만큼_커서가_움직인다)
{
	BYTE raw[64] = {};
	BufferWriter w(raw, sizeof(raw));

	uint32 v = 1;
	w.Write(&v);

	EXPECT_EQ(w.WriteSize(), sizeof(uint32));
	EXPECT_EQ(w.FreeSize(), sizeof(raw) - sizeof(uint32));
}

TEST(Buffer, 용량을_넘기면_쓰기가_실패한다)
{
	BYTE raw[4] = {};
	BufferWriter w(raw, sizeof(raw));

	uint32 v = 1;
	EXPECT_TRUE(w.Write(&v));
	EXPECT_FALSE(w.Write(&v));
}

TEST(Buffer, 용량을_넘기면_읽기가_실패한다)
{
	BYTE raw[4] = {};
	BufferReader r(raw, sizeof(raw));

	uint32 v = 0;
	EXPECT_TRUE(r.Read(&v));
	EXPECT_FALSE(r.Read(&v));
}

TEST(Buffer, Peek은_커서를_움직이지_않는다)
{
	BYTE raw[8] = {};
	uint32 v = 0xABCD1234;

	BufferWriter w(raw, sizeof(raw));
	w.Write(&v);

	BufferReader r(raw, sizeof(raw));

	uint32 peeked = 0;
	EXPECT_TRUE(r.Peek(&peeked));
	EXPECT_EQ(peeked, v);
	EXPECT_EQ(r.ReadSize(), 0u);

	uint32 read = 0;
	EXPECT_TRUE(r.Read(&read));
	EXPECT_EQ(read, v);
	EXPECT_EQ(r.ReadSize(), sizeof(uint32));
}

TEST(Buffer, Reserve는_공간이_없으면_nullptr을_준다)
{
	BYTE raw[4] = {};
	BufferWriter w(raw, sizeof(raw));

	EXPECT_NE(w.Reserve<uint32>(), nullptr);
	EXPECT_EQ(w.Reserve<uint32>(), nullptr);
}