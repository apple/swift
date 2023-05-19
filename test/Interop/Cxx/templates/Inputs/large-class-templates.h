template <class T>
struct HasTypeWithSelfAsParam {
  using TT = HasTypeWithSelfAsParam<HasTypeWithSelfAsParam<T>>;
};

using WillBeInfinite = HasTypeWithSelfAsParam<int>;

namespace RegressionTest {

// This is a regression test to check that we don't instantiate exponentially
// large templates. The below template will instantiate hundreds of thousands
// (or more) of specializations before hitting the 8-template-param-deep limit.
template<class E, int>
struct SliceExpr {
  using type = typename E::type;
  E expr;
  
  typename E::type* test() { return nullptr; }
};

template<class T>
struct Array {
  using type = T;
};

template<class E>
struct ValExpr {
  using type = typename E::type;
  E expr;
  
  ValExpr<SliceExpr<E, 1>> test1() { return {SliceExpr<E, 1>{expr}}; }
  ValExpr<SliceExpr<E, 2>> test2() { return {SliceExpr<E, 2>{expr}}; }
  ValExpr<SliceExpr<E, 3>> test3() { return {SliceExpr<E, 3>{expr}}; }
  ValExpr<SliceExpr<E, 4>> test4() { return {SliceExpr<E, 4>{expr}}; }
  ValExpr<SliceExpr<E, 5>> test5() { return {SliceExpr<E, 5>{expr}}; }
  ValExpr<SliceExpr<E, 6>> test6() { return {SliceExpr<E, 6>{expr}}; }
  ValExpr<SliceExpr<E, 7>> test7() { return {SliceExpr<E, 7>{expr}}; }
  ValExpr<SliceExpr<E, 8>> test8() { return {SliceExpr<E, 8>{expr}}; }
  ValExpr<SliceExpr<E, 9>> test9() { return {SliceExpr<E, 8>{expr}}; }
  ValExpr<SliceExpr<E, 11>> test11() { return {SliceExpr<E, 11>{expr}}; }
  ValExpr<SliceExpr<E, 12>> test12() { return {SliceExpr<E, 12>{expr}}; }
};

// This class template is exponentially slow to *fully* instantiate (and the
// exponent is the number of testX methods). Make sure that we can still
// partially import it without importing all of its child template
// instantiations.
void user(ValExpr<Array<int>> *) { }

} // end namespace 'RegressionTest'
