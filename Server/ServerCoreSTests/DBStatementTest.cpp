#include "pch.h"
#include "DBConnection.h"
#include "DBStatement.h"

// prepared statement 래퍼 (DBStatement.h, 2026-10-21). 실 DB 가 필요하다 - FBTK_DB_PASSWORD 가 있고 127.0.0.1:3306 의 forbetheking 에
// fbtk_app 으로 붙을 수 있을 때만 검사한다 (CI 러너에는 DB 가 없다 - 그때는 건너뛴 것을 출력하고 통과)

namespace
{
	bool OpenDevDb(OUT DBConnection& conn)
	{
		char* raw = nullptr;
		size_t length = 0;
		if (_dupenv_s(&raw, &length, "FBTK_DB_PASSWORD") != 0 || raw == nullptr)
			return false;
		const string password = raw;
		::free(raw);

		return conn.Connect("127.0.0.1", 3306, "fbtk_app", password.c_str(), "forbetheking");
	}

	#define SKIP_WITHOUT_DB(conn)																		\
		if (OpenDevDb(OUT conn) == false)																\
		{																								\
			printf("[  SKIPPED ] no local DB (FBTK_DB_PASSWORD / 127.0.0.1:3306 / fbtk_app)\n");			\
			return;																						\
		}
}

TEST(DBStatement, 바인딩한_문자열은_따옴표와_세미콜론이_있어도_그대로_돌아온다)
{
	DBConnection conn;
	SKIP_WITHOUT_DB(conn);

	const string tricky = "a'b\"c\\d; DROP TABLE accounts; -- \xEA\xB0\x80";		// 작은따옴표 · 큰따옴표 · 역슬래시 · 세미콜론 · 주석 · 한글
	DBStatement stmt(conn, "SELECT ?, ? + 1");
	ASSERT_TRUE(stmt.IsValid()) << stmt.Error();
	ASSERT_TRUE(stmt.BindString(tricky).BindUInt64(41).Query()) << stmt.Error();

	vector<string> row;
	ASSERT_TRUE(stmt.Fetch(OUT row));
	ASSERT_EQ(row.size(), 2u);
	EXPECT_EQ(row[0], tricky);
	EXPECT_EQ(row[1], "42");
	EXPECT_FALSE(stmt.Fetch(OUT row));
}

TEST(DBStatement, 처음_버퍼보다_긴_열은_잘리지_않는다)
{
	DBConnection conn;
	SKIP_WITHOUT_DB(conn);

	const string longText(5000, 'x');		// RESULT_BUFFER_BYTES(256) 를 넘긴다 - Fetch 의 다시 받기 경로
	DBStatement stmt(conn, "SELECT ?");
	ASSERT_TRUE(stmt.BindString(longText).Query()) << stmt.Error();

	vector<string> row;
	ASSERT_TRUE(stmt.Fetch(OUT row));
	ASSERT_EQ(row.size(), 1u);
	EXPECT_EQ(row[0].size(), longText.size());
	EXPECT_EQ(row[0], longText);
}

TEST(DBStatement, NULL_은_빈_문자열이고_숫자_열은_문자열로_온다)
{
	DBConnection conn;
	SKIP_WITHOUT_DB(conn);

	DBStatement stmt(conn, "SELECT NULL, 12345678901234, COUNT(*) FROM accounts WHERE username = ?");
	ASSERT_TRUE(stmt.BindString("no-such-user-\x27").Query()) << stmt.Error();

	vector<string> row;
	ASSERT_TRUE(stmt.Fetch(OUT row));
	ASSERT_EQ(row.size(), 3u);
	EXPECT_EQ(row[0], "");
	EXPECT_EQ(row[1], "12345678901234");
	EXPECT_EQ(row[2], "0");
}

TEST(DBStatement, 잘못된_문장은_IsValid_false_이고_오류_번호가_있다)
{
	DBConnection conn;
	SKIP_WITHOUT_DB(conn);

	DBStatement stmt(conn, "SELEC nonsense FROM nowhere");
	EXPECT_FALSE(stmt.IsValid());
	EXPECT_NE(stmt.ErrorNo(), 0u);
	EXPECT_FALSE(stmt.Execute());
}

TEST(DBStatement, 매개변수_수가_다르면_실행하지_않는다)
{
	DBConnection conn;
	SKIP_WITHOUT_DB(conn);

	DBStatement stmt(conn, "SELECT ?, ?");
	ASSERT_TRUE(stmt.IsValid()) << stmt.Error();
	EXPECT_FALSE(stmt.BindString("only one").Query());
}
