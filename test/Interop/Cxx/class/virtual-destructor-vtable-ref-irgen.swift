// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-swiftxx-frontend -emit-ir -I %t/Inputs -validate-tbd-against-ir=none %t/test.swift | %FileCheck %s
// RUN: %target-swiftxx-frontend -verify -emit-ir -I %t/Inputs -validate-tbd-against-ir=none %t/test.swift -Xcc -DDELETED -o /dev/null

//--- Inputs/module.modulemap
module VtableDestructorRef {
  header "test.h"
  requires cplusplus
}
//--- Inputs/test.h


namespace impl {

#ifdef DELETED

template<class T>
class BaseClass
{
public:
    ~BaseClass() = delete;
};

template<class Fp, class T>
class Func: public BaseClass<T>
{
   Fp x;
public:
    inline explicit Func(Fp x);
    Func(const Func &) = delete;
    ~Func();
};

#else

template<class T>
class BaseClass
{
public:
    virtual ~BaseClass() {}
};

template<class Fp, class T>
class Func: public BaseClass<T>
{
   Fp x;
public:
    
    inline explicit Func(Fp x) : x(x) {}
};

#endif

template <class _Fp> class ValueFunc;

template <class _Rp, class... _ArgTypes> class ValueFunc<_Rp(_ArgTypes...)>
{
    typedef impl::BaseClass<_Rp(_ArgTypes...)> FuncTy;
    FuncTy* _Nullable f;
  public:

    template <class _Fp>
    ValueFunc(_Fp fp) {
        typedef impl::Func<_Fp, _Rp(_ArgTypes...)> _Fun;
        f = ::new _Fun(fp);
    }

    ValueFunc(ValueFunc&& other) {
        if (other.f == nullptr)
            f = nullptr;
        else
        {
            f = other.f;
            other.f = nullptr;
        }
    }
};

template<class _Rp>
class Function;

template<class _Rp, class ..._ArgTypes>
class Function<_Rp(_ArgTypes...)> {
    ValueFunc<_Rp(_ArgTypes...)> f;
public:
  template<class _Fp>
  Function(_Fp);
};

template <class _Rp, class... _ArgTypes>
template <class _Fp>
Function<_Rp(_ArgTypes...)>::Function(_Fp f) : f(f) {}

}

class MyFutureBase {
public:
  void OnCompletion(impl::Function<void(const MyFutureBase&)> callback) const;
};

template<class T>
class MyFuture : public MyFutureBase {
public:
    void OnCompletion(
      void (* _Nonnull completion)(void * _Nullable),
       void * _Nullable user_data) const {
    MyFutureBase::OnCompletion(
        [completion, user_data](const MyFutureBase&) {
          completion(user_data);
        });
  }
};

using MyFutureInt = MyFuture<int>;

//--- test.swift

import VtableDestructorRef

public func test() {
  let f = MyFutureInt()
  f.OnCompletion({ _ in
    print("done")
  }, nil)
}

// Make sure we reach the virtual destructor of 'Func'.
// CHECK: define linkonce_odr {{.*}} @{{_ZN4impl4FuncIZNK8MyFutureIiE12OnCompletionEPFvPvES3_EUlRK12MyFutureBaseE_FvS8_EED2Ev|"\?\?1\?\$BaseClass@\$\$A6AXAEBVMyFutureBase@@@Z@impl@@UEAA@XZ"}}
