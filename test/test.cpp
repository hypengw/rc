#include <gtest/gtest.h>
#include <string>

import rstd.rc;

using namespace rstd::rc;

struct TestStruct {
    int value;
    bool* destroyed;
    
    explicit TestStruct(int v, bool* d) : value(v), destroyed(d) {}
    ~TestStruct() { if (destroyed) *destroyed = true; }
};

TEST(RcTest, BasicConstruction) {
    Rc<int> empty;
    EXPECT_EQ(empty.get(), nullptr);
    
    auto ptr = new int(42);
    Rc<int> rc(ptr);
    EXPECT_EQ(*rc, 42);
    EXPECT_EQ(rc.strong_count(), 1);
    EXPECT_EQ(rc.weak_count(), 0);
}

TEST(RcTest, MakeRc) {
    auto rc = make_rc<std::string>("test");
    EXPECT_EQ(*rc, "test");
    EXPECT_EQ(rc.strong_count(), 1);
}

TEST(RcTest, CopyAndMove) {
    auto rc1 = make_rc<int>(42);
    auto rc2 = rc1;
    EXPECT_EQ(rc1.strong_count(), 2);
    EXPECT_EQ(rc2.strong_count(), 2);
    
    auto rc3 = std::move(rc2);
    EXPECT_EQ(rc2.get(), nullptr);
    EXPECT_EQ(rc1.strong_count(), 2);
    EXPECT_EQ(rc3.strong_count(), 2);
}

TEST(RcTest, Destruction) {
    bool destroyed = false;
    {
        auto rc = make_rc<TestStruct>(42, &destroyed);
        EXPECT_FALSE(destroyed);
    }
    EXPECT_TRUE(destroyed);
}

TEST(RcTest, WeakReference) {
    auto rc = make_rc<int>(42);
    auto weak = rc.downgrade();
    
    EXPECT_EQ(rc.strong_count(), 1);
    EXPECT_EQ(rc.weak_count(), 1);
    
    {
        auto upgraded = weak.upgrade();
        ASSERT_TRUE(upgraded.has_value());
        EXPECT_EQ(**upgraded, 42);
        EXPECT_EQ(rc.strong_count(), 2);
    }
    
    rc = Rc<int>();  // Reset original
    auto upgraded = weak.upgrade();
    EXPECT_FALSE(upgraded.has_value());
}

TEST(RcTest, CustomDeleter) {
    bool custom_deleted = false;
    {
        auto deleter = [&custom_deleted](int* p) {
            custom_deleted = true;
            delete p;
        };
        Rc<int> rc(new int(42), deleter);
    }
    EXPECT_TRUE(custom_deleted);
}

TEST(RcTest, ArraySupport) {
    auto rc = Rc<int[]>::make(3, 42);
    EXPECT_EQ(rc.get()[0], 42);
    EXPECT_EQ(rc.get()[1], 42);
    EXPECT_EQ(rc.get()[2], 42);
}

TEST(RcTest, Uniqueness) {
    auto rc1 = make_rc<int>(42);
    EXPECT_TRUE(rc1.is_unique());
    
    auto weak = rc1.downgrade();
    EXPECT_FALSE(rc1.is_unique());
    
    auto rc2 = rc1;
    EXPECT_FALSE(rc1.is_unique());
    EXPECT_FALSE(rc2.is_unique());
}

TEST(RcTest, SwapOperation) {
    auto rc1 = make_rc<int>(1);
    auto rc2 = make_rc<int>(2);
    
    swap(rc1, rc2);
    EXPECT_EQ(*rc1, 2);
    EXPECT_EQ(*rc2, 1);
}
