#include "pch.h"
#include "LockQueue.h"

TEST(LockQueue, 빈_큐에서_Pop하면_기본값을_돌려준다)
{
	LockQueue<shared_ptr<int>> q;
	EXPECT_EQ(q.Pop(), nullptr);
}

TEST(LockQueue, 넣은_순서대로_나온다)
{
	LockQueue<shared_ptr<int>> q;
	q.Push(make_shared<int>(1));
	q.Push(make_shared<int>(2));

	EXPECT_EQ(*q.Pop(), 1);
	EXPECT_EQ(*q.Pop(), 2);
}

TEST(LockQueue, Clear하면_비워진다)
{
	LockQueue<shared_ptr<int>> q;
	for (int i = 1; i <= 10; i++)
		q.Push(make_shared<int>(i));

	q.Clear();
	EXPECT_EQ(q.Pop(), nullptr);
}

TEST(LockQueue, PopAll이_자기_자신을_다시_잠그지_않는다)
{
	LockQueue<shared_ptr<int>> q;
	for (int i = 1; i <= 100; i++)
		q.Push(make_shared<int>(i));

	vector<shared_ptr<int>> items;
	q.PopAll(OUT items);

	EXPECT_EQ(items.size(), 100u);
}

TEST(LockQueue, 기본값과_구분되지_않는_원소는_PopAll을_조기_종료시킨다)
{
	LockQueue<int> q;
	q.Push(1);
	q.Push(0);   
	q.Push(2);

	vector<int> items;
	q.PopAll(OUT items);

	EXPECT_EQ(items.size(), 1u);
	EXPECT_EQ(items[0], 1);
}
