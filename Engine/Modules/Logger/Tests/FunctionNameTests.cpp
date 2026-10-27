// The function name a record keeps, cut from the literal function text of clang, GCC and MSVC.

#include "LoggerInternals.h"

#include <gtest/gtest.h>

namespace
{

using Logger::QualifiedFunctionName;

TEST(FunctionNameTest, AnonymousNamespaceIsDroppedOnEveryCompiler)
{
    EXPECT_EQ(QualifiedFunctionName("void (anonymous namespace)::Helper(int)"), "Helper");
    EXPECT_EQ(QualifiedFunctionName("virtual void (anonymous namespace)::Fixture::TestBody()"), "Fixture::TestBody");
    EXPECT_EQ(QualifiedFunctionName("virtual void {anonymous}::Fixture::TestBody()"), "Fixture::TestBody");
    EXPECT_EQ(QualifiedFunctionName("`anonymous-namespace'::Fixture::TestBody"), "Fixture::TestBody");
}

TEST(FunctionNameTest, CallOperatorKeepsItsName)
{
    EXPECT_EQ(QualifiedFunctionName("void Rendering::Pass::operator()()"), "Rendering::Pass::operator()");
    EXPECT_EQ(QualifiedFunctionName("Rendering::Pass::operator ()"), "Rendering::Pass::operator ()");
    EXPECT_EQ(QualifiedFunctionName("bool Rendering::Pass::operator<(const Pass &) const"),
              "Rendering::Pass::operator<");
}

TEST(FunctionNameTest, LambdaKeepsItsEnclosingFunction)
{
    EXPECT_EQ(QualifiedFunctionName("auto Rendering::Outer()::(lambda)::operator()(int) const"),
              "Rendering::Outer()::(lambda)::operator()");
    EXPECT_EQ(QualifiedFunctionName("Rendering::Outer()::<lambda(int)>"), "Rendering::Outer");
    EXPECT_EQ(QualifiedFunctionName("Rendering::Outer::<lambda_1>::operator ()"),
              "Rendering::Outer::<lambda_1>::operator ()");
}

TEST(FunctionNameTest, TemplateArgumentsAndReturnTypeAreCut)
{
    EXPECT_EQ(QualifiedFunctionName("std::vector<T> Rendering::Make(T) [T = int]"), "Rendering::Make");
    EXPECT_EQ(QualifiedFunctionName("std::vector<T, std::allocator<T> > Rendering::Make(T) [with T = int]"),
              "Rendering::Make");
    EXPECT_EQ(QualifiedFunctionName("Rendering::Make"), "Rendering::Make");
}

TEST(FunctionNameTest, ConstMemberLosesItsQualifier)
{
    EXPECT_EQ(QualifiedFunctionName("void Rendering::Pass::Execute(int) const"), "Rendering::Pass::Execute");
    EXPECT_EQ(QualifiedFunctionName("Rendering::Pass::Execute"), "Rendering::Pass::Execute");
}

} // namespace
